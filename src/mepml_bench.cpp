// Persistent benchmarks for everything mepml does without a window: the
// parser, the editor's highlight spans, every exporter and importer, the
// language server's analyses and the tree-sitter grammar. Run by `just
// bench-mepml` (and `just bench`), results appended to
// bench_results/history.jsonl like the other bench binaries -- see
// plans/MEPML_PERFORMANCE_PLAN.md for what they found.
//
// Every scenario records the *median* of several runs of one operation,
// with n = the document's line count, so ops_per_sec reads as lines per
// second and scenarios at different sizes compare directly: a linear
// pass holds it flat as the document grows, a quadratic one halves it
// every time the size doubles.
//
// The corpora are generated (a *.mepml a user wrote is not something a
// benchmark can depend on), in two families: `synth_N`, N lines of a
// realistic mix of every construct test.mepml uses, and `patho_*`, one
// construct pushed hard (a huge table, thousands of footnotes, deep
// nesting, one enormous paragraph ...) to find what scales badly. The
// repo's own test.mepml runs too, as the real-document anchor.
//
//   mep-mepml-bench [--no-record] [--quick] [--only SUBSTR] [--dump CORPUS]
//
// --dump writes one corpus to stdout instead of benchmarking (for
// bisecting a slow case with another tool).

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "bench_util.h"
#include "mepml_convert.h"
#include "mepml_doc.h"
#include "mepml_lsp.h"

extern "C" const TSLanguage *tree_sitter_mepml(void);

using mep::bench::RecordResult;
using mep::bench::Timer;

namespace {

constexpr const char *kHistoryPath = "bench_results/history.jsonl";
constexpr const char *kBinary = "mepml_bench";

bool g_record = true;
bool g_quick = false;
std::string g_only;
std::string g_dump;
// Scenarios slower than this per run are measured fewer times.
constexpr double kSlowMs = 250.0;

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::string Join(const std::vector<std::string> &lines) {
    std::string s;
    for (const std::string &l : lines) {
        s += l;
        s += '\n';
    }
    return s;
}

/**
 * @brief Times `fn` several times and records the median, with n = `lines` (so ops/sec reads as lines/sec).
 * @param scenario The scenario name in the history file.
 * @param lines The document's line count.
 * @param fn The operation to time.
 */
void Measure(const std::string &scenario, size_t lines, const std::function<void()> &fn) {
    if (!g_only.empty() && scenario.find(g_only) == std::string::npos) return;
    Timer first;
    fn();  // warm-up, and a first estimate of the cost
    const double est = first.ElapsedMs();
    int reps = est > 2000.0 ? 1 : est > kSlowMs ? 3 : est > 20.0 ? 7 : 15;
    if (g_quick) reps = std::min(reps, 3);
    std::vector<double> ms;
    for (int i = 0; i < reps; i++) {
        Timer t;
        fn();
        ms.push_back(t.ElapsedMs());
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2];
    const mep::bench::Result r{scenario, static_cast<long long>(lines), median};
    if (g_record) {
        RecordResult(kBinary, kHistoryPath, r);
    } else {
        std::printf("%-52s n=%-8lld %10.3f ms  %14.1f lines/sec\n", r.scenario.c_str(), r.n, r.ms, r.OpsPerSec());
    }
}

// --- Corpora ---------------------------------------------------------------

/**
 * @brief One realistic section (~70 lines) using every construct test.mepml does.
 * @param i The section's index, woven into labels, keys and text so no two are identical.
 * @return The section's lines.
 */
std::vector<std::string> Section(int i) {
    const std::string n = std::to_string(i);
    std::vector<std::string> s = {
        "> Section " + n + " heading",
        "",
        "Here, section " + n + " has some _underlined_ text and *bold text* and `verbatim " + n +
            "` and a [link|https://example.com/" + n + "]. Also ~italic~ and ^sup^ and ,,sub,, and",
        "<small> and >big< and |mono| and =highlight= and -strike- and +insert+ and !delete!. Some",
        "\\f(Helvetica, \\fs(12, Helvetica, size 12)) text, \\color(red, red text) and a \\fn(footnote " + n +
            ") with a citation \\cite(key" + n + ") and \\citep(key" + std::to_string(i / 2) + ").",
        "",
        "Inline math $x_" + n + "^2 + y^2 = r^2$ and \\(\\frac{a_" + n + "}{b}\\) then display math:",
        "$$",
        "  \\int_0^{" + n + "} e^{-x^2} dx = \\sum_{k=0}^{\\infty} \\frac{(-1)^k}{k!}",
        "$$",
        "\\alttext(An integral for section " + n + ")",
        "",
        "- first item with *bold* inside",
        "- second item",
        "  - a nested item",
        "  - [ ] an open task",
        "  - [x] a finished task",
        "1. numbered",
        "2. and another",
        "",
        ">> Subsection " + n + ".1",
        "",
        "| Left | Centre | Right |",
        "| :--- | :----: | ----: |",
        "| a" + n + " | b | 1 |",
        "| `c` | *d* | 2.50 |",
        "| a longer cell | >big< | 100 |",
        "\\caption(Table for section " + n + ")",
        "",
        "// A comment about the block to come",
        "//? Option: Option1=value1",
        "```{python, eval=true}",
        "for i in range(" + n + "):",
        "    print(i * i)",
        "```",
        "// result_begin:",
        "// 0",
        "// 1",
        "// result_end",
        "",
        "// NOTE: a callout in section " + n,
        "",
        "\\image(./figures/fig" + n + ".png)",
        "\\caption(Figure for section " + n + ")",
        "\\alttext(Alt text " + n + ")",
        "",
        "\\glossary(Term " + n + ", The body of definition " + n + ", with a comma.)",
        "",
        "This paragraph is \\when(html, a web page) \\otherwise(not a web page) in section " + n + ".",
        "",
        "\\citation(key" + n + ",",
        "  author = Author " + n + ",",
        "  title = {Title " + n + "},",
        "  year = 1999",
        ")",
        "",
        "-------------------------------------------------------------------------------",
        "",
    };
    return s;
}

std::vector<std::string> Header() {
    return {
        "//? Title: A Benchmark Document",
        "",
        "\\abstract(",
        "This document exercises *every* construct mepml has, from ~inline markup~ and",
        "$e^{i\\pi} + 1 = 0$ to executable code blocks.",
        ")",
        "",
        "\\toc",
        "",
        "\\define(glossary(term, body),",
        "\\raw(html, <div class=\"definition\">)",
        "",
        "*Definition (#term).* #body",
        "",
        "\\raw(html, </div>)",
        ")",
        "",
    };
}

std::vector<std::string> Synth(size_t target_lines) {
    std::vector<std::string> lines = Header();
    for (int i = 1; lines.size() < target_lines; i++) {
        for (std::string &l : Section(i)) lines.push_back(std::move(l));
    }
    lines.push_back("\\bibliography");
    return lines;
}

// The same sections, each wrapped as a slide.
std::vector<std::string> Slides(int count) {
    std::vector<std::string> lines = {"//? Title: A Benchmark Deck", ""};
    for (int i = 1; i <= count; i++) {
        const std::string n = std::to_string(i);
        for (const std::string &l : std::vector<std::string>{
                 "\\slide(", ">> Slide " + n, "- a point with *bold* and $x^" + n + "$", "- another point \\fn(note " + n + ")",
                 "", "| a | b |", "| - | - |", "| " + n + " | 2 |", ")", ""})
            lines.push_back(l);
    }
    return lines;
}

std::vector<std::string> HugeTable(int rows) {
    std::vector<std::string> lines = {"> Big table", "", "| c1 | c2 | c3 | c4 | c5 | c6 | c7 | c8 |",
                                      "| --- | :---: | ---: | --- | --- | --- | --- | --- |"};
    for (int r = 0; r < rows; r++) {
        const std::string n = std::to_string(r);
        lines.push_back("| *" + n + "* | `v" + n + "` | " + n + ".5 | $x_{" + n + "}$ | a | b | ~c~ | d |");
    }
    lines.push_back("\\caption(A very large table)");
    return lines;
}

std::vector<std::string> ManyNotes(int count) {
    std::vector<std::string> lines = {"> Notes", ""};
    for (int i = 0; i < count; i++) {
        const std::string n = std::to_string(i);
        lines.push_back("Sentence " + n + " with a note \\fn(note " + n + ") and \\cite(k" + n + ") and \\citep(k" +
                        std::to_string(i / 3) + ").");
        if (i % 5 == 4) lines.push_back("");
    }
    lines.push_back("");
    for (int i = 0; i < count; i++) {
        const std::string n = std::to_string(i);
        lines.push_back("\\citation(k" + n + ", author = A" + n + ", title = {T" + n + "}, year = 2000)");
    }
    lines.push_back("");
    lines.push_back("\\bibliography");
    return lines;
}

std::vector<std::string> ManyHeadings(int count) {
    std::vector<std::string> lines = {"\\toc", ""};
    for (int i = 0; i < count; i++) {
        const int level = 1 + i % 3;
        lines.push_back(std::string(static_cast<size_t>(level), '>') + " Heading " + std::to_string(i));
        lines.push_back("");
        lines.push_back("Text under heading " + std::to_string(i) + ".");
        lines.push_back("");
    }
    return lines;
}

std::vector<std::string> ManyMath(int count) {
    std::vector<std::string> lines = {"> Maths", ""};
    std::string para;
    for (int i = 0; i < count; i++) {
        const std::string n = std::to_string(i);
        para += "term $a_{" + n + "} = \\frac{" + n + "}{2}$ ";
        if (i % 6 == 5) {
            lines.push_back(para);
            para.clear();
        }
        if (i % 30 == 29) {
            lines.push_back("");
            lines.push_back("\\[ \\sum_{k=0}^{" + n + "} k^2 = \\frac{n(n+1)(2n+1)}{6} \\]");
            lines.push_back("");
        }
    }
    lines.push_back(para);
    return lines;
}

std::vector<std::string> ManyUserCommands(int count) {
    std::vector<std::string> lines = {
        "\\define(box(title, body), \\raw(html, <div class=\"box\">)*#title* #body\\raw(html, </div>))",
        "\\define(twice(x), \\box(#x, \\box(#x, #x)))", ""};
    for (int i = 0; i < count; i++) {
        lines.push_back("Call " + std::to_string(i) + ": \\twice(item " + std::to_string(i) +
                        ") and \\when(html, web) \\otherwise(print).");
        if (i % 4 == 3) lines.push_back("");
    }
    return lines;
}

// One paragraph of `words` words on a single line: long lines are what
// a pasted document or a soft-wrapped writer produces.
std::vector<std::string> LongLine(int words) {
    std::string line;
    for (int i = 0; i < words; i++) {
        line += (i % 7 == 0) ? "*bold* " : (i % 11 == 0) ? "$x_" + std::to_string(i) + "$ " : "word ";
    }
    return {"> Long", "", line};
}

std::vector<std::string> DeepNesting(int depth) {
    std::string open, close;
    for (int i = 0; i < depth; i++) {
        open += "\\color(red, *";
        close = "*)" + close;
    }
    return {"> Deep", "", open + "core" + close};
}

// Unclosed constructs: the recovery path a document mid-edit is always in.
std::vector<std::string> Unclosed(size_t target_lines) {
    std::vector<std::string> lines = Synth(target_lines);
    for (size_t i = 0; i < lines.size(); i += 97) lines[i] += " *unclosed \\fn(open $x^2";
    return lines;
}

struct Corpus {
    std::string name;
    std::vector<std::string> lines;
};

// --- Scenario groups -------------------------------------------------------

std::filesystem::path g_tmp;

void BenchCore(const Corpus &c) {
    const size_t n = c.lines.size();
    const std::string p = c.name + "/";
    mepml::Document doc = mepml::Parse(c.lines);
    Measure(p + "parse", n, [&] { (void)mepml::Parse(c.lines); });
    // The same document through the \import path the editor scan takes.
    const std::string file = (g_tmp / "doc.mepml").string();
    auto read = [](const std::string &path, std::vector<std::string> *out) {
        std::ifstream f(path);
        if (!f) return false;
        std::string l;
        while (std::getline(f, l)) out->push_back(l);
        return true;
    };
    Measure(p + "parse_with_imports", n, [&] { (void)mepml::ParseWithImports(file, c.lines, read); });
    Measure(p + "highlight", n, [&] { (void)mepml::Highlight(doc); });
    // What the editor does on one keystroke: parse + highlight spans.
    Measure(p + "parse_plus_highlight", n, [&] { (void)mepml::Highlight(mepml::Parse(c.lines)); });
    Measure(p + "block_labels", n, [&] { (void)mepml::BlockLabels(doc); });
    Measure(p + "render_toc", n, [&] { (void)mepml::RenderToc(doc, 80); });
    Measure(p + "render_bibliography", n, [&] { (void)mepml::RenderBibliography(doc, 80); });
    Measure(p + "slides", n, [&] { (void)mepml::Slides(doc, static_cast<int>(n)); });
    Measure(p + "export_hidden", n, [&] { (void)mepml::ExportHidden(doc); });
}

void BenchExport(const Corpus &c) {
    const size_t n = c.lines.size();
    const std::string p = c.name + "/";
    const std::string file = (g_tmp / "doc.mepml").string();
    auto read = [](const std::string &, std::vector<std::string> *) { return false; };
    Measure(p + "parse_for_export_html", n,
            [&] { (void)mepml::ParseForExport(file, c.lines, read, {"html"}); });
    const mepml::Document doc = mepml::ParseForExport(file, c.lines, read, {"html"});
    Measure(p + "to_html", n, [&] { (void)mepml::ToHtml(doc); });
    Measure(p + "to_slides_html", n, [&] { (void)mepml::ToSlidesHtml(doc); });
    Measure(p + "to_latex", n, [&] { (void)mepml::ToLatex(doc, g_tmp.string()); });
    Measure(p + "to_markdown", n, [&] { (void)mepml::ToMarkdown(doc); });
    Measure(p + "to_org", n, [&] { (void)mepml::ToOrg(doc); });
    Measure(p + "to_rtf", n, [&] { (void)mepml::ToRtf(doc, g_tmp.string()); });
    Measure(p + "to_text", n, [&] { (void)mepml::ToPlainText(doc); });
    std::string err;
    Measure(p + "write_docx", n, [&] { (void)mepml::WriteDocx(doc, (g_tmp / "o.docx").string(), g_tmp.string(), &err); });
    Measure(p + "write_odt", n, [&] { (void)mepml::WriteOdt(doc, (g_tmp / "o.odt").string(), g_tmp.string(), &err); });
    if (mepml::IsPresentation(doc)) {
        Measure(p + "write_pptx", n, [&] { (void)mepml::WritePptx(doc, (g_tmp / "o.pptx").string(), g_tmp.string(), &err); });
        Measure(p + "to_beamer", n, [&] { (void)mepml::ToBeamer(doc, g_tmp.string(), &err); });
    }
}

void BenchImport(const Corpus &c) {
    const size_t n = c.lines.size();
    const std::string p = c.name + "/";
    const mepml::Document doc = mepml::Parse(c.lines);
    const std::string md = mepml::ToMarkdown(doc), org = mepml::ToOrg(doc), html = mepml::ToHtml(doc);
    const std::string rtf = mepml::ToRtf(doc, g_tmp.string());
    Measure(p + "from_markdown", n, [&] { (void)mepml::FromMarkdown(md); });
    Measure(p + "from_org", n, [&] { (void)mepml::FromOrg(org); });
    Measure(p + "from_html", n, [&] { (void)mepml::FromHtml(html); });
    Measure(p + "from_rtf", n, [&] { (void)mepml::FromRtf(rtf); });
    std::string err;
    const std::string docx = (g_tmp / "rt.docx").string();
    if (mepml::WriteDocx(doc, docx, g_tmp.string(), &err)) {
        std::ifstream f(docx, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Measure(p + "from_docx", n, [&] {
            std::string out, e;
            (void)mepml::FromOffice(bytes, false, (g_tmp / "media").string(), "media", &out, &e);
        });
    }
}

void BenchLsp(const Corpus &c) {
    const size_t n = c.lines.size();
    const std::string p = c.name + "/";
    MepmlLspOptions opts;
    opts.check_files = false;
    const int mid = static_cast<int>(n / 2);
    const int col = static_cast<int>(std::min<size_t>(10, c.lines[static_cast<size_t>(mid)].size()));
    Measure(p + "lsp_diagnostics", n, [&] { (void)MepmlLspDiagnostics(c.lines, opts); });
    Measure(p + "lsp_symbols", n, [&] { (void)MepmlLspSymbols(c.lines); });
    Measure(p + "lsp_folds", n, [&] { (void)MepmlLspFolds(c.lines); });
    Measure(p + "lsp_format", n, [&] { (void)MepmlLspFormat(c.lines); });
    Measure(p + "lsp_completion", n, [&] { (void)MepmlLspCompletions(c.lines, mid, col, opts); });
    Measure(p + "lsp_hover", n, [&] { (void)MepmlLspHover(c.lines, mid, col, opts); });
}

void BenchTreeSitter(const Corpus &c) {
    const size_t n = c.lines.size();
    const std::string text = Join(c.lines);
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_mepml());
    Measure(c.name + "/ts_parse", n, [&] {
        TSTree *tree = ts_parser_parse_string(parser, nullptr, text.c_str(), static_cast<uint32_t>(text.size()));
        ts_tree_delete(tree);
    });
    ts_parser_delete(parser);
}

}  // namespace

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--no-record") == 0) g_record = false;
        else if (std::strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) g_only = argv[++i];
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) g_dump = argv[++i];
    }
    std::error_code ec;
    g_tmp = std::filesystem::temp_directory_path(ec) / "mep-mepml-bench";
    std::filesystem::create_directories(g_tmp / "media", ec);

    std::vector<Corpus> scaling;
    {
        std::ifstream f(MEPML_REFERENCE_FILE);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!text.empty()) scaling.push_back({"test_mepml", SplitLines(text)});
    }
    const std::vector<size_t> sizes = g_quick ? std::vector<size_t>{1000, 4000} : std::vector<size_t>{1000, 4000, 16000};
    for (size_t s : sizes) scaling.push_back({"synth_" + std::to_string(s), Synth(s)});

    // Pathological shapes, each at two sizes so the growth shows.
    const std::vector<Corpus> patho = {
        {"patho_table_500", HugeTable(500)},       {"patho_table_2000", HugeTable(2000)},
        {"patho_notes_500", ManyNotes(500)},       {"patho_notes_2000", ManyNotes(2000)},
        {"patho_headings_500", ManyHeadings(500)}, {"patho_headings_2000", ManyHeadings(2000)},
        {"patho_math_1000", ManyMath(1000)},       {"patho_math_4000", ManyMath(4000)},
        {"patho_usercmd_250", ManyUserCommands(250)}, {"patho_usercmd_1000", ManyUserCommands(1000)},
        {"patho_longline_5k", LongLine(5000)},     {"patho_longline_20k", LongLine(20000)},
        {"patho_nesting_50", DeepNesting(50)},     {"patho_nesting_200", DeepNesting(200)},
        {"patho_unclosed_4000", Unclosed(4000)},   {"patho_slides_100", Slides(100)},
        {"patho_slides_400", Slides(400)},
    };
    if (!g_dump.empty()) {
        std::vector<Corpus> all = scaling;
        for (const Corpus &c : patho) all.push_back(c);
        for (const Corpus &c : all) {
            if (c.name == g_dump) {
                std::fputs(Join(c.lines).c_str(), stdout);
                return 0;
            }
        }
        std::fprintf(stderr, "no corpus named %s\n", g_dump.c_str());
        return 1;
    }
    for (const Corpus &c : scaling) {
        BenchCore(c);
        BenchTreeSitter(c);
        BenchLsp(c);
        BenchExport(c);
        BenchImport(c);
    }

    for (const Corpus &c : patho) {
        BenchCore(c);
        BenchTreeSitter(c);
        BenchLsp(c);
        BenchExport(c);
    }
    std::filesystem::remove_all(g_tmp, ec);
    return 0;
}
