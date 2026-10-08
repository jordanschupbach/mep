// mep-mepml: mepml's converter as a command-line tool.
//
//   mep-mepml convert [--beamer] [--no-notice] IN OUT
//   mep-mepml build [--beamer] [--no-notice] IN.mepml [FORMAT...]
//
// Formats come from the file extensions. From .mepml to anything mep can
// export (html md org rtf docx odt tex pdf txt); from anything it can
// import (html md org rtf docx odt txt) to .mepml; and between any two of
// those by way of mepml. PDF is the LaTeX export compiled by tectonic,
// which must be on PATH. An export of a .mepml starts with a comment
// saying it was generated from that file and is not the one to edit
// (mepml_convert.h, ExportOptions); --no-notice leaves it out, as does
// `//? Notice: no` in the document.
//
// `build` is what the editor's export does: each FORMAT (the header's
// `//? Export:`, else html, when none is given) is written into the
// document's build directory (`build` beside it, or its `//? Build:`),
// and then the document's `//? Post:` command runs once, in the
// document's directory, with MEP_SOURCE, MEP_BUILD and MEP_OUT set
// (mepml_convert.h, PostEnvironment).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "a11y_file.h"
#include "mepml_convert.h"
#include "mepml_style.h"

namespace {

bool ReadLines(const std::string &path, std::vector<std::string> *lines) {
    std::ifstream f(path);
    if (!f) return false;
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines->push_back(l);
    }
    return true;
}

std::vector<std::string> SplitText(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string l;
    while (std::getline(ss, l)) out.push_back(l);
    return out;
}

std::string ShellQuote(const std::string &s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

std::string ReadText(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ColorText(const mepml::style::Color &c) {
    using mepml::style::Color;
    if (c.kind == Color::None) return "none";
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%06x", static_cast<unsigned>(c.rgb & 0xffffffu));
    std::string out = c.kind == Color::Rgb ? std::string(hex) : "theme(" + c.group + (c.has_fallback ? ", " + std::string(hex) : "") + ")";
    if (c.alpha != 1.0f) {
        char a[32];
        std::snprintf(a, sizeof(a), "%.3g", static_cast<double>(c.alpha));
        out = "fade(" + out + ", " + a + ")";
    }
    return out;
}

// The properties of a computed style that differ from the initial ones.
std::string StyleText(const mepml::style::Computed &c) {
    using namespace mepml::style;
    std::string o;
    auto put = [&](const char *name, const std::string &value) { o += std::string(" ") + name + ": " + value + ";"; };
    if (c.has_color) put("color", ColorText(c.color));
    if (c.background.kind != Color::None) put("background", ColorText(c.background));
    if (c.bold) put("font-weight", "bold");
    if (c.italic) put("font-style", "italic");
    if (c.underline || c.strike) put("text-decoration", std::string(c.underline ? "underline" : "") + (c.underline && c.strike ? " " : "") + (c.strike ? "line-through" : ""));
    if (c.has_decoration_color) put("text-decoration-color", ColorText(c.decoration_color));
    if (c.font_size != 1.0f) {
        char n[32];
        std::snprintf(n, sizeof(n), "%.4g", static_cast<double>(c.font_size));
        put("font-size", n);
    }
    if (!c.font_family.empty() || !c.font_names.empty()) {
        std::string f;
        for (const std::string &n : c.font_names) f += "\"" + n + "\", ";
        put("font-family", f + (c.font_family.empty() ? "body" : c.font_family));
    }
    if (c.vertical_align != VerticalAlign::Baseline) put("vertical-align", c.vertical_align == VerticalAlign::Super ? "super" : "sub");
    if (c.text_align != TextAlign::Left) put("text-align", c.text_align == TextAlign::Center ? "center" : "right");
    if (c.has_content) put("content", "\"" + c.content + "\"");
    if (c.border_color.kind != Color::None) put("border-color", ColorText(c.border_color));
    if (c.border_left_color.kind != Color::None) put("border-left-color", ColorText(c.border_left_color));
    return o;
}

// `mep-mepml tree FILE` and `mep-mepml style FILE [--media a,b] [--sheet
// S.mepss]...`: the document's structure, and the style every element of
// it computes to (docs/mepml-spec).
int Inspect(int argc, char **argv) {
    const std::string what = argv[1];
    std::string file, media = "editor,screen";
    std::vector<std::string> sheets;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--media" && i + 1 < argc) media = argv[++i];
        else if (a == "--sheet" && i + 1 < argc) sheets.push_back(argv[++i]);
        else if (file.empty()) file = a;
        else return 2;
    }
    std::vector<std::string> lines;
    if (file.empty() || !ReadLines(file, &lines)) {
        std::fprintf(stderr, "mep-mepml: cannot read %s\n", file.c_str());
        return 1;
    }
    std::error_code ec;
    const std::string abs = std::filesystem::absolute(file, ec).string();
    const mepml::Document doc = mepml::ParseWithImports(abs, lines, ReadLines);
    if (what == "tree") {
        std::printf("%s\n", mepml::ElementTreeJson(doc).c_str());
        return 0;
    }
    mepml::style::Cascade cascade;
    cascade.sheets.push_back(std::make_shared<mepml::style::Sheet>(mepml::style::DefaultSheet()));
    // The document's own sheets, then any named on the command line.
    std::vector<std::string> paths;
    for (const mepml::StyleRef &ref : doc.styles) paths.push_back(mepml::ResolvePath(ref.base.empty() ? abs : ref.base, ref.path));
    paths.insert(paths.end(), sheets.begin(), sheets.end());
    for (const std::string &path : paths) {
        std::ifstream probe(path);
        if (!probe) {
            std::fprintf(stderr, "mep-mepml: cannot read %s\n", path.c_str());
            return 1;
        }
        auto sheet = std::make_shared<mepml::style::Sheet>(mepml::style::Parse(ReadText(path), path));
        for (const mepml::style::Diagnostic &d : sheet->diagnostics)
            std::fprintf(stderr, "%s:%d:%d: %s\n", path.c_str(), d.line + 1, d.col + 1, d.message.c_str());
        cascade.sheets.push_back(std::move(sheet));
    }
    // ... then the sheets written in the document (`\raw(style, ...)`).
    for (const std::string &text : mepml::InlineStyleSheets(doc)) {
        auto sheet = std::make_shared<mepml::style::Sheet>(mepml::style::Parse(text, file));
        for (const mepml::style::Diagnostic &d : sheet->diagnostics)
            std::fprintf(stderr, "%s: in \\raw(style, ...): %s\n", file.c_str(), d.message.c_str());
        cascade.sheets.push_back(std::move(sheet));
    }
    for (const std::string &tag : SplitText([&] {
             std::string t = media;
             for (char &c : t)
                 if (c == ',') c = '\n';
             return t;
         }()))
        if (!tag.empty()) cascade.media.push_back(tag);
    mepml::ElementPaths tree;
    mepml::Highlight(doc, &tree);
    const std::vector<mepml::style::Computed> styles = cascade.ComputeAll(tree);
    for (size_t n = 0; n < tree.nodes.size(); ++n) {
        std::string path;
        for (const mepml::Element *e : tree.Path(static_cast<int>(n))) {
            if (!e->part.empty()) {
                path += "::" + e->part;
                continue;
            }
            path += (path.empty() ? "" : " > ") + e->name;
            for (const auto &kv : e->attrs) path += "[" + kv.first + (kv.second.empty() ? "" : "=" + kv.second) + "]";
        }
        std::printf("%s {%s }\n", path.c_str(), StyleText(styles[n]).c_str());
    }
    return 0;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: mep-mepml convert [--beamer] [--no-notice] IN OUT\n"
                 "       mep-mepml build [--beamer] [--no-notice] IN.mepml [FORMAT...]\n"
                 "                                                export into the document's build directory\n"
                 "                                                (//? Build:, `build` by default) in each FORMAT\n"
                 "                                                (//? Export:, else html), then run its //? Post:\n"
                 "       mep-mepml tree FILE                      the document's element tree, as JSON\n"
                 "       mep-mepml style FILE [--media a,b] [--sheet S.mepss]...\n"
                 "                                                the style each element computes to\n"
                 "       mep-mepml a11y FILE [read|tree|check]    the document as a screen reader meets it: how it\n"
                 "                                                reads, its structure, or what such a reader is\n"
                 "                                                missing (mepml pdf html docx odt rtf md org)\n"
                 "  export from .mepml to: html md org rtf docx odt tex pdf txt pptx odp\n"
                 "  (with //? Type: presentation, html/tex/pdf are a slideshow and a Beamer deck;\n"
                 "  --beamer makes tex/pdf the Beamer deck of the \\slide blocks whatever the Type;\n"
                 "  --no-notice leaves out the comment at the top saying the file is generated)\n"
                 "  import to .mepml from: html md org rtf docx odt txt\n");
    return 2;
}

// `in` to `out`, formats by extension (see the file's head). Exit status.
int Convert(const std::string &in, const std::string &out, bool beamer, mepml::ExportOptions opts) {
    const mepml::Format fin = mepml::FormatFromPath(in), fout = mepml::FormatFromPath(out);
    std::error_code ec;
    const std::string in_abs = std::filesystem::absolute(in, ec).string();
    const std::string base_dir = std::filesystem::path(in_abs).parent_path().string();
    // The notice names the .mepml the export came from; a conversion between
    // two other formats only passed through mepml, and gets none.
    if (fin == mepml::Format::Mepml) opts.source = std::filesystem::path(in).filename().string();
    else opts.notice = false;

    // Everything goes through a mepml document.
    std::vector<std::string> lines;
    std::string err;
    if (fin == mepml::Format::Mepml) {
        if (!ReadLines(in, &lines)) {
            std::fprintf(stderr, "mep-mepml: cannot read %s\n", in.c_str());
            return 1;
        }
    } else if (mepml::CanImport(fin)) {
        std::string text;
        const std::string target = fout == mepml::Format::Mepml ? out : (std::filesystem::path(out).replace_extension(".mepml").string());
        if (!mepml::ImportFile(in, target, &text, &err)) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
        if (fout == mepml::Format::Mepml) {
            std::ofstream o(out);
            o << text;
            return o ? 0 : 1;
        }
        lines = SplitText(text);
    } else {
        std::fprintf(stderr, "mep-mepml: cannot read %s files\n", in.c_str());
        return 2;
    }
    // Commands, \when and \raw are expanded for this export: its tags say
    // what it is (html, pdf, beamer, docx ...).
    const std::vector<std::string> tags = mepml::ExportTags(fout, mepml::Parse(lines), beamer);
    const mepml::Document doc = mepml::ParseForExport(
        in_abs, lines, [](const std::string &p, std::vector<std::string> *l) { return ReadLines(p, l); }, tags);
    for (const mepml::Diagnostic &d : doc.diagnostics)
        if (d.message.rfind("expanding commands: ", 0) == 0) std::fprintf(stderr, "mep-mepml: %s\n", d.message.c_str());
    std::string latex;
    if (fout == mepml::Format::Pdf || fout == mepml::Format::Latex) {
        latex = beamer ? mepml::ToBeamer(doc, base_dir, &err) : mepml::ToLatex(doc, base_dir);
        if (latex.empty()) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
        latex = mepml::WithGeneratedNotice(mepml::Format::Latex, doc, opts, latex);
    } else if (beamer) {
        std::fprintf(stderr, "mep-mepml: --beamer writes .tex or .pdf, not %s\n", out.c_str());
        return 2;
    }
    if (fout == mepml::Format::Latex) {
        std::ofstream o(out);
        o << latex;
        return o ? 0 : 1;
    }
    if (fout == mepml::Format::Pdf) {
        const std::filesystem::path pdf = std::filesystem::absolute(out, ec);
        const std::filesystem::path tex = std::filesystem::path(pdf).replace_extension(".tex");
        {
            std::ofstream o(tex);
            o << latex;
        }
        const std::string cmd = "tectonic -X compile " + ShellQuote(tex.string()) + " --outdir " +
                                ShellQuote(pdf.parent_path().string()) + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        std::filesystem::remove(tex, ec);
        if (rc != 0) {
            std::fprintf(stderr, "mep-mepml: tectonic failed (is it on PATH?)\n");
            return 1;
        }
        return 0;
    }
    if (!mepml::CanExport(fout)) return Usage();
    if (!mepml::ExportFile(doc, out, base_dir, &err, opts)) {
        std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

// `build IN [FORMAT...]`: every format into the build directory, then the
// Post command once.
int Build(const std::string &in, std::vector<std::string> formats, bool beamer, const mepml::ExportOptions &opts) {
    if (mepml::FormatFromPath(in) != mepml::Format::Mepml) {
        std::fprintf(stderr, "mep-mepml: build takes a .mepml, not %s\n", in.c_str());
        return 2;
    }
    std::vector<std::string> lines;
    if (!ReadLines(in, &lines)) {
        std::fprintf(stderr, "mep-mepml: cannot read %s\n", in.c_str());
        return 1;
    }
    std::error_code ec;
    const std::string in_abs = std::filesystem::absolute(in, ec).string();
    const mepml::Document doc = mepml::ParseForExport(
        in_abs, lines, [](const std::string &p, std::vector<std::string> *l) { return ReadLines(p, l); }, {});
    if (formats.empty()) {
        std::string f = mepml::MetaValue(doc, "export");
        if (!f.empty() && f[0] == '.') f.erase(0, 1);
        formats.push_back(f.empty() ? "html" : f);
    }
    std::vector<std::string> outs;
    for (std::string f : formats) {
        for (char &c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!f.empty() && f[0] == '.') f.erase(0, 1);
        if (f == "markdown") f = "md";
        else if (f == "latex") f = "tex";
        else if (f == "text") f = "txt";
        else if (f == "powerpoint") f = "pptx";
        else if (f == "impress") f = "odp";
        const bool deck = f == "beamer";
        if (deck) f = "pdf";
        const std::string out = mepml::ExportPath(doc, in, f);
        std::string err;
        if (!mepml::EnsureExportDir(out, &err)) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
        const int rc = Convert(in, out, beamer || deck, opts);
        if (rc != 0) return rc;
        std::printf("  %s -> %s\n", in.c_str(), out.c_str());
        outs.push_back(out);
    }
    std::string err;
    if (!mepml::RunPostCommand(doc, in, outs, &err)) {
        std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    // `a11y FILE [read|tree|check]`: the document as a screen reader meets
    // it -- how it reads, its structure, or what such a reader is missing
    // (exit 1 when that includes an error). Any format with a reader:
    // .mepml, .pdf, .html, .docx, .odt, .rtf, .md, .org.
    if (argc >= 3 && std::string(argv[1]) == "a11y") {
        const std::string what = argc >= 4 ? argv[3] : "read";
        if (argc > 4 || (what != "read" && what != "tree" && what != "check")) return Usage();
        a11y::Document doc;
        std::string err;
        if (!a11y::FromFile(argv[2], &doc, &err)) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
        std::fputs(a11y::Report(doc, what).c_str(), stdout);
        if (what != "check") return 0;
        for (const a11y::Issue &i : a11y::Check(doc))
            if (i.severity == a11y::Severity::Error) return 1;
        return 0;
    }
    if (argc >= 3 && (std::string(argv[1]) == "tree" || std::string(argv[1]) == "style")) {
        const int rc = Inspect(argc, argv);
        return rc == 2 ? Usage() : rc;
    }
    bool beamer = false;
    mepml::ExportOptions opts;
    int i = 2;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (std::string(argv[i]) == "--beamer") beamer = true;
        else if (std::string(argv[i]) == "--no-notice") opts.notice = false;
        else return Usage();
    }
    if (argc < 2) return Usage();
    if (std::string(argv[1]) == "build") {
        if (i >= argc) return Usage();
        return Build(argv[i], std::vector<std::string>(argv + i + 1, argv + argc), beamer, opts);
    }
    if (argc != i + 2 || std::string(argv[1]) != "convert") return Usage();
    return Convert(argv[argc - 2], argv[argc - 1], beamer, opts);
}
