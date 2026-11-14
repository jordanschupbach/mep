// Windowless test for apl_lsp.cpp -- the analysis half of mep's own APL
// language server (`mep-apl-lsp`) -- and the formatter it uses. Drives the
// pure functions directly: no process, no JSON-RPC client. CHECK(), never
// assert(): the Release build strips assert().
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "apl_lex.h"
#include "apl_lsp.h"
#include "apl_lsp_vocab.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

struct P {
    int line = -1, col = -1;
};

// The position of the nth occurrence of `needle` (plus `offset` bytes).
P Find(const std::vector<std::string> &lines, const std::string &needle, int nth = 0, int offset = 0) {
    for (size_t li = 0; li < lines.size(); li++) {
        size_t from = 0;
        while (true) {
            const size_t at = lines[li].find(needle, from);
            if (at == std::string::npos) break;
            if (nth-- == 0) return {static_cast<int>(li), static_cast<int>(at) + offset};
            from = at + 1;
        }
    }
    std::fprintf(stderr, "needle not found: %s\n", needle.c_str());
    std::abort();
}

bool Has(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

bool HasCode(const std::vector<AplLspDiagnostic> &ds, const std::string &code, int line = -1) {
    for (const AplLspDiagnostic &d : ds) {
        if (d.code == code && (line < 0 || d.range.line == line)) return true;
    }
    return false;
}

const AplLspCompletionItem *Item(const std::vector<AplLspCompletionItem> &items, const std::string &label) {
    for (const AplLspCompletionItem &it : items) {
        if (it.label == label) return &it;
    }
    return nullptr;
}

// --- a datamunge-like namespace, as the Rosetta APL backend generates -------------------

const char *kBinding =
    ":Namespace datamunge\n"                            // 0
    "⍝ Bindings to the datamunge C++ library.\n"        // 1
    "\n"                                                // 2
    "    ∇ R←A Add B;T\n"                               // 3
    "    ⍝ Adds two arrays elementwise.\n"              // 4
    "    ⍝\n"                                           // 5
    "    ⍝ **Parameters**\n"                            // 6
    "    ⍝ - `A`: left array\n"                         // 7
    "    ⍝ - `B`: right array\n"                        // 8
    "      T←A\n"                                       // 9
    "      R←T+B\n"                                     // 10
    "    ∇\n"                                           // 11
    "\n"                                                // 12
    "    ⍝ The mean of a vector.\n"                     // 13
    "    Mean←{(+/⍵)÷≢⍵}\n"                             // 14
    "\n"                                                // 15
    "    :Namespace Tensor\n"                           // 16
    "        ∇ R←zeros Shape\n"                         // 17
    "        ⍝ A tensor of zeros.\n"                    // 18
    "          R←Shape⍴0\n"                             // 19
    "        ∇\n"                                       // 20
    "    :EndNamespace\n"                               // 21
    "\n"                                                // 22
    "    Version←'0.0.1' ⍝ the library version\n"       // 23
    ":EndNamespace\n";                                  // 24

void TestLexer() {
    using apl::Tok;
    auto t = apl::LexLine("x←¯1.5E¯3 1J¯2 'it''s' ⍝ c");
    CHECK(t.size() == 6);
    CHECK(t[0].kind == Tok::Name && t[1].kind == Tok::Assign);
    CHECK(t[2].kind == Tok::Number && t[2].text == "¯1.5E¯3");
    CHECK(t[3].kind == Tok::Number && t[3].text == "1J¯2");
    CHECK(t[4].kind == Tok::String && t[4].text == "'it''s'");
    CHECK(t[5].kind == Tok::Comment);
    t = apl::LexLine("#.ns.fn ⎕IO ⎕ ##.x a∆b ⍺⍺ ∇∇ ⍬");
    CHECK(t[0].kind == Tok::QualName && t[0].text == "#.ns.fn");
    CHECK(t[1].kind == Tok::SysName && t[1].text == "⎕IO");
    CHECK(t[2].kind == Tok::SysName && t[2].text == "⎕");
    CHECK(t[3].kind == Tok::QualName && t[3].text == "##.x");
    CHECK(t[4].kind == Tok::Name && t[4].text == "a∆b");
    CHECK(t[5].kind == Tok::Special && t[5].text == "⍺⍺");
    CHECK(t[6].kind == Tok::DelDel);
    CHECK(t[7].kind == Tok::Zilde);
    t = apl::LexLine("  :If x ⋄ :EndIf");
    CHECK(t[0].kind == Tok::Keyword && t[3].kind == Tok::Keyword);
    t = apl::LexLine("{⍵:If}");
    CHECK(t[2].kind == Tok::Colon && t[3].kind == Tok::Name);
    t = apl::LexLine(":For i :In ⍳3");
    CHECK(t[2].kind == Tok::Keyword && t[2].text == ":In");
    t = apl::LexLine(")LOAD ws");
    CHECK(t.size() == 1 && t[0].kind == Tok::SysCommand);
    t = apl::LexLine("+.×/1.5 .5");
    CHECK(t[1].kind == Tok::Operator && t[1].text == ".");
    CHECK(t[4].kind == Tok::Number && t[5].kind == Tok::Number && t[5].text == ".5");
    t = apl::LexLine("'open");
    CHECK(t[0].kind == Tok::BadString);
    t = apl::LexLine("\"a\\\"b\" $");
    CHECK(t[0].kind == Tok::String && t[1].kind == Tok::Unknown);
}

void TestVocab() {
    CHECK(apl::FindGlyph("⍴") != nullptr);
    CHECK(std::strcmp(apl::FindGlyph("⍴")->monadic, "Shape") == 0);
    CHECK(std::strcmp(apl::FindGlyph("⍴")->dyadic, "Reshape") == 0);
    CHECK(apl::FindSystemName("⎕na") != nullptr);
    CHECK(apl::FindSystemName("IO") != nullptr);
    CHECK(apl::FindSystemName("⎕FIO") != nullptr);   // GNU
    CHECK(apl::FindSystemName("⎕NGET") != nullptr);  // Dyalog
    CHECK(apl::FindSystemName("⎕NOPE") == nullptr);
    CHECK(apl::GlyphForKey('r') == "⍴");
    CHECK(apl::GlyphForKey('i') == "⍳");
    CHECK(apl::GlyphForKey('w') == "⍵");
    CHECK(apl::GlyphForKey('[') == "←");
    CHECK(apl::FindControlWord(":endif") != nullptr);
    CHECK(apl::FindSystemCommand(")load") != nullptr);
    // Every glyph the lexer knows as a primitive or operator has documentation.
    for (const char *g : {"+", "×", "⍴", "⍳", "⊂", "⊃", "⍋", "⌹", "/", "¨", "⍨", "⍣", ".", "∘", "⍤", "@", "⌸"}) {
        CHECK(apl::FindGlyph(g) != nullptr);
    }
    // No two glyphs share a backtick key.
    bool unique_keys = true;
    for (const apl::Glyph &a : apl::Glyphs()) {
        for (const apl::Glyph &b : apl::Glyphs()) {
            if (&a != &b && a.key[0] && std::strcmp(a.key, b.key) == 0) unique_keys = false;
        }
    }
    CHECK(unique_keys);
}

void TestDefinitions() {
    const auto lines = L(kBinding);
    const auto doc = AplLspAnalyze(lines, "/w/datamunge.apln");
    const auto &defs = AplLspDefinitions(*doc);
    const AplDef *add = nullptr, *mean = nullptr, *zeros = nullptr, *ver = nullptr, *ns = nullptr, *tns = nullptr;
    for (const AplDef &d : defs) {
        if (d.name == "Add") add = &d;
        if (d.name == "Mean") mean = &d;
        if (d.name == "zeros") zeros = &d;
        if (d.name == "Version") ver = &d;
        if (d.name == "datamunge") ns = &d;
        if (d.name == "Tensor") tns = &d;
    }
    CHECK(ns && ns->kind == AplDefKind::Namespace && ns->range.end_line == 24);
    CHECK(ns->doc == "Bindings to the datamunge C++ library.");
    CHECK(add && add->kind == AplDefKind::Tradfn);
    CHECK(add->qualified == "datamunge.Add");
    CHECK(add->result == "R" && add->left == "A" && add->right == "B");
    CHECK(add->locals.size() == 1 && add->locals[0] == "T");
    CHECK(add->header == "R←A Add B;T");
    CHECK(add->doc == "Adds two arrays elementwise.\n\n**Parameters**\n- `A`: left array\n- `B`: right array");
    CHECK(add->range.line == 3 && add->range.end_line == 11);
    CHECK(mean && mean->kind == AplDefKind::Dfn && mean->doc == "The mean of a vector.");
    CHECK(mean->qualified == "datamunge.Mean");
    CHECK(zeros && zeros->qualified == "datamunge.Tensor.zeros" && zeros->doc == "A tensor of zeros.");
    CHECK(tns && tns->qualified == "datamunge.Tensor");
    CHECK(ver && ver->kind == AplDefKind::Variable && ver->doc == "the library version");
    CHECK(AplLspDiagnostics(*doc).empty());
}

void TestHeaders() {
    const auto doc = AplLspAnalyze(L("∇ {R}←{L} Fn Y;a;⎕IO\nR←Y\n∇\n"
                                     "∇ R←(F Twice) Y\nR←F F Y\n∇\n"
                                     "∇ R←(F Over G) Y\nR←F G Y\n∇\n"
                                     "∇ Niladic\n∇\n"
                                     "∇Z←Mon Y\nZ←Y\n∇\n"
                                     "∇ (A B)←Pair Y\nA B←Y\n∇\n"));
    const auto &d = AplLspDefinitions(*doc);
    CHECK(d.size() == 6);
    CHECK(d[0].name == "Fn" && d[0].left == "L" && d[0].left_optional && d[0].shy_result && d[0].locals.size() == 2);
    CHECK(d[1].kind == AplDefKind::TradOp && d[1].name == "Twice" && d[1].left_operand == "F" &&
          d[1].right_operand.empty());
    CHECK(d[2].kind == AplDefKind::TradOp && d[2].name == "Over" && d[2].right_operand == "G");
    CHECK(d[3].name == "Niladic" && d[3].right.empty());
    CHECK(d[4].name == "Mon" && d[4].right == "Y" && d[4].left.empty());
    CHECK(d[5].name == "Pair" && d[5].result == "(A B)");
    CHECK(AplLspDiagnostics(*doc).empty());
    // Malformed headers.
    const auto bad = AplLspAnalyze(L("∇ R←1+2\n∇\n∇ a b c d\n∇"));
    CHECK(HasCode(AplLspDiagnostics(*bad), "syntax-error", 0));
    CHECK(HasCode(AplLspDiagnostics(*bad), "syntax-error", 2));
}

void TestAplf() {
    // A Dyalog .aplf file: the first line is the header, no ∇.
    const auto lines = L(" r←Foo y\n⍝ Doubles y.\n r←2×y\n :If r>3\n   r←3\n :EndIf\n");
    const auto doc = AplLspAnalyze(lines, "/w/Foo.aplf");
    const auto &d = AplLspDefinitions(*doc);
    CHECK(d.size() == 1 && d[0].name == "Foo" && d[0].doc == "Doubles y.");
    CHECK(d[0].range.end_line == 6);
    CHECK(AplLspDiagnostics(*doc).empty());
    // The same text as a script: `r←Foo y` assigns a top-level variable.
    const auto script = AplLspAnalyze(lines, "/w/foo.apl");
    CHECK(AplLspDefinitions(*script).size() == 1);
    CHECK(AplLspDefinitions(*script)[0].kind == AplDefKind::Variable);
}

void TestDiagnostics() {
    // Unbalanced brackets.
    auto doc = AplLspAnalyze(L("x←(1 2\ny←1 2]\nz←{⍵+1\n"));
    auto ds = AplLspDiagnostics(*doc);
    CHECK(HasCode(ds, "unbalanced", 0));
    CHECK(HasCode(ds, "unbalanced", 1));
    CHECK(HasCode(ds, "unbalanced", 2));
    // Mismatched bracket.
    doc = AplLspAnalyze(L("x←(1 2]"));
    ds = AplLspDiagnostics(*doc);
    bool mismatch = false;
    for (const auto &d : ds) mismatch |= Has(d.message, "does not match");
    CHECK(mismatch);
    // Control structures.
    doc = AplLspAnalyze(L("∇ F\n:If 1\n:EndFor\n∇\n∇ G\n:Else\n:For i :In 1\n∇\n:EndIf\n"));
    ds = AplLspDiagnostics(*doc);
    CHECK(HasCode(ds, "control-structure", 2));  // :EndFor closes :If
    CHECK(HasCode(ds, "control-structure", 5));  // :Else outside
    CHECK(HasCode(ds, "control-structure", 6));  // :For not closed before ∇
    CHECK(HasCode(ds, "control-structure", 8));  // :EndIf without opener
    bool named = false;
    for (const auto &d : ds) named |= Has(d.message, "missing :EndFor");
    CHECK(named);
    // :Leave outside a loop, a control word inside a dfn, an unknown control word.
    doc = AplLspAnalyze(L(":Leave\nf←{\n:If ⍵\n}\n:Frobnicate\n"));
    ds = AplLspDiagnostics(*doc);
    CHECK(HasCode(ds, "control-structure", 0));
    CHECK(HasCode(ds, "control-structure", 2));
    CHECK(HasCode(ds, "unknown-keyword", 4));
    // Unclosed tradfn.
    doc = AplLspAnalyze(L("∇ R←F Y\nR←Y\n"));
    CHECK(HasCode(AplLspDiagnostics(*doc), "unbalanced", 0));
    // Unknown ⎕ names; known ones in any case; ⎕ alone.
    doc = AplLspAnalyze(L("⎕IO←0 ⋄ ⎕io ⋄ ⎕←1 ⋄ ⎕FROB 3 ⋄ ⎕NA 'I4 lib|f I4'"));
    ds = AplLspDiagnostics(*doc);
    CHECK(ds.size() == 1 && ds[0].code == "unknown-system-name" && Has(ds[0].message, "⎕FROB"));
    CHECK(ds[0].severity == AplLspSeverity::Warning);
    // Parse errors: unterminated string, a stray character, a missing value, ⍵ and ∇ outside a dfn, a stray colon.
    doc = AplLspAnalyze(L("x←'abc\ny←1 $ 2\nz←\n⍵+1\n1 ∇ 2\nx:3\n"));
    ds = AplLspDiagnostics(*doc);
    CHECK(HasCode(ds, "syntax-error", 0));
    CHECK(HasCode(ds, "syntax-error", 1));
    CHECK(HasCode(ds, "syntax-error", 2));
    CHECK(HasCode(ds, "outside-dfn", 3));
    CHECK(HasCode(ds, "syntax-error", 4));
    // Valid code is quiet: dfns with guards, error guards, labels, dops, diamonds, control structures.
    doc = AplLspAnalyze(L("fact←{⍵≤1:1 ⋄ ⍵×∇ ⍵-1}\n"
                          "safe←{0::'err' ⋄ ⍺⍺ ⍵}\n"
                          "∇ R←Loop N\nR←0\nL1:R←R+1\n→(R<N)/L1\n∇\n"
                          "∇ R←Sel x\n:Select x\n:Case 1 ⋄ R←'one'\n:CaseList 2 3\nR←'few'\n:Else\nR←'many'\n:EndSelect\n∇\n"
                          "∇ T\n:Repeat\n:Trap 11\n:Leave\n:EndTrap\n:Until 1\n:While 0\n:EndWhile\n∇\n"
                          ")LOAD ws\n]box on\n#!/usr/bin/apl --script\n"));
    ds = AplLspDiagnostics(*doc);
    for (const auto &d : ds) std::fprintf(stderr, "unexpected: %s\n", AplLspFormatDiagnostic("x", d).c_str());
    CHECK(ds.empty());
    // Unknown )command.
    doc = AplLspAnalyze(L(")FROB"));
    CHECK(HasCode(AplLspDiagnostics(*doc), "unknown-command", 0));
    CHECK(AplLspFormatDiagnostic("f.apl", AplLspDiagnostics(*doc)[0]) ==
          "f.apl:1:1: warning: unknown system command )FROB [unknown-command]");
}

void TestOutline() {
    const auto doc = AplLspAnalyze(L(kBinding));
    const auto syms = AplLspSymbols(*doc);
    // datamunge > Add, Mean, Tensor > zeros, Version
    CHECK(syms.size() == 6);
    CHECK(syms[0].name == "datamunge" && syms[0].kind == AplLspSymbolKind::Namespace && syms[0].parent == -1);
    CHECK(syms[1].name == "Add" && syms[1].kind == AplLspSymbolKind::Function && syms[1].parent == 0);
    CHECK(syms[2].name == "Mean" && syms[2].parent == 0);
    CHECK(syms[3].name == "Tensor" && syms[3].parent == 0);
    CHECK(syms[4].name == "zeros" && syms[4].parent == 3);
    CHECK(syms[5].name == "Version" && syms[5].kind == AplLspSymbolKind::Variable && syms[5].parent == 0);
    // Operators; a variable assigned twice is listed once.
    const auto ops = AplLspAnalyze(L("twice←{⍺⍺ ⍺⍺ ⍵}\n∇ R←(F Each2) Y\nR←F¨Y\n∇\nx←3\nx←4\n"));
    const auto s2 = AplLspSymbols(*ops);
    CHECK(s2.size() == 3);
    CHECK(s2[0].kind == AplLspSymbolKind::Operator && s2[1].kind == AplLspSymbolKind::Operator);
    CHECK(s2[2].name == "x");
    // Folds: the namespace, the tradfns, the inner namespace, the comment block.
    const auto folds = AplLspFolds(*doc);
    bool ns = false, add = false, comment = false, inner = false;
    for (const auto &f : folds) {
        ns |= f.start_line == 0 && f.end_line == 24;
        add |= f.start_line == 3 && f.end_line == 11;
        comment |= f.start_line == 4 && f.end_line == 8 && f.kind == "comment";
        inner |= f.start_line == 16 && f.end_line == 21;
    }
    CHECK(ns && add && comment && inner);
}

void TestHover() {
    const auto lines = L(kBinding);
    const auto doc = AplLspAnalyze(lines);
    // A glyph: both meanings and how to type it.
    P p = Find(lines, "⍴0");
    AplLspHoverInfo h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "Shape") && Has(h.text, "Reshape") && Has(h.text, "monadic") && Has(h.text, "`r"));
    p = Find(lines, "/⍵");
    h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "Reduce") && Has(h.text, "Replicate"));
    p = Find(lines, "⍵)");
    h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "right argument"));
    // A user function: header and docs as Markdown.
    p = Find(lines, "Add B");
    h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "```apl\nR←A Add B;T\n```") && Has(h.text, "Adds two arrays elementwise."));
    CHECK(Has(h.text, "- `A`: left array"));
    // Locals and arguments: their role in the header.
    p = Find(lines, "T+B");
    h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "local of **Add**"));
    p = Find(lines, "T+B", 0, 2);
    h = AplLspHover(*doc, p.line, p.col);
    CHECK(h.found && Has(h.text, "right argument of **Add**"));
    // ⎕ names, control words, )commands.
    const auto other =
        L("⎕IO←0 ⋄ ⎕NA 'I4 lib|f'\n:If 1\n:EndIf\n)LOAD ws\nr←datamunge.Tensor.zeros 2 3\nm←datamunge.Mean 1 2");
    const auto od = AplLspAnalyze(other);
    h = AplLspHover(*od, 0, 1);
    CHECK(h.found && Has(h.text, "Index origin"));
    p = Find(other, "⎕NA");
    h = AplLspHover(*od, p.line, p.col + 3);
    CHECK(h.found && Has(h.text, "Name association") && Has(h.text, "GNU APL and Dyalog"));
    h = AplLspHover(*od, 1, 1);
    CHECK(h.found && Has(h.text, "control word"));
    h = AplLspHover(*od, 3, 2);
    CHECK(h.found && Has(h.text, "Load a saved workspace"));
    // Cross-file: another file's namespace members, qualified.
    AplLspWorkspace ws;
    ws.docs.push_back(AplLspAnalyze(lines, "/w/datamunge.apln"));
    p = Find(other, "zeros");
    h = AplLspHover(*od, p.line, p.col + 1, &ws);
    CHECK(h.found && Has(h.text, "A tensor of zeros.") && Has(h.text, "datamunge.apln:18"));
    p = Find(other, "Tensor");
    h = AplLspHover(*od, p.line, p.col + 1, &ws);
    CHECK(h.found && Has(h.text, "*namespace*"));
    p = Find(other, "Mean");
    h = AplLspHover(*od, p.line, p.col, &ws);
    CHECK(h.found && Has(h.text, "The mean of a vector."));
    // A dfn documented by ⍝ lines right after `name←{`.
    const auto dl = L("norm←{\n    ⍝ Euclidean norm.\n    ⍝ Uses +.×.\n    (⍵+.×⍵)*0.5\n}\nnorm 3 4");
    const auto dd = AplLspAnalyze(dl);
    h = AplLspHover(*dd, 5, 1);
    CHECK(h.found && Has(h.text, "Euclidean norm.\nUses +.×."));
    // Nothing on a number.
    CHECK(!AplLspHover(*dd, 5, 6).found);
    // --doc: a glyph by name, a ⎕name, a function.
    CHECK(Has(AplLspDocFor("rho", nullptr, nullptr), "Reshape"));
    CHECK(Has(AplLspDocFor("⍳", nullptr, nullptr), "Index Generator"));
    CHECK(Has(AplLspDocFor("⎕NA", nullptr, nullptr), "Name association"));
    CHECK(Has(AplLspDocFor("NA", nullptr, nullptr), "Name association"));
    CHECK(Has(AplLspDocFor("datamunge.Add", nullptr, &ws), "Adds two arrays"));
    CHECK(Has(AplLspDocFor("Add", nullptr, &ws), "Adds two arrays"));
    CHECK(Has(AplLspDocFor(":For", nullptr, nullptr), ":In"));
    CHECK(AplLspDocFor("NoSuchThing", nullptr, &ws).empty());
}

void TestCompletion() {
    // Backtick: the APL keyboard.
    auto lines = L("x←`");
    auto doc = AplLspAnalyze(lines);
    auto items = AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size()));
    CHECK(items.size() > 40);
    CHECK(Item(items, "`r ⍴") && Item(items, "`r ⍴")->insert_text == "⍴");
    lines = L("x←`r");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size()));
    CHECK(items.size() == 1 && items[0].insert_text == "⍴" &&
          items[0].replace_start == static_cast<int>(std::string("x←").size()));
    CHECK(items[0].filter_text == "`r");
    // Glyph names.
    lines = L("x←rh");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size()));
    CHECK(Item(items, "rho ⍴") && Item(items, "rho ⍴")->insert_text == "⍴");
    lines = L("y←iot");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size()));
    CHECK(Item(items, "iota ⍳"));
    // ⎕ names.
    lines = L("⎕n");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size()));
    CHECK(Item(items, "⎕NA") && Item(items, "⎕NGET") && !Item(items, "⎕IO"));
    CHECK(Item(items, "⎕NA")->replace_start == 0);
    // Control words and )commands.
    lines = L("  :En");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, 5);
    CHECK(Item(items, ":EndIf") && Item(items, ":EndFor") && Item(items, ":EndIf")->replace_start == 2);
    lines = L(")lo");
    doc = AplLspAnalyze(lines);
    items = AplLspCompletions(*doc, 0, 3);
    CHECK(Item(items, ")LOAD"));
    // User functions here, locals, and across files with namespace members.
    const auto bl = L(kBinding);
    AplLspWorkspace ws;
    ws.docs.push_back(AplLspAnalyze(bl, "/w/datamunge.apln"));
    lines = L("∇ R←F Width;Wide\nR←Wi\n∇\nr←datamunge.\nr←datamunge.Tensor.ze\nr←datamunge.M\nq←Ad");
    doc = AplLspAnalyze(lines, "/w/use.apl");
    items = AplLspCompletions(*doc, 1, static_cast<int>(lines[1].size()), &ws);
    CHECK(Item(items, "Width") && Item(items, "Wide"));
    items = AplLspCompletions(*doc, 3, static_cast<int>(lines[3].size()), &ws);
    CHECK(Item(items, "Add") && Item(items, "Mean") && Item(items, "Tensor") && Item(items, "Version"));
    CHECK(!Item(items, "zeros"));  // not a direct member
    CHECK(Has(Item(items, "Add")->documentation, "Adds two arrays"));
    items = AplLspCompletions(*doc, 4, static_cast<int>(lines[4].size()), &ws);
    CHECK(items.size() == 1 && items[0].label == "zeros");
    items = AplLspCompletions(*doc, 5, static_cast<int>(lines[5].size()), &ws);
    CHECK(items.size() == 1 && items[0].label == "Mean");
    items = AplLspCompletions(*doc, 6, static_cast<int>(lines[6].size()), &ws);
    CHECK(Item(items, "datamunge.Add") && Item(items, "datamunge.Add")->filter_text == "Add");
    // Nothing inside strings or comments.
    lines = L("x←'rh' ⍝ rh");
    doc = AplLspAnalyze(lines);
    CHECK(AplLspCompletions(*doc, 0, 6).empty());
    CHECK(AplLspCompletions(*doc, 0, static_cast<int>(lines[0].size())).empty());
}

void TestSignature() {
    const auto lines = L(kBinding);
    AplLspWorkspace ws;
    ws.docs.push_back(AplLspAnalyze(lines, "/w/datamunge.apln"));
    const auto use = L("r←1 2 datamunge.Add 3 4\nm←datamunge.Mean 1 2\nr←5 datamunge.Add");
    const auto doc = AplLspAnalyze(use, "/w/use.apl");
    P p = Find(use, "3 4");
    AplLspSignatureHelp sh = AplLspSignatureAt(*doc, p.line, p.col, &ws);
    CHECK(sh.found && sh.label == "R←A Add B" && sh.params.size() == 2 && sh.params[0] == "A" && sh.params[1] == "B");
    CHECK(sh.active_param == 1);
    CHECK(Has(sh.documentation, "Adds two arrays"));
    p = Find(use, "1 2");
    sh = AplLspSignatureAt(*doc, p.line, p.col, &ws);  // in "1 2": the left argument
    CHECK(sh.found && sh.active_param == 0);
    p = Find(use, "1 2", 1);
    sh = AplLspSignatureAt(*doc, p.line, p.col, &ws);
    CHECK(sh.found && sh.label == "Mean ⍵" && Has(sh.documentation, "mean"));
    sh = AplLspSignatureAt(*doc, 2, static_cast<int>(use[2].size()), &ws);
    CHECK(sh.found && sh.active_param == 1);
}

void TestDefinitionAndReferences() {
    const auto lines = L(kBinding);
    AplLspWorkspace ws;
    ws.docs.push_back(AplLspAnalyze(lines, "/w/datamunge.apln"));
    const auto use = L("r←datamunge.Add 3\n∇ R←Loop N\nL1:R←N\n→L1\n∇\nLoop 3");
    const auto doc = AplLspAnalyze(use, "/w/use.apl");
    auto locs = AplLspDefinition(*doc, 0, Find(use, "Add").col, &ws);
    CHECK(locs.size() == 1 && locs[0].path == "/w/datamunge.apln" && locs[0].range.line == 3);
    locs = AplLspDefinition(*doc, 3, Find(use, "L1", 1).col, &ws);  // →L1
    CHECK(locs.size() == 1 && locs[0].path.empty() && locs[0].range.line == 2);
    locs = AplLspDefinition(*doc, 2, Find(use, "N", 1).col, &ws);  // N in L1:R←N: the header
    CHECK(locs.size() == 1 && locs[0].range.line == 1);
    locs = AplLspDefinition(*doc, 5, 1, &ws);
    CHECK(locs.size() == 1 && locs[0].range.line == 1);
    const auto refs = AplLspReferences(*doc, 5, 1);
    CHECK(refs.size() == 2);
    std::string err;
    const auto edits = AplLspRename(*doc, 5, 1, "Spin", &err);
    CHECK(err.empty() && edits.size() == 2);
    AplLspRename(*doc, 5, 1, "1x", &err);
    CHECK(!err.empty());
}

void TestFormatting() {
    std::string err;
    const auto lines = L("∇ R←F Y\n:If Y>0\nR ← 1 ⋄ R+←1⍝note\n:Else\nR←{\n⍝doc\n⍵}0\n:EndIf\n∇");
    const auto edits = AplLspFormat(lines, 100, &err);
    CHECK(err.empty() && edits.size() == 1);
    CHECK(edits[0].new_text ==
          "  :If Y>0\n      R←1 ⋄ R+←1 ⍝ note\n  :Else\n      R←{\n          ⍝ doc\n          ⍵}0\n  :EndIf\n");
    CHECK(edits[0].range.line == 1 && edits[0].range.end_line == 8);
    // Refuses (with a reason) on an unterminated string.
    AplLspFormat(L("x←'open"), 100, &err);
    CHECK(Has(err, "unterminated"));
    err.clear();
    CHECK(AplLspFormat(L("x←1"), 100, &err).empty() && err.empty());
}

}  // namespace

int main() {
    TestLexer();
    TestVocab();
    TestDefinitions();
    TestHeaders();
    TestAplf();
    TestDiagnostics();
    TestOutline();
    TestHover();
    TestCompletion();
    TestSignature();
    TestDefinitionAndReferences();
    TestFormatting();
    std::printf("apl_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
