/* OpenImprenta spike: JSON drawing generator for librevenge.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#include "JSONDrawingGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace oi
{

// ---------------------------------------------------------------------------
// Json
// ---------------------------------------------------------------------------

Json &Json::set(const std::string &k, Json v)
{
  for (auto &kv : o)
    if (kv.first == k)
    {
      kv.second = std::move(v);
      return kv.second;
    }
  o.emplace_back(k, std::move(v));
  return o.back().second;
}

Json *Json::get(const std::string &k)
{
  for (auto &kv : o)
    if (kv.first == k)
      return &kv.second;
  return nullptr;
}

Json &Json::push(Json v)
{
  a.push_back(std::move(v));
  return a.back();
}

static void escapeString(const std::string &s, std::string &out)
{
  out += '"';
  for (unsigned char c : s)
  {
    switch (c)
    {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20)
      {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      }
      else
        out += char(c);
    }
  }
  out += '"';
}

void Json::serialize(std::string &out) const
{
  switch (type)
  {
  case Null: out += "null"; break;
  case Bool: out += b ? "true" : "false"; break;
  case Num:
  {
    if (!std::isfinite(n))
    {
      out += "null";
      break;
    }
    char buf[32];
    // 4 decimals in points is far below print resolution.
    double r = std::round(n * 10000.0) / 10000.0;
    std::snprintf(buf, sizeof(buf), "%.10g", r);
    out += buf;
    break;
  }
  case Str: escapeString(s, out); break;
  case Arr:
  {
    out += '[';
    for (size_t i = 0; i < a.size(); ++i)
    {
      if (i) out += ',';
      a[i].serialize(out);
    }
    out += ']';
    break;
  }
  case Obj:
  {
    out += '{';
    for (size_t i = 0; i < o.size(); ++i)
    {
      if (i) out += ',';
      escapeString(o[i].first, out);
      out += ':';
      o[i].second.serialize(out);
    }
    out += '}';
    break;
  }
  }
}

// ---------------------------------------------------------------------------
// Base64 (librevenge hands binary data over as base64 strings)
// ---------------------------------------------------------------------------

bool decodeBase64(const char *in, size_t len, std::vector<unsigned char> &out)
{
  // 0-63: value, 64: '=', 65: whitespace, 255: invalid
  static unsigned char table[256];
  static bool tableReady = false;
  if (!tableReady)
  {
    for (unsigned char &t : table)
      t = 255;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (unsigned i = 0; i < 64; ++i)
      table[static_cast<unsigned char>(alphabet[i])] = static_cast<unsigned char>(i);
    table[static_cast<unsigned char>('=')] = 64;
    table[static_cast<unsigned char>(' ')] = table[static_cast<unsigned char>('\n')] = 65;
    table[static_cast<unsigned char>('\r')] = table[static_cast<unsigned char>('\t')] = 65;
    tableReady = true;
  }
  out.resize(len / 4 * 3 + 3);
  size_t o = 0;
  unsigned acc = 0;
  int bits = 0;
  size_t pad = 0;
  for (size_t i = 0; i < len; ++i)
  {
    const unsigned char v = table[static_cast<unsigned char>(in[i])];
    if (v < 64)
    {
      if (pad)
        return false; // data after padding
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8)
      {
        bits -= 8;
        out[o++] = static_cast<unsigned char>((acc >> bits) & 0xFF);
      }
    }
    else if (v == 64)
      ++pad;
    else if (v != 65)
      return false;
  }
  out.resize(o);
  return pad <= 2;
}

// ---------------------------------------------------------------------------
// Property conversion
// ---------------------------------------------------------------------------

static Json propToJson(const char *key, const librevenge::RVNGProperty *prop)
{
  switch (prop->getUnit())
  {
  case librevenge::RVNG_INCH:
    return Json::number(prop->getDouble() * 72.0);
  case librevenge::RVNG_POINT:
    return Json::number(prop->getDouble());
  case librevenge::RVNG_TWIP:
    return Json::number(prop->getDouble() / 20.0);
  case librevenge::RVNG_PERCENT:
    return Json::string(prop->getStr().cstr());
  case librevenge::RVNG_GENERIC:
    return Json::number(prop->getDouble());
  case librevenge::RVNG_UNIT_ERROR:
  default:
  {
    std::string s = prop->getStr().cstr();
    if (s == "true")
      return Json::boolean(true);
    if (s == "false")
      return Json::boolean(false);
    (void)key;
    return Json::string(s);
  }
  }
}

static Json vectorToJson(const librevenge::RVNGPropertyListVector &vec, bool dropBinary)
{
  Json arr = Json::array();
  for (unsigned long i = 0; i < vec.count(); ++i)
    arr.push(propsToJson(vec[i], dropBinary));
  return arr;
}

Json propsToJson(const librevenge::RVNGPropertyList &propList, bool dropBinary)
{
  Json obj = Json::object();
  librevenge::RVNGPropertyList::Iter it(propList);
  for (it.rewind(); it.next();)
  {
    const char *key = it.key();
    if (it.child())
    {
      obj.set(key, vectorToJson(*it.child(), dropBinary));
      continue;
    }
    if (!it())
      continue;
    if (dropBinary && (std::strcmp(key, "office:binary-data") == 0 || std::strcmp(key, "draw:fill-image") == 0))
    {
      obj.set(key, Json::string("<binary omitted>"));
      continue;
    }
    obj.set(key, propToJson(key, it()));
  }
  return obj;
}

static const librevenge::RVNGProperty *P(const librevenge::RVNGPropertyList &l, const char *k)
{
  return l[k];
}

static Json lengthOrNull(const librevenge::RVNGPropertyList &l, const char *k)
{
  const librevenge::RVNGProperty *p = P(l, k);
  if (!p)
    return Json::null();
  return propToJson(k, p);
}

static Json strOrNull(const librevenge::RVNGPropertyList &l, const char *k)
{
  const librevenge::RVNGProperty *p = P(l, k);
  return p ? Json::string(p->getStr().cstr()) : Json::null();
}

// Number of a non-ODF property set by our libmspub patches, or null.
static Json numberOrNull(const librevenge::RVNGPropertyList &l, const char *k)
{
  const librevenge::RVNGProperty *p = P(l, k);
  if (!p)
    return Json::null();
  const double v = p->getDouble();
  return std::isfinite(v) ? Json::number(v) : Json::null();
}

Json JSONDrawingGenerator::blobFrom(const librevenge::RVNGProperty *prop, const librevenge::RVNGProperty *mimeType)
{
  if (!prop)
    return Json::null();
  const librevenge::RVNGString b64 = prop->getStr();
  Blob blob;
  if (!decodeBase64(b64.cstr(), b64.size(), blob.data) || blob.data.empty())
  {
    count("!badBinaryData");
    return Json::null();
  }
  blob.mimeType = mimeType ? mimeType->getStr().cstr() : "";
  // libmspub repeats the picture in every setStyle() that carries its fill
  // (fill, then stroke) and in every shape that uses it: store it once.
  for (size_t i = 0; i < m_blobs.size(); ++i)
    if (m_blobs[i].mimeType == blob.mimeType && m_blobs[i].data == blob.data)
      return Json::number(double(i));
  m_blobs.push_back(std::move(blob));
  return Json::number(double(m_blobs.size() - 1));
}

// Curated summary of the current graphic style (fill + stroke).
Json JSONDrawingGenerator::summarizeStyle(const librevenge::RVNGPropertyList &style)
{
  Json out = Json::object();

  Json fill = Json::object();
  std::string fillKind = P(style, "draw:fill") ? P(style, "draw:fill")->getStr().cstr() : "none";
  fill.set("kind", Json::string(fillKind));
  if (fillKind == "solid")
  {
    fill.set("color", strOrNull(style, "draw:fill-color"));
    fill.set("opacity", strOrNull(style, "draw:opacity"));
  }
  else if (fillKind == "gradient")
  {
    fill.set("angle", lengthOrNull(style, "draw:angle"));
    fill.set("style", strOrNull(style, "draw:style"));
    if (style.child("svg:linearGradient"))
      fill.set("stops", vectorToJson(*style.child("svg:linearGradient"), true));
    else if (style.child("svg:radialGradient"))
      fill.set("stops", vectorToJson(*style.child("svg:radialGradient"), true));
  }
  else if (fillKind == "bitmap")
  {
    fill.set("mimeType", strOrNull(style, "librevenge:mime-type"));
    fill.set("repeat", strOrNull(style, "style:repeat"));
    // libmspub draws pictures as shapes with a bitmap fill (not with
    // drawGraphicObject). The picture travels as a binary blob.
    fill.set("blob", blobFrom(P(style, "draw:fill-image"), P(style, "librevenge:mime-type")));
    // Picture crop (patch 0003): fractions of the picture hidden on each side.
    Json top = numberOrNull(style, "libmspub:crop-from-top");
    Json right = numberOrNull(style, "libmspub:crop-from-right");
    Json bottom = numberOrNull(style, "libmspub:crop-from-bottom");
    Json left = numberOrNull(style, "libmspub:crop-from-left");
    if (top.type == Json::Num || right.type == Json::Num || bottom.type == Json::Num || left.type == Json::Num)
    {
      Json crop = Json::object();
      crop.set("top", top.type == Json::Num ? top : Json::number(0));
      crop.set("right", right.type == Json::Num ? right : Json::number(0));
      crop.set("bottom", bottom.type == Json::Num ? bottom : Json::number(0));
      crop.set("left", left.type == Json::Num ? left : Json::number(0));
      fill.set("crop", std::move(crop));
    }
  }
  out.set("fill", fill);

  Json stroke = Json::object();
  std::string strokeKind = P(style, "draw:stroke") ? P(style, "draw:stroke")->getStr().cstr() : "none";
  stroke.set("kind", Json::string(strokeKind));
  if (strokeKind != "none")
  {
    stroke.set("color", strOrNull(style, "svg:stroke-color"));
    stroke.set("width", lengthOrNull(style, "svg:stroke-width"));
    stroke.set("opacity", strOrNull(style, "svg:stroke-opacity"));
    stroke.set("linecap", strOrNull(style, "svg:stroke-linecap"));
    stroke.set("linejoin", strOrNull(style, "svg:stroke-linejoin"));
  }
  out.set("stroke", stroke);

  if (P(style, "draw:shadow") && std::string(P(style, "draw:shadow")->getStr().cstr()) == "visible")
  {
    Json sh = Json::object();
    sh.set("color", strOrNull(style, "draw:shadow-color"));
    sh.set("offsetX", lengthOrNull(style, "draw:shadow-offset-x"));
    sh.set("offsetY", lengthOrNull(style, "draw:shadow-offset-y"));
    sh.set("opacity", strOrNull(style, "draw:shadow-opacity"));
    out.set("shadow", sh);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

JSONDrawingGenerator::JSONDrawingGenerator()
  : m_doc(Json::object()), m_style(Json::object()), m_stack(), m_calls(),
    m_listOrdered(), m_link(), m_inLink(false), m_pages(0), m_orphanText(0)
{
  m_doc.set("format", Json::string("openimprenta-libmspub-json"));
  m_doc.set("version", Json::number(2));
  m_doc.set("units", Json::string("pt"));
  m_doc.set("metadata", Json::object());
  m_doc.set("embeddedFonts", Json::array());
  m_doc.set("pages", Json::array());
  m_stack.push_back(Frame{K_DOC, &m_doc, "pages"});
}

JSONDrawingGenerator::~JSONDrawingGenerator() {}

unsigned JSONDrawingGenerator::pageCount() const { return m_pages; }

std::vector<Blob> JSONDrawingGenerator::takeBlobs()
{
  std::vector<Blob> out;
  out.swap(m_blobs);
  return out;
}

std::string JSONDrawingGenerator::result()
{
  // No copy of the tree: the statistics go into m_doc itself.
  Json &doc = m_doc;
  Json blobs = Json::array();
  for (const Blob &b : m_blobs)
  {
    Json o = Json::object();
    o.set("size", Json::number(double(b.data.size())));
    o.set("mimeType", Json::string(b.mimeType));
    blobs.push(std::move(o));
  }
  doc.set("blobs", std::move(blobs));
  Json calls = Json::object();
  for (const auto &kv : m_calls)
    calls.set(kv.first, Json::number(kv.second));
  Json stats = Json::object();
  stats.set("calls", calls);
  stats.set("orphanText", Json::number(m_orphanText));
  stats.set("unclosedFrames", Json::number(double(m_stack.size() - 1)));
  doc.set("stats", stats);
  std::string out;
  out.reserve(1 << 16);
  doc.serialize(out);
  return out;
}

void JSONDrawingGenerator::count(const char *call) { ++m_calls[call]; }

Json *JSONDrawingGenerator::currentChildren()
{
  Frame &f = m_stack.back();
  return f.node->get(f.childrenKey);
}

void JSONDrawingGenerator::push(Kind kind, Json node, const char *childrenKey)
{
  if (*childrenKey)
    node.set(childrenKey, Json::array());
  Json *children = currentChildren();
  if (!children)
    return;
  Json &added = children->push(std::move(node));
  m_stack.push_back(Frame{kind, &added, childrenKey});
}

bool JSONDrawingGenerator::inStack(Kind kind) const
{
  for (const auto &f : m_stack)
    if (f.kind == kind)
      return true;
  return false;
}

bool JSONDrawingGenerator::pop(Kind kind)
{
  // Ignore unbalanced close calls (broken files) instead of corrupting the tree.
  if (!inStack(kind) || kind == K_DOC)
  {
    count("!unbalancedClose");
    return false;
  }
  while (m_stack.size() > 1)
  {
    Kind k = m_stack.back().kind;
    m_stack.pop_back();
    if (k == kind)
      break;
  }
  return true;
}

// ----- document -----

void JSONDrawingGenerator::startDocument(const librevenge::RVNGPropertyList &propList)
{
  count("startDocument");
  if (!propList.empty())
    m_doc.set("documentProps", propsToJson(propList));
}

void JSONDrawingGenerator::endDocument()
{
  count("endDocument");
  while (m_stack.size() > 1)
    m_stack.pop_back();
}

void JSONDrawingGenerator::setDocumentMetaData(const librevenge::RVNGPropertyList &propList)
{
  count("setDocumentMetaData");
  m_doc.set("metadata", propsToJson(propList));
}

void JSONDrawingGenerator::defineEmbeddedFont(const librevenge::RVNGPropertyList &propList)
{
  count("defineEmbeddedFont");
  Json f = Json::object();
  f.set("name", strOrNull(propList, "librevenge:name"));
  f.set("mimeType", strOrNull(propList, "librevenge:mime-type"));
  f.set("blob", blobFrom(P(propList, "office:binary-data"), P(propList, "librevenge:mime-type")));
  f.set("props", propsToJson(propList, true));
  m_doc.get("embeddedFonts")->push(std::move(f));
}

// ----- pages -----

void JSONDrawingGenerator::startPage(const librevenge::RVNGPropertyList &propList)
{
  count("startPage");
  // A page always hangs from the document, whatever was left open.
  while (m_stack.size() > 1)
    m_stack.pop_back();
  Json page = Json::object();
  page.set("index", Json::number(m_pages));
  page.set("width", lengthOrNull(propList, "svg:width"));
  page.set("height", lengthOrNull(propList, "svg:height"));
  page.set("props", propsToJson(propList, true));
  push(K_PAGE, std::move(page), "elements");
  ++m_pages;
}

void JSONDrawingGenerator::endPage()
{
  count("endPage");
  pop(K_PAGE);
}

void JSONDrawingGenerator::startMasterPage(const librevenge::RVNGPropertyList &) { count("startMasterPage"); }
void JSONDrawingGenerator::endMasterPage() { count("endMasterPage"); }

void JSONDrawingGenerator::setStyle(const librevenge::RVNGPropertyList &propList)
{
  count("setStyle");
  // binary data (the bitmap fill) goes only into summary.fill.data
  m_style = propsToJson(propList, true);
  m_style.set("summary", summarizeStyle(propList));
}

void JSONDrawingGenerator::startLayer(const librevenge::RVNGPropertyList &propList)
{
  count("startLayer");
  Json layer = Json::object();
  layer.set("type", Json::string("layer"));
  layer.set("props", propsToJson(propList));
  push(K_LAYER, std::move(layer), "elements");
}

void JSONDrawingGenerator::endLayer()
{
  count("endLayer");
  pop(K_LAYER);
}

void JSONDrawingGenerator::startEmbeddedGraphics(const librevenge::RVNGPropertyList &) { count("startEmbeddedGraphics"); }
void JSONDrawingGenerator::endEmbeddedGraphics() { count("endEmbeddedGraphics"); }

void JSONDrawingGenerator::openGroup(const librevenge::RVNGPropertyList &propList)
{
  count("openGroup");
  Json g = Json::object();
  g.set("type", Json::string("group"));
  g.set("props", propsToJson(propList));
  push(K_GROUP, std::move(g), "elements");
}

void JSONDrawingGenerator::closeGroup()
{
  count("closeGroup");
  pop(K_GROUP);
}

// ----- shapes -----

void JSONDrawingGenerator::addShape(const char *type, const librevenge::RVNGPropertyList &propList)
{
  Json *children = currentChildren();
  Kind k = m_stack.back().kind;
  if (!children || (k != K_PAGE && k != K_GROUP && k != K_LAYER))
  {
    count("!shapeOutsidePage");
    return;
  }
  Json shape = Json::object();
  shape.set("type", Json::string(type));
  shape.set("geometry", propsToJson(propList));
  Json *summary = m_style.get("summary");
  shape.set("fill", summary && summary->get("fill") ? *summary->get("fill") : Json::null());
  shape.set("stroke", summary && summary->get("stroke") ? *summary->get("stroke") : Json::null());
  if (summary && summary->get("shadow"))
    shape.set("shadow", *summary->get("shadow"));
  Json style = m_style;
  // keep the raw style, minus the summary we already expanded
  for (auto it = style.o.begin(); it != style.o.end(); ++it)
    if (it->first == "summary")
    {
      style.o.erase(it);
      break;
    }
  shape.set("style", std::move(style));
  children->push(std::move(shape));
}

void JSONDrawingGenerator::drawRectangle(const librevenge::RVNGPropertyList &p) { count("drawRectangle"); addShape("rect", p); }
void JSONDrawingGenerator::drawEllipse(const librevenge::RVNGPropertyList &p) { count("drawEllipse"); addShape("ellipse", p); }
void JSONDrawingGenerator::drawPolygon(const librevenge::RVNGPropertyList &p) { count("drawPolygon"); addShape("polygon", p); }
void JSONDrawingGenerator::drawPolyline(const librevenge::RVNGPropertyList &p) { count("drawPolyline"); addShape("polyline", p); }
void JSONDrawingGenerator::drawPath(const librevenge::RVNGPropertyList &p) { count("drawPath"); addShape("path", p); }
void JSONDrawingGenerator::drawConnector(const librevenge::RVNGPropertyList &p) { count("drawConnector"); addShape("connector", p); }

void JSONDrawingGenerator::drawGraphicObject(const librevenge::RVNGPropertyList &propList)
{
  count("drawGraphicObject");
  Json *children = currentChildren();
  Kind k = m_stack.back().kind;
  if (!children || (k != K_PAGE && k != K_GROUP && k != K_LAYER))
  {
    count("!imageOutsidePage");
    return;
  }
  Json img = Json::object();
  img.set("type", Json::string("image"));
  img.set("x", lengthOrNull(propList, "svg:x"));
  img.set("y", lengthOrNull(propList, "svg:y"));
  img.set("width", lengthOrNull(propList, "svg:width"));
  img.set("height", lengthOrNull(propList, "svg:height"));
  img.set("rotate", lengthOrNull(propList, "librevenge:rotate"));
  img.set("mimeType", strOrNull(propList, "librevenge:mime-type"));
  img.set("blob", blobFrom(propList["office:binary-data"], propList["librevenge:mime-type"]));
  img.set("props", propsToJson(propList, true));
  children->push(std::move(img));
}

// ----- text -----

void JSONDrawingGenerator::startTextObject(const librevenge::RVNGPropertyList &propList)
{
  count("startTextObject");
  Kind k = m_stack.back().kind;
  if (k != K_PAGE && k != K_GROUP && k != K_LAYER)
  {
    count("!textOutsidePage");
    return;
  }
  Json t = Json::object();
  t.set("type", Json::string("text"));
  t.set("x", lengthOrNull(propList, "svg:x"));
  t.set("y", lengthOrNull(propList, "svg:y"));
  t.set("width", lengthOrNull(propList, "svg:width"));
  t.set("height", lengthOrNull(propList, "svg:height"));
  t.set("rotate", lengthOrNull(propList, "librevenge:rotate"));
  Json pad = Json::object();
  pad.set("top", lengthOrNull(propList, "fo:padding-top"));
  pad.set("right", lengthOrNull(propList, "fo:padding-right"));
  pad.set("bottom", lengthOrNull(propList, "fo:padding-bottom"));
  pad.set("left", lengthOrNull(propList, "fo:padding-left"));
  t.set("padding", pad);
  t.set("verticalAlign", strOrNull(propList, "draw:textarea-vertical-align"));
  t.set("columns", lengthOrNull(propList, "fo:column-count"));
  t.set("columnGap", lengthOrNull(propList, "fo:column-gap"));
  t.set("props", propsToJson(propList, true));
  push(K_TEXT, std::move(t), "paragraphs");
}

void JSONDrawingGenerator::endTextObject()
{
  count("endTextObject");
  pop(K_TEXT);
}

static Json paragraphFrom(const librevenge::RVNGPropertyList &propList)
{
  Json p = Json::object();
  p.set("align", strOrNull(propList, "fo:text-align"));
  p.set("lineHeight", lengthOrNull(propList, "fo:line-height"));
  p.set("marginTop", lengthOrNull(propList, "fo:margin-top"));
  p.set("marginBottom", lengthOrNull(propList, "fo:margin-bottom"));
  p.set("marginLeft", lengthOrNull(propList, "fo:margin-left"));
  p.set("marginRight", lengthOrNull(propList, "fo:margin-right"));
  p.set("textIndent", lengthOrNull(propList, "fo:text-indent"));
  p.set("props", propsToJson(propList, true));
  return p;
}

void JSONDrawingGenerator::openParagraph(const librevenge::RVNGPropertyList &propList)
{
  count("openParagraph");
  Kind k = m_stack.back().kind;
  if (k == K_SPAN) { pop(K_SPAN); k = m_stack.back().kind; }
  if (k == K_PARA) { pop(K_PARA); k = m_stack.back().kind; }
  if (k != K_TEXT && k != K_CELL)
  {
    count("!paragraphOutsideText");
    return;
  }
  push(K_PARA, paragraphFrom(propList), "spans");
}

// libmspub keeps Publisher's paragraph mark (U+000D) at the end of the last
// span of every paragraph; the paragraph boundary already says that.
static void stripParagraphMark(Json *para)
{
  Json *spans = para ? para->get("spans") : nullptr;
  if (!spans || spans->a.empty())
    return;
  Json *text = spans->a.back().get("text");
  if (text && !text->s.empty() && text->s.back() == '\r')
  {
    text->s.pop_back();
    // the mark often comes in a span of its own: drop it if it is now empty
    if (text->s.empty() && spans->a.size() > 1)
      spans->a.pop_back();
  }
}

void JSONDrawingGenerator::closeParagraph()
{
  count("closeParagraph");
  if (m_stack.back().kind == K_SPAN)
    pop(K_SPAN);
  if (m_stack.back().kind == K_PARA)
  {
    stripParagraphMark(m_stack.back().node);
    pop(K_PARA);
  }
}

Json *JSONDrawingGenerator::ensureParagraph()
{
  Kind k = m_stack.back().kind;
  if (k == K_PARA)
    return m_stack.back().node;
  if (k == K_TEXT || k == K_CELL)
  {
    count("!implicitParagraph");
    push(K_PARA, paragraphFrom(librevenge::RVNGPropertyList()), "spans");
    return m_stack.back().node;
  }
  return nullptr;
}

void JSONDrawingGenerator::openSpan(const librevenge::RVNGPropertyList &propList)
{
  count("openSpan");
  if (m_stack.back().kind == K_SPAN)
    pop(K_SPAN);
  if (!ensureParagraph())
  {
    count("!spanOutsideText");
    return;
  }
  Json s = Json::object();
  s.set("text", Json::string(""));
  s.set("font", strOrNull(propList, "style:font-name"));
  s.set("size", lengthOrNull(propList, "fo:font-size"));
  const librevenge::RVNGProperty *w = propList["fo:font-weight"];
  s.set("bold", Json::boolean(w && std::string(w->getStr().cstr()) == "bold"));
  const librevenge::RVNGProperty *st = propList["fo:font-style"];
  s.set("italic", Json::boolean(st && std::string(st->getStr().cstr()) == "italic"));
  s.set("underline", strOrNull(propList, "style:text-underline-type"));
  s.set("color", strOrNull(propList, "fo:color"));
  if (m_inLink)
    s.set("link", m_link);
  s.set("props", propsToJson(propList, true));
  // spans have no children array (empty key)
  push(K_SPAN, std::move(s), "");
}

void JSONDrawingGenerator::closeSpan()
{
  count("closeSpan");
  if (m_stack.back().kind == K_SPAN)
    pop(K_SPAN);
}

Json *JSONDrawingGenerator::ensureSpan()
{
  if (m_stack.back().kind == K_SPAN)
    return m_stack.back().node;
  if (!ensureParagraph())
    return nullptr;
  count("!implicitSpan");
  Json s = Json::object();
  s.set("text", Json::string(""));
  push(K_SPAN, std::move(s), "");
  return m_stack.back().node;
}

static void appendText(Json *span, const std::string &txt)
{
  Json *t = span->get("text");
  if (t)
    t->s += txt;
}

void JSONDrawingGenerator::insertText(const librevenge::RVNGString &text)
{
  count("insertText");
  Json *span = ensureSpan();
  if (!span)
  {
    ++m_orphanText;
    return;
  }
  appendText(span, text.cstr());
}

void JSONDrawingGenerator::insertTab()
{
  count("insertTab");
  if (Json *span = ensureSpan()) appendText(span, "\t");
}

void JSONDrawingGenerator::insertSpace()
{
  count("insertSpace");
  if (Json *span = ensureSpan()) appendText(span, " ");
}

void JSONDrawingGenerator::insertLineBreak()
{
  count("insertLineBreak");
  if (Json *span = ensureSpan()) appendText(span, "\n");
}

void JSONDrawingGenerator::insertField(const librevenge::RVNGPropertyList &propList)
{
  count("insertField");
  if (Json *span = ensureSpan())
  {
    Json *fields = span->get("fields");
    if (!fields)
      fields = &span->set("fields", Json::array());
    fields->push(propsToJson(propList, true));
  }
}

// ----- lists (libmspub 0.1.5 does not emit them; kept for completeness) -----

void JSONDrawingGenerator::openOrderedListLevel(const librevenge::RVNGPropertyList &) { count("openOrderedListLevel"); m_listOrdered.push_back(true); }
void JSONDrawingGenerator::openUnorderedListLevel(const librevenge::RVNGPropertyList &) { count("openUnorderedListLevel"); m_listOrdered.push_back(false); }
void JSONDrawingGenerator::closeOrderedListLevel() { count("closeOrderedListLevel"); if (!m_listOrdered.empty()) m_listOrdered.pop_back(); }
void JSONDrawingGenerator::closeUnorderedListLevel() { count("closeUnorderedListLevel"); if (!m_listOrdered.empty()) m_listOrdered.pop_back(); }

void JSONDrawingGenerator::openListElement(const librevenge::RVNGPropertyList &propList)
{
  count("openListElement");
  openParagraph(propList);
  if (m_stack.back().kind == K_PARA)
  {
    Json list = Json::object();
    list.set("level", Json::number(double(m_listOrdered.size())));
    list.set("ordered", Json::boolean(!m_listOrdered.empty() && m_listOrdered.back()));
    m_stack.back().node->set("list", list);
  }
}

void JSONDrawingGenerator::closeListElement()
{
  count("closeListElement");
  closeParagraph();
}

void JSONDrawingGenerator::defineParagraphStyle(const librevenge::RVNGPropertyList &) { count("defineParagraphStyle"); }
void JSONDrawingGenerator::defineCharacterStyle(const librevenge::RVNGPropertyList &) { count("defineCharacterStyle"); }

void JSONDrawingGenerator::openLink(const librevenge::RVNGPropertyList &propList)
{
  count("openLink");
  m_link = propsToJson(propList, true);
  m_inLink = true;
}

void JSONDrawingGenerator::closeLink()
{
  count("closeLink");
  m_inLink = false;
}

// ----- tables -----

void JSONDrawingGenerator::startTableObject(const librevenge::RVNGPropertyList &propList)
{
  count("startTableObject");
  Kind k = m_stack.back().kind;
  if (k != K_PAGE && k != K_GROUP && k != K_LAYER)
  {
    count("!tableOutsidePage");
    return;
  }
  Json t = Json::object();
  t.set("type", Json::string("table"));
  t.set("x", lengthOrNull(propList, "svg:x"));
  t.set("y", lengthOrNull(propList, "svg:y"));
  t.set("width", lengthOrNull(propList, "svg:width"));
  t.set("height", lengthOrNull(propList, "svg:height"));
  if (propList.child("librevenge:table-columns"))
    t.set("columns", vectorToJson(*propList.child("librevenge:table-columns"), true));
  t.set("props", propsToJson(propList, true));
  push(K_TABLE, std::move(t), "rows");
}

void JSONDrawingGenerator::openTableRow(const librevenge::RVNGPropertyList &propList)
{
  count("openTableRow");
  if (m_stack.back().kind != K_TABLE)
  {
    count("!rowOutsideTable");
    return;
  }
  Json r = Json::object();
  r.set("height", lengthOrNull(propList, "librevenge:row-height"));
  r.set("props", propsToJson(propList, true));
  push(K_ROW, std::move(r), "cells");
}

void JSONDrawingGenerator::closeTableRow()
{
  count("closeTableRow");
  pop(K_ROW);
}

void JSONDrawingGenerator::openTableCell(const librevenge::RVNGPropertyList &propList)
{
  count("openTableCell");
  if (m_stack.back().kind != K_ROW)
  {
    count("!cellOutsideRow");
    return;
  }
  Json c = Json::object();
  c.set("colSpan", lengthOrNull(propList, "table:number-columns-spanned"));
  c.set("rowSpan", lengthOrNull(propList, "table:number-rows-spanned"));
  c.set("props", propsToJson(propList, true));
  push(K_CELL, std::move(c), "paragraphs");
}

void JSONDrawingGenerator::closeTableCell()
{
  count("closeTableCell");
  pop(K_CELL);
}

void JSONDrawingGenerator::insertCoveredTableCell(const librevenge::RVNGPropertyList &propList)
{
  count("insertCoveredTableCell");
  if (m_stack.back().kind != K_ROW)
    return;
  Json c = Json::object();
  c.set("covered", Json::boolean(true));
  c.set("props", propsToJson(propList, true));
  currentChildren()->push(std::move(c));
}

void JSONDrawingGenerator::endTableObject()
{
  count("endTableObject");
  pop(K_TABLE);
}

} // namespace oi
