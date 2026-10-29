// mep-mepml: mepml's converter as a command-line tool.
//
//   mep-mepml convert [--beamer] [--no-notice] [TEX OPTIONS] IN OUT
//   mep-mepml build [--beamer] [--no-notice] [TEX OPTIONS] IN.mepml [FORMAT...]
//
// Formats come from the file extensions. From .mepml to anything mep can
// export (html md org rtf docx odt tex pdf txt); from anything it can
// import (html md org rtf docx odt txt) to .mepml; and between any two of
// those by way of mepml. PDF is the LaTeX export compiled by the TeX
// switch's engine (tex/tex_backend.h): `--tex-engine builtin|system`
// (default $MEP_TEX_ENGINE, else builtin) and `--tex-command CMD` (the
// system engine: tectonic, pdflatex, xelatex, lualatex, latexmk or an
// argv; default $MEP_TEX_COMMAND, else tectonic) -- the editor's
// tex_engine / tex_system_command settings, for the command line. An export of a .mepml starts with a comment
// saying it was generated from that file and is not the one to edit
// (mepml_convert.h, ExportOptions); --no-notice leaves it out, as does
// `//? Notice: no` in the document.
//
// `run` runs every code block of the documents and writes their results
// into them, as the editor's C-c C-c does, with no window: it is `mep
// --run-mepml` (main.cpp, RunHeadlessMepml), which holds the editor's block
// runner, and is found beside this binary ($MEP_EXECUTABLE or --mep PATH
// otherwise). `convert --run` and `build --run` run the blocks first and
// then export, so a document compiles from any directory to any format in
// one command; the export is written even when a block failed (the block
// keeps its previous results), and the exit status is then 1.
//
// `build` is what the editor's export does: each FORMAT (the header's
// `//? Export:`, else html, when none is given) is written into the
// document's build directory (`build` beside it, or its `//? Build:`),
// and then the document's `//? Post:` command runs once, in the
// document's directory, with MEP_SOURCE, MEP_BUILD and MEP_OUT set
// (mepml_convert.h, PostEnvironment).
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "a11y_file.h"
#include "mepml_convert.h"
#include "mepml_style.h"
#include "tex/tex_backend.h"

namespace {

// The TeX switch for a PDF export (see the file's head).
struct TexChoice {
    std::string engine = "builtin";
    std::string command = "tectonic";
    std::string builtin_tool = "mep-tex";  // beside this binary, when it is there
};
TexChoice g_tex;

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

// The mep that runs code blocks: --mep, else $MEP_EXECUTABLE, else the
// `mep` beside this binary, else whatever `mep` is on PATH.
std::string g_mep;
std::string MepExecutable() {
    if (!g_mep.empty()) return g_mep;
    if (const char *e = std::getenv("MEP_EXECUTABLE"); e && *e) return e;
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const std::filesystem::path sibling = self.parent_path() / "mep";
        if (std::filesystem::exists(sibling, ec)) return sibling.string();
    }
    return "mep";
}

// `mep --run-mepml ARGS...`, waited for. Its exit status (127 when it
// could not be started).
int RunBlocks(const std::vector<std::string> &args) {
    std::vector<std::string> argv = {MepExecutable(), "--run-mepml"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char *> c;
    for (std::string &a : argv) c.push_back(a.data());
    c.push_back(nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        execvp(c[0], c.data());
        std::fprintf(stderr, "mep-mepml: cannot run %s (set $MEP_EXECUTABLE or pass --mep PATH)\n", c[0]);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 127;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: mep-mepml convert [--run [RUN OPTIONS]] [--beamer] [--no-notice] [TEX OPTIONS] IN OUT\n"
                 "       mep-mepml build [--run [RUN OPTIONS]] [-d DIR] [--no-post] [--beamer] [--no-notice]\n"
                 "                       [TEX OPTIONS] IN.mepml [FORMAT...]\n"
                 "                                                export into the document's build directory\n"
                 "                                                (//? Build:, `build` by default; -d/--out-dir DIR\n"
                 "                                                instead) in each FORMAT (//? Export:, else html),\n"
                 "                                                then run its //? Post: (not with --no-post)\n"
                 "       mep-mepml run [RUN OPTIONS] FILE.mepml...  run every code block, as C-c C-c does, and\n"
                 "                                                write the results into the document\n"
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
                 "  TEX OPTIONS, for pdf: --tex-engine builtin|system ($MEP_TEX_ENGINE, else builtin)\n"
                 "  and --tex-command tectonic|pdflatex|xelatex|lualatex|latexmk|'ARGV'\n"
                 "  ($MEP_TEX_COMMAND, else tectonic; in an ARGV %%f is the .tex and %%o the output dir)\n"
                 "  import to .mepml from: html md org rtf docx odt txt\n"
                 "  --run, with convert/build: run the code blocks first (in the document's directory,\n"
                 "  writing their results into it), then export; the export is written even if a\n"
                 "  block fails (it keeps its previous results), but the exit status is then 1\n"
                 "  RUN OPTIONS: --timeout SECONDS (per block), --report FILE.json (each block's\n"
                 "  outcome), --write-errors (a failed block's errors become its results),\n"
                 "  -q/--quiet, -v/--verbose, --mep PATH (the mep that runs them; $MEP_EXECUTABLE,\n"
                 "  else the mep beside this program, else mep on PATH); `run` also takes -o OUT\n"
                 "  Options may come before or after the files.\n");
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
    // OUT may be anywhere: its directory is made when missing.
    if (!mepml::EnsureExportDir(std::filesystem::absolute(out, ec).string(), &err)) {
        std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
        return 1;
    }
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
        std::string tex_err;
        const bool ok = TexCompile(g_tex.engine, g_tex.command, g_tex.builtin_tool, tex.string(),
                                   pdf.parent_path().string(), pdf.parent_path().string(), &tex_err);
        std::filesystem::remove(tex, ec);
        if (!ok) {
            std::fprintf(stderr, "mep-mepml: PDF compile failed: %s\n", tex_err.c_str());
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
int Build(const std::string &in, std::vector<std::string> formats, bool beamer, const mepml::ExportOptions &opts,
          const std::string &out_dir, bool post) {
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
        const std::string out = out_dir.empty()
                                    ? mepml::ExportPath(doc, in, f)
                                    : (std::filesystem::path(out_dir) / std::filesystem::path(in).stem()).string() + "." + f;
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
    if (post && !mepml::RunPostCommand(doc, in, outs, &err)) {
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
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    if (cmd != "convert" && cmd != "build" && cmd != "run") return Usage();
    bool beamer = false, run = cmd == "run", post = true;
    mepml::ExportOptions opts;
    std::string out_dir;
    std::vector<std::string> run_args, files;
    if (const char *e = std::getenv("MEP_TEX_ENGINE"); e && *e) g_tex.engine = e;
    if (const char *c = std::getenv("MEP_TEX_COMMAND"); c && *c) g_tex.command = c;
    {
        std::error_code ec;
        std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) self = std::filesystem::absolute(argv[0], ec);
        const std::filesystem::path tool = self.parent_path() / "mep-tex";
        if (std::filesystem::exists(tool, ec)) g_tex.builtin_tool = tool.string();
    }
    // Options anywhere; the rest are the files (and build's formats).
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--beamer" && !run) beamer = true;
        else if (a == "--no-notice" && !run) opts.notice = false;
        else if (a == "--tex-engine" && has_value && !run) g_tex.engine = argv[++i];
        else if (a == "--tex-command" && has_value && !run) g_tex.command = argv[++i];
        else if ((a == "-d" || a == "--out-dir") && has_value && cmd == "build") out_dir = argv[++i];
        else if (a == "--no-post" && cmd == "build") post = false;
        else if (a == "--run") run = true;
        else if (a == "--mep" && has_value) g_mep = argv[++i];
        else if ((a == "--timeout" || a == "--report" || ((a == "-o" || a == "--output") && cmd == "run")) && has_value) {
            run_args.push_back(a);
            run_args.push_back(argv[++i]);
        } else if (a == "--write-errors" || a == "-q" || a == "--quiet" || a == "-v" || a == "--verbose") {
            run_args.push_back(a);
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "mep-mepml: unknown option %s\n", a.c_str());
            return Usage();
        } else {
            files.push_back(a);
        }
    }
    if (g_tex.engine != "builtin" && g_tex.engine != "system") return Usage();
    if (cmd == "run") {
        if (files.empty()) return Usage();
        run_args.insert(run_args.end(), files.begin(), files.end());
        return RunBlocks(run_args);
    }
    if (files.empty() || (cmd == "convert" && files.size() != 2)) return Usage();
    // --run: the blocks first, in place; a failure still exports (the
    // failed blocks keep their previous results) but sets the exit status.
    int run_rc = 0;
    if (run) {
        if (mepml::FormatFromPath(files[0]) != mepml::Format::Mepml) {
            std::fprintf(stderr, "mep-mepml: --run takes a .mepml, not %s\n", files[0].c_str());
            return 2;
        }
        run_args.push_back(files[0]);
        run_rc = RunBlocks(run_args);
        if (run_rc == 2 || run_rc == 127) return run_rc;
    }
    const int rc = cmd == "build"
                       ? Build(files[0], std::vector<std::string>(files.begin() + 1, files.end()), beamer, opts, out_dir, post)
                       : Convert(files[0], files[1], beamer, opts);
    return rc != 0 ? rc : run_rc;
}
