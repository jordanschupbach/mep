// mep-mepml: mepml's converter as a command-line tool.
//
//   mep-mepml convert IN OUT
//
// Formats come from the file extensions. From .mepml to anything mep can
// export (html md org rtf docx odt tex pdf txt); from anything it can
// import (html md org rtf docx odt txt) to .mepml; and between any two of
// those by way of mepml. PDF is the LaTeX export compiled by tectonic,
// which must be on PATH.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
                 "usage: mep-mepml convert [--beamer] IN OUT\n"
                 "       mep-mepml tree FILE                      the document's element tree, as JSON\n"
                 "       mep-mepml style FILE [--media a,b] [--sheet S.mepss]...\n"
                 "                                                the style each element computes to\n"
                 "  export from .mepml to: html md org rtf docx odt tex pdf txt pptx odp\n"
                 "  (with //? Type: presentation, html/tex/pdf are a slideshow and a Beamer deck;\n"
                 "  --beamer makes tex/pdf the Beamer deck of the \\slide blocks whatever the Type)\n"
                 "  import to .mepml from: html md org rtf docx odt txt\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 3 && (std::string(argv[1]) == "tree" || std::string(argv[1]) == "style")) {
        const int rc = Inspect(argc, argv);
        return rc == 2 ? Usage() : rc;
    }
    const bool beamer = argc == 5 && std::string(argv[2]) == "--beamer";
    if (argc != 4 + (beamer ? 1 : 0) || std::string(argv[1]) != "convert") return Usage();
    const std::string in = argv[argc - 2], out = argv[argc - 1];
    const mepml::Format fin = mepml::FormatFromPath(in), fout = mepml::FormatFromPath(out);
    std::error_code ec;
    const std::string in_abs = std::filesystem::absolute(in, ec).string();
    const std::string base_dir = std::filesystem::path(in_abs).parent_path().string();

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
    if (!mepml::ExportFile(doc, out, base_dir, &err)) {
        std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
