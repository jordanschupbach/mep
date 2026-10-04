#pragma once
// An HTML page as a screen reader meets it (a11y_doc.h): its headings,
// paragraphs, lists and tables from the DOM (html_doc.h), an <img> as its
// alt (alt="" or role="presentation": decoration), a formula as the
// aria-label of what wraps it, a table's description from aria-description
// (or the older summary), the page's language from <html lang> and its
// title from <title>. What aria-hidden hides is left out.

#include "a11y_doc.h"

#include <string>

struct DomNode;

namespace a11y {

Document FromHtmlDom(const DomNode *root);
// Parses `html` (a whole page or a fragment) first.
Document FromHtml(const std::string &html);

}  // namespace a11y
