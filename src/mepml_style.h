#ifndef MEP_MEPML_STYLE_H
#define MEP_MEPML_STYLE_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mepml_element.h"

// mepml style sheets (`.mepss`): docs/mepml-spec/style.md is the
// specification, this its implementation. A sheet is parsed into rules; a
// Cascade of sheets computes the style of an element from its path of
// ancestors. Pure -- no editor, no window -- like mepml_doc: every renderer
// (the editor's scan, the presentation view, each export) asks the same
// cascade and draws what it is told.

namespace mepml::style {

// A colour as a sheet writes it. `theme(Group, #fallback)` stays symbolic:
// an editor resolves the group against its colour scheme, an export takes
// the fallback.
struct Color {
    enum Kind { None, Rgb, Theme };
    Kind kind = None;         // None: transparent / not set
    std::uint32_t rgb = 0;    // 0xRRGGBB: the colour (Rgb), the fallback (Theme, when has_fallback)
    bool has_fallback = false;
    std::string group;        // Theme
    float alpha = 1.0f;       // fade()

    bool operator==(const Color &o) const {
        return kind == o.kind && rgb == o.rgb && has_fallback == o.has_fallback && group == o.group && alpha == o.alpha;
    }
    bool operator!=(const Color &o) const { return !(*this == o); }
};

enum class VerticalAlign { Baseline, Super, Sub };
enum class TextAlign { Left, Center, Right };

// An element's style once the cascade has run (style.md §6).
struct Computed {
    // Text.
    bool has_color = false;  // false: the renderer's own text colour
    Color color;
    Color background;
    bool bold = false, italic = false;
    bool underline = false, strike = false;
    bool has_decoration_color = false;  // false: the text's colour
    Color decoration_color;
    float font_size = 1.0f;            // × body text
    std::string font_family;           // "" (body), "serif", "sans", "mono"
    std::vector<std::string> font_names;  // named families before the generic one, for exports
    VerticalAlign vertical_align = VerticalAlign::Baseline;
    TextAlign text_align = TextAlign::Left;
    // Generated text.
    bool has_content = false;
    std::string content;
    // Boxes.
    Color border_color, border_left_color;
    // Custom properties (`--name`), inherited.
    std::map<std::string, std::string> vars;
};

// One property a sheet may declare: the specification's table (style.md
// §6), which the parser validates against and tooling completes from.
struct PropertyInfo {
    const char *name;
    bool inherited;
    const char *values;  // what it accepts, for messages and completion
    const char *doc;
};
const std::vector<PropertyInfo> &Properties();

struct Diagnostic {
    int line = 0, col = 0;  // 0-based, in the sheet's text
    std::string message;
};

// --- A parsed sheet.

struct AttrSelector {
    std::string name;
    bool has_value = false;
    std::string value;
};
// `name[attr=value]`: one step of a selector.
struct Compound {
    std::string name;  // "" for `*` (or none)
    std::vector<AttrSelector> attrs;
    bool child = false;  // joined to the compound before it by `>` (else a descendant)
};
struct Selector {
    std::vector<Compound> compounds;  // outermost first; the last is the subject
    std::string part;                 // `::part` on the subject
    int spec_attrs = 0, spec_names = 0;  // specificity
};
struct Declaration {
    std::string property;  // "color", or "--name" for a custom property
    std::string value;     // as written, trimmed
    int line = 0, col = 0;
};
struct MediaTerm {
    bool negate = false;
    std::string tag;
};
struct Rule {
    std::vector<Selector> selectors;
    std::vector<Declaration> declarations;
    std::vector<MediaTerm> media;  // empty: every medium
    int line = 0;  // where its selectors start (0-based), for messages
};

struct Sheet {
    std::string origin;  // where it came from (a path, "default"), for messages
    std::vector<Rule> rules;
    std::vector<Diagnostic> diagnostics;
};

// Never fails: what cannot be understood is dropped and reported
// (Sheet::diagnostics), and the rest of the sheet stands.
Sheet Parse(const std::string &text, const std::string &origin = "");

// mep's built-in look (assets/mepml/default.mepss), parsed once.
const Sheet &DefaultSheet();
// Its text.
const char *DefaultSheetText();

// A colour value: `#rrggbb`, a mepml colour name, `theme(G[, c])`,
// `fade(c, a)`, `none`. False for anything else.
bool ParseColorValue(const std::string &text, Color *out);
// A named family as one of the three every renderer has: "serif", "sans"
// or "mono" (`\f(Helvetica, ...)` is sans).
std::string GenericFamily(const std::string &family);
// Whether `selector` selects the element at the end of `path` (outermost
// first).
bool Matches(const Selector &selector, const std::vector<const Element *> &path);

// --- The cascade.

class Cascade {
  public:
    // Sheets in cascade order: the default first, the document's last.
    std::vector<std::shared_ptr<const Sheet>> sheets;
    // The renderer's media tags ("editor", "screen" ...).
    std::vector<std::string> media;

    // The style of the element at the end of `path`, whose parent's
    // computed style is `parent` (Computed() for the outermost).
    Computed Compute(const std::vector<const Element *> &path, const Computed &parent) const;
    // The style of each node of `paths`, parents before children: one
    // Compute per distinct path.
    std::vector<Computed> ComputeAll(const ElementPaths &paths) const;

    // Whether a rule's @media applies to this cascade.
    bool MediaApplies(const std::vector<MediaTerm> &terms) const;
};

// A `content` string with its placeholders filled in: `%n` the number, `%k`
// the kind (or key) as written, `%t` the title, `%%` a percent sign.
std::string ExpandContent(const std::string &content, const std::string &number, const std::string &kind,
                          const std::string &title);

}  // namespace mepml::style

#endif  // MEP_MEPML_STYLE_H
