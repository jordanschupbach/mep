// Windowless test for org_lsp.cpp -- the analysis half of mep's own
// org-mode language server (`mep-org-lsp`, src/org_lsp_server.cpp).
// Drives the pure functions directly: no process, no JSON-RPC client, no
// GL context. CHECK(), never assert(): the Release build strips assert()
// entirely.
//
// The diagnostic assertions are written against OrgLspDiagnostic::code,
// never against message wording, so the messages stay free to improve
// without a test churn. Every case also asserts the *absence* of
// diagnostics where org is legal but looks suspicious -- false positives
// are what makes a linter get switched off, so they are tested as
// deliberately as the true ones.
#include "org_lsp.h"

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

using Lines = std::vector<std::string>;

// Never touches the filesystem: every path-existence check is off, so the
// suite gives the same answers in any working directory.
OrgLspOptions NoFiles() {
    OrgLspOptions opts;
    opts.check_files = false;
    return opts;
}

std::vector<OrgLspDiagnostic> Lint(const Lines &lines) { return OrgLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<OrgLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const OrgLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

/** @brief Reports whether any diagnostic carries a given code. */
bool HasCode(const std::vector<OrgLspDiagnostic> &diags, const std::string &code) { return CountCode(diags, code) > 0; }

/** @brief Returns the first diagnostic carrying a given code, aborting the test if there is none. */
const OrgLspDiagnostic &FindCode(const std::vector<OrgLspDiagnostic> &diags, const std::string &code) {
    for (const OrgLspDiagnostic &d : diags) {
        if (d.code == code) return d;
    }
    Check(false, code.c_str(), __LINE__);
    return diags[0];  // unreachable: Check aborts
}

/** @brief Reports whether a completion list offers an item with exactly this insert text. */
bool Offers(const std::vector<OrgLspCompletionItem> &items, const std::string &insert_text) {
    for (const OrgLspCompletionItem &it : items) {
        if (it.insert_text == insert_text) return true;
    }
    return false;
}

/** @brief Completes at the end of the last line of `lines`. */
std::vector<OrgLspCompletionItem> CompleteAtEnd(const Lines &lines) {
    const int row = static_cast<int>(lines.size()) - 1;
    return OrgLspCompletions(lines, row, static_cast<int>(lines[static_cast<size_t>(row)].size()), NoFiles());
}
}  // namespace

int main() {
    // ================= Diagnostics: block structure ===================

    // --- An unclosed block is reported at the line that opened it, not
    //     at the end of the file where it was discovered.
    {
        const Lines src = {"#+begin_src python", "print(1)", "", "* Next headline"};
        const std::vector<OrgLspDiagnostic> d = Lint(src);
        CHECK(CountCode(d, "unclosed-block") == 1);
        CHECK(FindCode(d, "unclosed-block").line == 0);
        CHECK(FindCode(d, "unclosed-block").severity == OrgLspSeverity::Error);
    }

    // --- A matched pair is silent, and the block's body is not linted as
    //     org (the `[[unclosed` and the impossible date inside it would
    //     both fire outside a literal block).
    {
        const Lines src = {"#+begin_src python", "x = '[[nope'  # <2026-13-45 Xyz>", "#+end_src"};
        CHECK(Lint(src).empty());
    }

    // --- Case is irrelevant to org and so to the matcher.
    {
        const Lines src = {"#+BEGIN_SRC sh", "ls", "#+end_src"};
        CHECK(Lint(src).empty());
    }

    // --- A mismatched closer, and an orphan closer.
    {
        const Lines src = {"#+begin_quote", "hello", "#+end_example"};
        const std::vector<OrgLspDiagnostic> d = Lint(src);
        CHECK(HasCode(d, "mismatched-end"));
        CHECK(FindCode(d, "mismatched-end").line == 2);
    }
    {
        const Lines src = {"text", "#+end_src"};
        CHECK(HasCode(Lint({"text", "#+end_src"}), "unmatched-end"));
        CHECK(FindCode(Lint(src), "unmatched-end").line == 1);
    }

    // --- Nesting a source block inside a quote block is legal org.
    {
        const Lines src = {"#+begin_quote", "#+begin_src sh", "ls", "#+end_src", "#+end_quote"};
        CHECK(Lint(src).empty());
    }

    // --- A `#+end_src` *inside* an example block is literal text, not a
    //     closer -- so the example block is still the one that must end.
    {
        const Lines src = {"#+begin_example", "#+end_src", "#+end_example"};
        CHECK(Lint(src).empty());
    }

    // --- Dynamic blocks (`#+BEGIN:` ... `#+END:`) use their own syntax
    //     and get the same unclosed/orphan treatment.
    {
        CHECK(HasCode(Lint({"#+BEGIN: clocktable :maxlevel 2", "| a |"}), "unclosed-block"));
        CHECK(Lint({"#+BEGIN: clocktable :maxlevel 2", "| a |", "#+END:"}).empty());
        CHECK(HasCode(Lint({"text", "#+END:"}), "unmatched-end"));
    }

    // ================= Diagnostics: drawers ===========================

    {
        const Lines src = {"* Task", ":PROPERTIES:", ":ID: abc", "* Other"};
        const std::vector<OrgLspDiagnostic> d = Lint(src);
        CHECK(CountCode(d, "unclosed-drawer") == 1);
        CHECK(FindCode(d, "unclosed-drawer").line == 1);
    }
    {
        const Lines src = {"* Task", ":PROPERTIES:", ":ID: abc", ":END:"};
        CHECK(Lint(src).empty());
    }
    // A planning line may sit between the headline and its drawer.
    {
        const Lines src = {"* Task", "SCHEDULED: <2026-09-21 Mon>", ":PROPERTIES:", ":ID: abc", ":END:"};
        CHECK(Lint(src).empty());
    }
    // A property drawer that is not directly under its headline is inert.
    {
        const Lines src = {"* Task", "some body text", ":PROPERTIES:", ":ID: abc", ":END:"};
        CHECK(HasCode(Lint(src), "misplaced-properties"));
    }
    // Only `:NAME: value` lines belong in a property drawer.
    {
        const Lines src = {"* Task", ":PROPERTIES:", "just prose", ":END:"};
        const std::vector<OrgLspDiagnostic> d = Lint(src);
        CHECK(HasCode(d, "bad-property"));
        CHECK(FindCode(d, "bad-property").line == 2);
    }
    // A LOGBOOK drawer holds free text and is never shape-checked.
    {
        const Lines src = {"* Task", ":LOGBOOK:", "CLOCK: [2026-09-21 Mon 09:00]--[2026-09-21 Mon 10:30] =>  1:30", ":END:"};
        CHECK(Lint(src).empty());
    }
    {
        CHECK(HasCode(Lint({"* Task", ":END:"}), "unmatched-drawer-end"));
    }

    // ================= Diagnostics: timestamps ========================

    CHECK(Lint({"* Task", "SCHEDULED: <2026-09-21 Mon>"}).empty());
    CHECK(HasCode(Lint({"<2026-13-01 Mon>"}), "bad-date"));
    CHECK(HasCode(Lint({"<2026-02-30 Mon>"}), "bad-date"));
    // 2024 is a leap year, 2026 is not.
    CHECK(!HasCode(Lint({"<2024-02-29 Thu>"}), "bad-date"));
    CHECK(HasCode(Lint({"<2026-02-29 Sun>"}), "bad-date"));
    // 2026-09-21 really is a Monday.
    CHECK(!HasCode(Lint({"<2026-09-21 Mon>"}), "wrong-weekday"));
    CHECK(HasCode(Lint({"<2026-09-21 Fri>"}), "wrong-weekday"));
    // A localized weekday name is left alone rather than guessed at --
    // and not reported as a malformed repeater cookie either, which is
    // how it would otherwise reach the end of the token classifier.
    CHECK(Lint({"<2026-09-21 Mo>"}).empty());
    CHECK(Lint({"<2026-09-21 lun.>"}).empty());
    CHECK(Lint({"<2026-09-21 Mo 09:00 +1w>"}).empty());
    CHECK(HasCode(Lint({"<2026-9-21 Mon>"}), "unpadded-date"));
    CHECK(HasCode(Lint({"<2026-09-21 Mon 25:00>"}), "bad-time"));
    CHECK(HasCode(Lint({"<2026-09-21 Mon 09:0x>"}), "bad-time"));
    // One malformed time is one diagnostic, not two.
    CHECK(CountCode(Lint({"<2026-09-21 Mon 25:00>"}), "bad-time") == 1);
    CHECK(Lint({"<2026-09-21 Mon 09:00-10:30>"}).empty());
    CHECK(Lint({"<2026-09-21 Mon 09:00 +1w -2d>"}).empty());
    CHECK(Lint({"<2026-09-21 Mon .+1m>"}).empty());
    CHECK(HasCode(Lint({"<2026-09-21 Mon frobnicate>"}), "bad-timestamp-part"));
    // Inactive timestamps are checked the same way.
    CHECK(HasCode(Lint({"[2026-02-30 Mon]"}), "bad-date"));
    // The many other uses of brackets and angle brackets are not
    // timestamps and must never be reported on.
    CHECK(Lint({"- [ ] a checkbox", "- [X] done [1/2] [50%]", "<<a radio target>>", "a < b and c > d"}).empty());
    // ... nor is a timestamp shown inside verbatim markup, which org
    // treats as literal text.
    CHECK(Lint({"Write it as =<2026-13-45 Xyz>= in the file."}).empty());

    // ================= Diagnostics: links =============================

    {
        const Lines src = {"* Target headline", "", "See [[*Target headline]] and [[https://example.com][docs]]."};
        CHECK(Lint(src).empty());
    }
    CHECK(HasCode(Lint({"broken [[*No such headline]]"}), "unresolved-link"));
    CHECK(HasCode(Lint({"broken [[#no-such-id]]"}), "unresolved-link"));
    CHECK(HasCode(Lint({"broken [[nothing-named-this]]"}), "unresolved-link"));
    CHECK(HasCode(Lint({"broken [[target"}), "unclosed-link"));
    CHECK(HasCode(Lint({"empty [[]]"}), "empty-link"));
    // `#+NAME:`, `<<targets>>` and `:CUSTOM_ID:` all resolve a fuzzy link.
    {
        const Lines src = {"#+NAME: fig-one", "| a |", "", "<<anchor>>", "", "* H", ":PROPERTIES:", ":CUSTOM_ID: cid", ":END:",
                           "[[fig-one]] [[anchor]] [[#cid]]"};
        CHECK(Lint(src).empty());
    }
    // A macro target expands at export time, so it is never resolved.
    CHECK(!HasCode(Lint({"[[{{{url}}}]]"}), "unresolved-link"));
    // A duplicated `#+NAME:` silently shadows the second element.
    {
        const Lines src = {"#+NAME: t", "| a |", "", "#+NAME: t", "| b |"};
        CHECK(HasCode(Lint(src), "duplicate-name"));
    }

    // ================= Diagnostics: footnotes =========================

    CHECK(Lint({"Body text[fn:1].", "", "[fn:1] The note."}).empty());
    CHECK(HasCode(Lint({"Body text[fn:missing]."}), "undefined-footnote"));
    CHECK(HasCode(Lint({"[fn:1] A note nobody cites."}), "unused-footnote"));
    CHECK(HasCode(Lint({"x[fn:1]", "[fn:1] one", "[fn:1] two"}), "duplicate-footnote"));
    // An inline definition both defines and references its label.
    CHECK(Lint({"Body[fn:label:the definition inline]."}).empty());

    // ================= Diagnostics: keywords ==========================

    CHECK(Lint({"#+TITLE: A document", "#+AUTHOR: Someone", "#+OPTIONS: toc:nil num:2"}).empty());
    {
        const std::vector<OrgLspDiagnostic> d = Lint({"#+TITEL: typo"});
        CHECK(HasCode(d, "unknown-keyword"));
        // A near miss is a warning carrying the suggestion; something
        // unrecognizable is only a hint.
        CHECK(FindCode(d, "unknown-keyword").severity == OrgLspSeverity::Warning);
        CHECK(FindCode(d, "unknown-keyword").message.find("TITLE") != std::string::npos);
        CHECK(FindCode(Lint({"#+ZZQQXX: whatever"}), "unknown-keyword").severity == OrgLspSeverity::Hint);
    }
    // Open-ended backend families are never called unknown.
    CHECK(Lint({"#+ATTR_HTML: :width 400", "#+LATEX_HEADER: \\usepackage{tikz}", "#+HTML: <hr>"}).empty());
    CHECK(HasCode(Lint({"#+TITLE A document"}), "keyword-no-colon"));
    // A plain `#` comment is not a malformed keyword.
    CHECK(Lint({"# just a comment"}).empty());

    // --- `#+TODO:` sequences.
    CHECK(Lint({"#+TODO: TODO NEXT(n) | DONE CANCELED(c@)"}).empty());
    CHECK(HasCode(Lint({"#+TODO:"}), "bad-todo-line"));
    CHECK(HasCode(Lint({"#+TODO: TODO | DONE | EXTRA"}), "bad-todo-line"));
    CHECK(HasCode(Lint({"#+TODO: TO DO(x"}), "bad-todo-line"));

    // --- `#+OPTIONS:` and `#+STARTUP:`.
    CHECK(Lint({"#+OPTIONS: ^:{} \\n:t |:t -:t"}).empty());
    CHECK(HasCode(Lint({"#+OPTIONS: tocnil"}), "bad-option"));
    CHECK(HasCode(Lint({"#+OPTIONS: tock:nil"}), "unknown-option"));
    CHECK(Lint({"#+STARTUP: overview indent inlineimages"}).empty());
    CHECK(HasCode(Lint({"#+STARTUP: overvew"}), "unknown-startup"));

    // ================= Diagnostics: source blocks =====================

    CHECK(Lint({"#+begin_src python :results output", "print(1)", "#+end_src"}).empty());
    CHECK(HasCode(Lint({"#+begin_src", "x", "#+end_src"}), "src-no-language"));
    {
        const std::vector<OrgLspDiagnostic> d = Lint({"#+begin_src pythn", "x", "#+end_src"});
        CHECK(HasCode(d, "unknown-src-language"));
        CHECK(FindCode(d, "unknown-src-language").message.find("python") != std::string::npos);
    }
    // A language babel cannot run but org still knows is not a typo.
    CHECK(!HasCode(Lint({"#+begin_src emacs-lisp", "(+ 1 1)", "#+end_src"}), "unknown-src-language"));
    // maxima moved the other way: it used to be one of those, and now
    // has a real backend of its own.
    CHECK(!HasCode(Lint({"#+begin_src maxima", "integrate(x^2, x);", "#+end_src"}), "unknown-src-language"));
    CHECK(!HasCode(Lint({"#+begin_src gap", "Size(SymmetricGroup(4));", "#+end_src"}), "unknown-src-language"));
    // gap's own header arguments are header arguments -- the linter's list
    // is language-agnostic, so they are accepted the way `:includes` and
    // `:main` already are.
    CHECK(Lint({"#+begin_src gap :screen-width 80 :packages no :memory 2g", "1+1;", "#+end_src"}).empty());
    CHECK(HasCode(Lint({"#+begin_src gap :packages maybe", "1+1;", "#+end_src"}), "unknown-header-value"));
    CHECK(HasCode(Lint({"#+begin_src python :reslts output", "x", "#+end_src"}), "unknown-header-arg"));
    CHECK(HasCode(Lint({"#+begin_src python :results ouput", "x", "#+end_src"}), "unknown-header-value"));
    CHECK(HasCode(Lint({"#+begin_src python :var x", "x", "#+end_src"}), "bad-var"));
    CHECK(Lint({"#+begin_src python :var x=1 :var y=\"a :b\" :exports both", "x", "#+end_src"}).empty());
    CHECK(HasCode(Lint({"#+begin_export", "<hr>", "#+end_export"}), "export-no-backend"));
    // `#+PROPERTY:` and `#+HEADER:` carry the same header arguments.
    CHECK(Lint({"#+PROPERTY: header-args:python :results output"}).empty());
    CHECK(HasCode(Lint({"#+HEADER: :reslts output"}), "unknown-header-arg"));

    // ================= Diagnostics: headlines =========================

    CHECK(Lint({"* One", "** Two", "*** Three"}).empty());
    CHECK(HasCode(Lint({"* One", "*** Three"}), "level-skip"));
    CHECK(HasCode(Lint({"* TODO [#AB] Task"}), "bad-priority"));
    CHECK(Lint({"* TODO [#A] Task :work:urgent:"}).empty());
    CHECK(HasCode(Lint({"* Task :two words:"}), "bad-tags"));
    CHECK(HasCode(Lint({"* Task :work::urgent:"}), "bad-tags"));
    // An ordinary headline that merely ends in a colon is not a tag list.
    CHECK(Lint({"* Consider the following:"}).empty());

    // ================= Diagnostics: planning lines ====================

    CHECK(Lint({"* Task", "DEADLINE: <2026-09-21 Mon>"}).empty());
    CHECK(HasCode(Lint({"* Task", "", "DEADLINE: <2026-09-21 Mon>"}), "misplaced-planning"));
    // Prose that merely starts with the word is left alone.
    CHECK(!HasCode(Lint({"* Task", "", "DEADLINE: sometime next week"}), "misplaced-planning"));

    // ================= Completion =====================================

    // --- `#+` opens the keyword and block vocabulary, matched
    //     case-insensitively and offered back in the case being typed.
    {
        const std::vector<OrgLspCompletionItem> items = CompleteAtEnd({"#+TI"});
        CHECK(Offers(items, "TITLE:"));
        CHECK(!Offers(items, "title:"));
        CHECK(items[0].replace_start == 2 && items[0].replace_end == 4);
    }
    CHECK(Offers(CompleteAtEnd({"#+ti"}), "title:"));
    CHECK(Offers(CompleteAtEnd({"#+beg"}), "begin_src"));
    CHECK(Offers(CompleteAtEnd({"#+BEG"}), "BEGIN_SRC"));

    // --- Inside an open block, the closer that matches it is offered.
    {
        const std::vector<OrgLspCompletionItem> items = CompleteAtEnd({"#+begin_quote", "text", "#+en"});
        CHECK(Offers(items, "end_quote"));
        CHECK(items[0].insert_text == "end_quote");  // ranked first
    }

    // --- Source-block languages, header-argument names and values.
    CHECK(Offers(CompleteAtEnd({"#+begin_src py"}), "python"));
    CHECK(Offers(CompleteAtEnd({"#+begin_src python :res"}), "results"));
    CHECK(Offers(CompleteAtEnd({"#+begin_src python :results ou"}), "output"));
    CHECK(Offers(CompleteAtEnd({"#+begin_src python :exports bo"}), "both"));
    CHECK(Offers(CompleteAtEnd({"#+begin_export ht"}), "html"));

    // --- Export options and startup words.
    CHECK(Offers(CompleteAtEnd({"#+OPTIONS: to"}), "toc:"));
    CHECK(Offers(CompleteAtEnd({"#+STARTUP: ind"}), "indent"));

    // --- Inside a source block nothing org-shaped is offered: mep bridges
    //     that text to a real server for the block's own language.
    CHECK(OrgLspCompletions({"#+begin_src python", "#+ti", "#+end_src"}, 1, 4, NoFiles()).empty());

    // --- Link targets: headlines, names, radio targets, custom ids and
    //     the scheme prefixes.
    {
        const Lines src = {"* Target headline", "#+NAME: fig-one", "| a |", "", "[[fig"};
        CHECK(Offers(CompleteAtEnd(src), "fig-one"));
    }
    {
        const Lines src = {"* Target headline", "", "[[*Tar"};
        CHECK(Offers(CompleteAtEnd(src), "Target headline"));
    }
    CHECK(Offers(CompleteAtEnd({"[[fi"}), "file:"));

    // --- TODO keywords, priorities and tags on a headline.
    {
        const Lines src = {"#+TODO: TODO NEXT | DONE", "", "* NE"};
        CHECK(Offers(CompleteAtEnd(src), "NEXT"));
    }
    {
        const Lines src = {"* Task :work:", "", "* Other :wo"};
        CHECK(Offers(CompleteAtEnd(src), "work:"));
    }

    // --- Property names inside a `:PROPERTIES:` drawer.
    {
        const Lines src = {"* Task", ":PROPERTIES:", ":CUST"};
        CHECK(Offers(CompleteAtEnd(src), "CUSTOM_ID:"));
    }

    // --- Planning keywords directly under a headline.
    CHECK(Offers(CompleteAtEnd({"* Task", "SCHED"}), "SCHEDULED:"));

    // --- Org entities after a backslash.
    CHECK(Offers(CompleteAtEnd({"An \\alph"}), "alpha"));

    // --- Footnote labels after `[fn:`.
    CHECK(Offers(CompleteAtEnd({"[fn:note] A note.", "", "Referenced[fn:no"}), "note"));

    // --- Every candidate replaces exactly the typed word and nothing
    //     wider: mep's client splices over that span and ignores
    //     textEdit, so a range reaching back over the `#+` would
    //     duplicate it.
    {
        const std::vector<OrgLspCompletionItem> items = CompleteAtEnd({"  #+begin_sr"});
        CHECK(!items.empty());
        for (const OrgLspCompletionItem &it : items) {
            CHECK(it.replace_start == 4 && it.replace_end == 12);
            CHECK(it.insert_text.find('\n') == std::string::npos);
            CHECK(it.insert_text.rfind("begin_sr", 0) == 0);
        }
    }

    // ================= Hover ==========================================

    {
        const OrgLspHoverInfo h = OrgLspHover({"#+TITLE: A document"}, 0, 4);
        CHECK(h.found && h.text.find("title") != std::string::npos);
    }
    {
        const OrgLspHoverInfo h = OrgLspHover({"* Task", "SCHEDULED: <2026-09-21 Mon>"}, 1, 15);
        CHECK(h.found && h.text.find("Monday") != std::string::npos);
    }
    {
        const OrgLspHoverInfo h = OrgLspHover({"#+begin_src python :results output", "x", "#+end_src"}, 0, 21);
        CHECK(h.found && h.text.find("results") != std::string::npos);
    }
    {
        // A language tag hovers as what mep will actually run it with.
        const OrgLspHoverInfo h = OrgLspHover({"#+begin_src maxima", "1+1;", "#+end_src"}, 0, 14);
        CHECK(h.found && h.text.find("Runs with maxima") != std::string::npos);
    }
    {
        const OrgLspHoverInfo h = OrgLspHover({"#+begin_src gap", "1+1;", "#+end_src"}, 0, 13);
        CHECK(h.found && h.text.find("Runs with gap") != std::string::npos);
    }
    {
        const OrgLspHoverInfo h = OrgLspHover({"An \\alpha particle"}, 0, 5);
        CHECK(h.found && h.text.find("alpha") != std::string::npos);
    }
    {
        const OrgLspHoverInfo h = OrgLspHover({"* TODO [#A] Task :work:"}, 0, 14);
        CHECK(h.found && h.text.find("TODO") != std::string::npos && h.text.find("work") != std::string::npos);
    }
    // Nothing under the cursor means no hover, not an empty one.
    CHECK(!OrgLspHover({"plain prose"}, 0, 3).found);

    // ================= Document symbols ===============================

    {
        const Lines src = {"* One", "body", "** Two", "*** Three", "* Four"};
        const std::vector<OrgLspSymbol> syms = OrgLspSymbols(src);
        CHECK(syms.size() == 4);
        CHECK(syms[0].name == "One" && syms[0].parent == -1);
        CHECK(syms[1].name == "Two" && syms[1].parent == 0);
        CHECK(syms[2].name == "Three" && syms[2].parent == 1);
        CHECK(syms[3].name == "Four" && syms[3].parent == -1);
        CHECK(syms[0].line_start == 0 && syms[0].line_end == 3);
        // selectionRange points at the title text itself.
        CHECK(src[0].substr(static_cast<size_t>(syms[0].sel_col_start),
                            static_cast<size_t>(syms[0].sel_col_end - syms[0].sel_col_start)) == "One");
    }
    {
        const std::vector<OrgLspSymbol> syms = OrgLspSymbols({"* TODO [#A] Task :work:"});
        CHECK(syms.size() == 1 && syms[0].name == "Task");
        CHECK(syms[0].detail.find("TODO") != std::string::npos);
    }

    // ================= Folding ========================================

    {
        const Lines src = {"* One", "#+begin_src sh", "ls", "#+end_src", "* Two"};
        const std::vector<OrgLspFold> folds = OrgLspFolds(src);
        bool headline = false, block = false;
        for (const OrgLspFold &f : folds) {
            if (f.start_line == 0 && f.end_line == 3) headline = true;
            if (f.start_line == 1 && f.end_line == 3) block = true;
        }
        CHECK(headline && block);
    }

    // ================= Definition =====================================

    {
        const Lines src = {"* Target headline", "", "[[*Target headline]]"};
        const OrgLspLocation loc = OrgLspDefinition(src, 2, 5, NoFiles());
        CHECK(loc.found && loc.path.empty() && loc.line == 0);
    }
    {
        const Lines src = {"#+NAME: fig", "| a |", "", "[[fig]]"};
        const OrgLspLocation loc = OrgLspDefinition(src, 3, 3, NoFiles());
        CHECK(loc.found && loc.line == 0);
    }
    {
        const Lines src = {"* H", ":PROPERTIES:", ":CUSTOM_ID: cid", ":END:", "[[#cid]]"};
        const OrgLspLocation loc = OrgLspDefinition(src, 4, 4, NoFiles());
        CHECK(loc.found && loc.line == 0);
    }
    {
        const Lines src = {"see <<anchor>> here", "[[anchor]]"};
        const OrgLspLocation loc = OrgLspDefinition(src, 1, 4, NoFiles());
        CHECK(loc.found && loc.line == 0 && loc.col == 4);
    }
    // An external link has no definition to jump to.
    CHECK(!OrgLspDefinition({"[[https://example.com]]"}, 0, 5, NoFiles()).found);

    // ================= Robustness =====================================

    // Empty and degenerate documents must not crash or invent findings.
    CHECK(Lint({}).empty());
    CHECK(Lint({""}).empty());
    CHECK(OrgLspCompletions({}, 0, 0, NoFiles()).empty());
    CHECK(OrgLspCompletions({"abc"}, 5, 99, NoFiles()).empty());
    CHECK(!OrgLspHover({"abc"}, -1, 0).found);
    CHECK(!OrgLspDefinition({"abc"}, 0, 99, NoFiles()).found);
    // A column past the end of the line clamps rather than reading past it.
    CHECK(OrgLspCompletions({"#+TI"}, 0, 99, NoFiles()).size() > 0);
    // Truncated markup of every kind on one page: the assertion is
    // simply that this returns (each scanner stops at end of line rather
    // than reading past it) -- what it finds there is not interesting.
    Lint({"#+", "#+begin_", "#+end_", "[[", "[[a][", "[fn:", "<2026-", "<2026-09-", ":", ":X", "*", "**", "\\",
          "#+begin_src", "#+OPTIONS:", "#+TODO: |", "=unclosed", "<<"});
    for (int c = 0; c <= 12; c++) {
        OrgLspCompletions({"#+begin_src py :results"}, 0, c, NoFiles());
        OrgLspHover({"#+begin_src py :results"}, 0, c);
    }

    std::printf("org-lsp tests passed\n");
    return 0;
}
