// Tests for src/mepml_style.cpp against docs/mepml-spec/style.md: the
// .mepss parser, selector matching and the cascade.
//
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "mepml_style.h"

#include "mepml_doc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using mepml::Element;
using mepml::style::Cascade;
using mepml::style::Color;
using mepml::style::Computed;
using mepml::style::Sheet;

std::shared_ptr<const Sheet> SheetOf(const std::string &text) {
    return std::make_shared<Sheet>(mepml::style::Parse(text, "test"));
}

// The style of the last of `els`, each computed under the one before it.
Computed Style(const Cascade &cascade, const std::vector<Element> &els) {
    std::vector<const Element *> path;
    Computed c;
    for (const Element &e : els) {
        path.push_back(&e);
        c = cascade.Compute(path, c);
    }
    return c;
}

Cascade One(const std::string &text, std::vector<std::string> media = {}) {
    Cascade c;
    c.sheets.push_back(SheetOf(text));
    c.media = std::move(media);
    return c;
}

void TestParsing() {
    const Sheet s = mepml::style::Parse("/* c */ bold { font-weight: bold; color: #f00 }\n"
                                        "box[kind=proof]::end, a > b c { content: \"x\"; }\n"
                                        "@media present, not html { big { font-size: 2 } }\n");
    CHECK(s.diagnostics.empty());
    CHECK(s.rules.size() == 3);
    CHECK(s.rules[0].declarations.size() == 2);
    CHECK(s.rules[1].selectors.size() == 2);
    const mepml::style::Selector &a = s.rules[1].selectors[0];
    CHECK(a.compounds.size() == 1 && a.compounds[0].name == "box" && a.part == "end");
    CHECK(a.compounds[0].attrs.size() == 1 && a.compounds[0].attrs[0].value == "proof");
    CHECK(a.spec_attrs == 1 && a.spec_names == 2);
    const mepml::style::Selector &b = s.rules[1].selectors[1];
    CHECK(b.compounds.size() == 3 && b.compounds[1].child && !b.compounds[2].child);
    CHECK(s.rules[2].media.size() == 2 && s.rules[2].media[1].negate && s.rules[2].media[1].tag == "html");
}

void TestErrorsNeverStopASheet() {
    const Sheet s = mepml::style::Parse("bold { colour: red; font-weight: heavy; font-style: italic }\n"
                                        "a[ { color: red }\n"
                                        "@font-face { x: y }\n"
                                        "@import \"x\";\n"
                                        "italic { color: nope; ; color: blue }\n"
                                        "}\n"
                                        "big { font-size: 2\n");
    // Unknown property, bad value, bad selector, two unknown @rules, bad
    // colour, stray brace, unclosed block.
    CHECK(s.diagnostics.size() == 8);
    CHECK(s.diagnostics[0].line == 0 && s.diagnostics[0].message.find("colour") != std::string::npos);
    CHECK(s.rules.size() == 3);
    CHECK(s.rules[0].declarations.size() == 1 && s.rules[0].declarations[0].property == "font-style");
    CHECK(s.rules[1].declarations.size() == 1 && s.rules[1].declarations[0].value == "blue");
    CHECK(s.rules[2].declarations.size() == 1);
    // Text that is not a sheet at all.
    CHECK(mepml::style::Parse("").rules.empty());
    CHECK(mepml::style::Parse("{{{{").rules.empty());
    CHECK(mepml::style::Parse("/* never closed").rules.empty());
    CHECK(mepml::style::Parse("a { content: \"never closed }").rules.size() <= 1);
}

void TestSelectors() {
    const Cascade c = One("heading { color: #111 }\n"
                          "heading[level=2] { color: #222 }\n"
                          "slide heading { font-weight: bold }\n"
                          "slide > heading { font-style: italic }\n"
                          "box[kind=definition]::label { content: \"Def\" }\n"
                          "box::title bold { color: #333 }\n"
                          "box bold { text-decoration: underline }\n"
                          "list-item[ordered] { color: #444 }\n"
                          "* { text-align: center }\n");
    const Element doc("document"), slide("slide", "number", "1"), box("box", "kind", "definition");
    Computed h1 = Style(c, {doc, Element("heading", "level", "1")});
    CHECK(h1.color.rgb == 0x111111 && !h1.bold && !h1.italic);
    CHECK(h1.text_align == mepml::style::TextAlign::Center);
    Computed h2 = Style(c, {doc, Element("heading", "level", "2")});
    CHECK(h2.color.rgb == 0x222222);
    Computed sh = Style(c, {doc, slide, Element("heading", "level", "2")});
    CHECK(sh.bold && sh.italic);
    // `>` is a child, not any descendant.
    Computed bh = Style(c, {doc, slide, box, Element("heading", "level", "2")});
    CHECK(bh.bold && !bh.italic);
    // A part: selected only by `::part`, and not an element of the tree.
    Computed label = Style(c, {doc, box, box.Part("label")});
    CHECK(label.has_content && label.content == "Def");
    CHECK(label.text_align == mepml::style::TextAlign::Center);  // inherited; `*` is not a part
    Computed plain = Style(c, {doc, box});
    CHECK(!plain.has_content);
    // What a title holds is the box's descendant.
    Computed tb = Style(c, {doc, box, box.Part("title"), Element("bold")});
    CHECK(tb.underline);
    CHECK(!tb.has_color);  // `box::title bold` is not a selector that can match: a part ends one
    // Attribute presence.
    Element item("list-item");
    CHECK(!Style(c, {doc, item}).has_color);
    item.With("ordered");
    CHECK(Style(c, {doc, item}).color.rgb == 0x444444);
}

void TestCascadeOrder() {
    // Specificity beats order; order breaks ties.
    Cascade c = One("heading[level=1] { color: #111 }\n"
                    "heading { color: #222; font-weight: bold }\n"
                    "heading { font-weight: normal }\n");
    const Element h("heading", "level", "1");
    Computed s = Style(c, {h});
    CHECK(s.color.rgb == 0x111111 && !s.bold);
    // A later sheet beats an earlier one whatever the specificity.
    c.sheets.push_back(SheetOf("* { color: #333 }"));
    CHECK(Style(c, {h}).color.rgb == 0x333333);
    // A selector list counts as its most specific matching selector.
    Cascade l = One("heading, heading[level=1] { color: #111 }\nheading { color: #222 }");
    CHECK(Style(l, {h}).color.rgb == 0x111111);
}

void TestInheritance() {
    const Cascade c = One("box { color: #123456; background: #eee; border-left-color: red; font-size: 1.5 }\n"
                          "bold { font-weight: bold; font-size: 2 }\n"
                          "italic { background: inherit; color: initial; font-size: 12pt }\n"
                          "underline { text-decoration: underline }\n"
                          "strike { text-decoration: line-through }\n"
                          "mono { text-decoration: no-underline }\n"
                          "verbatim { text-decoration: none }\n");
    const Element box("box"), p("paragraph"), bold("bold"), italic("italic");
    Computed para = Style(c, {box, p});
    CHECK(para.has_color && para.color.rgb == 0x123456);
    CHECK(para.background.kind == Color::None);  // not inherited
    CHECK(para.border_left_color.kind == Color::None);
    Computed b = Style(c, {box, p, bold});
    CHECK(b.bold && b.font_size > 2.99f && b.font_size < 3.01f);  // 1.5 x 2
    Computed i = Style(c, {box, p, bold, italic});
    CHECK(i.bold && !i.has_color);
    CHECK(i.font_size == 1.0f);  // pt is of body text, not of the parent
    Computed bi = Style(c, {box, italic});
    CHECK(bi.background.kind == Color::Rgb && bi.background.rgb == 0xeeeeee);
    // Decorations add up down the tree, and can be taken away.
    Computed us = Style(c, {Element("underline"), Element("strike")});
    CHECK(us.underline && us.strike);
    Computed usm = Style(c, {Element("underline"), Element("strike"), Element("mono")});
    CHECK(!usm.underline && usm.strike);
    Computed usv = Style(c, {Element("underline"), Element("strike"), Element("verbatim")});
    CHECK(!usv.underline && !usv.strike);
}

void TestMedia() {
    const std::string text = "big { font-size: 1.3 }\n"
                             "@media present { big { font-size: 2 } }\n"
                             "@media not editor { big { color: red } }\n"
                             "@media html, source { big { font-weight: bold } }\n";
    const Element big("big");
    Computed e = Style(One(text, {"editor", "screen"}), {big});
    CHECK(e.font_size > 1.29f && e.font_size < 1.31f && !e.has_color && !e.bold);
    Computed p = Style(One(text, {"present", "slides"}), {big});
    CHECK(p.font_size == 2.0f && p.has_color && !p.bold);
    Computed s = Style(One(text, {"editor", "source"}), {big});
    CHECK(s.bold && !s.has_color);
}

void TestValues() {
    Color c;
    CHECK(mepml::style::ParseColorValue("#abc", &c) && c.kind == Color::Rgb && c.rgb == 0xaabbcc && c.alpha == 1.0f);
    CHECK(mepml::style::ParseColorValue("#11223380", &c) && c.rgb == 0x112233 && c.alpha > 0.49f && c.alpha < 0.51f);
    CHECK(mepml::style::ParseColorValue("Red", &c) && c.kind == Color::Rgb);
    CHECK(mepml::style::ParseColorValue("theme(Blue)", &c) && c.kind == Color::Theme && c.group == "Blue" && !c.has_fallback);
    CHECK(mepml::style::ParseColorValue("theme( Blue , #2c7fb8 )", &c) && c.has_fallback && c.rgb == 0x2c7fb8);
    CHECK(mepml::style::ParseColorValue("fade(theme(Blue, #000), 0.25)", &c) && c.kind == Color::Theme && c.alpha == 0.25f);
    CHECK(mepml::style::ParseColorValue("fade(fade(#fff, 0.5), 0.5)", &c) && c.alpha == 0.25f);
    CHECK(mepml::style::ParseColorValue("none", &c) && c.kind == Color::None);
    CHECK(!mepml::style::ParseColorValue("theme()", &c));
    CHECK(!mepml::style::ParseColorValue("theme(Blue, theme(Red))", &c));
    CHECK(!mepml::style::ParseColorValue("fade(#fff)", &c));
    CHECK(!mepml::style::ParseColorValue("notacolour", &c));
    CHECK(!mepml::style::ParseColorValue("", &c));

    const Cascade s = One("a { content: \"a\\\"b\" 'c' \"\\220E \" }\n"
                          "b { font-family: \"Fira Sans\", Georgia, serif }\n"
                          "c { font-family: mono }\n"
                          "d { vertical-align: super; text-align: right; border-color: fade(#000, 0.3) }\n");
    Computed a = Style(s, {Element("a")});
    CHECK(a.has_content && a.content == "a\"bc\xE2\x88\x8E");
    Computed b = Style(s, {Element("b")});
    CHECK(b.font_family == "serif" && b.font_names.size() == 2 && b.font_names[0] == "Fira Sans" && b.font_names[1] == "Georgia");
    CHECK(Style(s, {Element("c")}).font_family == "mono");
    Computed d = Style(s, {Element("d")});
    CHECK(d.vertical_align == mepml::style::VerticalAlign::Super && d.text_align == mepml::style::TextAlign::Right);
    CHECK(d.border_color.kind == Color::Rgb && d.border_color.alpha > 0.29f && d.border_color.alpha < 0.31f);

    CHECK(mepml::style::ExpandContent("Slide %n: %t (%k) 100%%", "3", "x", "Intro") == "Slide 3: Intro (x) 100%");
    CHECK(mepml::style::ExpandContent("50% %z", "", "", "") == "50% %z");
}

void TestCustomProperties() {
    const Cascade c = One("box { --accent: #2c7fb8; background: fade(var(--accent), 0.1); }\n"
                          "box[kind=fact] { --accent: #d95f0e }\n"
                          "box::label { color: var(--accent) }\n"
                          "bold { color: var(--missing, #010203); background: var(--missing) }\n"
                          "italic { --loop: var(--loop); color: var(--loop, red) }\n");
    const Element def("box", "kind", "definition"), fact("box", "kind", "fact");
    Computed d = Style(c, {def});
    CHECK(d.background.rgb == 0x2c7fb8 && d.background.alpha > 0.09f && d.background.alpha < 0.11f);
    // A later, more specific rule's custom property reaches the earlier rule's var().
    Computed f = Style(c, {fact});
    CHECK(f.background.rgb == 0xd95f0e);
    // Custom properties inherit.
    CHECK(Style(c, {fact, fact.Part("label")}).color.rgb == 0xd95f0e);
    CHECK(Style(c, {fact, Element("paragraph"), Element("box", "kind", "x"), Element("box").Part("label")}).color.rgb == 0x2c7fb8);
    // A fallback; with none, the declaration is dropped.
    Computed b = Style(c, {Element("bold")});
    CHECK(b.color.rgb == 0x010203 && b.background.kind == Color::None);
    Computed i = Style(c, {Element("italic")});
    CHECK(i.has_color);
}

void TestPresentationalMarkup() {
    const Cascade c = One("color { color: #000 } font-size { font-size: 3 } font { font-family: mono } bold { color: #111 }");
    Computed col = Style(c, {Element("color", "value", "red")});
    std::uint32_t red = 0;
    CHECK(mepml::ParseColor("red", &red));
    CHECK(col.has_color && col.color.rgb == red);
    CHECK(Style(c, {Element("color", "value", "red"), Element("bold")}).color.rgb == 0x111111);
    CHECK(Style(c, {Element("font-size", "pt", "24")}).font_size == 2.0f);
    Computed f = Style(c, {Element("font", "family", "Helvetica")});
    CHECK(f.font_family == "sans" && f.font_names.size() == 1 && f.font_names[0] == "Helvetica");
    CHECK(mepml::style::GenericFamily("Courier New") == "mono" && mepml::style::GenericFamily("Times") == "serif");
}

void TestComputeAll() {
    mepml::ElementPaths paths;
    const int doc = paths.Intern(-1, Element("document"));
    const int box = paths.Intern(doc, Element("box", "kind", "fact"));
    const int para = paths.Intern(box, Element("paragraph"));
    const int bold = paths.Intern(para, Element("bold"));
    CHECK(paths.Intern(para, Element("bold")) == bold);
    CHECK(paths.Intern(doc, Element("bold")) != bold);
    CHECK(paths.Path(bold).size() == 4);
    const Cascade c = One("box { color: #123 } bold { font-weight: bold }");
    const std::vector<Computed> all = c.ComputeAll(paths);
    CHECK(all.size() == 5);
    CHECK(!all[static_cast<size_t>(doc)].has_color);
    CHECK(all[static_cast<size_t>(bold)].bold && all[static_cast<size_t>(bold)].color.rgb == 0x112233);
}

void TestDefaultSheet() {
    const Sheet &d = mepml::style::DefaultSheet();
    for (const mepml::style::Diagnostic &dg : d.diagnostics)
        std::fprintf(stderr, "default.mepss:%d:%d: %s\n", dg.line + 1, dg.col + 1, dg.message.c_str());
    CHECK(d.diagnostics.empty());
    CHECK(d.rules.size() > 40);
    Cascade c;
    c.sheets.push_back(std::make_shared<Sheet>(d));
    c.media = {"editor", "screen"};
    const Element doc("document"), para("paragraph");
    Computed link = Style(c, {doc, para, Element("link")});
    CHECK(link.underline && link.color.kind == Color::Theme && link.color.group == "Blue" && link.color.has_fallback);
    // A link stays a link's colour through what it holds.
    CHECK(Style(c, {doc, para, Element("link"), Element("verbatim")}).color.group == "Blue");
    const Element warn("callout", "kind", "warning");
    CHECK(Style(c, {doc, warn}).color.group == "Yellow");
    CHECK(Style(c, {doc, warn, Element("verbatim")}).color.group == "Yellow");
    Computed badge = Style(c, {doc, warn, warn.Part("label")});
    CHECK(badge.bold && mepml::style::ExpandContent(badge.content, "", "WARNING", "") == " WARNING ");
    const Element proof("box", "kind", "proof");
    CHECK(Style(c, {doc, proof, proof.Part("end")}).content == "\xE2\x88\x8E");
    CHECK(Style(c, {doc, proof, proof.Part("label")}).color.group == "Comment");
    // Markup is muted and plain whatever it belongs to.
    const Element bold("bold");
    Computed mk = Style(c, {doc, para, bold, bold.Part("markup")});
    CHECK(!mk.bold && mk.color.group == "Comment");
    // Size shows as colour only where the source is shown.
    CHECK(!Style(c, {doc, para, Element("big")}).has_color);
    c.media.push_back("source");
    CHECK(Style(c, {doc, para, Element("big")}).color.group == "Yellow");
    // Every property the engine knows is in the spec's table order.
    CHECK(mepml::style::Properties().size() == 13);
}

std::string PathText(const mepml::ElementPaths &paths, int node) {
    std::string out;
    for (const Element *e : paths.Path(node)) {
        if (!e->part.empty()) {
            out += "::" + e->part;
            continue;
        }
        out += (out.empty() ? "" : " > ") + e->name;
        for (const auto &kv : e->attrs) out += "[" + kv.first + (kv.second.empty() ? "" : "=" + kv.second) + "]";
    }
    return out;
}

// The highlighter names an element for every span (structure.md).
void TestElementPaths() {
    const std::vector<std::string> lines = {
        "> Title with *bold*",                    // 0
        "",
        "Some *bold ~both~* and [a link|u] \\cite(nokey) text.",  // 2
        "",
        "\\slide(",                               // 4
        "\\definition(A group,",                  // 5
        "- [x] done `v`",                         // 6
        ")",                                      // 7
        ")",                                      // 8
        "// WARNING: careful",                    // 9
        "| h |",                                  // 10
        "| - |",                                  // 11
        "| c |",                                  // 12
    };
    const mepml::Document doc = mepml::Parse(lines);
    mepml::ElementPaths paths;
    const std::vector<mepml::Span> spans = mepml::Highlight(doc, &paths);
    CHECK(!spans.empty());
    auto at = [&](int line, int col, bool source = false) {
        for (const mepml::Span &sp : spans)
            if (sp.line == line && col >= sp.col_start && col < sp.col_end) return PathText(paths, source ? sp.source_path : sp.path);
        return std::string("(none)");
    };
    for (const mepml::Span &sp : spans) CHECK(sp.path >= 0 && sp.source_path >= 0);
    CHECK(at(0, 0) == "document[type=document] > heading[level=1]::markup");
    CHECK(at(0, 3) == "document[type=document] > heading[level=1]");
    CHECK(at(0, 14) == "document[type=document] > heading[level=1] > bold");
    CHECK(at(2, 5) == "document[type=document] > paragraph > bold::markup");
    CHECK(at(2, 13) == "document[type=document] > paragraph > bold > italic");
    CHECK(at(2, 25) == "document[type=document] > paragraph > link");
    CHECK(at(2, 36) == "document[type=document] > paragraph > cite[missing]");
    CHECK(at(4, 0) == "document[type=document] > slide[number=1]::label");
    CHECK(at(4, 0, true) == "document[type=document] > slide[number=1]::markup");
    CHECK(at(5, 0) == "document[type=document] > slide[number=1] > box[kind=definition]::label");
    CHECK(at(5, 14) == "document[type=document] > slide[number=1] > box[kind=definition]::title");
    CHECK(at(6, 0) == "document[type=document] > slide[number=1] > box[kind=definition] > list > list-item[checked=true]::marker");
    CHECK(at(6, 12) == "document[type=document] > slide[number=1] > box[kind=definition] > list > list-item[checked=true] > verbatim");
    CHECK(at(7, 0) == "document[type=document] > slide[number=1] > box[kind=definition]::end");
    CHECK(at(8, 0) == "document[type=document] > slide[number=1]::end");
    CHECK(at(9, 0) == "document[type=document] > callout[kind=warning]::label");
    CHECK(at(9, 12) == "document[type=document] > callout[kind=warning]");
    CHECK(at(10, 0) == "document[type=document] > table::rule");
    CHECK(at(10, 2) == "document[type=document] > table > table-cell[header]");
    CHECK(at(12, 2) == "document[type=document] > table > table-cell");
    // The default sheet gives the pieces their look.
    Cascade c;
    c.sheets.push_back(std::make_shared<Sheet>(mepml::style::DefaultSheet()));
    c.media = {"editor", "screen"};
    const std::vector<Computed> styles = c.ComputeAll(paths);
    auto style_at = [&](int line, int col) {
        for (const mepml::Span &sp : spans)
            if (sp.line == line && col >= sp.col_start && col < sp.col_end) return styles[static_cast<size_t>(sp.path)];
        return Computed();
    };
    CHECK(style_at(0, 3).color.group == "Purple");
    CHECK(style_at(2, 13).bold && style_at(2, 13).italic);
    CHECK(style_at(2, 36).color.group == "Red");
    CHECK(style_at(5, 0).content == "Definition" && style_at(5, 0).color.group == "Blue" && style_at(5, 0).bold);
    CHECK(style_at(5, 14).bold);
    CHECK(style_at(6, 0).content == "\xE2\x9C\x93" && style_at(6, 8).strike);
    CHECK(style_at(6, 12).strike && style_at(6, 12).color.group == "Green");
    CHECK(style_at(9, 12).color.group == "Yellow");
    CHECK(style_at(10, 2).bold && !style_at(12, 2).bold);
}

// The spec's own reference document: every span is some element.
void TestReferenceFile() {
    std::vector<std::string> lines;
    std::FILE *f = std::fopen(MEPML_REFERENCE_FILE, "r");
    CHECK(f != nullptr);
    std::string cur;
    for (int ch = std::fgetc(f); ch != EOF; ch = std::fgetc(f)) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += static_cast<char>(ch);
        }
    }
    std::fclose(f);
    const mepml::Document doc = mepml::Parse(lines);
    mepml::ElementPaths paths;
    const std::vector<mepml::Span> spans = mepml::Highlight(doc, &paths);
    CHECK(spans.size() > 200);
    for (const mepml::Span &sp : spans) {
        CHECK(sp.path >= 0 && static_cast<size_t>(sp.path) < paths.nodes.size());
        CHECK(sp.source_path >= 0 && static_cast<size_t>(sp.source_path) < paths.nodes.size());
        // Every path starts at the document.
        CHECK(paths.Path(sp.path).front()->name == "document");
    }
    // One node per distinct path, however many spans share it.
    CHECK(paths.nodes.size() < spans.size());
    // Spans without a tree are the same spans.
    const std::vector<mepml::Span> plain = mepml::Highlight(doc);
    CHECK(plain.size() == spans.size());
}

// `:active` and the other states are the renderer's: an element carries
// one only where the renderer says so.
void TestStates() {
    const Sheet parsed = mepml::style::Parse("box:active { border-color: #111 } box: { color: red } box:active::label { color: #222 }");
    CHECK(parsed.rules.size() == 2 && parsed.diagnostics.size() == 1);
    CHECK(parsed.rules[0].selectors[0].spec_attrs == 1);
    const Cascade c = One("box { border-color: #000 } box:active { border-color: #111 }");
    Element box("box", "kind", "fact");
    CHECK(Style(c, {box}).border_color.rgb == 0x000000);
    box.With(":active");
    CHECK(Style(c, {box}).border_color.rgb == 0x111111);
}

// `//? Style:` lines name the document's sheets, an import's before its own.
void TestStyleRefs() {
    const std::vector<std::string> main_lines = {"//? Style: a.mepss", "//? Import: sub/part.mepml", "//? Style: b.mepss", "", "text"};
    const std::vector<std::string> part_lines = {"//? Style: part.mepss", "", "more"};
    const mepml::Document plain = mepml::Parse(main_lines);
    CHECK(plain.styles.size() == 2 && plain.styles[0].path == "a.mepss" && plain.styles[1].line == 2);
    const mepml::Document doc = mepml::ParseWithImports("/d/main.mepml", main_lines, [&](const std::string &path, std::vector<std::string> *out) {
        if (path != "/d/sub/part.mepml") return false;
        *out = part_lines;
        return true;
    });
    CHECK(doc.styles.size() == 3);
    CHECK(doc.styles[0].path == "part.mepss" && doc.styles[0].base == "/d/sub/part.mepml" && doc.styles[0].line == 1);
    CHECK(doc.styles[1].path == "a.mepss" && doc.styles[1].base.empty());
    CHECK(doc.styles[2].path == "b.mepss");
    CHECK(mepml::ResolvePath(doc.styles[0].base, doc.styles[0].path) == "/d/sub/part.mepss");
}

// The tree as JSON: containers nest, results follow their block.
void TestElementTreeJson() {
    const std::vector<std::string> lines = {
        "//? Option: n=3",
        "",
        "\\slide(",
        "> T",
        "\\definition(A \"q\",",
        "body *b*",
        ")",
        ")",
        "```{r}",
        "1",
        "```",
        "// result_begin:",
        "// [1] 1",
        "// result_end",
    };
    const std::string json = mepml::ElementTreeJson(mepml::Parse(lines));
    CHECK(json.find("{\"name\":\"meta\",\"attrs\":{\"key\":\"option\",\"type\":\"int\"}") != std::string::npos);
    const size_t slide = json.find("{\"name\":\"slide\"");
    const size_t heading = json.find("{\"name\":\"heading\"");
    const size_t box = json.find("{\"name\":\"box\",\"attrs\":{\"kind\":\"definition\"}");
    const size_t bold = json.find("{\"name\":\"bold\"");
    const size_t code = json.find("{\"name\":\"code\",\"attrs\":{\"lang\":\"r\"}");
    const size_t results = json.find("{\"name\":\"results\",\"attrs\":{\"format\":\"text\"}");
    CHECK(slide != std::string::npos && heading > slide && box > heading && bold > box && code > bold && results > code);
    CHECK(json.find("A \\\"q\\\"") != std::string::npos);  // escaped
    // Balanced, whatever was left open.
    for (const std::vector<std::string> &doc : {lines, std::vector<std::string>{"\\slide(", "\\proof(", "x"}, std::vector<std::string>{")", ")"}}) {
        const std::string j = mepml::ElementTreeJson(mepml::Parse(doc));
        int depth = 0;
        bool in_string = false;
        for (size_t i = 0; i < j.size(); ++i) {
            if (in_string) {
                if (j[i] == '\\') ++i;
                else if (j[i] == '"') in_string = false;
            } else if (j[i] == '"') {
                in_string = true;
            } else if (j[i] == '{' || j[i] == '[') {
                ++depth;
            } else if (j[i] == '}' || j[i] == ']') {
                --depth;
                CHECK(depth >= 0);
            }
        }
        CHECK(depth == 0 && !in_string);
    }
}

// Exports: the document's sheets change an export's built-in look where
// they say something the default sheet does not, and nothing otherwise.
void TestExports() {
    const std::vector<std::string> lines = {
        "//? Style: s.mepss",
        "",
        "> Title",
        "",
        "Some *bold* and `code` and [a link|https://x.y].",
        "",
        "- item",
        "1. one",
        "",
        "\\definition(Group,",
        "A set.",
        ")",
        "\\proof(",
        "Trivial.",
        ")",
    };
    const std::string sheet = "heading[level=1] { color: #112233; font-size: 2; }\n"
                              "bold { color: theme(Red, #aa0000); font-style: italic; }\n"
                              "verbatim { color: theme(Green); }\n"            // no fallback: nothing an export can use
                              "link { text-decoration: none; }\n"
                              "list-item::marker { content: \"*\"; }\n"
                              "box[kind=definition] { --accent: #00aa00; }\n"
                              "box[kind=definition]::label { content: \"Def.\"; }\n"
                              "box[kind=proof]::end { content: \"QED\"; }\n"
                              "box:active { border-color: red; }\n"            // a state: the editor's only
                              "bold::markup { color: red; }\n"                 // source text: the editor's only
                              "@media editor { italic { color: red; } }\n"
                              "@media html { italic { color: #010203; } }\n"
                              "@media print { italic { color: #040506; } }\n";
    auto read = [&](const std::string &path, std::vector<std::string> *out) {
        if (path != "/d/s.mepss") return false;
        out->clear();
        size_t at = 0;
        while (at < sheet.size()) {
            const size_t nl = sheet.find('\n', at);
            out->push_back(sheet.substr(at, nl - at));
            at = nl + 1;
        }
        return true;
    };
    // Without sheets of its own a document exports in the built-in look --
    // which, for HTML, is the default sheet written as CSS (headings,
    // markers, cards and tables as the editor draws them), so the page and
    // the editor's view of it agree.
    const mepml::Document plain = mepml::Parse(lines);
    CHECK(plain.sheets.empty());
    const std::string plain_css = mepml::ExportSheetCss(plain);
    CHECK(plain_css.find(":root :is(h1:not(.title), h2.slide-title):not(.title-slide > h1) { color: var(--fg); font-size: 1.6em; }") != std::string::npos);
    CHECK(plain_css.find(":root ul > li::marker { color: #9a6700; content: \"\xE2\x80\xA2 \"; text-decoration-line: none; }") != std::string::npos);
    CHECK(plain_css.find(":root .mono { color: #1b7f86; }") != std::string::npos);
    CHECK(plain_css.find(":root table th { background: rgba(11, 92, 173, 0.12); }") != std::string::npos);  // the table's hue, as tints
    CHECK(plain_css.find("color-mix") == std::string::npos || plain_css.find("var(--accent)") != std::string::npos);  // literal fades are rgba()
    CHECK(plain_css.find("markup") == std::string::npos);  // the editor's own: `::markup`, `@media source`
    const mepml::BoxLook builtin = mepml::ExportBoxLook(plain, "definition");
    CHECK(builtin.label == "Definition" && builtin.color == "#2c7fb8" && builtin.tint == "#eef5fb" && builtin.end.empty());
    CHECK(mepml::ExportBoxLook(plain, "proof").end == "\xE2\x88\x8E");

    // HTML: the sheet as CSS over the page's own.
    const mepml::Document html_doc = mepml::ParseForExport("/d/doc.mepml", lines, read, {"html"});
    CHECK(html_doc.sheets.size() == 1 && html_doc.sheets[0]->diagnostics.empty());
    const std::string css = mepml::ExportSheetCss(html_doc);
    CHECK(css.find(":root :is(h1:not(.title), h2.slide-title):not(.title-slide > h1) { color: #112233; font-size: 2em; }") != std::string::npos);
    CHECK(css.find(":root strong { color: #aa0000; font-style: italic; }") != std::string::npos);
    // (`theme(Green)` with no fallback: the default sheet's colour stands, nothing of the document's is written.)
    CHECK(css.find("code { color: #2f7d32") == std::string::npos && css.find(":root :not(pre) > code { color: var(--ins); }") != std::string::npos);
    CHECK(css.rfind(":root :not(pre) > code {") == css.find(":root :not(pre) > code {"));
    CHECK(css.find(":root a:not(.cite) { text-decoration-line: none; }") != std::string::npos);
    CHECK(css.find(":root ul > li::marker { content: \"* \"; }") != std::string::npos);
    CHECK(css.find(":root .mbox-definition { --accent: #00aa00; --c: #00aa00; }") != std::string::npos);
    CHECK(css.find(":root .mbox-proof::after { content: \"QED\"; }") != std::string::npos);
    CHECK(css.find("active") == std::string::npos && css.find("red") == std::string::npos);
    CHECK(css.find(":root em { color: #010203; }") != std::string::npos && css.find("#040506") == std::string::npos);
    const std::string page = mepml::ToHtml(html_doc);
    CHECK(page.find(css) != std::string::npos);
    CHECK(page.find("<span class=\"mbox-label\">Def.</span>") != std::string::npos);
    CHECK(page.find("mep-style") == std::string::npos);  // CSS does it

    // What the other exports draw a box with.
    const mepml::BoxLook def = mepml::ExportBoxLook(html_doc, "definition");
    CHECK(def.label == "Def." && def.color == "#00aa00");
    CHECK(def.tint != "#eef5fb" && def.tint != "#ffffff" && def.tint.size() == 7);  // the accent, faded onto paper
    CHECK(mepml::ExportBoxLook(html_doc, "proof").end == "QED");
    const mepml::BoxLook thm = mepml::ExportBoxLook(html_doc, "theorem");
    CHECK(thm.label == "Theorem" && thm.color == "#6a51a3" && thm.tint == "#f4f1fa");  // untouched: the built-in one

    // LaTeX reads the markup: the changes are on the runs themselves.
    const mepml::Document tex_doc = mepml::ParseForExport("/d/doc.mepml", lines, read, {"pdf", "tex", "latex"});
    const std::string tex_html = mepml::ToHtml(tex_doc);
    CHECK(tex_html.find("<h1 id=\"title\"><span class=\"mep-style\" data-color=\"112233\">Title</span></h1>") != std::string::npos);
    CHECK(tex_html.find("<strong><span class=\"mep-style\" data-color=\"aa0000\" data-italic=\"1\">bold</span></strong>") != std::string::npos);
    CHECK(tex_html.find("<code>code</code>") != std::string::npos);
    CHECK(tex_html.find("data-kind=\"definition\" data-color=\"00aa00\"") != std::string::npos);
    CHECK(tex_html.find("data-end=\"QED\"") != std::string::npos);
    // The same document with no sheet to read: exactly the plain export.
    const mepml::Document bare = mepml::ParseForExport("/d/doc.mepml", lines, [](const std::string &, std::vector<std::string> *) { return false; },
                                                       {"pdf", "tex", "latex"});
    CHECK(bare.sheets.empty());
    CHECK(mepml::ToHtml(bare).find("mep-style") == std::string::npos && mepml::ToHtml(bare).find("data-color") == std::string::npos);
}

// A page of the presentation view is a piece of its document: inside its
// slide, and the title page's lines the header's.
void TestPresentationContext() {
    mepml::HighlightContext slide;
    slide.container = Element("slide", "number", "3");
    const mepml::Document page = mepml::Parse({"> A title", "", "Some *bold* text."});
    mepml::ElementPaths paths;
    std::vector<int> nodes;
    mepml::Highlight(page, &paths, &nodes, &slide);
    CHECK(PathText(paths, nodes[0]) == "document[type=document] > slide[number=3] > heading[level=1]");
    CHECK(PathText(paths, nodes[1]) == "document[type=document] > slide[number=3] > paragraph");
    // So `slide heading` selects there, and only under `present` if it says so.
    Cascade c = One("slide heading { color: #010203 } @media present { slide bold { color: #040506 } }", {"present", "slides", "screen"});
    const std::vector<Computed> st = c.ComputeAll(paths);
    CHECK(st[static_cast<size_t>(nodes[0])].color.rgb == 0x010203);
    bool bold_seen = false;
    for (size_t n = 0; n < paths.nodes.size(); ++n)
        if (paths.nodes[n].element.name == "bold" && paths.nodes[n].element.part.empty()) {
            bold_seen = true;
            CHECK(st[n].color.rgb == 0x040506);
        }
    CHECK(bold_seen);

    // The title page, as PresentationPages writes it.
    mepml::HighlightContext title;
    title.container = Element("slide", "number", "0");
    title.container.With("title");
    title.title_page = true;
    const mepml::Document tp = mepml::Parse({"> The Title", "", ">A subtitle<", "", "An Author  ", "~2026~"});
    mepml::ElementPaths tpaths;
    std::vector<int> tnodes;
    mepml::Highlight(tp, &tpaths, &tnodes, &title);
    CHECK(PathText(tpaths, tnodes[0]) == "document[type=document] > slide[number=0][title] > meta[key=title]");
    CHECK(PathText(tpaths, tnodes[1]) == "document[type=document] > slide[number=0][title] > meta[key=subtitle]");
    CHECK(PathText(tpaths, tnodes[2]) == "document[type=document] > slide[number=0][title] > meta[key=author]");
    // The default sheet keeps the title page as it always was while
    // presenting: the title at a heading's size, the subtitle plain.
    Cascade d;
    d.sheets.push_back(std::make_shared<Sheet>(mepml::style::DefaultSheet()));
    d.media = {"present", "slides", "screen"};
    const std::vector<Computed> ts = d.ComputeAll(tpaths);
    const Computed &t = ts[static_cast<size_t>(tnodes[0])], &sub = ts[static_cast<size_t>(tnodes[1])];
    CHECK(t.font_size > 1.59f && t.font_size < 1.61f && !t.bold && t.color.group == "Purple");
    CHECK(sub.font_size == 1.0f && !sub.italic && !sub.has_color);
    // And a slide has no paper of its own there (it has, as a card, in the editor).
    CHECK(Style(d, {Element("document"), title.container}).background.kind == Color::None);
    d.media = {"editor", "screen"};
    CHECK(Style(d, {Element("document"), slide.container}).background.kind != Color::None);
}

std::string ReadAll(const std::string &path) {
    std::string out;
    std::FILE *f = std::fopen(path.c_str(), "r");
    CHECK(f != nullptr);
    for (int ch = std::fgetc(f); ch != EOF; ch = std::fgetc(f)) out += static_cast<char>(ch);
    std::fclose(f);
    return out;
}

// The specification and the code name the same things (docs/mepml-spec):
// every property, every element, and the default sheet's own selectors.
void TestSpecification() {
    const std::string style_md = ReadAll(std::string(MEPML_SPEC_DIR) + "/style.md");
    const std::string structure_md = ReadAll(std::string(MEPML_SPEC_DIR) + "/structure.md");
    for (const mepml::style::PropertyInfo &p : mepml::style::Properties()) {
        if (style_md.find("| `" + std::string(p.name) + "` |") == std::string::npos) std::fprintf(stderr, "style.md lacks property %s\n", p.name);
        CHECK(style_md.find("| `" + std::string(p.name) + "` |") != std::string::npos);
    }
    for (const std::string &name : mepml::ElementNames()) {
        if (structure_md.find("| `" + name + "` |") == std::string::npos) std::fprintf(stderr, "structure.md lacks element %s\n", name.c_str());
        CHECK(structure_md.find("| `" + name + "` |") != std::string::npos);
    }
    // Every element row of structure.md's two tables is an element the code knows.
    const size_t blocks = structure_md.find("## 1. Block elements"), parts = structure_md.find("## 3. Parts");
    CHECK(blocks != std::string::npos && parts != std::string::npos);
    const std::vector<std::string> &names = mepml::ElementNames();
    for (size_t at = structure_md.find("\n| `", blocks); at != std::string::npos && at < parts; at = structure_md.find("\n| `", at + 1)) {
        const size_t end = structure_md.find('`', at + 4);
        const std::string name = structure_md.substr(at + 4, end - at - 4);
        if (std::find(names.begin(), names.end(), name) == names.end()) std::fprintf(stderr, "structure.md names unknown element %s\n", name.c_str());
        CHECK(std::find(names.begin(), names.end(), name) != names.end());
    }
    // What a real document's tree holds, and what the default sheet selects.
    std::vector<std::string> lines;
    {
        const std::string text = ReadAll(MEPML_REFERENCE_FILE);
        size_t from = 0;
        for (size_t nl = text.find('\n'); nl != std::string::npos; from = nl + 1, nl = text.find('\n', from)) lines.push_back(text.substr(from, nl - from));
    }
    mepml::ElementPaths paths;
    mepml::Highlight(mepml::Parse(lines), &paths);
    for (const mepml::ElementPaths::Node &n : paths.nodes) CHECK(std::find(names.begin(), names.end(), n.element.name) != names.end());
    for (const mepml::style::Rule &rule : mepml::style::DefaultSheet().rules)
        for (const mepml::style::Selector &sel : rule.selectors)
            for (const mepml::style::Compound &c : sel.compounds) {
                if (!c.name.empty() && std::find(names.begin(), names.end(), c.name) == names.end())
                    std::fprintf(stderr, "default.mepss selects unknown element %s\n", c.name.c_str());
                CHECK(c.name.empty() || std::find(names.begin(), names.end(), c.name) != names.end());
            }
}

// Kinds, names and sheets of the document's own: `\boxed(kind, ...)`,
// `\class(name, ...)` with the `.name` selector, `\raw(style, ...)`.
void TestDocumentsOwn() {
    const std::vector<std::string> lines = {
        "\\raw(style,",                                   // 0
        "box[kind=axiom]::label { content: \"Ax.\"; }",    // 1
        ".term { color: #010203; }",                       // 2
        "paragraph .warn, heading.warn { font-weight: bold; }",  // 3
        ")",                                               // 4
        "",
        "A \\class(term, group) and *a \\class(warn, flag)*.",  // 6
        "",
        "\\boxed(axiom, Choice,",                          // 8
        "Text.",                                           // 9
        ")",                                               // 10
        "\\boxed(key-result, Up 12%.)",                    // 11
        "\\boxed(not a kind, x)",                          // 12: a paragraph
    };
    const mepml::Document doc = mepml::Parse(lines);
    std::map<mepml::BlockKind, int> count;
    for (const mepml::Block &b : doc.blocks) ++count[b.kind];
    CHECK(count[mepml::BlockKind::BoxBegin] == 2 && count[mepml::BlockKind::BoxEnd] == 1 && count[mepml::BlockKind::Raw] == 1);
    const mepml::Block *axiom = nullptr, *key = nullptr;
    for (const mepml::Block &b : doc.blocks) {
        if (b.kind == mepml::BlockKind::BoxBegin && b.keyword == "axiom") axiom = &b;
        if (b.kind == mepml::BlockKind::BoxBegin && b.keyword == "key-result") key = &b;
    }
    CHECK(axiom && axiom->caption == "Choice" && !axiom->box_closed);
    CHECK(key && key->box_closed && key->caption.empty() && mepml::InlinePlainText(key->inlines) == "Up 12%.");
    CHECK(mepml::BoxLabel("axiom") == "Axiom" && mepml::BoxLabel("key-result") == "Key result" && mepml::BoxLabel("proof") == "Proof");
    CHECK(mepml::BoxOpenerText("axiom") == "\\boxed(axiom, " && mepml::BoxOpenerText("lemma") == "\\lemma(");
    CHECK(mepml::IsBoxKindName("key-result") && !mepml::IsBoxKindName("2x") && !mepml::IsBoxKindName("a b") && !mepml::IsBoxKindName(""));

    // The sheet in the document, and what it selects.
    const std::vector<std::string> sheets = mepml::InlineStyleSheets(doc);
    CHECK(sheets.size() == 1);
    const Sheet sheet = mepml::style::Parse(sheets[0]);
    CHECK(sheet.diagnostics.empty() && sheet.rules.size() == 3);
    CHECK(sheet.rules[1].selectors[0].compounds[0].name.empty() && sheet.rules[1].selectors[0].compounds[0].attrs[0].name == "class");
    Cascade c;
    c.sheets.push_back(std::make_shared<Sheet>(mepml::style::DefaultSheet()));
    c.sheets.push_back(std::make_shared<Sheet>(sheet));
    c.media = {"editor", "screen"};
    mepml::ElementPaths paths;
    const std::vector<mepml::Span> spans = mepml::Highlight(doc, &paths);
    const std::vector<Computed> st = c.ComputeAll(paths);
    auto node_of = [&](const std::string &text) {
        for (size_t n = 0; n < paths.nodes.size(); ++n)
            if (PathText(paths, static_cast<int>(n)) == text) return static_cast<int>(n);
        return -1;
    };
    const int term = node_of("document[type=document] > paragraph > span[class=term]");
    const int warn = node_of("document[type=document] > paragraph > bold > span[class=warn]");
    const int label = node_of("document[type=document] > box[kind=axiom]::label");
    CHECK(term >= 0 && warn >= 0 && label >= 0);
    CHECK(st[static_cast<size_t>(term)].color.rgb == 0x010203 && !st[static_cast<size_t>(term)].bold);
    CHECK(st[static_cast<size_t>(warn)].bold);
    CHECK(st[static_cast<size_t>(label)].content == "Ax.");
    // The opener is all of `\boxed(axiom, `: the title starts after it.
    bool opener = false;
    for (const mepml::Span &sp : spans)
        if (sp.line == 8 && sp.col_start == 0 && sp.markup) {
            opener = true;
            CHECK(sp.col_end == 14 && sp.replace == "Axiom: ");
        }
    CHECK(opener);
    CHECK(mepml::style::Parse(". { color: red }").rules.empty());

    // Exports: the kind's own label and class, the sheet's rules, no \raw.
    auto none = [](const std::string &, std::vector<std::string> *) { return false; };
    const mepml::Document html = mepml::ParseForExport("/d/x.mepml", lines, none, {"html"});
    CHECK(html.sheets.size() == 1);
    const std::string page = mepml::ToHtml(html);
    CHECK(page.find("<div class=\"mbox mbox-axiom\" data-kind=\"axiom\">") != std::string::npos);
    CHECK(page.find("<span class=\"mbox-label\">Ax.</span>") != std::string::npos);
    CHECK(page.find("<span class=\"mbox-label\">Key result</span>") != std::string::npos);
    CHECK(page.find("<span class=\"mep-term\">group</span>") != std::string::npos);
    CHECK(page.find(":root .mep-term { color: #010203; }") != std::string::npos);
    CHECK(page.find(":root p .mep-warn { font-weight: bold; }") != std::string::npos);
    CHECK(page.find("content: \"Ax.\"") == std::string::npos);  // (a label is text on the page, not CSS)
    CHECK(page.find("box[kind") == std::string::npos);           // the \raw(style, ...) itself is in no export
    CHECK(mepml::ExportBoxLook(html, "axiom").label == "Ax." && mepml::ExportBoxLook(html, "key-result").label == "Key result");
}

// Random edits of a real sheet: the parser and the cascade must not crash,
// hang or read out of bounds (run under the sanitize build).
void TestFuzz() {
    const std::string base = mepml::style::DefaultSheetText();
    std::mt19937 rng(20261002u);
    const std::string alphabet = "{}[]():;,\"'*>@/\\ \n-_=#%abv0123456789";
    const Element doc("document"), box("box", "kind", "definition"), bold("bold");
    for (int round = 0; round < 3000; ++round) {
        std::string text = base.substr(0, rng() % (base.size() + 1));
        if (round % 3 == 0) text = base;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits && !text.empty(); ++e) {
            const size_t at = rng() % text.size();
            switch (rng() % 3) {
                case 0: text.erase(at, 1 + rng() % 5); break;
                case 1: text.insert(at, 1, alphabet[rng() % alphabet.size()]); break;
                default: text[at] = alphabet[rng() % alphabet.size()]; break;
            }
        }
        Cascade c;
        c.sheets.push_back(SheetOf(text));
        c.media = {"editor", "source"};
        Style(c, {doc, box, box.Part("label")});
        Style(c, {doc, box, Element("paragraph"), bold, bold.Part("markup")});
    }
}

}  // namespace

int main() {
    TestParsing();
    TestErrorsNeverStopASheet();
    TestSelectors();
    TestCascadeOrder();
    TestInheritance();
    TestMedia();
    TestValues();
    TestCustomProperties();
    TestPresentationalMarkup();
    TestComputeAll();
    TestDefaultSheet();
    TestStates();
    TestStyleRefs();
    TestElementPaths();
    TestElementTreeJson();
    TestReferenceFile();
    TestExports();
    TestPresentationContext();
    TestDocumentsOwn();
    TestSpecification();
    TestFuzz();
    std::printf("mepml_style_test: all checks passed\n");
    return 0;
}
