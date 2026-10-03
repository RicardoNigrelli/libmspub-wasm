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
#include <emscripten/heap.h>
#include <emscripten/val.h>

#include <librevenge-generators/librevenge-generators.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>
#include <libmspub/libmspub.h>

#include <unicode/ucnv.h>
#include <unicode/ucsdet.h>
#include <unicode/udata.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

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
// (raw bytes), so `bytes` below is the file content. That conversion copies
// the file twice inside the wasm heap: fine for isSupported/toSVG/toRaw
// (debugging), not for real documents, which go through the binary API below.

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

// ---------------------------------------------------------------------------
// Binary API (JSON format version 2)
//
// The file goes into a buffer inside the wasm heap that JS fills directly
// (no std::string copies), and the pictures come back as separate binary
// blobs instead of base64 inside the JSON:
//
//   view = inputBuffer(n)  -> Uint8Array over n bytes of wasm memory; JS fills
//                             it right away (the view dies if memory grows)
//   r = parseInput()       -> { ok, pages, json, blobs, error?, unsupported? }
//                             the input buffer is freed as soon as libmspub
//                             has its own copy of the stream
//   blob(i)                -> Uint8Array view of picture i ("blob": i in the
//                             JSON); copy it out (slice()) before calling
//                             anything else
//   release()              -> frees the blobs and any pending input
//
// The wasm memory itself never shrinks (WebAssembly has no way to give it
// back): release() makes the space reusable for the next document, and
// dropping the instance is the only way to return it to the system.
// ---------------------------------------------------------------------------

struct PendingState
{
  unsigned char *input = nullptr;
  size_t inputSize = 0;
  std::vector<oi::Blob> blobs;
} g_state;

void freeInput()
{
  std::free(g_state.input);
  g_state.input = nullptr;
  g_state.inputSize = 0;
}

void release()
{
  freeInput();
  std::vector<oi::Blob>().swap(g_state.blobs);
}

val inputBuffer(unsigned size)
{
  release();
  if (size == 0)
    return val::null();
  g_state.input = static_cast<unsigned char *>(std::malloc(size));
  if (!g_state.input)
    return val::null();
  g_state.inputSize = size;
  return val(emscripten::typed_memory_view(size, g_state.input));
}

bool isSupportedInput()
{
  if (!g_state.input)
    return false;
  try
  {
    librevenge::RVNGStringStream input(g_state.input, static_cast<unsigned>(g_state.inputSize));
    return libmspub::MSPUBDocument::isSupported(&input);
  }
  catch (...)
  {
    return false;
  }
}

val parseInput()
{
  if (!g_state.input)
    return fail("no input: call inputBuffer() first");
  try
  {
    std::unique_ptr<librevenge::RVNGStringStream> input(
      new librevenge::RVNGStringStream(g_state.input, static_cast<unsigned>(g_state.inputSize)));
    freeInput(); // the stream keeps its own copy
    if (!libmspub::MSPUBDocument::isSupported(input.get()))
    {
      val r = fail("unsupported");
      r.set("unsupported", true);
      return r;
    }
    oi::JSONDrawingGenerator generator;
    bool parsed = libmspub::MSPUBDocument::parse(input.get(), &generator);
    input.reset(); // free the file before serializing
    std::string json = generator.result();
    g_state.blobs = generator.takeBlobs();
    val r = val::object();
    r.set("ok", parsed);
    r.set("pages", generator.pageCount());
    r.set("json", json);
    r.set("blobs", static_cast<unsigned>(g_state.blobs.size()));
    if (!parsed)
      r.set("error", std::string("parse() returned false"));
    return r;
  }
  catch (const std::exception &e)
  {
    release();
    return fail(std::string("C++ exception: ") + e.what());
  }
  catch (...)
  {
    release();
    return fail("unknown C++ exception");
  }
}

val blob(unsigned index)
{
  if (index >= g_state.blobs.size())
    return val::null();
  const std::vector<unsigned char> &d = g_state.blobs[index].data;
  return val(emscripten::typed_memory_view(d.size(), d.data()));
}

// Current size of the wasm linear memory, in bytes. It only grows, so after a
// parse it is the peak of the wasm heap so far.
double heapSize()
{
  return static_cast<double>(emscripten_get_heap_size());
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
  emscripten::function("inputBuffer", &inputBuffer);
  emscripten::function("isSupportedInput", &isSupportedInput);
  emscripten::function("parseInput", &parseInput);
  emscripten::function("blob", &blob);
  emscripten::function("release", &release);
  emscripten::function("heapSize", &heapSize);
  emscripten::function("toRaw", &toRaw);
  emscripten::function("icuProbe", &icuProbe);
}
