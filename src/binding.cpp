/* OpenImprenta spike: embind entry points for libmspub.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * Every entry point returns a plain object { ok, ..., error } instead of
 * throwing, so the JS side never has to deal with WebAssembly.Exception.
 * Traps (out-of-bounds, abort, stack overflow) still surface as a JS
 * RuntimeError; the JS wrapper discards the instance in that case.
 */
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <librevenge-generators/librevenge-generators.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>
#include <libmspub/libmspub.h>

#include <unicode/ucnv.h>
#include <unicode/ucsdet.h>
#include <unicode/udata.h>

#include <exception>
#include <string>

#include "JSONDrawingGenerator.h"

using emscripten::val;

#ifndef OI_NO_ICUDATA
extern "C" const unsigned char oi_icudata_subset[];
#endif

namespace
{

// Register the filtered ICU data (icu/make-icudata.sh) before anything uses
// ICU. Without it only algorithmic converters (UTF-8/16) are available.
struct IcuDataInit
{
  UErrorCode status = U_ZERO_ERROR;
#ifndef OI_NO_ICUDATA
  IcuDataInit() { udata_setCommonData(oi_icudata_subset, &status); }
#else
  IcuDataInit() { status = U_UNSUPPORTED_ERROR; } // stubdata only (comparison build)
#endif
} g_icuDataInit;

// Note: embind converts Uint8Array/ArrayBuffer arguments to std::string
// (raw bytes), so `bytes` below is the file content.

val fail(const std::string &msg)
{
  val r = val::object();
  r.set("ok", false);
  r.set("error", msg);
  return r;
}

bool isSupported(const std::string &bytes)
{
  if (bytes.empty())
    return false;
  try
  {
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char *>(bytes.data()),
                                       static_cast<unsigned>(bytes.size()));
    return libmspub::MSPUBDocument::isSupported(&input);
  }
  catch (...)
  {
    return false;
  }
}

val toSVG(const std::string &bytes)
{
  try
  {
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char *>(bytes.data()),
                                       static_cast<unsigned>(bytes.size()));
    if (!libmspub::MSPUBDocument::isSupported(&input))
      return fail("unsupported");
    librevenge::RVNGStringVector pages;
    librevenge::RVNGSVGDrawingGenerator generator(pages, "svg");
    bool parsed = libmspub::MSPUBDocument::parse(&input, &generator);
    val arr = val::array();
    for (unsigned i = 0; i < pages.size(); ++i)
      arr.call<void>("push", std::string(pages[i].cstr()));
    val r = val::object();
    r.set("ok", parsed);
    r.set("pages", arr);
    if (!parsed)
      r.set("error", std::string("parse() returned false"));
    return r;
  }
  catch (const std::exception &e)
  {
    return fail(std::string("C++ exception: ") + e.what());
  }
  catch (...)
  {
    return fail("unknown C++ exception");
  }
}

val toJSON(const std::string &bytes)
{
  try
  {
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char *>(bytes.data()),
                                       static_cast<unsigned>(bytes.size()));
    if (!libmspub::MSPUBDocument::isSupported(&input))
      return fail("unsupported");
    oi::JSONDrawingGenerator generator;
    bool parsed = libmspub::MSPUBDocument::parse(&input, &generator);
    val r = val::object();
    r.set("ok", parsed);
    r.set("pages", generator.pageCount());
    r.set("json", generator.result());
    if (!parsed)
      r.set("error", std::string("parse() returned false"));
    return r;
  }
  catch (const std::exception &e)
  {
    return fail(std::string("C++ exception: ") + e.what());
  }
  catch (...)
  {
    return fail("unknown C++ exception");
  }
}

// Raw librevenge dump (debugging aid: shows every call libmspub makes).
val toRaw(const std::string &bytes)
{
  try
  {
    librevenge::RVNGStringStream input(reinterpret_cast<const unsigned char *>(bytes.data()),
                                       static_cast<unsigned>(bytes.size()));
    if (!libmspub::MSPUBDocument::isSupported(&input))
      return fail("unsupported");
    // prints every call to stdout (console.log in JS)
    librevenge::RVNGRawDrawingGenerator generator(false);
    bool parsed = libmspub::MSPUBDocument::parse(&input, &generator);
    val r = val::object();
    r.set("ok", parsed);
    return r;
  }
  catch (...)
  {
    return fail("exception");
  }
}

// Diagnostic: which ICU converters can be opened in this build. The
// emscripten ICU port links *stubdata*, so table-based codepages (needed by
// libmspub for Publisher 97 text) may be missing.
val icuProbe()
{
  val r = val::object();
  const char *names[] = {"UTF-16LE", "UTF-8", "windows-1252", "windows-1251", "windows-1250",
                         "windows-1253", "windows-1254", "windows-1255", "windows-1256", "windows-1257",
                         "windows-1258", "windows-874", "windows-932", "windows-936", "windows-950", "macintosh"};
  for (const char *n : names)
  {
    UErrorCode status = U_ZERO_ERROR;
    UConverter *c = ucnv_open(n, &status);
    r.set(n, std::string(u_errorName(status)));
    if (c)
      ucnv_close(c);
  }
  UErrorCode st = U_ZERO_ERROR;
  UCharsetDetector *d = ucsdet_open(&st);
  r.set("ucsdet_open", std::string(u_errorName(st)));
  if (d)
    ucsdet_close(d);
  r.set("udata_setCommonData", std::string(u_errorName(g_icuDataInit.status)));
  return r;
}

} // anonymous namespace

EMSCRIPTEN_BINDINGS(libmspub_spike)
{
  emscripten::function("isSupported", &isSupported);
  emscripten::function("toSVG", &toSVG);
  emscripten::function("toJSON", &toJSON);
  emscripten::function("toRaw", &toRaw);
  emscripten::function("icuProbe", &icuProbe);
}
