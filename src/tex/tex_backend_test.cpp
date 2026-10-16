// Windowless test for tex/tex_backend.cpp -- the TeX switch's command
// table, shared by the editor (mep.tex_command) and mep-mepml. Nothing here
// runs a TeX: it pins how each engine is spelled, which output line is
// reported as the error, and when a log asks for another pass.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "tex/tex_backend.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Argv = std::vector<std::string>;
}  // namespace

int main() {
    // --- The command table --------------------------------------------
    {
        const TexCommand t = TexCompileCommand("system", "tectonic", "/bin/mep-tex", "/d/a.tex", "/o", false);
        CHECK((t.argv == Argv{"tectonic", "-X", "compile", "/d/a.tex", "--outfmt", "pdf", "--outdir", "/o"}));
        CHECK(t.exe == "tectonic");
        CHECK(t.reruns);
        CHECK(!t.leaves_files);
        // A preview reads its box height from the log, which tectonic only
        // keeps when asked.
        const TexCommand k = TexCompileCommand("system", "tectonic", "", "/d/a.tex", "/o", true);
        CHECK(k.argv.back() == "--keep-logs");
        // Unset means tectonic, as the setting's default does.
        CHECK(TexCompileCommand("system", "", "", "/d/a.tex", "/o", false).exe == "tectonic");
    }
    {
        const TexCommand p = TexCompileCommand("system", "pdflatex", "", "/d/a.tex", "/o", false);
        CHECK((p.argv == Argv{"pdflatex", "-interaction=nonstopmode", "-halt-on-error", "-file-line-error",
                              "-output-directory=/o", "/d/a.tex"}));
        // A bare engine needs passes from the caller, and leaves its files.
        CHECK(!p.reruns);
        CHECK(p.leaves_files);
        CHECK(TexCompileCommand("system", "lualatex", "", "a.tex", "o", false).exe == "lualatex");
        const TexCommand m = TexCompileCommand("system", "latexmk", "", "a.tex", "o", false);
        CHECK(m.reruns);
        CHECK(m.argv.back() == "a.tex");
    }
    {
        // A typed command line: %f and %o substituted, the file appended
        // when it has no %f.
        const TexCommand c = TexCompileCommand("system", "latexmk -xelatex -outdir=%o %f", "", "/d/a.tex", "/o", false);
        CHECK((c.argv == Argv{"latexmk", "-xelatex", "-outdir=/o", "/d/a.tex"}));
        CHECK(c.exe == "latexmk");
        const TexCommand d = TexCompileCommand("system", "xelatex -shell-escape", "", "/d/a.tex", "/o", false);
        CHECK((d.argv == Argv{"xelatex", "-shell-escape", "/d/a.tex"}));
        // Only spaces: falls back to the default rather than running nothing.
        CHECK(TexCompileCommand("system", "   ", "", "a.tex", "o", false).exe == "tectonic");
    }
    {
        const TexCommand b = TexCompileCommand("builtin", "pdflatex", "/bin/mep-tex", "/d/a.tex", "/o", false);
        CHECK((b.argv == Argv{"/bin/mep-tex", "compile", "/d/a.tex", "--outdir", "/o"}));
        CHECK(b.exe == "/bin/mep-tex");
    }
    {
        const std::vector<std::string> &names = TexSystemCommandNames();
        CHECK(names.size() == 5);
        CHECK(names[0] == "tectonic");
    }

    // --- The error line -------------------------------------------------
    {
        // tectonic: the first real `error:`, never its generic wrappers or
        // the warning it prints first on every run.
        CHECK(TexErrorLine({"warning: accessing absolute path `/dev/null`",
                            "error: the XeTeX engine had an error",
                            "error: a.tex:3: Undefined control sequence"}) == "a.tex:3: Undefined control sequence");
        CHECK(TexErrorLine({"error: ! Missing $ inserted."}) == "Missing $ inserted.");
        // pdflatex: TeX's own `! ...`, else the -file-line-error form; the
        // banner on its first line is never the answer.
        CHECK(TexErrorLine({"This is pdfTeX, Version 3.141592653", "! Undefined control sequence.", "l.3 \\foo"}) ==
              "Undefined control sequence.");
        CHECK(TexErrorLine({"This is pdfTeX", "./a.tex:12: LaTeX Error: File `x.sty' not found."}) ==
              "LaTeX Error: File `x.sty' not found.");
        // Nothing recognisable: the last non-blank line.
        CHECK(TexErrorLine({"mep-tex: error: cannot typeset", ""}) == "mep-tex: error: cannot typeset");
        CHECK(TexErrorLine({}).empty());
    }

    // --- Reruns -----------------------------------------------------------
    {
        CHECK(TexLogWantsRerun("LaTeX Warning: Label(s) may have changed. Rerun to get cross-references right."));
        CHECK(TexLogWantsRerun("Package rerunfilecheck Warning: File `a.out' has changed. Rerun to get outlines right"));
        CHECK(!TexLogWantsRerun("Output written on a.pdf (1 page)."));
    }

    std::printf("tex_backend tests passed\n");
    return 0;
}
