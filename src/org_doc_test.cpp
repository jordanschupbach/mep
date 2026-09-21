// Windowless test for org_doc.cpp's Todo-panel sync helpers
// (OrgTodoListItems / OrgTodoListApply) -- the org side of the
// activity-bar Todo panel's TODO.org backing (editor.cpp's
// ActivityTodoLoad/ActivityTodoSave, kBuiltinActivityBar in main.cpp).
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "org_doc.h"

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

const Lines kSample = {
    "#+TITLE: Project",
    "",
    "* DONE Ship the tab bar",
    " - notes under the done item",
    "** TODO Follow-up from the tab bar",
    "   child body",
    "* Plain section header",
    "  prose that is not a task",
    "* TODO Some test todo",
    "",
};
}  // namespace

int main() {
    // --- Load: every keyworded headline, any level, in document order.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        CHECK(items.size() == 3);
        CHECK(items[0].done && items[0].text == "Ship the tab bar" && items[0].line == 2 && items[0].level == 1);
        CHECK(items[0].keyword == "DONE");
        CHECK(!items[1].done && items[1].text == "Follow-up from the tab bar" && items[1].line == 4 && items[1].level == 2);
        CHECK(!items[2].done && items[2].text == "Some test todo" && items[2].line == 8);
        CHECK(items[2].keyword == "TODO");
    }

    // --- Apply with no changes is the identity.
    {
        Lines out = OrgTodoListApply(kSample, OrgTodoListItems(kSample));
        CHECK(out == kSample);
    }

    // --- Toggling done rewrites only that headline's keyword token.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        items[2].done = true;
        items[0].done = false;
        Lines out = OrgTodoListApply(kSample, items);
        CHECK(out.size() == kSample.size());
        CHECK(out[8] == "* DONE Some test todo");
        CHECK(out[2] == "* TODO Ship the tab bar");
        CHECK(out[3] == kSample[3]);  // body untouched
        CHECK(out[4] == kSample[4]);
        CHECK(out[6] == kSample[6]);
        // Toggling back round-trips exactly.
        std::vector<OrgTodoItem> again = OrgTodoListItems(out);
        again[2].done = false;
        again[0].done = true;
        CHECK(OrgTodoListApply(out, again) == kSample);
    }

    // --- Clear done: an unreferenced headline goes with its whole subtree
    //     (child headline + body), everything else keeps its text.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        std::vector<OrgTodoItem> kept;
        for (const auto &it : items) if (!it.done) kept.push_back(it);
        Lines out = OrgTodoListApply(kSample, kept);
        Lines expect = {"#+TITLE: Project", "", "* Plain section header", "  prose that is not a task",
                        "* TODO Some test todo", ""};
        CHECK(out == expect);
    }

    // --- Removing a child leaves its parent and siblings alone.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        items.erase(items.begin() + 1);
        Lines out = OrgTodoListApply(kSample, items);
        Lines expect = {"#+TITLE: Project", "", "* DONE Ship the tab bar", " - notes under the done item",
                        "* Plain section header", "  prose that is not a task", "* TODO Some test todo", ""};
        CHECK(out == expect);
    }

    // --- New items (line == -1) append as "* TODO text" at the end.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        OrgTodoItem fresh;
        fresh.text = "Write the docs";
        items.push_back(fresh);
        Lines out = OrgTodoListApply(kSample, items);
        CHECK(out.size() == kSample.size() + 1);
        CHECK(out.back() == "* TODO Write the docs");
        std::vector<OrgTodoItem> reloaded = OrgTodoListItems(out);
        CHECK(reloaded.size() == 4 && reloaded[3].text == "Write the docs" && !reloaded[3].done);
        CHECK(reloaded[3].line == static_cast<int>(out.size()) - 1);
    }

    // --- Appending to an empty buffer doesn't leave a stray blank first line;
    //     appending to a missing file (no lines at all) works too.
    {
        OrgTodoItem fresh;
        fresh.text = "First";
        Lines out = OrgTodoListApply(Lines{""}, {fresh});
        CHECK(out == Lines{"* TODO First"});
        Lines out2 = OrgTodoListApply(Lines{}, {fresh});
        CHECK(out2 == Lines{"* TODO First"});
        fresh.done = true;
        Lines out3 = OrgTodoListApply(Lines{}, {fresh});
        CHECK(out3 == Lines{"* DONE First"});
    }

    // --- A custom "#+TODO:" sequence: the first keyword on each side of the
    //     split is what toggling/appending uses; a mid-pipeline keyword
    //     already on the right side is left alone.
    {
        Lines custom = {"#+TODO: NEXT DOING | FINISHED CANCELLED", "* DOING In flight", "* FINISHED Old", "* CANCELLED Nope"};
        std::vector<OrgTodoItem> items = OrgTodoListItems(custom);
        CHECK(items.size() == 3);
        CHECK(!items[0].done && items[1].done && items[2].done);
        // No-op toggle-to-same-side keeps DOING.
        Lines same = OrgTodoListApply(custom, items);
        CHECK(same == custom);
        items[0].done = true;
        items[2].done = false;
        OrgTodoItem fresh;
        fresh.text = "Brand new";
        items.push_back(fresh);
        Lines out = OrgTodoListApply(custom, items);
        CHECK(out[1] == "* FINISHED In flight");
        CHECK(out[2] == "* FINISHED Old");
        CHECK(out[3] == "* NEXT Nope");
        CHECK(out[4] == "* NEXT Brand new");
    }

    // --- Stale references (line no longer a keyworded headline) are ignored,
    //     never re-appended as duplicates; a duplicate reference to the same
    //     line only counts once.
    {
        std::vector<OrgTodoItem> items = OrgTodoListItems(kSample);
        OrgTodoItem stale = items[2];
        stale.line = 6;  // "* Plain section header" -- no keyword
        items.push_back(stale);
        OrgTodoItem out_of_range = items[2];
        out_of_range.line = 999;
        items.push_back(out_of_range);
        OrgTodoItem dup = items[2];
        dup.done = true;  // second reference to the same line: ignored
        items.push_back(dup);
        Lines out = OrgTodoListApply(kSample, items);
        CHECK(out == kSample);
    }

    // --- Priority cookies and tags survive a keyword toggle.
    {
        Lines tagged = {"* TODO [#A] Important thing :work:urgent:"};
        std::vector<OrgTodoItem> items = OrgTodoListItems(tagged);
        CHECK(items.size() == 1 && items[0].text == "Important thing");
        items[0].done = true;
        Lines out = OrgTodoListApply(tagged, items);
        CHECK(out == Lines{"* DONE [#A] Important thing :work:urgent:"});
    }

    // --- Retitle (the sidebar's 'e' key): stars, keyword, priority and
    //     tags survive; a plain headline, a stale line or an empty title
    //     is a no-op.
    {
        Lines tagged = {"#+TODO: TODO DOING | DONE", "* Plain", "** DOING [#B] Old title :work:", "   body"};
        Lines out = OrgTodoListRetitle(tagged, 2, "New title");
        const Lines expect = {"#+TODO: TODO DOING | DONE", "* Plain", "** DOING [#B] New title :work:", "   body"};
        CHECK(out == expect);
        CHECK(OrgTodoListRetitle(tagged, 1, "Nope") == tagged);   // plain headline, no keyword
        CHECK(OrgTodoListRetitle(tagged, 3, "Nope") == tagged);   // body text
        CHECK(OrgTodoListRetitle(tagged, 99, "Nope") == tagged);  // out of range
        CHECK(OrgTodoListRetitle(tagged, 2, "") == tagged);       // empty title
    }

    // --- Archive (the sidebar's 'A' key): the tag goes after any existing
    //     tags, and the whole tagged subtree (headline + body + children)
    //     relocates to the end of the file, sinking below every other
    //     headline; the archived headline and its subtree also vanish from
    //     the checklist, but a save of that checklist leaves them in the
    //     file (they're never "unreferenced" -- see OrgTodoListApply). A
    //     plain headline, a stale line or an already-archived one is a
    //     no-op.
    {
        Lines doc = {"#+TODO: TODO | DONE", "* TODO [#A] Parent :work:", "** TODO Child", "   body",
                     "* DONE Sibling", "* TODO Other"};
        Lines out = OrgTodoListArchive(doc, 1);
        const Lines expect_out = {"#+TODO: TODO | DONE", "* DONE Sibling", "* TODO Other",
                                   "* TODO [#A] Parent :work:ARCHIVE:", "** TODO Child", "   body"};
        CHECK(out == expect_out);  // Parent's subtree moved below Sibling and Other
        std::vector<OrgTodoItem> items = OrgTodoListItems(out);
        CHECK(items.size() == 2);  // Parent (and Child with it) is hidden
        CHECK(items[0].text == "Sibling" && items[0].line == 1);
        CHECK(items[1].text == "Other" && items[1].line == 2);
        // Saving the (archived-free) checklist back must not drop the
        // archived subtree; toggling a visible sibling still works.
        items[1].done = true;
        Lines saved = OrgTodoListApply(out, items);
        CHECK(saved.size() == out.size());
        CHECK(saved[2] == "* DONE Other");
        CHECK(saved[3] == "* TODO [#A] Parent :work:ARCHIVE:");
        CHECK(saved[4] == "** TODO Child");
        CHECK(saved[5] == "   body");
        // Retitling the visible sibling doesn't disturb the archived rows either.
        CHECK(OrgTodoListRetitle(saved, 2, "Renamed")[3] == "* TODO [#A] Parent :work:ARCHIVE:");
        // No-ops.
        CHECK(OrgTodoListArchive(out, 3) == out);    // already archived (Parent, now at line 3)
        CHECK(OrgTodoListArchive(doc, 0) == doc);    // "#+TODO:" line, not a headline
        CHECK(OrgTodoListArchive(doc, 3) == doc);    // body text
        CHECK(OrgTodoListArchive(doc, 99) == doc);   // out of range
        CHECK(OrgTodoListArchive(doc, -1) == doc);   // negative
        Lines plain = {"* Plain header", "* TODO Task"};
        CHECK(OrgTodoListArchive(plain, 0) == plain);  // keywordless headline
        const Lines plain_archived = {"* Plain header", "* TODO Task :ARCHIVE:"};
        CHECK(OrgTodoListArchive(plain, 1) == plain_archived);
        // An inherited tag: a keyworded child under an archived *plain*
        // header is hidden too.
        Lines nested = {"* Old stuff :ARCHIVE:", "** TODO Buried", "* TODO Live"};
        std::vector<OrgTodoItem> vis = OrgTodoListItems(nested);
        CHECK(vis.size() == 1 && vis[0].text == "Live");
        CHECK(OrgTodoListApply(nested, vis) == nested);
    }

    // --- Move (the sidebar's Ctrl-j/Ctrl-k): swaps a headline's whole
    //     subtree with its previous/next sibling; a no-op at either end of
    //     the sibling run, or when a would-be sibling belongs to a
    //     different parent.
    {
        Lines doc = {"#+TODO: TODO | DONE", "* TODO First", "* TODO Second", "* TODO Third",
                     "** TODO Child of Third", "* TODO Fourth"};
        int new_line = -1;
        Lines out = OrgTodoListMove(doc, 2, 1, &new_line);  // Second, down
        const Lines expect = {"#+TODO: TODO | DONE", "* TODO First", "* TODO Third",
                              "** TODO Child of Third", "* TODO Second", "* TODO Fourth"};
        CHECK(out == expect);
        CHECK(new_line == 4);

        new_line = -1;
        CHECK(OrgTodoListMove(doc, 1, -1, &new_line) == doc);  // First, up: no previous sibling
        CHECK(new_line == 1);

        new_line = -1;
        CHECK(OrgTodoListMove(doc, 5, 1, &new_line) == doc);  // Fourth, down: no next sibling
        CHECK(new_line == 5);

        new_line = -1;
        CHECK(OrgTodoListMove(doc, 4, -1, &new_line) == doc);  // Child of Third, up: only child
        CHECK(new_line == 4);

        // Two children: the second moves past the first, staying under
        // the same parent -- the third-party sibling boundary is never
        // crossed.
        Lines two_kids = {"#+TODO: TODO | DONE", "* TODO First", "* TODO Third",
                          "** TODO Child A", "** TODO Child B", "* TODO Fourth"};
        new_line = -1;
        Lines kids_out = OrgTodoListMove(two_kids, 4, -1, &new_line);
        const Lines kids_expect = {"#+TODO: TODO | DONE", "* TODO First", "* TODO Third",
                                   "** TODO Child B", "** TODO Child A", "* TODO Fourth"};
        CHECK(kids_out == kids_expect);
        CHECK(new_line == 3);

        // No-ops: stale/non-headline lines, bad delta.
        CHECK(OrgTodoListMove(doc, 0, 1, &new_line) == doc);    // "#+TODO:" line
        CHECK(OrgTodoListMove(doc, 99, 1, &new_line) == doc);   // out of range
        CHECK(OrgTodoListMove(doc, -1, 1, &new_line) == doc);   // negative
        CHECK(OrgTodoListMove(doc, 2, 0, &new_line) == doc);    // bad delta
    }

    // --- Clock line matching and timestamp parsing.
    {
        std::string ts;
        CHECK(OrgMatchOpenClockLine("  CLOCK: [2026-09-05 Sat 10:00]", &ts) && ts == "2026-09-05 Sat 10:00");
        CHECK(OrgMatchOpenClockLine("CLOCK:[2026-09-05 Sat 10:00]   ", nullptr));
        CHECK(!OrgMatchOpenClockLine("  CLOCK: [2026-09-05 Sat 10:00]--[2026-09-05 Sat 11:30] =>  1:30", nullptr));
        CHECK(!OrgMatchOpenClockLine("  :LOGBOOK:", nullptr));
        CHECK(!OrgMatchOpenClockLine("  CLOCK: 2026-09-05 Sat 10:00", nullptr));
        int y = 0, mo = 0, d = 0, hh = 0, mm = 0;
        CHECK(OrgParseClockTimestamp("2026-09-05 Sat 10:07", &y, &mo, &d, &hh, &mm));
        CHECK(y == 2026 && mo == 9 && d == 5 && hh == 10 && mm == 7);
        CHECK(OrgParseClockTimestamp("[2026-01-31 Samstag 23:59]", &y, &mo, &d, &hh, &mm) && d == 31 && mm == 59);
        CHECK(!OrgParseClockTimestamp("2026-09-05 Sat", &y, &mo, &d, &hh, &mm));
        CHECK(!OrgParseClockTimestamp("2026-09-05 10:07", &y, &mo, &d, &hh, &mm));
    }

    // --- Clock start: a fresh drawer goes after the planning line and
    //     :PROPERTIES: drawer; an existing :LOGBOOK: gets the new entry at
    //     its top; a second start while one is open is refused; a non-
    //     headline line is refused.
    {
        Lines file = {
            "* TODO Task A",
            "  SCHEDULED: <2026-09-06 Sun>",
            "  :PROPERTIES:",
            "  :ID: a",
            "  :END:",
            "  some body text",
            "* TODO Task B",
            "  :LOGBOOK:",
            "  CLOCK: [2026-09-01 Tue 09:00]--[2026-09-01 Tue 09:30] =>  0:30",
            "  :END:",
        };
        CHECK(OrgFindOpenClock(file).line == -1);
        Lines a = OrgClockStartLines(file, 0, "2026-09-05 Sat 10:00");
        CHECK(a.size() == file.size() + 3);
        CHECK(a[5] == "  :LOGBOOK:" && a[6] == "  CLOCK: [2026-09-05 Sat 10:00]" && a[7] == "  :END:");
        CHECK(a[8] == "  some body text");
        OrgOpenClock open = OrgFindOpenClock(a);
        CHECK(open.line == 6 && open.headline_line == 0 && open.start_ts == "2026-09-05 Sat 10:00");
        CHECK(OrgClockStartLines(a, 9, "2026-09-05 Sat 10:01") == a);  // already running

        Lines b = OrgClockStartLines(file, 6, "2026-09-05 Sat 10:00");
        CHECK(b.size() == file.size() + 1);
        CHECK(b[7] == "  :LOGBOOK:" && b[8] == "  CLOCK: [2026-09-05 Sat 10:00]");
        CHECK(b[9] == "  CLOCK: [2026-09-01 Tue 09:00]--[2026-09-01 Tue 09:30] =>  0:30");
        CHECK(OrgFindOpenClock(b).headline_line == 6);

        CHECK(OrgClockStartLines(file, 5, "2026-09-05 Sat 10:00") == file);   // body text, not a headline
        CHECK(OrgClockStartLines(file, 42, "2026-09-05 Sat 10:00") == file);  // out of range

        // A subtree's LOGBOOK belongs to the child, not the parent: the
        // parent gets its own drawer.
        Lines nested = {"* TODO Parent", "** TODO Child", "   :LOGBOOK:", "   :END:"};
        Lines n = OrgClockStartLines(nested, 0, "2026-09-05 Sat 10:00");
        CHECK(n.size() == nested.size() + 3 && n[2] == "  CLOCK: [2026-09-05 Sat 10:00]" && n[4] == "** TODO Child");
    }

    // --- Clock stop: closes the open line with the duration (across
    //     midnight too), reports minutes, and is a no-op when none is open.
    {
        Lines running = {"* TODO Task", "  :LOGBOOK:", "  CLOCK: [2026-09-05 Sat 23:50]", "  :END:"};
        int mins = 0;
        Lines out = OrgClockStopLines(running, "2026-09-06 Sun 01:05", &mins);
        CHECK(mins == 75);
        CHECK(out[2] == "  CLOCK: [2026-09-05 Sat 23:50]--[2026-09-06 Sun 01:05] =>  1:15");
        CHECK(OrgFindOpenClock(out).line == -1);
        int none = 0;
        CHECK(OrgClockStopLines(out, "2026-09-06 Sun 01:06", &none) == out && none == -1);
        // The clock's own end can't precede its start.
        Lines back = OrgClockStopLines(running, "2026-09-05 Sat 23:40", &mins);
        CHECK(mins == 0 && back[2] == "  CLOCK: [2026-09-05 Sat 23:50]--[2026-09-05 Sat 23:40] =>  0:00");
    }

    // --- Headline depth: strict about what a headline is, since DrawPane
    //     draws one at a larger size and on a wider column grid.
    {
        CHECK(OrgHeadlineLevel("* Top") == 1);
        CHECK(OrgHeadlineLevel("*** Third") == 3);
        CHECK(OrgHeadlineLevel("* ") == 1);  // an empty title is still a headline
        CHECK(OrgHeadlineLevel("*bold* opening a line") == 0);   // no space after the stars
        CHECK(OrgHeadlineLevel("**bold** opening a line") == 0);
        CHECK(OrgHeadlineLevel("  * an indented list bullet") == 0);  // stars must be at column 0
        CHECK(OrgHeadlineLevel("*") == 0);   // nothing after the stars at all
        CHECK(OrgHeadlineLevel("") == 0);
        CHECK(OrgHeadlineLevel("plain prose") == 0);
    }

    // --- Link spans: what gets drawn in place of the markup, and what
    //     columns a click has to hit. The edge cases are the point.
    {
        // A described link displays its description; the span covers the
        // whole `[[...]]`, not just the description inside it.
        std::vector<OrgLinkSpanInfo> one = ScanOrgLinkSpans("see [[file:notes.org][Notes]] please");
        CHECK(one.size() == 1);
        CHECK(one[0].col_start == 4 && one[0].col_end == 29);
        CHECK(one[0].target == "file:notes.org");
        CHECK(one[0].display == "Notes");
        CHECK(one[0].bracketed);

        // Undescribed links drop the scheme noise org also drops.
        CHECK(ScanOrgLinkSpans("[[file:a/b.org]]")[0].display == "a/b.org");
        CHECK(ScanOrgLinkSpans("[[id:9f3c]]")[0].display == "9f3c");
        CHECK(ScanOrgLinkSpans("[[*Some heading]]")[0].display == "Some heading");
        CHECK(ScanOrgLinkSpans("[[#custom-id]]")[0].display == "custom-id");
        CHECK(ScanOrgLinkSpans("[[https://example.org]]")[0].display == "https://example.org");

        // Two links on one line, reported in column order.
        std::vector<OrgLinkSpanInfo> two = ScanOrgLinkSpans("[[a][A]] and [[b][B]]");
        CHECK(two.size() == 2);
        CHECK(two[0].col_start == 0 && two[0].display == "A");
        CHECK(two[1].col_start == 13 && two[1].display == "B");

        // A `]` that isn't followed by `[` belongs to the target.
        CHECK(ScanOrgLinkSpans("[[a]b][c]]")[0].target == "a]b");

        // Nothing to follow, nothing to conceal.
        CHECK(ScanOrgLinkSpans("[[]]").empty());
        CHECK(ScanOrgLinkSpans("[[][desc]]").empty());
        CHECK(ScanOrgLinkSpans("[[unterminated").empty());
        CHECK(ScanOrgLinkSpans("no links here at all").empty());
    }

    // --- Bare URLs: recognized in prose, but never reported twice when
    //     one is already inside a bracket link.
    {
        std::vector<OrgLinkSpanInfo> bare = ScanOrgLinkSpans("go to https://example.org/a?b=1 now");
        CHECK(bare.size() == 1);
        CHECK(bare[0].col_start == 6 && bare[0].col_end == 31);
        CHECK(bare[0].target == "https://example.org/a?b=1");
        CHECK(bare[0].display == bare[0].target);
        CHECK(!bare[0].bracketed);

        // The URL inside a bracket link is the bracket link, once.
        std::vector<OrgLinkSpanInfo> inside = ScanOrgLinkSpans("[[https://example.org][Site]]");
        CHECK(inside.size() == 1 && inside[0].bracketed && inside[0].display == "Site");

        // A bracket link and a separate bare URL on the same line: both,
        // in column order.
        std::vector<OrgLinkSpanInfo> mixed = ScanOrgLinkSpans("[[https://a.org][A]] vs https://b.org");
        CHECK(mixed.size() == 2);
        CHECK(mixed[0].bracketed && mixed[0].col_start == 0);
        CHECK(!mixed[1].bracketed && mixed[1].target == "https://b.org");

        CHECK(ScanOrgLinkSpans("http://x.test").size() == 1);
        CHECK(ScanOrgLinkSpans("https://").empty());        // scheme with no body
        CHECK(ScanOrgLinkSpans("httpx://example.org").empty());
        CHECK(ScanOrgLinkSpans("mailto:me@example.org").empty());  // documented: brackets required
        CHECK(ScanOrgLinkSpans("www.example.org").empty());        // ditto
    }

    // --- Verbatim/code runs are literal: a help page showing link syntax
    //     must not end up with a live link in the middle of its prose.
    {
        CHECK(ScanOrgLinkSpans("write =[[file:notes.org]]= to link").empty());
        CHECK(ScanOrgLinkSpans("write ~[[a][B]]~ to link").empty());
        CHECK(ScanOrgLinkSpans("the =https://example.org= scheme").empty());
        // ...but only what's actually inside the run.
        std::vector<OrgLinkSpanInfo> after = ScanOrgLinkSpans("=verbatim= then [[file:x][X]]");
        CHECK(after.size() == 1 && after[0].display == "X");
        // An unterminated marker isn't a run, so the link still counts.
        CHECK(ScanOrgLinkSpans("= [[file:x][X]]").size() == 1);
        // `snake_case=value` doesn't open a run (no word boundary).
        CHECK(ScanOrgLinkSpans("k=v [[file:x][X]]").size() == 1);
        // A marker can only *open* after org's own PRE set, so the `=` in
        // a URL's query string never starts a verbatim run that swallows
        // the link that follows it (OrgEmphasisPreOk, org_doc.h).
        CHECK(ScanOrgLinkSpans("https://a.test/?q=1 [[file:x][X]] =v= ").size() == 2);
    }

    // --- Emphasis marker boundaries: org's own PRE/POST classes, not
    //     "any non-word character". The `/` in `//` is the case that
    //     mattered -- see OrgEmphasisPreOk's own comment (org_doc.h).
    {
        CHECK(OrgEmphasisPreOk('\0'));  // start of line
        CHECK(OrgEmphasisPreOk(' ') && OrgEmphasisPreOk('\t'));
        CHECK(OrgEmphasisPreOk('-') && OrgEmphasisPreOk('(') && OrgEmphasisPreOk('{'));
        CHECK(OrgEmphasisPreOk('\'') && OrgEmphasisPreOk('"'));
        CHECK(!OrgEmphasisPreOk('/'));  // the `https://` case
        CHECK(!OrgEmphasisPreOk(':') && !OrgEmphasisPreOk('$') && !OrgEmphasisPreOk('='));
        CHECK(!OrgEmphasisPreOk('a') && !OrgEmphasisPreOk('7') && !OrgEmphasisPreOk(')'));

        CHECK(OrgEmphasisPostOk('\0'));  // end of line
        CHECK(OrgEmphasisPostOk(' ') && OrgEmphasisPostOk('.') && OrgEmphasisPostOk(','));
        CHECK(OrgEmphasisPostOk(')') && OrgEmphasisPostOk('}') && OrgEmphasisPostOk('['));
        CHECK(!OrgEmphasisPostOk('/') && !OrgEmphasisPostOk('a') && !OrgEmphasisPostOk('('));

        CHECK(OrgEmphasisBorderBlank(' ') && OrgEmphasisBorderBlank('\t'));
        CHECK(!OrgEmphasisBorderBlank('x') && !OrgEmphasisBorderBlank('\0'));
    }

    std::printf("org_doc_test: all checks passed\n");
    return 0;
}
