// Windowless test for the mepml language server's analysis half
// (mepml_lsp.h): diagnostics, completion, hover, definition, references,
// rename, symbols, folds, code actions and formatting, driven directly --
// no process, no JSON-RPC. Then every entry point at every position of
// randomly damaged copies of test.mepml (run it in the Sanitize build).
// CHECK(), never assert(): the Release build strips assert() entirely.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "mepml_lsp.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

MepmlLspOptions NoFiles() {
    MepmlLspOptions o;
    o.check_files = false;
    return o;
}

bool Has(const std::vector<MepmlLspDiagnostic> &ds, const std::string &code, int line = -1) {
    for (const MepmlLspDiagnostic &d : ds)
        if (d.code == code && (line < 0 || d.line == line)) return true;
    return false;
}

// A copy (the lists are temporaries), empty when the label is not offered.
std::optional<MepmlLspCompletionItem> Item(const std::vector<MepmlLspCompletionItem> &items, const std::string &label) {
    for (const MepmlLspCompletionItem &it : items)
        if (it.label == label) return it;
    return std::nullopt;
}

// Applies edits the way mep's client does (latest first).
Lines Apply(Lines lines, std::vector<MepmlLspTextEdit> edits) {
    std::sort(edits.begin(), edits.end(), [](const MepmlLspTextEdit &a, const MepmlLspTextEdit &b) {
        return a.start_line != b.start_line ? a.start_line > b.start_line : a.start_col > b.start_col;
    });
    for (const MepmlLspTextEdit &e : edits) {
        const std::string prefix = lines[static_cast<size_t>(e.start_line)].substr(0, static_cast<size_t>(e.start_col));
        const std::string suffix = lines[static_cast<size_t>(e.end_line)].substr(static_cast<size_t>(e.end_col));
        Lines repl;
        size_t pos = 0;
        while (true) {
            const size_t nl = e.new_text.find('\n', pos);
            repl.push_back(e.new_text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        repl.front() = prefix + repl.front();
        repl.back() += suffix;
        lines.erase(lines.begin() + e.start_line, lines.begin() + e.end_line + 1);
        lines.insert(lines.begin() + e.start_line, repl.begin(), repl.end());
    }
    return lines;
}

// A cursor position: the column of `needle` on `line` (+ offset).
int ColOf(const Lines &lines, int line, const std::string &needle, int offset = 0) {
    const size_t k = lines[static_cast<size_t>(line)].find(needle);
    CHECK(k != std::string::npos);
    return static_cast<int>(k) + offset;
}

const Lines kDoc = {
    "//? Title: Test",                            // 0
    "//? Option: A=1",                            // 1
    "",                                           // 2
    "> Intro",                                    // 3
    "",                                           // 4
    "See \\cite(knuth84) and \\citep(knuth84).",  // 5
    "Jump to [the method|#the-method].",          // 6
    "",                                           // 7
    ">> The Method",                              // 8
    "",                                           // 9
    "```{python, file=\"a.png\"}",                // 10
    "print(1)",                                   // 11
    "```",                                        // 12
    "",                                           // 13
    "| a | bb |",                                 // 14
    "|---|---:|",                                 // 15
    "| 1 | 2 |",                                  // 16
    "",                                           // 17
    "\\citation(knuth84,",                          // 18
    "  author = {Donald E. Knuth},",              // 19
    "  title = {Literate Programming},",          // 20
    "  year = 1984",                              // 21
    ")",                                          // 22
};

void TestDiagnostics() {
    // The clean document is clean.
    const std::vector<MepmlLspDiagnostic> clean = MepmlLspDiagnostics(kDoc, NoFiles());
    for (const MepmlLspDiagnostic &d : clean) std::fprintf(stderr, "unexpected: %d %s %s\n", d.line, d.code.c_str(), d.message.c_str());
    CHECK(clean.empty());

    const Lines bad = {
        "//? Option: A=1",
        "//? Option: A=2",
        "> One",
        ">>> Three",
        "\\cite(nobody) \\color(rde, x) \\fn() [go|#nowhere]",
        "@printbibliography",
        "@imgae{x.png}",
        "| a | b |",
        "| 1 |",
        "\\citation(lonely, author = A, year = 1)",
        "```python",
        "never closed",
    };
    const std::vector<MepmlLspDiagnostic> ds = MepmlLspDiagnostics(bad, NoFiles());
    CHECK(Has(ds, "duplicate-option", 1));
    CHECK(Has(ds, "heading-skip", 3));
    CHECK(Has(ds, "unknown-citation", 4));
    CHECK(Has(ds, "unknown-color", 4));
    CHECK(Has(ds, "empty-footnote", 4));
    CHECK(Has(ds, "unknown-anchor", 4));
    CHECK(Has(ds, "deprecated-directive", 5));
    CHECK(Has(ds, "unknown-directive", 6));
    CHECK(Has(ds, "table-columns", 8));
    CHECK(Has(ds, "unused-citation", 9));
    CHECK(Has(ds, "unclosed-code", 10));
    // Unclosed directives, in either spelling.
    const std::vector<MepmlLspDiagnostic> open = MepmlLspDiagnostics({"\\image(a.png", "@import{b.mepml"}, NoFiles());
    CHECK(Has(open, "unterminated-directive", 0) && Has(open, "unterminated-directive", 1));
    // Ranges are exact: the colour's name, the anchor, the citation's key.
    for (const MepmlLspDiagnostic &d : ds) {
        const std::string text = bad[static_cast<size_t>(d.line)].substr(static_cast<size_t>(d.col_start),
                                                                        static_cast<size_t>(d.col_end - d.col_start));
        if (d.code == "unknown-color") CHECK(text == "rde");
        if (d.code == "unknown-anchor") CHECK(text == "#nowhere");
        if (d.code == "unused-citation") CHECK(text == "lonely");
        if (d.code == "unknown-citation") CHECK(text == "\\cite(nobody)");
    }
    // Sorted by position.
    for (size_t i = 1; i < ds.size(); ++i)
        CHECK(ds[i - 1].line < ds[i].line || (ds[i - 1].line == ds[i].line && ds[i - 1].col_start <= ds[i].col_start));

    // The repo's reference document has no errors.
    std::ifstream f(MEPML_REFERENCE_FILE);
    Lines ref;
    std::string l;
    while (std::getline(f, l)) ref.push_back(l);
    CHECK(!ref.empty());
    for (const MepmlLspDiagnostic &d : MepmlLspDiagnostics(ref, NoFiles())) {
        if (d.severity == MepmlLspSeverity::Error) std::fprintf(stderr, "test.mepml:%d %s\n", d.line + 1, d.message.c_str());
        CHECK(d.severity != MepmlLspSeverity::Error);
    }
}

void TestFiles() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("mep-mepml-lsp-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir / "img");
    std::ofstream(dir / "img" / "here.png") << "x";
    std::ofstream(dir / "refs.mepml") << "\\citation(imp, author = Ada, title = {Notes}, year = 1843)\n";
    MepmlLspOptions opts;
    opts.doc_path = (dir / "doc.mepml").string();
    const Lines doc = {
        "\\import(refs.mepml)",
        "\\image(img/here.png)",
        "@image{img/gone.png}",  // the older spelling
        "As \\cite(imp) says, see [the notes|notes.pdf].",
        "\\image(im",
    };
    const std::vector<MepmlLspDiagnostic> ds = MepmlLspDiagnostics(doc, opts);
    CHECK(!Has(ds, "image-missing", 1));
    CHECK(Has(ds, "image-missing", 2));
    CHECK(!Has(ds, "unknown-citation"));  // defined in the import
    CHECK(Has(ds, "missing-file", 3));
    // Definition of an imported citation: the other file.
    const MepmlLspLocation loc = MepmlLspDefinition(doc, 3, ColOf(doc, 3, "imp"), opts);
    CHECK(loc.found && loc.path.find("refs.mepml") != std::string::npos && loc.line == 0);
    // ...which cannot be renamed from here.
    std::string err;
    CHECK(MepmlLspRename(doc, 3, ColOf(doc, 3, "imp"), "ada", opts, &err).empty());
    CHECK(err.find("refs.mepml") != std::string::npos);
    // \import's path goes to the file.
    const MepmlLspLocation imp = MepmlLspDefinition(doc, 0, 3, opts);
    CHECK(imp.found && imp.path == (dir / "refs.mepml").string());
    // Path completion: `\image(im` offers the directory, then its image.
    const std::vector<MepmlLspCompletionItem> c1 = MepmlLspCompletions(doc, 4, 9, opts);
    const std::optional<MepmlLspCompletionItem> img = Item(c1, "img/");
    CHECK(img && img->insert_text == "img/" && img->replace_start == 7);
    const Lines doc2 = {"\\image(img/he"};
    const std::optional<MepmlLspCompletionItem> here = Item(MepmlLspCompletions(doc2, 0, 13, opts), "here.png");
    CHECK(here && here->insert_text == "here.png" && here->replace_start == 11);
    // Header imports: `//? Import: file` completes, goes to the file, and
    // says in hover what it brings; its options are not "set twice".
    std::ofstream(dir / "opts.mepml") << "//? Option: speed=3\n//? Option: name=\"x\"\n\nIntro text.\n";
    const Lines head = {"//? Import: opts.mepml", "//? Option: speed=5", "//? Import: refs.mepml", "\\import(refs.mepml)", "", "\\cite(imp)", "//? Import: op"};
    const std::vector<MepmlLspDiagnostic> hd = MepmlLspDiagnostics(head, opts);
    CHECK(!Has(hd, "duplicate-option"));
    CHECK(!Has(hd, "unknown-citation"));  // refs.mepml came in through the header
    CHECK(Has(hd, "duplicate-import", 3));
    const MepmlLspLocation to = MepmlLspDefinition(head, 0, 14, opts);
    CHECK(to.found && to.path == (dir / "opts.mepml").string());
    const MepmlLspHoverInfo hv = MepmlLspHover(head, 0, 5, opts);
    CHECK(hv.found && hv.text.find("speed = 3 (overridden here)") != std::string::npos && hv.text.find("1 block") != std::string::npos);
    const std::optional<MepmlLspCompletionItem> op = Item(MepmlLspCompletions(head, 6, 14, opts), "opts.mepml");
    CHECK(op && op->insert_text == "opts.mepml" && op->replace_start == 12);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void TestCompletion() {
    const MepmlLspOptions o = NoFiles();
    auto at = [&](const std::string &line, const std::string &label) {
        Lines doc = kDoc;
        doc.push_back(line);
        return Item(MepmlLspCompletions(doc, static_cast<int>(doc.size()) - 1, static_cast<int>(line.size()), o), label);
    };
    const std::optional<MepmlLspCompletionItem> d = at("\\im", "\\image");
    CHECK(d && d->insert_text == "image(" && d->replace_start == 1);
    CHECK(at("\\to", "\\toc") && at("\\to", "\\toc")->insert_text == "toc");
    CHECK(!at("text \\im", "\\image").has_value());  // a directive starts its line
    // The older `@` spelling completes to the new one, `@` and all.
    const std::optional<MepmlLspCompletionItem> old = at("@im", "\\image");
    CHECK(old && old->insert_text == "\\image(" && old->replace_start == 0);
    const std::optional<MepmlLspCompletionItem> c = at("text \\ci", "\\citep");
    CHECK(c && c->insert_text == "citep(");
    const std::optional<MepmlLspCompletionItem> k = at("x \\cite(kn", "knuth84");
    CHECK(k && k->insert_text == "knuth84" && k->detail.find("Knuth") != std::string::npos);
    CHECK(at("x \\cite{kn", "knuth84").has_value());
    const std::optional<MepmlLspCompletionItem> red = at("x \\color(re", "red");
    CHECK(red && red->insert_text == "red, ");  // on to the text
    CHECK(!at("x \\color(red, re", "red").has_value());  // after the comma is text
    CHECK(!at("x \\fs(12, (a \\color(red, re", "red").has_value());
    CHECK(at("x \\fn(note (a \\ci", "\\cite").has_value());  // a command inside text
    CHECK(at("x \\color{re", "red").has_value());
    CHECK(!at("x \\color{red}{re", "red").has_value());  // the second group is text
    const std::optional<MepmlLspCompletionItem> a = at("see [it|#the-me", "#the-method");
    CHECK(a && a->insert_text == "method");  // only the word being typed is replaced
    CHECK(at("```pyt", "python").has_value());
    CHECK(at("```exec-", "exec-gui").has_value());
    const std::optional<MepmlLspCompletionItem> opt = at("```{r, fi", "file");
    CHECK(opt && opt->insert_text == "file=");
    CHECK(at("```{r, cmd", "cmdline").has_value());  // a babel header argument
    const std::optional<MepmlLspCompletionItem> m = at("//? Ti", "Title");
    CHECK(m && m->insert_text == "Title: ");
    CHECK(at("// WAR", "WARNING").has_value());
    CHECK(!at("// the", "THE").has_value());
    // Nothing inside a code block's body.
    CHECK(MepmlLspCompletions(kDoc, 11, 3, o).empty());
}

void TestHoverAndDefinition() {
    const MepmlLspOptions o = NoFiles();
    MepmlLspHoverInfo h = MepmlLspHover(kDoc, 5, ColOf(kDoc, 5, "knuth84"), o);
    CHECK(h.found && h.text.find("Literate Programming") != std::string::npos && h.text.find("Knuth (1984)") != std::string::npos);
    h = MepmlLspHover(kDoc, 6, ColOf(kDoc, 6, "#the"), o);
    CHECK(h.found && h.text.find("The Method") != std::string::npos);
    h = MepmlLspHover(kDoc, 8, 4, o);
    CHECK(h.found && h.text.find("#the-method") != std::string::npos && h.text.find("Links to it: 1") != std::string::npos);
    h = MepmlLspHover(kDoc, 10, 4, o);
    CHECK(h.found && h.text.find("python") != std::string::npos && h.text.find("file") != std::string::npos);
    CHECK(!MepmlLspHover(kDoc, 11, 2, o).found);  // code is its language's business
    h = MepmlLspHover(kDoc, 18, 3, o);
    CHECK(h.found && h.text.find("Cited 2 times") != std::string::npos);
    const Lines colours = {"a \\color(red, b) \\fn(a note)"};
    CHECK(MepmlLspHover(colours, 0, 5, o).text.find("#e06c75") != std::string::npos);
    CHECK(MepmlLspHover(colours, 0, 20, o).text.find("a note") != std::string::npos);
    // results=exec: a program of any language, run in a terminal.
    const Lines exec_block = {"```{c, results=exec}", "int main(void) { return 0; }", "```"};
    h = MepmlLspHover(exec_block, 0, 2, o);
    CHECK(h.found && h.text.find("in a terminal") != std::string::npos && h.text.find("C-c C-k") != std::string::npos);
    CHECK(MepmlLspHover(kDoc, 10, 4, o).text.find("in a terminal") == std::string::npos);
    // exec-gui: a program's window in the results.
    const Lines gui_block = {"```{exec-gui, rows=12}", "xclock", "```"};
    h = MepmlLspHover(gui_block, 0, 2, o);
    CHECK(h.found && h.text.find("shows its window") != std::string::npos && h.text.find("Ctrl-\\") != std::string::npos);
    const Lines gui_results = {"```{python, results=exec-gui}", "import tkinter", "```"};
    CHECK(MepmlLspHover(gui_results, 0, 2, o).text.find("shows its window") != std::string::npos);

    MepmlLspLocation loc = MepmlLspDefinition(kDoc, 5, ColOf(kDoc, 5, "knuth84"), o);
    CHECK(loc.found && loc.path.empty() && loc.line == 18 && loc.col_start == 10 && loc.col_end == 17);
    loc = MepmlLspDefinition(kDoc, 6, ColOf(kDoc, 6, "#the"), o);
    CHECK(loc.found && loc.line == 8 && loc.col_start == 3);
}

void TestReferencesAndRename() {
    const MepmlLspOptions o = NoFiles();
    MepmlLspReferenceSet refs = MepmlLspReferences(kDoc, 5, ColOf(kDoc, 5, "knuth84"), o);
    CHECK(refs.found && refs.refs.size() == 3);
    CHECK(refs.refs.back().is_definition && refs.refs.back().line == 18);
    std::string err;
    Lines renamed = Apply(kDoc, MepmlLspRename(kDoc, 18, 12, "knuth1984", o, &err));
    CHECK(renamed[5] == "See \\cite(knuth1984) and \\citep(knuth1984).");
    CHECK(renamed[18] == "\\citation(knuth1984,");
    CHECK(MepmlLspDiagnostics(renamed, o).empty());
    CHECK(MepmlLspRename(kDoc, 18, 12, "two words", o, &err).empty() && !err.empty());

    // A heading: its title and every link to its anchor.
    refs = MepmlLspReferences(kDoc, 8, 5, o);
    CHECK(refs.found && refs.is_heading && refs.refs.size() == 2);
    renamed = Apply(kDoc, MepmlLspRename(kDoc, 6, ColOf(kDoc, 6, "#the"), "Our Approach", o, &err));
    CHECK(renamed[8] == ">> Our Approach");
    CHECK(renamed[6] == "Jump to [the method|#our-approach].");
    CHECK(MepmlLspDiagnostics(renamed, o).empty());
    CHECK(!MepmlLspReferences(kDoc, 13, 0, o).found);
}

void TestStructure() {
    const std::vector<MepmlLspSymbol> syms = MepmlLspSymbols(kDoc);
    // Intro, The Method (under Intro), its code block and table, the citation.
    CHECK(syms.size() == 5);
    CHECK(syms[0].name == "Intro" && syms[0].parent == -1 && syms[0].line_end == 22);
    CHECK(syms[1].name == "The Method" && syms[1].parent == 0);
    CHECK(syms[2].kind == MepmlLspSymbolKind::Function && syms[2].parent == 1 && syms[2].sel_line == 10);
    CHECK(syms[3].kind == MepmlLspSymbolKind::Array && syms[3].parent == 1);
    CHECK(syms[4].kind == MepmlLspSymbolKind::Key && syms[4].name == "knuth84");

    const std::vector<MepmlLspFold> folds = MepmlLspFolds(kDoc);
    auto has_fold = [&](int a, int b) {
        for (const MepmlLspFold &f : folds)
            if (f.start_line == a && f.end_line == b) return true;
        return false;
    };
    CHECK(has_fold(0, 1));    // the document header
    CHECK(has_fold(3, 22));   // Intro's section
    CHECK(has_fold(10, 12));  // the code block
    CHECK(has_fold(14, 16));  // the table
    CHECK(has_fold(18, 22));  // the citation
    // A block with results: the code and the results fold separately.
    const Lines with_results = {"```python", "print(1)", "print(2)", "```", "// result_begin:", "// 1", "// 2", "// result_end"};
    const std::vector<MepmlLspFold> rf = MepmlLspFolds(with_results);
    auto has = [&](int a, int b) {
        for (const MepmlLspFold &f : rf)
            if (f.start_line == a && f.end_line == b) return true;
        return false;
    };
    CHECK(has(0, 3));  // the code, fences included
    CHECK(has(4, 7));  // the results
    CHECK(!has(0, 7));
}

void TestActionsAndFormat() {
    const MepmlLspOptions o = NoFiles();
    // Formatting lines a table up, honouring its alignment, and is idempotent.
    const Lines table = {"| name | n |", "|:--|--:|", "| alpha | 1 |", "| b | 22 |"};
    const std::vector<MepmlLspTextEdit> fmt = MepmlLspFormat(table);
    CHECK(fmt.size() == 1);
    const Lines formatted = Apply(table, fmt);
    CHECK(formatted[0] == "| name  |   n |");
    CHECK(formatted[1] == "| :---- | --: |");
    CHECK(formatted[2] == "| alpha |   1 |");
    CHECK(formatted[3] == "| b     |  22 |");
    CHECK(MepmlLspFormat(formatted).empty());
    CHECK(MepmlLspFormat(kDoc).size() == 1);  // kDoc's own table is unaligned

    // An unknown citation: add an entry (which then resolves), or pick the near one.
    Lines doc = kDoc;
    doc[5] = "See \\cite(knuth48).";
    const std::vector<MepmlLspCodeAction> acts = MepmlLspCodeActions(doc, 5, o);
    const MepmlLspCodeAction *add = nullptr, *change = nullptr;
    for (const MepmlLspCodeAction &a : acts) {
        if (a.title.find("Add a \\citation(knuth48)") != std::string::npos) add = &a;
        if (a.title == "Change to \\cite(knuth84)") change = &a;
    }
    CHECK(add && change);
    CHECK(!Has(MepmlLspDiagnostics(Apply(doc, add->edits), o), "unknown-citation"));
    CHECK(Apply(doc, change->edits)[5] == "See \\cite(knuth84).");

    // Misspellings: directive, anchor, colour.
    auto first_fix = [&](const Lines &d, int line) {
        const std::vector<MepmlLspCodeAction> a = MepmlLspCodeActions(d, line, o);
        CHECK(!a.empty());
        return Apply(d, a.front().edits)[static_cast<size_t>(line)];
    };
    CHECK(first_fix({"@imgae{x.png}"}, 0) == "@image{x.png}");
    CHECK(first_fix({"@printbibliography"}, 0) == "\\bibliography");
    CHECK(first_fix({"> The Method", "[m|#the-methd]"}, 1) == "[m|#the-method]");
    CHECK(first_fix({"\\color(gren, x)"}, 0) == "\\color(green, x)");
    CHECK(first_fix({"\\color{gren}{x}"}, 0) == "\\color{green}{x}");
    // The old @abstract{...}: rewritten, its closing brace too.
    const Lines old_abstract = {"@abstract{", "Some {braced} text.", "} // end", "", "After."};
    const std::vector<MepmlLspCodeAction> fixes = MepmlLspCodeActions(old_abstract, 0, o);
    CHECK(!fixes.empty() && fixes.front().fixes == "deprecated-directive");
    const Lines new_abstract = Apply(old_abstract, fixes.front().edits);
    CHECK(new_abstract[0] == "\\abstract(" && new_abstract[1] == "Some {braced} text." && new_abstract[2] == ") // end");
    CHECK(MepmlLspDiagnostics(new_abstract, o).empty());
    // A ragged table: pad it.
    const Lines ragged = {"| a | b |", "| 1 |"};
    const Lines padded = Apply(ragged, MepmlLspCodeActions(ragged, 1, o).front().edits);
    CHECK(padded[1] == "| 1   |     |");
    CHECK(!Has(MepmlLspDiagnostics(padded, o), "table-columns"));
}

// Every entry point at every position of damaged copies of the reference
// document: nothing may crash or read out of bounds.
void TestFuzz() {
    std::ifstream f(MEPML_REFERENCE_FILE);
    Lines ref;
    std::string l;
    while (std::getline(f, l)) ref.push_back(l);
    std::mt19937 rng(42);
    const MepmlLspOptions o = NoFiles();
    const std::string noise = "{}[]|#@\\`$*_~^<>-=+!,:/ \t";
    for (int round = 0; round < 12; ++round) {
        Lines doc = ref;
        for (int k = 0; k < 6; ++k) {
            std::string &line = doc[rng() % doc.size()];
            const size_t at = line.empty() ? 0 : rng() % (line.size() + 1);
            if (rng() % 2 && !line.empty() && at < line.size()) line.erase(at, 1 + rng() % 3);
            else line.insert(at, 1, noise[rng() % noise.size()]);
        }
        MepmlLspDiagnostics(doc, o);
        MepmlLspSymbols(doc);
        MepmlLspFolds(doc);
        const Lines formatted = Apply(doc, MepmlLspFormat(doc));
        CHECK(MepmlLspFormat(formatted).empty());
        for (int line = 0; line < static_cast<int>(doc.size()); line += 1 + static_cast<int>(rng() % 3)) {
            const int len = static_cast<int>(doc[static_cast<size_t>(line)].size());
            for (int col = 0; col <= len; col += 1 + static_cast<int>(rng() % 4)) {
                MepmlLspCompletions(doc, line, col, o);
                MepmlLspHover(doc, line, col, o);
                MepmlLspDefinition(doc, line, col, o);
                MepmlLspReferences(doc, line, col, o);
                std::string err;
                Apply(doc, MepmlLspRename(doc, line, col, "renamed", o, &err));
            }
            for (const MepmlLspCodeAction &a : MepmlLspCodeActions(doc, line, o)) Apply(doc, a.edits);
        }
    }
}
}  // namespace

int main() {
    TestDiagnostics();
    TestFiles();
    TestCompletion();
    TestHoverAndDefinition();
    TestReferencesAndRename();
    TestStructure();
    TestActionsAndFormat();
    TestFuzz();
    std::printf("mepml_lsp_test: all checks passed\n");
    return 0;
}
