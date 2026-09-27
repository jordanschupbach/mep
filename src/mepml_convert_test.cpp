// Windowless test for mepml's converters (mepml_export.cpp,
// mepml_import.cpp): the repo's test.mepml is exported to every format and
// imported back, and the result must have the same structure -- the same
// headings, paragraphs (as plain text), code, tables, lists, figures and
// maths -- with each format keeping what it is able to keep. Plus property
// tests of the escaping every importer relies on.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "mepml_convert.h"
#include "mepml_doc.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;
using namespace mepml;

bool ReadLines(const std::string &path, Lines *out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) out->push_back(line);
    return true;
}

Lines Split(const std::string &s) {
    Lines out;
    std::istringstream ss(s);
    std::string l;
    while (std::getline(ss, l)) out.push_back(l);
    return out;
}

std::string Squash(const std::string &s) {
    std::string o;
    for (char c : s) {
        const bool sp = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (sp && (o.empty() || o.back() == ' ')) continue;
        o += sp ? ' ' : c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string Plain(const std::vector<Inline> &ins) { return Squash(InlinePlainText(ins)); }

// How much of the document a format is expected to carry back.
struct Keeps {
    bool code_options = false;  // ```{lang, Name=value} (not just ```lang)
    bool captions = true;       // table/figure captions
    bool code_captions = false; // a caption on a code block that has not been run
    bool list_kinds = true;     // bullets vs numbers per item
};

// One line per block that matters, in order.
Lines Signature(const Document &doc, const Keeps &k) {
    Lines sig;
    for (const Block &b : doc.blocks) {
        std::string s;
        switch (b.kind) {
            case BlockKind::Paragraph: s = "P " + Plain(b.inlines); break;
            case BlockKind::Heading: s = "H" + std::to_string(b.level) + " " + Plain(b.inlines); break;
            case BlockKind::Callout: s = "C " + b.keyword + " " + Plain(b.inlines); break;
            case BlockKind::MathBlock: s = "M " + Squash(b.code); break;
            case BlockKind::Code: {
                std::string code = b.code;
                while (!code.empty() && code.back() == '\n') code.pop_back();
                s = "K " + b.lang;
                if (k.code_options)
                    for (const Option &o : b.options) s += " " + o.name + "=" + o.value.s;
                s += "\n" + code;
                for (const std::string &r : b.result_lines) s += "\n> " + r;
                if (k.code_captions) s += "\ncaption " + Plain(b.caption_inlines);
                break;
            }
            case BlockKind::Image:
                s = "I";
                if (k.captions) s += " caption=" + Plain(b.caption_inlines);
                break;
            case BlockKind::Table:
                s = "T" + std::to_string(b.header_rows);
                for (const auto &row : b.rows) {
                    s += "\n";
                    for (const auto &cell : row) s += "|" + Plain(cell.content);
                }
                if (k.captions) s += "\ncaption " + Plain(b.caption_inlines);
                break;
            case BlockKind::List: {
                // Bullets then numbers are one mepml list but two HTML ones:
                // adjacent lists count as one.
                const bool merge = !sig.empty() && sig.back().rfind("L\n", 0) == 0;
                s = merge ? sig.back() : "L";
                if (merge) sig.pop_back();
                for (const ListItem &it : b.items)
                    s += "\n" + std::string(static_cast<size_t>(it.indent), ' ') + (k.list_kinds ? (it.ordered ? "1. " : "- ") : "* ") +
                         (it.checkbox < 0 ? "" : it.checkbox ? "[x] " : "[ ] ") + Plain(it.content);
                break;
            }
            case BlockKind::Rule: s = "R"; break;
            case BlockKind::TableOfContents: s = "TOC"; break;
            case BlockKind::Bibliography: s = "BIB"; break;
            case BlockKind::Comment:
            case BlockKind::Meta:
            case BlockKind::Import:
            case BlockKind::Citation: continue;
        }
        sig.push_back(s);
    }
    return sig;
}

// First difference, printed, so a failure says what was lost.
bool Same(const Lines &want, const Lines &got, const char *what) {
    const size_t n = std::max(want.size(), got.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string w = i < want.size() ? want[i] : "<none>";
        const std::string g = i < got.size() ? got[i] : "<none>";
        if (w != g) {
            std::fprintf(stderr, "%s: block %zu differs\n  want: %s\n  got:  %s\n", what, i, w.c_str(), g.c_str());
            return false;
        }
    }
    return true;
}

int CountKind(const std::vector<Inline> &ins, InlineKind k) {
    int n = 0;
    for (const Inline &x : ins) n += (x.kind == k) + CountKind(x.children, k);
    return n;
}
int CountKind(const Document &doc, InlineKind k) {
    int n = 0;
    for (const Block &b : doc.blocks) {
        n += CountKind(b.inlines, k) + CountKind(b.caption_inlines, k);
        for (const auto &row : b.rows)
            for (const auto &cell : row) n += CountKind(cell.content, k);
        for (const ListItem &it : b.items) n += CountKind(it.content, k);
    }
    return n;
}

std::string Temp(const std::string &name) {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path() / ("mep-mepml-convert-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(d);
        return d;
    }();
    return (dir / name).string();
}

// test.mepml -> FORMAT -> mepml, through the files a user would get.
Document RoundTrip(const Document &doc, const std::string &base_dir, const std::string &ext, std::string *text_out = nullptr) {
    const std::string out = Temp("rt." + ext), back = Temp("rt_" + ext + ".mepml");
    std::string err;
    CHECK(ExportFile(doc, out, base_dir, &err));
    std::string mepml;
    CHECK(ImportFile(out, back, &mepml, &err));
    CHECK(!mepml.empty());
    if (text_out) *text_out = mepml;
    return Parse(Split(mepml));
}

void TestEscaping() {
    // Random prose made of the characters mepml gives meaning to: escaped,
    // it must parse back to exactly itself as plain text.
    std::mt19937 rng(1234);
    const std::string alphabet = "ab *~_^<>|=-+!,.\\`{}[]()$@/#:";
    for (int round = 0; round < 20000; ++round) {
        std::string t;
        const int len = 1 + static_cast<int>(rng() % 24);
        for (int i = 0; i < len; ++i) t += alphabet[rng() % alphabet.size()];
        const std::string esc = EscapeInline(t);
        // (Whitespace is prose's own business: paragraphs trim and fold it.)
        const std::string got = Squash(InlinePlainText(ParseInlines(esc)));
        t = Squash(t);
        if (got != t) {
            std::fprintf(stderr, "EscapeInline: [%s] -> [%s] -> [%s]\n", t.c_str(), esc.c_str(), got.c_str());
            CHECK(got == t);
        }
    }
    // A line of prose never becomes a block.
    const char *starts[] = {"> not a heading", "// not a comment", "- not a list", "1. not a list", "| not | a table |",
                            "@image{x.png}", "```", "$$", "---", "//? Key: value", "\\[ x \\]", "@toc"};
    for (const char *s : starts) {
        const Document d = Parse({EscapeLineStart(EscapeInline(s))});
        CHECK(d.blocks.size() == 1);
        CHECK(d.blocks[0].kind == BlockKind::Paragraph);
        if (Plain(d.blocks[0].inlines) != s) std::fprintf(stderr, "EscapeLineStart: [%s] -> [%s]\n", s, Plain(d.blocks[0].inlines).c_str());
        CHECK(Plain(d.blocks[0].inlines) == s);
    }
}

void TestFormats() {
    CHECK(FormatFromPath("a/b.MD") == Format::Markdown);
    CHECK(FormatFromPath("x.htm") == Format::Html);
    CHECK(FormatFromPath("x.docx") == Format::Docx);
    CHECK(FormatFromPath("x.odt") == Format::Odt);
    CHECK(FormatFromPath("x.mepml") == Format::Mepml);
    CHECK(FormatFromPath("x.zzz") == Format::Unknown);
    CHECK(FormatFromName("markdown") == Format::Markdown);
    CHECK(FormatFromName("org") == Format::Org);
    for (Format f : {Format::Html, Format::Markdown, Format::Org, Format::Rtf, Format::Docx, Format::Odt, Format::Latex, Format::Text}) {
        CHECK(CanExport(f));
        CHECK(FormatFromPath("x." + FormatExtension(f)) == f);
    }
    CHECK(!CanImport(Format::Latex));
    CHECK(!CanImport(Format::Pdf));
}

void TestReference() {
    Lines lines;
    CHECK(ReadLines(MEPML_REFERENCE_FILE, &lines));
    const std::string base_dir = std::filesystem::path(MEPML_REFERENCE_FILE).parent_path().string();
    const Document doc = ParseWithImports(MEPML_REFERENCE_FILE, lines, [](const std::string &p, Lines *l) { return ReadLines(p, l); });

    struct Case {
        const char *ext = "";
        Keeps keeps;
    };
    Keeps office;
    office.code_options = true;
    office.code_captions = true;
    Keeps md;
    md.code_captions = true;
    const Case cases[] = {{"md", md}, {"org", Keeps()}, {"html", Keeps()}, {"rtf", office}, {"docx", office}, {"odt", office}};
    for (const Case &c : cases) {
        std::string text;
        const Document back = RoundTrip(doc, base_dir, c.ext, &text);
        const std::string what = std::string("round trip through .") + c.ext;
        if (!Same(Signature(doc, c.keeps), Signature(back, c.keeps), what.c_str())) {
            std::fprintf(stderr, "---- imported mepml ----\n%s\n", text.c_str());
            CHECK(false);
        }
        // The inline constructs every format can carry.
        for (InlineKind k : {InlineKind::Bold, InlineKind::Italic, InlineKind::Link, InlineKind::Footnote, InlineKind::Math,
                             InlineKind::Superscript, InlineKind::Subscript}) {
            if (CountKind(doc, k) != CountKind(back, k))
                std::fprintf(stderr, "%s: inline kind %d: %d -> %d\n", what.c_str(), static_cast<int>(k), CountKind(doc, k), CountKind(back, k));
            CHECK(CountKind(doc, k) == CountKind(back, k));
        }
        // Deleted text may come back as struck-through (Org has only the one).
        CHECK(CountKind(doc, InlineKind::Strike) + CountKind(doc, InlineKind::Delete) ==
              CountKind(back, InlineKind::Strike) + CountKind(back, InlineKind::Delete));
        CHECK(back.title == doc.title);
        // The bibliography's entries travel along (as data attributes, in a
        // comment, or as custom properties).
        CHECK(back.citations.size() == doc.citations.size());
        for (const auto &kv : doc.citations) {
            CHECK(back.citations.count(kv.first) == 1);
            CHECK(back.citations.at(kv.first).fields == kv.second.fields);
        }
    }

    // The word-processor formats carry what they have no element for as
    // custom properties: metadata and options, and every inline construct.
    for (const char *ext : {"docx", "odt", "rtf"}) {
        const Document back = RoundTrip(doc, base_dir, ext);
        // Every header key but Import (an export has its imports inlined).
        const auto keys = [](const Document &d) {
            size_t n = 0;
            for (const auto &kv : d.meta) n += kv.first != "Import";
            return n;
        };
        CHECK(keys(back) == keys(doc));
        CHECK(back.options.size() == doc.options.size());
        for (int k = 0; k <= static_cast<int>(InlineKind::Comment); ++k) {
            const InlineKind kind = static_cast<InlineKind>(k);
            if (kind == InlineKind::Comment || kind == InlineKind::Text) continue;
            if (CountKind(doc, kind) != CountKind(back, kind))
                std::fprintf(stderr, "%s: inline kind %d: %d -> %d\n", ext, k, CountKind(doc, kind), CountKind(back, kind));
            CHECK(CountKind(doc, kind) == CountKind(back, kind));
        }
    }

    // Plain text and LaTeX are export-only; they must at least produce the
    // words.
    const std::string txt = ToPlainText(doc);
    CHECK(txt.find("Heading level 1") != std::string::npos);
    CHECK(txt.find("echo \"Hello, world\"") != std::string::npos);
    const std::string tex = ToLatex(doc, base_dir);
    CHECK(tex.find("\\section") != std::string::npos);
}

// The packages must be readable ZIPs with the parts other programs look
// for first.
void TestPackages() {
    Document doc = Parse({"//? Title: T", "", "> H", "", "Some *text*.", "", "| a | b |", "| - | - |", "| 1 | 2 |"});
    std::string err;
    for (const char *ext : {"docx", "odt"}) {
        const std::string path = Temp(std::string("pkg.") + ext);
        CHECK(ExportFile(doc, path, ".", &err));
        std::ifstream f(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(bytes.size() > 100);
        CHECK(bytes.compare(0, 2, "PK") == 0);
        if (std::string(ext) == "odt") CHECK(bytes.find("mimetypeapplication/vnd.oasis.opendocument.text") == 30);
        std::string back;
        CHECK(ImportFile(path, Temp(std::string("pkg_") + ext + ".mepml"), &back, &err));
        CHECK(back.find("> H") != std::string::npos);
        CHECK(back.find("*text*") != std::string::npos);
    }
    // Not a package at all: an error, not a crash.
    const std::string junk = Temp("junk.docx");
    std::ofstream(junk) << "not a zip";
    std::string out;
    CHECK(!ImportFile(junk, Temp("junk.mepml"), &out, &err));
}
}  // namespace

int main() {
    TestFormats();
    TestEscaping();
    TestPackages();
    TestReference();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(Temp("x")).parent_path(), ec);
    std::printf("mepml convert tests passed\n");
    return 0;
}
