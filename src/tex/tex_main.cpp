// mep-tex -- mep's in-house TeX toolchain as a helper binary beside `mep`
// (found through mep.bundled_tool, like mep-org-lsp). It is what the
// `builtin` backends of the TeX switch run (plans/TEX_ENGINE_PLAN.md):
//
//   mep-tex pdf-to-png IN.pdf OUT.png [--dpi N] [--page N]
//       one page rasterized by mep's own PDF renderer -- the in-house
//       `pdftoppm -png -r N -singlefile` (tex_rasterizer = builtin).
//
//   mep-tex compile FILE.tex [--outdir DIR] [--format F] [--tree DIR]...
//       FILE.tex typeset to DIR/FILE.pdf by mep's own engine
//       (tex_engine = builtin): the format (pdflatex or pdftex, from the
//       document) built once and cached, then as many passes as the
//       cross-references need (tex/tex_compile.h).
//
//   mep-tex tex [--ini] [--fmt NAME] ... LINE...
//       the engine run directly on a first line of input.
//
// A separate process rather than a call inside the editor for the same
// reasons the system tools are: the job system already runs a compile or
// a rasterization off the UI thread and reports its exit, and a crash in
// a long TeX run takes down a helper, not the editor.

#include "pdf_raster.h"
#include "tex/gen/tex_gen.h"
#include "tex/tex_compile.h"
#include "tex/tex_resolver.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: mep-tex pdf-to-png IN.pdf OUT.png [--dpi N] [--page N]\n"
                 "       mep-tex compile FILE.tex [--outdir DIR] [--format pdflatex|pdftex|auto]\n"
                 "                       [--packages bundle|bundle+system|system] [--dir DIR]... [--tree DIR]...\n"
                 "                       [--cache-dir DIR] [--passes N] [--rebuild-format] [-v]\n"
                 "       mep-tex tex [--ini] [--fmt NAME] [--fmt-dir DIR] [--tree DIR]... [--dir DIR]...\n"
                 "                   [--output-dir DIR] [--std-printable] [--date EPOCH] LINE...\n"
                 "  pdf-to-png   rasterize one page (1-based, default 1) at N dpi (default 150)\n"
                 "  compile      typeset FILE.tex to DIR/FILE.pdf (TeX files: --tree, else mep's bundle and/or\n"
                 "               the system's as --packages says; formats are cached in $XDG_CACHE_HOME/mep/tex)\n"
                 "  tex          run the engine on LINE (TeX's first line of input)\n");
    return 2;
}

int PdfToPng(const std::vector<std::string> &args) {
    std::vector<std::string> pos;
    double dpi = 150;
    int page = 1;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--dpi" && i + 1 < args.size()) {
            dpi = std::atof(args[++i].c_str());
        } else if (args[i] == "--page" && i + 1 < args.size()) {
            page = std::atoi(args[++i].c_str());
        } else {
            pos.push_back(args[i]);
        }
    }
    if (pos.size() != 2 || dpi <= 0 || page < 1) return Usage();
    std::string err;
    if (!PdfRasterizeToPng(pos[0], pos[1], dpi, page - 1, &err)) {
        std::fprintf(stderr, "mep-tex: error: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

int Compile(const std::vector<std::string> &args) {
    meptex::CompileOptions o;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        const bool has = i + 1 < args.size();
        if ((a == "--outdir" || a == "--output-dir") && has) o.outdir = args[++i];
        else if (a == "--format" && has) o.format = args[++i] == "auto" ? "" : args[i];
        else if (a == "--tree" && has) o.trees.push_back(args[++i]);
        else if (a == "--packages" && has) o.packages = args[++i];
        else if (a == "--dir" && has) o.dirs.push_back(args[++i]);
        else if (a == "--cache-dir" && has) o.cache_dir = args[++i];
        else if (a == "--passes" && has) o.max_passes = std::atoi(args[++i].c_str());
        else if (a == "--rebuild-format") o.rebuild_format = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (o.tex.empty() && (a.empty() || a[0] != '-')) o.tex = a;
        else return Usage();
    }
    if (o.tex.empty()) return Usage();
    std::string err;
    const int rc = meptex::CompileDocument(o, &err);
    if (!err.empty()) std::fprintf(stderr, "mep-tex: error: %s\n", err.c_str());
    return rc;
}

// `mep-tex tex ...`: the engine run directly, its first line of input
// given on the command line as TeX's own `**` prompt would take it.
int Tex(const std::vector<std::string> &args) {
    meptex::Resolver resolver;
    resolver.add_dir(".");
    auto engine = std::make_unique<meptex::Engine>();
    meptex::EngineOptions &o = engine->opt;
    std::string fmt;
    std::vector<std::string> words;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        const bool has = i + 1 < args.size();
        if (a == "--ini") o.ini = true;
        else if (a == "--fmt" && has) fmt = args[++i];
        else if (a == "--fmt-dir" && has) o.format_dirs.push_back(args[++i]);
        else if (a == "--tree" && has) resolver.add_tree(args[++i]);
        else if (a == "--dir" && has) resolver.add_dir(args[++i]);
        else if (a == "--output-dir" && has) o.output_dir = args[++i];
        else if (a == "--std-printable") o.printable_8bit = false;
        else if (a == "--date" && has) o.source_date_epoch = std::atoll(args[++i].c_str());
        else words.push_back(a);
    }
    if (o.format_dirs.empty()) o.format_dirs.push_back(".");
    std::string line = fmt.empty() ? "" : "&" + fmt;
    for (const std::string &w : words) line += (line.empty() ? "" : " ") + w;
    o.first_line = line;
    engine->resolver = &resolver;
    return engine->run();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    const std::vector<std::string> rest(argv + 2, argv + argc);
    if (cmd == "pdf-to-png") return PdfToPng(rest);
    if (cmd == "compile") return Compile(rest);
    if (cmd == "tex") return Tex(rest);
    return Usage();
}
