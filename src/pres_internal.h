#ifndef MEP_PRES_INTERNAL_H
#define MEP_PRES_INTERNAL_H

// What src/pres_pptx.cpp and src/pres_odp.cpp share: XML lookups that
// ignore namespace prefixes (producers pick their own), colour arithmetic,
// and XML escaping. Not part of pres_doc.h's interface.

#include <string>
#include <vector>

#include "xml_doc.h"

namespace pres {
namespace detail {

// "p:sp" -> "sp".
const char *Local(const char *name);
bool Is(const xml::xml_node &n, const char *local);
// The first child element whose local name is `local`.
xml::xml_node Child(const xml::xml_node &n, const char *local);
std::vector<xml::xml_node> Children(const xml::xml_node &n, const char *local);
std::vector<xml::xml_node> Elements(const xml::xml_node &n);
// Follows a path of local names ("spPr/xfrm/off").
xml::xml_node Path(const xml::xml_node &n, const char *path);
// An attribute by local name ("r:embed" found as "embed").
std::string Attr(const xml::xml_node &n, const char *local);
bool HasAttr(const xml::xml_node &n, const char *local);
// Every text node below `n`, run together.
std::string AllText(const xml::xml_node &n);
// The document element of an XML string (false when it does not parse).
bool ParseXml(const std::string &text, xml::xml_document *doc, xml::xml_node *root);

std::string Esc(const std::string &s);  // for element content and attribute values
std::string Num(long v);

// RRGGBB <-> components, and PowerPoint's luminance modulation
// (<a:lumMod>/<a:lumOff>, in 1/1000 %), applied in HSL.
std::string ApplyLum(const std::string &rgb, int lum_mod, int lum_off);
std::string ApplyShade(const std::string &rgb, int shade, int tint);

// "a/b/../c.xml" -> "a/c.xml"; a relationship target resolved against the
// directory of the part that owns it.
std::string ResolvePart(const std::string &owner_part, const std::string &target);

std::string ExtOf(const std::string &path);  // "png", lower case
std::string MimeForExt(const std::string &ext);

}  // namespace detail
}  // namespace pres

#endif
