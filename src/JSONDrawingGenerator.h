/* OpenImprenta spike: a librevenge drawing interface that serialises the
 * drawing calls emitted by libmspub into JSON.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#pragma once

#include <librevenge/librevenge.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oi
{

// Minimal JSON value. Objects keep insertion order.
struct Json
{
  enum Type { Null, Bool, Num, Str, Arr, Obj };
  Type type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  static Json null() { return Json(); }
  static Json boolean(bool v) { Json j; j.type = Bool; j.b = v; return j; }
  static Json number(double v) { Json j; j.type = Num; j.n = v; return j; }
  static Json string(const std::string &v) { Json j; j.type = Str; j.s = v; return j; }
  static Json array() { Json j; j.type = Arr; return j; }
  static Json object() { Json j; j.type = Obj; return j; }

  Json &set(const std::string &k, Json v);   // object: add/replace key
  Json *get(const std::string &k);           // object: lookup
  Json &push(Json v);                        // array: append, returns ref
  void serialize(std::string &out) const;
};

class JSONDrawingGenerator : public librevenge::RVNGDrawingInterface
{
public:
  JSONDrawingGenerator();
  ~JSONDrawingGenerator() override;

  std::string result() const;
  unsigned pageCount() const;

  void startDocument(const librevenge::RVNGPropertyList &propList) override;
  void endDocument() override;
  void setDocumentMetaData(const librevenge::RVNGPropertyList &propList) override;
  void defineEmbeddedFont(const librevenge::RVNGPropertyList &propList) override;
  void startPage(const librevenge::RVNGPropertyList &propList) override;
  void endPage() override;
  void startMasterPage(const librevenge::RVNGPropertyList &propList) override;
  void endMasterPage() override;
  void setStyle(const librevenge::RVNGPropertyList &propList) override;
  void startLayer(const librevenge::RVNGPropertyList &propList) override;
  void endLayer() override;
  void startEmbeddedGraphics(const librevenge::RVNGPropertyList &propList) override;
  void endEmbeddedGraphics() override;
  void openGroup(const librevenge::RVNGPropertyList &propList) override;
  void closeGroup() override;

  void drawRectangle(const librevenge::RVNGPropertyList &propList) override;
  void drawEllipse(const librevenge::RVNGPropertyList &propList) override;
  void drawPolygon(const librevenge::RVNGPropertyList &propList) override;
  void drawPolyline(const librevenge::RVNGPropertyList &propList) override;
  void drawPath(const librevenge::RVNGPropertyList &propList) override;
  void drawGraphicObject(const librevenge::RVNGPropertyList &propList) override;
  void drawConnector(const librevenge::RVNGPropertyList &propList) override;

  void startTextObject(const librevenge::RVNGPropertyList &propList) override;
  void endTextObject() override;

  void startTableObject(const librevenge::RVNGPropertyList &propList) override;
  void openTableRow(const librevenge::RVNGPropertyList &propList) override;
  void closeTableRow() override;
  void openTableCell(const librevenge::RVNGPropertyList &propList) override;
  void closeTableCell() override;
  void insertCoveredTableCell(const librevenge::RVNGPropertyList &propList) override;
  void endTableObject() override;

  void insertTab() override;
  void insertSpace() override;
  void insertText(const librevenge::RVNGString &text) override;
  void insertLineBreak() override;
  void insertField(const librevenge::RVNGPropertyList &propList) override;

  void openOrderedListLevel(const librevenge::RVNGPropertyList &propList) override;
  void openUnorderedListLevel(const librevenge::RVNGPropertyList &propList) override;
  void closeOrderedListLevel() override;
  void closeUnorderedListLevel() override;
  void openListElement(const librevenge::RVNGPropertyList &propList) override;
  void closeListElement() override;

  void defineParagraphStyle(const librevenge::RVNGPropertyList &propList) override;
  void openParagraph(const librevenge::RVNGPropertyList &propList) override;
  void closeParagraph() override;
  void defineCharacterStyle(const librevenge::RVNGPropertyList &propList) override;
  void openSpan(const librevenge::RVNGPropertyList &propList) override;
  void closeSpan() override;
  void openLink(const librevenge::RVNGPropertyList &propList) override;
  void closeLink() override;

private:
  // Kind of the node on top of the build stack.
  enum Kind { K_DOC, K_PAGE, K_GROUP, K_LAYER, K_TEXT, K_PARA, K_SPAN, K_TABLE, K_ROW, K_CELL };
  // `node` points into the parent's children array, which is never modified
  // while the node is open, so the pointer stays valid. The children array is
  // looked up by key every time (never cached) for the same reason.
  struct Frame { Kind kind; Json *node; std::string childrenKey; };

  void count(const char *call);
  Json *currentChildren();               // where elements/paragraphs/spans go
  void push(Kind kind, Json node, const char *childrenKey);
  bool pop(Kind kind);                   // pops up to (and including) kind
  bool inStack(Kind kind) const;
  void addShape(const char *type, const librevenge::RVNGPropertyList &propList);
  Json *ensureParagraph();
  Json *ensureSpan();

  Json m_doc;
  Json m_style;                          // current graphic style (raw props)
  std::vector<Frame> m_stack;
  std::map<std::string, unsigned> m_calls;
  std::vector<bool> m_listOrdered;       // open list levels
  Json m_link;                           // current hyperlink (if any)
  bool m_inLink;
  unsigned m_pages;
  unsigned m_orphanText;                 // text that arrived outside a text object
};

// Converts a property list to JSON: lengths in points, percentages as
// strings ("50%"), booleans as booleans, binary data as base64 strings.
Json propsToJson(const librevenge::RVNGPropertyList &propList, bool dropBinary = false);

} // namespace oi
