// Windowless test for org_doc.cpp's Todo-panel sync helpers
// (OrgTodoListItems / OrgTodoListApply) -- the org side of the
// activity-bar Todo panel's TODO.org backing (editor.cpp's
// ActivityTodoLoad/ActivityTodoSave, kBuiltinActivityBar in main.cpp).
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "org_doc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
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
        CHECK((out == Lines{"* TODO First"}));
        Lines out2 = OrgTodoListApply(Lines{}, {fresh});
        CHECK((out2 == Lines{"* TODO First"}));
        fresh.done = true;
        Lines out3 = OrgTodoListApply(Lines{}, {fresh});
        CHECK((out3 == Lines{"* DONE First"}));
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
        CHECK((out == Lines{"* DONE [#A] Important thing :work:urgent:"}));
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

    // --- The leading stars the render hides, and the indent it draws in
    //     their place. Both renderers that hide them (DrawPane's row loop
    //     and its closed-fold summary) rely on the difference being 2 at
    //     every depth: that is how far left a title slides.
    {
        CHECK(OrgHeadlineStarHideLen("* Top") == 2);  // the star and its space
        CHECK(OrgHeadlineStarIndentCols("* Top") == 0);  // level 1 sits at the margin
        CHECK(OrgHeadlineStarHideLen("** Sub") == 3);
        CHECK(OrgHeadlineStarIndentCols("** Sub") == 1);
        CHECK(OrgHeadlineStarHideLen("***** Deep") == 6);
        CHECK(OrgHeadlineStarIndentCols("***** Deep") == 4);
        for (const char *h : {"* a", "** a", "*** a", "**** a", "***** a"}) {
            CHECK(OrgHeadlineStarHideLen(h) - OrgHeadlineStarIndentCols(h) == 2);
        }
        // Only the one separating space is eaten -- the rest of the line,
        // extra spaces included, is drawn exactly as stored.
        CHECK(OrgHeadlineStarHideLen("*   padded") == 2);
        // Everything OrgHeadlineLevel rejects has nothing to hide.
        CHECK(OrgHeadlineStarHideLen("*bold* opening a line") == 0);
        CHECK(OrgHeadlineStarIndentCols("*bold* opening a line") == 0);
        CHECK(OrgHeadlineStarHideLen("  * an indented list bullet") == 0);
        CHECK(OrgHeadlineStarHideLen("plain prose") == 0);
        CHECK(OrgHeadlineStarHideLen("") == 0);
        // An empty-titled headline hides its stars and draws nothing else.
        CHECK(OrgHeadlineStarHideLen("* ") == 2);
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
        CHECK((OrgEmphasisPreOk('-') && OrgEmphasisPreOk('(') && OrgEmphasisPreOk('{')));
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

    // --- Table wrap layout (PlanOrgTableWrap): the display-only
    //     re-budgeting of a table too wide for `:set textwidth`.
    {
        // Codepoint widths, not bytes -- an em dash is one column wide.
        CHECK(OrgTableDisplayWidth("abc") == 3);
        CHECK(OrgTableDisplayWidth("a\xe2\x80\x94" "b") == 3);
        CHECK(OrgTableDisplayWidth("") == 0);

        // Word wrap breaks on spaces and keeps every line inside width.
        std::vector<std::string> wrapped = OrgTableWrapCell("the quick brown fox jumps", 10);
        CHECK(wrapped.size() == 3);
        CHECK(wrapped[0] == "the quick");
        CHECK(wrapped[1] == "brown fox");
        CHECK(wrapped[2] == "jumps");
        for (const std::string &l : wrapped) CHECK(OrgTableDisplayWidth(l) <= 10);

        // A word wider than the column is hard-split rather than left to
        // overflow; a blank cell still yields one (empty) line.
        std::vector<std::string> split = OrgTableWrapCell("aaaaaaaaaaaa", 5);
        CHECK(split.size() == 3);
        CHECK(split[0] == "aaaaa" && split[1] == "aaaaa" && split[2] == "aa");
        CHECK(OrgTableWrapCell("", 8).size() == 1);
        CHECK(OrgTableWrapCell("", 8)[0].empty());
    }
    {
        // A table that already fits is reported unwrapped, and its
        // widths are exactly the natural (align-time) ones.
        std::vector<OrgTableCells> rows;
        OrgTableCells head;
        head.cells.push_back("Key");
        head.cells.push_back("Value");
        rows.push_back(head);
        OrgTableCells sep;
        sep.is_sep = true;
        rows.push_back(sep);
        OrgTableCells body;
        body.cells.push_back("a");
        body.cells.push_back("b");
        rows.push_back(body);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 80, 0);
        CHECK(!plan.wrapped);
        CHECK(plan.col_widths.size() == 2);
        CHECK(plan.col_widths[0] == 3 && plan.col_widths[1] == 5);
        CHECK(plan.rows.size() == 3);
        CHECK(plan.rows[0].size() == 1);
        CHECK(plan.rows[0][0].text == "| Key | Value |");
        CHECK(plan.rows[1][0].text == "|-----+-------|");
        CHECK(plan.rows[2][0].text == "| a   | b     |");
    }
    {
        // A wide prose column: the short column keeps its natural width
        // and the prose column gives up the columns, and every rendered
        // line lands inside the budget.
        std::vector<OrgTableCells> rows;
        OrgTableCells head;
        head.cells.push_back("Feature");
        head.cells.push_back("Notes");
        rows.push_back(head);
        OrgTableCells sep;
        sep.is_sep = true;
        rows.push_back(sep);
        OrgTableCells body;
        body.cells.push_back("wrapping");
        body.cells.push_back(
            "a very long description that runs well past eighty columns in total and has to be "
            "wrapped onto several rendered lines to fit");
        rows.push_back(body);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 80, 0);
        CHECK(plan.wrapped);
        CHECK(plan.col_widths.size() == 2);
        CHECK(plan.col_widths[0] == 8);  // natural width of "wrapping"/"Feature"
        // Total rendered width is exactly the budget: 2 columns cost
        // 3 `|` plus 4 padding spaces of chrome.
        CHECK(plan.col_widths[0] + plan.col_widths[1] + 7 == 80);
        CHECK(plan.rows[2].size() > 1);  // the prose row draws as several lines
        for (const std::vector<OrgTableWrapLine> &row_lines : plan.rows) {
            for (const OrgTableWrapLine &l : row_lines) CHECK(OrgTableDisplayWidth(l.text) == 80);
        }
        // The first line carries the first cell, the continuation lines
        // leave its column blank -- and every line keeps its `|` in the
        // same display columns, which is what lets the grid draw rules.
        CHECK(plan.rows[2][0].text.compare(0, 12, "| wrapping | ") != 0 ||
              plan.rows[2][1].text.compare(0, 12, "|          |") == 0);
        const std::string &first = plan.rows[2][0].text;
        for (const OrgTableWrapLine &l : plan.rows[2]) {
            CHECK(l.text.size() == first.size());
            for (size_t c = 0; c < first.size(); c++) {
                if (first[c] == '|') CHECK(l.text[c] == '|');
            }
        }
    }
    {
        // An indent is spent out of the same budget, so a nested table
        // still renders inside the margin.
        std::vector<OrgTableCells> rows;
        OrgTableCells body;
        body.cells.push_back("some reasonably long cell text here");
        body.cells.push_back("and a second column of prose as well");
        rows.push_back(body);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 40, 4);
        CHECK(plan.wrapped);
        for (const std::vector<OrgTableWrapLine> &row_lines : plan.rows) {
            for (const OrgTableWrapLine &l : row_lines) {
                CHECK(OrgTableDisplayWidth(l.text) == 40);
                CHECK(l.text.compare(0, 4, "    ") == 0);
            }
        }
        // Both columns had to give, and neither fell below the floor.
        CHECK(plan.col_widths[0] >= kOrgTableMinColWidth);
        CHECK(plan.col_widths[1] >= kOrgTableMinColWidth);
    }
    {
        // Ragged rows: a row with fewer cells than the widest one pads
        // out to the full column count rather than drawing short.
        std::vector<OrgTableCells> rows;
        OrgTableCells wide;
        wide.cells.push_back("a");
        wide.cells.push_back("b");
        wide.cells.push_back("c");
        rows.push_back(wide);
        OrgTableCells narrow;
        narrow.cells.push_back("x");
        rows.push_back(narrow);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 80, 0);
        CHECK(plan.col_widths.size() == 3);
        CHECK(plan.rows[1][0].text == "| x |   |   |");
        // An empty table is a no-op rather than a crash.
        CHECK(PlanOrgTableWrap(std::vector<OrgTableCells>(), 80, 0).col_widths.empty());
    }
    {
        // A budget too small to hold even the chrome must still produce
        // renderable lines (never a negative width or an empty row).
        std::vector<OrgTableCells> rows;
        OrgTableCells body;
        body.cells.push_back("hello there");
        body.cells.push_back("world");
        rows.push_back(body);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 4, 0);
        CHECK(plan.col_widths.size() == 2);
        for (int wv : plan.col_widths) CHECK(wv >= 1);
        CHECK(!plan.rows[0].empty());
    }

    // --- Links in a table (OrgTableCellDisplayText + the planner's
    //     carry-through): the cell is measured, wrapped and drawn as the
    //     description it renders to, not as its markup.
    {
        std::vector<OrgTableCellLink> links;
        CHECK(OrgTableCellDisplayText("[[file:docs/lua-api.org][Lua API]]", true, &links) == "Lua API");
        CHECK(links.size() == 1);
        CHECK(links[0].start == 0 && links[0].end == 7);
        CHECK(links[0].target == "file:docs/lua-api.org");
        CHECK(links[0].concealed);  // a description: Blue face, no underline
        // With concealment off the markup is what draws, so it is what
        // the columns have to be budgeted for -- but it is still a link.
        CHECK(OrgTableCellDisplayText("[[file:docs/lua-api.org][Lua API]]", false, &links) ==
              "[[file:docs/lua-api.org][Lua API]]");
        CHECK(links.size() == 1 && links[0].start == 0 && links[0].end == 34);
        CHECK(!links[0].concealed);  // raw markup drawn: underlined in place
        // A bare URL is its own display text either way, and prose
        // around a link survives intact.
        CHECK(OrgTableCellDisplayText("see https://example.org/x now", true, &links) ==
              "see https://example.org/x now");
        CHECK(links.size() == 1 && links[0].start == 4 && links[0].end == 25);
        CHECK(!links[0].concealed);
        CHECK(OrgTableCellDisplayText("plain text", true, &links) == "plain text");
        CHECK(links.empty());
        CHECK(OrgTableCellDisplayText("a [[x][one]] b [[y][two]] c", true, &links) == "a one b two c");
        CHECK(links.size() == 2);
        CHECK(links[0].start == 2 && links[0].end == 5 && links[0].target == "x");
        CHECK(links[1].start == 8 && links[1].end == 11 && links[1].target == "y");
    }
    {
        // The bug this section exists for: a table of link cells whose
        // *descriptions* fit inside the budget must not wrap at all. The
        // same rows measured as raw markup ran to 109 columns and got
        // re-budgeted, hard-splitting the URL inside the markup.
        std::vector<OrgTableCells> rows;
        const char *targets[] = {"file:docs/keybindings.org", "file:docs/lua-api.org"};
        const char *descs[] = {"Keybindings and commands", "Lua API"};
        const char *notes[] = {"every key and =:= command", "the =mep.*= table"};
        for (size_t i = 0; i < 2; i++) {
            OrgTableCells body;
            body.links.resize(2);
            body.cells.push_back(OrgTableCellDisplayText(
                std::string("[[") + targets[i] + "][" + descs[i] + "]]", true, &body.links[0]));
            body.cells.push_back(OrgTableCellDisplayText(notes[i], true, &body.links[1]));
            rows.push_back(std::move(body));
        }
        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 80, 0);
        CHECK(!plan.wrapped);
        CHECK(plan.col_widths[0] == 24);  // "Keybindings and commands", not the 55-column markup
        CHECK(plan.rows[0].size() == 1);
        CHECK(plan.rows[0][0].text == "| Keybindings and commands | every key and =:= command |");
        // The link comes back in the *rendered* line's columns, as one
        // run over the whole description rather than one span per word.
        CHECK(plan.rows[0][0].links.size() == 1);
        CHECK(plan.rows[0][0].links[0].col_start == 2);
        CHECK(plan.rows[0][0].links[0].col_end == 26);
        CHECK(plan.rows[0][0].links[0].target == "file:docs/keybindings.org");
        CHECK(plan.rows[0][0].text.compare(2, 24, "Keybindings and commands") == 0);
        // `wrapped` is false here, but the layout is still complete and
        // still narrower than the stored rows -- which is the signal
        // OrgTableWrapScan draws it by, to close the gutter the file's
        // markup-width padding leaves in the link column.
        const std::string stored =
            "| [[file:docs/keybindings.org][Keybindings and commands]] | every key and =:= command |";
        CHECK(OrgTableDisplayWidth(plan.rows[0][0].text) == 56);
        CHECK(OrgTableDisplayWidth(plan.rows[0][0].text) < OrgTableDisplayWidth(stored));
    }
    {
        // The other side of that signal: an aligned table with no links
        // lays out to exactly what is already stored, so
        // OrgTableWrapScan leaves it drawing its own text and nothing
        // about a link-free table changes.
        std::vector<OrgTableCells> rows;
        OrgTableCells head;
        head.cells.push_back("Key");
        head.cells.push_back("Value");
        rows.push_back(head);
        OrgTableCells body;
        body.cells.push_back("a");
        body.cells.push_back("bb");
        rows.push_back(body);

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 80, 0);
        CHECK(!plan.wrapped);
        CHECK(plan.rows[0][0].text == "| Key | Value |");
        CHECK(OrgTableDisplayWidth(plan.rows[0][0].text) == OrgTableDisplayWidth("| Key | Value |"));
        for (const std::vector<OrgTableWrapLine> &row_lines : plan.rows) {
            for (const OrgTableWrapLine &l : row_lines) CHECK(l.links.empty());
        }
    }
    {
        // A description that has to wrap: each line reports only the
        // part of the link that landed on it, and both parts point at
        // the same target.
        std::vector<OrgTableCells> rows;
        OrgTableCells body;
        body.links.resize(2);
        body.cells.push_back(OrgTableCellDisplayText("[[file:a.org][alpha beta gamma delta]]", true, &body.links[0]));
        body.cells.push_back(OrgTableCellDisplayText(
            "a second column of prose long enough to force the first one to give up columns", true, &body.links[1]));
        rows.push_back(std::move(body));

        OrgTableWrapPlan plan = PlanOrgTableWrap(rows, 40, 0);
        CHECK(plan.wrapped);
        CHECK(plan.rows[0].size() > 1);
        int link_lines = 0;
        for (const OrgTableWrapLine &l : plan.rows[0]) {
            for (const OrgTableWrapLink &lk : l.links) {
                CHECK(lk.target == "file:a.org");
                CHECK(lk.col_start >= 0 && lk.col_end <= static_cast<int>(l.text.size()));
                CHECK(lk.col_end > lk.col_start);
                // Every column the span claims is text the link put
                // there, never the cell's padding or a `|`.
                for (int c = lk.col_start; c < lk.col_end; c++) CHECK(l.text[static_cast<size_t>(c)] != '|');
            }
            if (!l.links.empty()) link_lines++;
        }
        CHECK(link_lines > 1);  // the description really did span lines
    }

    // --- Org inline images: the drawn figure's geometry (OrgImageLayoutFor).
    {
        // The default metrics of the built-in font: a 22px line height
        // with a ~0.52 advance ratio, and a pane wider than org's own
        // 80-column text width.
        const float cw = 11.44f, lh = 22.0f;
        const float text_w = 80.0f * cw;
        const float target_w = 80.0f * kOrgImageWidthFraction * cw;

        // A figure wider than the target is scaled down to exactly it,
        // and centered in the 80-column text column -- so the gaps on
        // either side are equal, and the one on the left is real.
        OrgImageLayout wide = OrgImageLayoutFor(1600, 900, cw, lh, 120, 80);
        CHECK(wide.width > target_w - 0.5f && wide.width < target_w + 0.5f);
        CHECK(wide.height > 0.0f);
        // Aspect preserved.
        CHECK(wide.height > wide.width * 900.0f / 1600.0f - 0.5f);
        CHECK(wide.height < wide.width * 900.0f / 1600.0f + 0.5f);
        const float expect_off = (text_w - wide.width) * 0.5f;
        CHECK(wide.offset_x > expect_off - 0.5f && wide.offset_x < expect_off + 0.5f);
        // No dead space: the reserved band is the drawn height rounded
        // up to whole line-heights, never more.
        CHECK(static_cast<float>(wide.slots) * lh >= wide.height);
        CHECK(static_cast<float>(wide.slots - 1) * lh < wide.height);
    }
    {
        const float cw = 11.44f, lh = 22.0f;
        // A figure already narrower than the target keeps its own size
        // rather than being stretched up to fill the column.
        OrgImageLayout small = OrgImageLayoutFor(64, 64, cw, lh, 120, 80);
        CHECK(small.width > 63.5f && small.width < 64.5f);
        CHECK(small.height > 63.5f && small.height < 64.5f);
        CHECK(small.slots == 3);  // ceil(64 / 22)
        CHECK(small.offset_x > 0.0f);
    }
    {
        const float cw = 11.44f, lh = 22.0f;
        // A very tall portrait shrinks to the height ceiling instead of
        // claiming screenfuls -- and still reserves exactly what it draws.
        OrgImageLayout tall = OrgImageLayoutFor(100, 100000, cw, lh, 120, 80);
        CHECK(tall.slots == kOrgImageMaxSlots);
        CHECK(static_cast<float>(tall.slots) * lh >= tall.height);
        CHECK(tall.width < 100.0f);  // scaled down together with the height
    }
    {
        const float cw = 11.44f, lh = 22.0f;
        // A pane narrower than org's text width clamps both the target
        // width and the column the figure is centered in, so nothing
        // overflows and the offset stays non-negative.
        OrgImageLayout narrow = OrgImageLayoutFor(1600, 900, cw, lh, 20, 80);
        CHECK(narrow.width <= 20.0f * cw + 0.5f);
        CHECK(narrow.offset_x >= 0.0f);
        CHECK(narrow.offset_x + narrow.width <= 20.0f * cw + 0.5f);
    }
    {
        const float cw = 11.44f, lh = 22.0f;
        // Unknown dimensions (a header that couldn't be sniffed) fall
        // back to a fixed, modest band rather than 0 or a screenful.
        OrgImageLayout unknown = OrgImageLayoutFor(0, 0, cw, lh, 120, 80);
        CHECK(unknown.slots == kOrgImageUnknownSlots);
        // `:set textwidth` is what the figure is measured against, and
        // `textwidth=0` falls back to org's conventional 80.
        OrgImageLayout tw60 = OrgImageLayoutFor(1600, 900, cw, lh, 200, 60);
        CHECK(tw60.width < OrgImageLayoutFor(1600, 900, cw, lh, 200, 80).width);
        OrgImageLayout tw0 = OrgImageLayoutFor(1600, 900, cw, lh, 200, 0);
        CHECK(tw0.width > OrgImageLayoutFor(1600, 900, cw, lh, 200, 80).width - 0.5f);
        CHECK(tw0.width < OrgImageLayoutFor(1600, 900, cw, lh, 200, 80).width + 0.5f);
        // Degenerate metrics must not divide by zero or go negative.
        OrgImageLayout degenerate = OrgImageLayoutFor(100, 100, 0.0f, 0.0f, 0, 0);
        CHECK(degenerate.slots >= 1);
        CHECK(degenerate.width > 0.0f);
        CHECK(degenerate.offset_x >= 0.0f);
    }


    // --- Per-src-block LSP status line (<leader>ots): the wording and
    // the severity ranking DrawPane draws along a src card's bottom edge,
    // from the facts Lua's mep.org_lsp_status_scan reports for the block.
    {
        // Nothing will ever attach: say why, and name the language, since
        // the card's title bar is the only other place it appears.
        OrgLspStatus none;
        none.lang = "sh";
        CHECK(FormatOrgLspStatus(none) == "LSP: no server for sh");
        CHECK(OrgLspStatusToneOf(none) == OrgLspStatusTone::kMuted);
        // A `#+begin_src` with no language tag at all.
        OrgLspStatus bare;
        CHECK(FormatOrgLspStatus(bare) == "LSP: no language set");
        // A server that resolved but a state that says otherwise is still
        // an "unsupported" line -- the two facts can't disagree.
        OrgLspStatus stale;
        stale.state = OrgLspState::kReady;
        stale.lang = "python";
        CHECK(FormatOrgLspStatus(stale) == "LSP: no server for python");
    }
    {
        OrgLspStatus st;
        st.lang = "python";
        st.server = "pyright";
        st.state = OrgLspState::kIdle;
        // Idle is the resting state of a block no LSP feature has run
        // in yet -- not a fault, and the line says what would attach it.
        CHECK(FormatOrgLspStatus(st) == "pyright: idle (attaches on first use)");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kMuted);
        st.state = OrgLspState::kStarting;
        CHECK(FormatOrgLspStatus(st) == "pyright: starting...");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kMuted);
        // A client that was started and is gone is a real fault.
        st.state = OrgLspState::kExited;
        CHECK(FormatOrgLspStatus(st) == "pyright: not running");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kWarn);
    }
    {
        OrgLspStatus st;
        st.lang = "python";
        st.server = "pyright";
        st.state = OrgLspState::kReady;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, no diagnostics");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kOk);
        // Singular/plural, and only the categories that actually have
        // findings -- a clean category must not print "0 warnings".
        st.errors = 1;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, 1 error");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kError);
        st.errors = 2;
        st.warnings = 1;
        st.hints = 3;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, 2 errors, 1 warning, 3 hints");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kError);
        // Warnings alone rank below errors but above a healthy line.
        st.errors = 0;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, 1 warning, 3 hints");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kWarn);
        // Hints alone never raise the tone: they are information, not a
        // problem, and a page of blocks shouldn't glow for them.
        st.warnings = 0;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, 3 hints");
        CHECK(OrgLspStatusToneOf(st) == OrgLspStatusTone::kOk);
        st.hints = 1;
        CHECK(FormatOrgLspStatus(st) == "pyright: ready, 1 hint");
    }
    {
        // The Lua bridge's state names, and the fallback for anything
        // this side doesn't know -- an unrecognized name must degrade to
        // "nothing is attached here", never to a confident "ready".
        CHECK(OrgLspStateFromName("idle") == OrgLspState::kIdle);
        CHECK(OrgLspStateFromName("starting") == OrgLspState::kStarting);
        CHECK(OrgLspStateFromName("ready") == OrgLspState::kReady);
        CHECK(OrgLspStateFromName("exited") == OrgLspState::kExited);
        CHECK(OrgLspStateFromName("unsupported") == OrgLspState::kUnsupported);
        CHECK(OrgLspStateFromName("") == OrgLspState::kUnsupported);
        CHECK(OrgLspStateFromName("Ready") == OrgLspState::kUnsupported);
    }
    {
        // The card play button (DrawPane's own control): what it decides,
        // and the wording of the tooltip that explains it.
        OrgBlockPlayInput in;
        in.is_src = true;
        in.closed = true;
        in.lang = "python";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kReady);
        CHECK(OrgBlockPlayHint(in) == "Run this python block (C-c C-c)");
        // A block still being typed has no body to run yet.
        in.closed = false;
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kHidden);
        CHECK(OrgBlockPlayHint(in).empty());
        // Nothing but src runs -- `quote`/`example`/`export` get a card,
        // not a button.
        in.closed = true;
        in.is_src = false;
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kHidden);
        in.is_src = true;
        // No language tag: mep_org_babel_resolve_lang has nothing to look
        // up, so the button must not offer a run it can't make.
        in.lang = "";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kDisabled);
        CHECK(OrgBlockPlayHint(in) == "No language on this block -- nothing to run");
        // The `:eval` gate, exactly as mep.org_babel_execute reads it:
        // `no`/`never` refuse, and every other value (`no-export`,
        // `query`, ...) still runs on an explicit request.
        in.lang = "sh";
        in.eval_arg = "no";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kDisabled);
        CHECK(OrgBlockPlayHint(in) == "Blocked by :eval no");
        in.eval_arg = "never";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kDisabled);
        CHECK(OrgBlockPlayHint(in) == "Blocked by :eval never");
        in.eval_arg = "no-export";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kReady);
        in.eval_arg = "query";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kReady);
        // Deliberately literal: the execution path matches these two
        // words verbatim, so a case-folding button here would claim a
        // block is blocked while C-c C-c happily ran it.
        in.eval_arg = "NO";
        CHECK(OrgBlockPlayFor(in) == OrgBlockPlay::kReady);
    }

    {
        // --- Block settings: which kinds get a gear at all. `src` takes
        // the whole header-arg vocabulary, `example` takes the same line
        // switches, and nothing else has options to set.
        CHECK(OrgBlockHasSettings("src"));
        CHECK(OrgBlockHasSettings("example"));
        CHECK(!OrgBlockHasSettings("quote"));
        CHECK(!OrgBlockHasSettings("export"));
        CHECK(!OrgBlockHasSettings(""));
        CHECK(OrgHeaderArgSpecsFor("quote", "").empty());
        // An example block's whole list is the four switches.
        const std::vector<OrgHeaderArgSpec> ex = OrgHeaderArgSpecsFor("example", "");
        CHECK(ex.size() == 4);
        CHECK(ex[0].key == "-n");
        CHECK(ex[0].kind == OrgHeaderArgKind::kSwitch);
        CHECK(ex[0].section == "Display");
        CHECK(ex[1].section.empty());  // continues the same section
    }
    {
        // The catalog is common-args-then-language: the same core list
        // for every src block, plus only the family's own options.
        /**
         * @brief Finds one option's spec in a catalog.
         * @param specs the catalog to search
         * @param key the header-arg key (or switch token) wanted
         * @return a pointer to the first spec with that key, or nullptr
         */
        auto find = [](const std::vector<OrgHeaderArgSpec> &specs, const std::string &key) -> const OrgHeaderArgSpec * {
            for (const OrgHeaderArgSpec &spec : specs) {
                if (spec.key == key) return &spec;
            }
            return nullptr;
        };
        const std::vector<OrgHeaderArgSpec> py = OrgHeaderArgSpecsFor("src", "python");
        CHECK(find(py, "results") != nullptr);
        CHECK(find(py, "exports") != nullptr);
        CHECK(find(py, "tangle") != nullptr);
        CHECK(find(py, "return") != nullptr);   // ob-python's own
        CHECK(find(py, "width") == nullptr);    // R's, not python's
        CHECK(find(py, "-n") != nullptr);       // the switches come last
        CHECK(py.front().facet == "collection");
        // The four `:results` rows are one key split four ways, so a
        // popup row can own a facet instead of the whole value.
        CHECK(py[0].key == "results" && py[1].key == "results" && py[2].key == "results" && py[3].key == "results");
        CHECK(py[1].facet == "type" && py[2].facet == "format" && py[3].facet == "handling");
        // Language families, including the aliases that share a backend.
        CHECK(find(OrgHeaderArgSpecsFor("src", "R"), "width") != nullptr);
        CHECK(find(OrgHeaderArgSpecsFor("src", "C++"), "namespaces") != nullptr);
        CHECK(find(OrgHeaderArgSpecsFor("src", "bash"), "cmdline") != nullptr);
        CHECK(find(OrgHeaderArgSpecsFor("src", "sqlite"), "engine") != nullptr);
        CHECK(find(OrgHeaderArgSpecsFor("src", "maxima"), "display2d") != nullptr);
        // `:width`/`:height` are R's *and* maxima's, sized in different
        // units by different graphics devices -- one key, two families.
        CHECK(find(OrgHeaderArgSpecsFor("src", "maxima"), "width") != nullptr);
        CHECK(find(OrgHeaderArgSpecsFor("src", "maxima"), "units") == nullptr);
        // A language with no backend-specific args still gets the common
        // list rather than an invented one.
        const std::vector<OrgHeaderArgSpec> lua_specs = OrgHeaderArgSpecsFor("src", "lua");
        CHECK(find(lua_specs, "exports") != nullptr);
        CHECK(find(lua_specs, "includes") == nullptr);
        // A numeric option carries the range j/k moves in.
        const OrgHeaderArgSpec *width = find(OrgHeaderArgSpecsFor("src", "r"), "width");
        CHECK(width != nullptr);
        CHECK(width->kind == OrgHeaderArgKind::kNumber);
        CHECK(width->default_value == 7.0);
        CHECK(width->min_value == 1.0);
    }
    {
        // --- `:results` facets: four independent word classes in one value.
        CHECK(OrgResultsFacetOf("output") == "collection");
        CHECK(OrgResultsFacetOf("table") == "type");
        CHECK(OrgResultsFacetOf("raw") == "format");
        CHECK(OrgResultsFacetOf("silent") == "handling");
        CHECK(OrgResultsFacetOf("OUTPUT") == "collection");  // org itself is case-insensitive here
        CHECK(OrgResultsFacetOf("nonsense").empty());
        CHECK(OrgResultsFacetValue("output table replace", "type") == "table");
        CHECK(OrgResultsFacetValue("output table replace", "format").empty());
        CHECK(OrgResultsFacetValue("", "collection").empty());
        // Writing one facet leaves the others exactly where they were.
        CHECK(OrgResultsWithFacet("output table", "type", "list") == "output list");
        CHECK(OrgResultsWithFacet("output table", "format", "raw") == "output table raw");
        CHECK(OrgResultsWithFacet("", "collection", "value") == "value");
        // Clearing a facet drops just that word.
        CHECK(OrgResultsWithFacet("output table", "type", "") == "output");
        CHECK(OrgResultsWithFacet("output", "collection", "").empty());
        // A word from no class at all is kept: this rewrite owns one
        // facet, not the whole value.
        CHECK(OrgResultsWithFacet("output mystery", "collection", "value") == "value mystery");
    }
    {
        // --- Writing one argument back into the line it lives on.
        // Appended when the block doesn't have it yet.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python", "results", "output") == "#+begin_src python :results output");
        // Replaced where it sits, with everything after it left alone.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python :results value :exports both", "results", "output") ==
              "#+begin_src python :results output :exports both");
        // An empty value removes the argument -- from the middle...
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python :results value :exports both", "results", "") ==
              "#+begin_src python :exports both");
        // ...and from the end, without leaving the separator behind.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python :exports both", "exports", "") == "#+begin_src python");
        // Removing something that was never there changes nothing.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python", "exports", "") == "#+begin_src python");
        // Keys match without regard to case, the way org reads them.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python :RESULTS value", "results", "output") ==
              "#+begin_src python :results output");
        // The language tag and any switches sit before the arguments and
        // are never mistaken for one.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python -n", "eval", "no") == "#+begin_src python -n :eval no");
        // A quoted value holding a colon is one value, not two arguments.
        CHECK(OrgSetHeaderArgOnLine("#+begin_src sh :dir \"a :b\" :eval yes", "eval", "no") ==
              "#+begin_src sh :dir \"a :b\" :eval no");
        // Indentation is preserved (a block nested under a list item).
        CHECK(OrgSetHeaderArgOnLine("  #+begin_src python", "cache", "yes") == "  #+begin_src python :cache yes");
        // A `#+HEADER:` line is the other place an argument lives, and
        // its own colon is part of the keyword, not an argument.
        CHECK(OrgSetHeaderArgOnLine("#+HEADER: :var x=1", "var", "y=2") == "#+HEADER: :var y=2");
        CHECK(OrgSetHeaderArgOnLine("#+HEADER: :var x=1", "cache", "yes") == "#+HEADER: :var x=1 :cache yes");
        // Anything that is neither is left alone rather than guessed at.
        CHECK(OrgSetHeaderArgOnLine("print('hello')", "eval", "no") == "print('hello')");
        CHECK(OrgSetHeaderArgOnLine("#+begin_src python", "", "x") == "#+begin_src python");
    }
    {
        // --- Values that would be reparsed as two arguments get quoted;
        // multi-word values that org reads as one must not be.
        CHECK(OrgQuoteHeaderArgValue("out.png") == "out.png");
        CHECK(OrgQuoteHeaderArgValue("my plot.png") == "my plot.png");
        CHECK(OrgQuoteHeaderArgValue("output table") == "output table");
        CHECK(OrgQuoteHeaderArgValue("a :b") == "\"a :b\"");
        CHECK(OrgQuoteHeaderArgValue(":x") == "\":x\"");
        CHECK(OrgQuoteHeaderArgValue("\"a :b\"") == "\"a :b\"");  // already quoted
        CHECK(OrgQuoteHeaderArgValue("").empty());
        CHECK(OrgQuoteHeaderArgValue("http://example.com") == "http://example.com");
    }
    {
        // --- Numbers: j/k stepping, clamped, and what an unset option does.
        CHECK(OrgFormatHeaderArgNumber(7.0) == "7");
        CHECK(OrgFormatHeaderArgNumber(6.5) == "6.5");
        OrgHeaderArgSpec spec;
        spec.kind = OrgHeaderArgKind::kNumber;
        spec.min_value = 1.0;
        spec.max_value = 10.0;
        spec.step = 1.0;
        spec.default_value = 7.0;
        // The first press on an unset option lands *on* the default, in
        // either direction -- it is the value someone opening this row
        // most likely wants, and one more press moves off it.
        CHECK(OrgStepHeaderArgNumber(spec, "", 1) == "7");
        CHECK(OrgStepHeaderArgNumber(spec, "", -1) == "7");
        CHECK(OrgStepHeaderArgNumber(spec, "7", 1) == "8");
        CHECK(OrgStepHeaderArgNumber(spec, "7", -1) == "6");
        CHECK(OrgStepHeaderArgNumber(spec, "10", 1) == "10");  // clamped
        CHECK(OrgStepHeaderArgNumber(spec, "1", -1) == "1");
        // A value that isn't wholly a number (an elisp form, a typo) is
        // treated as unset rather than half-parsed and mangled.
        CHECK(OrgStepHeaderArgNumber(spec, "(fig-width)", 1) == "7");
        CHECK(OrgStepHeaderArgNumber(spec, "7in", 1) == "7");
    }
    {
        // --- Bare line switches, which live before the first `:key`.
        CHECK(!OrgBlockLineHasSwitch("#+begin_src python", "-n"));
        CHECK(OrgBlockLineHasSwitch("#+begin_src python -n", "-n"));
        CHECK(!OrgBlockLineHasSwitch("#+begin_src python -n", "+n"));
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python", "-n", true) == "#+begin_src python -n");
        // Added at the end of the switch region: after the language,
        // before the arguments, which is where org reads them.
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python :results output", "-n", true) ==
              "#+begin_src python -n :results output");
        CHECK(OrgSetBlockSwitchOnLine("#+begin_example", "-n", true) == "#+begin_example -n");
        // Removed with one separator, from the middle of a run...
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python -n -r :eval no", "-n", false) ==
              "#+begin_src python -r :eval no");
        // ...and from the end of the line.
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python -n", "-n", false) == "#+begin_src python");
        // Setting what is already set (or clearing what isn't) is a no-op.
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python -n", "-n", true) == "#+begin_src python -n");
        CHECK(OrgSetBlockSwitchOnLine("#+begin_src python", "-n", false) == "#+begin_src python");
        // A `#+HEADER:` line has no switch region at all.
        CHECK(!OrgBlockLineHasSwitch("#+HEADER: -n", "-n"));
        CHECK(OrgSetBlockSwitchOnLine("#+HEADER: :var x=1", "-n", true) == "#+HEADER: :var x=1");
    }

    {
        // --- Header-arg reading: the whole value, not just its first word.
        const std::string args = ":dir ~/my notes :flags -O2 -Wall :eval no";
        CHECK(OrgHeaderArgValue(args, "dir") == "~/my notes");
        CHECK(OrgHeaderArgValue(args, "flags") == "-O2 -Wall");
        CHECK(OrgHeaderArgValue(args, "eval") == "no");
        CHECK(OrgHeaderArgValue(args, "session").empty());
        CHECK(OrgHeaderArgPresent(args, "eval"));
        CHECK(!OrgHeaderArgPresent(args, "session"));
        // A quoted value keeps its interior colon, and loses its quotes.
        CHECK(OrgHeaderArgValue(":file \"a: b.png\" :eval yes", "file") == "a: b.png");
        // `:file` must not be answered by `:file-ext`, nor the reverse.
        CHECK(OrgHeaderArgValue(":file-ext png", "file").empty());
        CHECK(OrgHeaderArgValue(":file-ext png", "file-ext") == "png");
        // Key matching ignores case; a bare key has an empty value but is
        // still present.
        CHECK(OrgHeaderArgValue(":EVAL never", "eval") == "never");
        CHECK(OrgHeaderArgPresent(":no-expand", "no-expand"));
        CHECK(OrgHeaderArgValue(":no-expand", "no-expand").empty());
        // A colon that does not start a token is part of the value.
        CHECK(OrgHeaderArgValue(":var url=https://example.com/x :eval yes", "var") == "url=https://example.com/x");
        std::vector<std::pair<std::string, std::string>> pairs = OrgHeaderArgPairs(":var a=1 :var b=2 :eval yes");
        CHECK(pairs.size() == 3);
        CHECK(pairs[0].first == "var" && pairs[0].second == "a=1");
        CHECK(pairs[1].second == "b=2");
        CHECK(pairs[2].first == "eval");
    }
    {
        // --- Merging the layers a block's arguments come from: a
        // file-wide `#+PROPERTY:`, then `#+HEADER:` lines, then the
        // `#+begin_src` line itself. Later wins, except `:var`, which
        // accumulates by name.
        const std::string merged = OrgMergeHeaderArgs({":results output :var base=1", ":var x=2", ":results value"});
        CHECK(OrgHeaderArgValue(merged, "results") == "value");
        CHECK(OrgHeaderArgPairs(merged).size() == 3);
        std::string base, x;
        for (const auto &kv : OrgHeaderArgPairs(merged)) {
            if (kv.second.compare(0, 5, "base=") == 0) base = kv.second;
            if (kv.second.compare(0, 2, "x=") == 0) x = kv.second;
        }
        CHECK(base == "base=1");
        CHECK(x == "x=2");
        // Same variable in two layers: the later one wins rather than
        // binding twice.
        const std::string rebound = OrgMergeHeaderArgs({":var x=1", ":var x=9"});
        CHECK(OrgHeaderArgPairs(rebound).size() == 1);
        CHECK(OrgHeaderArgValue(rebound, "var") == "x=9");
        // A key written with no value survives the round trip as one.
        CHECK((OrgMergeHeaderArgs({":eval no"}) == ":eval no"));
    }
    {
        // --- Results bodies: every `:results` word, and `:wrap`.
        const Lines out = {"a", "b"};
        OrgResultsOptions opts;
        // Unset: one line becomes `: x`, several become an example block.
        CHECK((OrgFormatResultsBody({"only"}, opts) == Lines{": only"}));
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_example", "a", "b", "#+end_example"}));
        CHECK(!OrgResultsBodyIsRaw(opts));
        // `raw`/`org`: inserted exactly as they stand.
        opts.format = "raw";
        CHECK(OrgFormatResultsBody(out, opts) == out);
        CHECK(OrgResultsBodyIsRaw(opts));
        // `drawer`, `html`, `latex`, `code`.
        opts.format = "drawer";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{":results:", "a", "b", ":end:"}));
        opts.format = "html";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_export html", "a", "b", "#+end_export"}));
        opts.format = "latex";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_export latex", "a", "b", "#+end_export"}));
        opts.format = "code";
        opts.lang = "python";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_src python", "a", "b", "#+end_src"}));
        // `:wrap` beats `:results format`, and closes on the block word
        // alone even when it opened with arguments.
        opts.wrap = "src html";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_src html", "a", "b", "#+end_src"}));
        opts.wrap = "example";
        CHECK((OrgFormatResultsBody(out, opts) == Lines{"#+begin_example", "a", "b", "#+end_example"}));
    }
    {
        // --- `:results` types.
        OrgResultsOptions opts;
        opts.type = "list";
        CHECK((OrgFormatResultsBody({"x", "y"}, opts) == Lines{"- x", "- y"}));
        // `verbatim`/`scalar` refuse every interpretation, including the
        // example fence a multi-line result would otherwise get.
        opts.type = "verbatim";
        CHECK((OrgFormatResultsBody({"a,b", "c,d"}, opts) == Lines{": a,b", ": c,d"}));
        opts.type = "scalar";
        CHECK((OrgFormatResultsBody({"1"}, opts) == Lines{": 1"}));
        // A `:file` link is inserted untouched whatever else is set.
        opts.type = "";
        opts.format = "raw";
        opts.file_link = true;
        CHECK((OrgFormatResultsBody({"[[file:p.png]]"}, opts) == Lines{"[[file:p.png]]"}));
    }
    {
        // --- Tables: auto-detected separator, `:sep`, `:colnames no`.
        CHECK((OrgFormatResultsTable({"a,b", "c,d"}, "", true) == Lines{"| a | b |", "|---+---|", "| c | d |"}));
        CHECK((OrgFormatResultsTable({"a,b", "c,d"}, "", false) == Lines{"| a | b |", "| c | d |"}));
        CHECK((OrgFormatResultsTable({"a|b"}, "|", true) == Lines{"| a | b |"}));
        CHECK((OrgFormatResultsTable({"a\tb"}, "\\t", true) == Lines{"| a | b |"}));
        // A tab anywhere wins over a comma, and output with neither is
        // left alone rather than forced into a one-column table.
        CHECK((OrgFormatResultsTable({"a,b\tc"}, "", true) == Lines{"| a,b | c |"}));
        CHECK((OrgFormatResultsTable({"plain"}, "", true) == Lines{"plain"}));
        // Reached through the full options path too.
        OrgResultsOptions opts;
        opts.type = "table";
        opts.sep = ",";
        opts.colnames = false;
        CHECK((OrgFormatResultsBody({"1,2"}, opts) == Lines{"| 1 | 2 |"}));
    }
    {
        // --- Reading the options off a header-args string.
        OrgResultsOptions opts = OrgResultsOptionsFrom(":results output table replace :sep , :colnames no", "R");
        CHECK(opts.collection == "output");
        CHECK(opts.type == "table");
        CHECK(opts.handling == "replace");
        CHECK(opts.sep == ",");
        CHECK(!opts.colnames);
        CHECK(opts.lang == "R");
        CHECK(opts.format.empty());
    }
    {
        // --- Noweb: when it expands, and what it expands to.
        CHECK(!OrgNowebExpandsIn("", "eval"));
        CHECK(!OrgNowebExpandsIn("no", "eval"));
        CHECK(OrgNowebExpandsIn("yes", "eval"));
        CHECK(OrgNowebExpandsIn("yes", "tangle"));
        CHECK(OrgNowebExpandsIn("tangle", "tangle"));
        CHECK(!OrgNowebExpandsIn("tangle", "eval"));
        CHECK(OrgNowebExpandsIn("eval", "eval"));
        CHECK(!OrgNowebExpandsIn("eval", "tangle"));
        CHECK(OrgNowebExpandsIn("no-export", "eval"));
        CHECK(!OrgNowebExpandsIn("no-export", "export"));

        std::map<std::string, std::string> blocks;
        blocks["helper"] = "def helper():\n    return 1";
        // A reference on its own line carries its indentation onto every
        // expanded line -- the whole point in Python.
        CHECK((OrgNowebExpand({"class C:", "    <<helper>>"}, blocks, "", 8) ==
              Lines{"class C:", "    def helper():", "        return 1"}));
        // Unresolved references are left exactly as written.
        CHECK((OrgNowebExpand({"<<missing>>"}, blocks, "", 8) == Lines{"<<missing>>"}));
        // References nest.
        blocks["outer"] = "<<helper>>";
        CHECK((OrgNowebExpand({"<<outer>>"}, blocks, "", 8) == Lines{"def helper():", "    return 1"}));
        // A self-reference terminates at the depth limit instead of hanging.
        blocks["loop"] = "<<loop>>";
        CHECK((OrgNowebExpand({"<<loop>>"}, blocks, "", 4) == Lines{"<<loop>>"}));
        // Arguments are accepted so the reference still resolves.
        CHECK((OrgNowebExpand({"<<helper(1)>>"}, blocks, "", 8) == Lines{"def helper():", "    return 1"}));
    }
    {
        // --- Display switches.
        OrgBlockSwitches sw = OrgParseBlockSwitches("#+begin_src python");
        CHECK(!sw.number);
        CHECK(sw.strip_refs);
        sw = OrgParseBlockSwitches("#+begin_src python -n :results output");
        CHECK(sw.number && !sw.continue_numbers && sw.start == 1);
        sw = OrgParseBlockSwitches("#+begin_src python -n 12");
        CHECK(sw.number && sw.start == 12);
        sw = OrgParseBlockSwitches("#+begin_example +n");
        CHECK(sw.number && sw.continue_numbers);
        CHECK(!OrgParseBlockSwitches("#+begin_src c -k").strip_refs);
        // A `:key` value that happens to read like a switch is not one.
        CHECK(!OrgParseBlockSwitches("#+begin_src sh :cmdline -n").number);

        int counter = 1;
        Lines numbered = OrgApplyBlockSwitches({"a", "b"}, OrgParseBlockSwitches("#+begin_src c -n"), &counter);
        CHECK((numbered == Lines{"1:  a", "2:  b"}));
        CHECK(counter == 3);
        // `+n` picks the count up where the last numbered block left it,
        // across an unnumbered block in between.
        OrgApplyBlockSwitches({"plain"}, OrgParseBlockSwitches("#+begin_src c"), &counter);
        numbered = OrgApplyBlockSwitches({"c"}, OrgParseBlockSwitches("#+begin_src c +n"), &counter);
        CHECK((numbered == Lines{"3:  c"}));
        // Numbers are right-aligned to the block's widest.
        counter = 1;
        Lines wide = OrgApplyBlockSwitches({"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"},
                                           OrgParseBlockSwitches("#+begin_src c -n"), &counter);
        CHECK(wide[0] == " 1:  a");
        CHECK(wide[9] == "10:  j");
        // `(ref:)` labels go unless `-k` keeps them.
        counter = 1;
        CHECK((OrgApplyBlockSwitches({"x = 1  (ref:one)"}, OrgParseBlockSwitches("#+begin_src c"), &counter) ==
              Lines{"x = 1"}));
        counter = 1;
        CHECK((OrgApplyBlockSwitches({"x = 1  (ref:one)"}, OrgParseBlockSwitches("#+begin_src c -k"), &counter) ==
              Lines{"x = 1  (ref:one)"}));
        // A `(ref:` that isn't a trailing label stays put.
        counter = 1;
        CHECK((OrgApplyBlockSwitches({"f((ref:x)) + 1"}, OrgParseBlockSwitches("#+begin_src c"), &counter) ==
              Lines{"f((ref:x)) + 1"}));
    }
    {
        // --- Tangle options.
        OrgTangleOptions opts = OrgTangleOptionsFrom(":tangle out.sh :shebang #!/bin/sh :mkdirp yes :padline no");
        CHECK(opts.shebang == "#!/bin/sh");
        CHECK(opts.mkdirp);
        CHECK(!opts.padline);
        CHECK(opts.comments.empty());
        CHECK(OrgTangleOptionsFrom(":comments no").comments.empty());
        CHECK(OrgTangleOptionsFrom(":comments link").comments == "link");
        CHECK(OrgTangleOptionsFrom("").padline);

        CHECK(OrgParseTangleMode("o755") == 0755);
        CHECK(OrgParseTangleMode("#o644") == 0644);
        CHECK(OrgParseTangleMode("755") == 0755);
        CHECK(OrgParseTangleMode("") == -1);
        CHECK(OrgParseTangleMode("o799") == -1);
        CHECK(OrgParseTangleMode("rwxr-xr-x") == -1);

        Lines open_comment = OrgTangleComment("link", "# ", "notes.org", "setup", 12, true);
        CHECK(open_comment.size() == 1);
        CHECK(open_comment[0] == "# [[file:notes.org::setup][setup]]");
        Lines close_comment = OrgTangleComment("link", "# ", "notes.org", "setup", 12, false);
        CHECK(close_comment[0] == "# [[file:notes.org::setup][setup]] ends here");
        // An unnamed block falls back to its row.
        CHECK(OrgTangleComment("link", "// ", "notes.org", "", 12, true)[0] ==
              "// [[file:notes.org::12][notes.org]]");
        CHECK(OrgTangleComment("", "# ", "notes.org", "setup", 1, true).empty());
        CHECK(OrgTangleComment("no", "# ", "notes.org", "setup", 1, true).empty());
    }
    {
        // --- `:exports`, which defaults to code.
        CHECK(OrgExportsCode(""));
        CHECK(!OrgExportsResults(""));
        CHECK(OrgExportsCode("both") && OrgExportsResults("both"));
        CHECK(!OrgExportsCode("results") && OrgExportsResults("results"));
        CHECK(!OrgExportsCode("none") && !OrgExportsResults("none"));
    }

    {
        // --- Finding and replacing the results block already under a run.
        const Lines doc = {"#+begin_src sh", "echo hi", "#+end_src", "#+RESULTS:", ": hi", "", "after"};
        int start = 0, end = 0;
        CHECK(OrgFindResultsBlock(doc, 3, &start, &end));
        CHECK(start == 4 && end == 5);
        // A named `#+RESULTS: plot` keyword counts, which the old
        // `%s*$`-anchored matcher refused.
        const Lines named = {"#+end_src", "#+RESULTS: plot", "| a | b |", "x"};
        CHECK(OrgFindResultsBlock(named, 1, &start, &end));
        CHECK(start == 2 && end == 3);
        // Fenced bodies run to their own closer, whatever `:wrap` named.
        const Lines wrapped = {"#+end_src", "#+RESULTS:", "#+begin_export html", "<b>x</b>", "#+end_export", "tail"};
        CHECK(OrgFindResultsBlock(wrapped, 1, &start, &end));
        CHECK(start == 2 && end == 5);
        const Lines drawer = {"#+end_src", "#+RESULTS:", ":results:", "x", ":end:"};
        CHECK(OrgFindResultsBlock(drawer, 1, &start, &end));
        CHECK(start == 2 && end == 5);
        // A keyword with nothing under it is just itself.
        const Lines bare = {"#+end_src", "#+RESULTS:", "", "prose"};
        CHECK(OrgFindResultsBlock(bare, 1, &start, &end));
        CHECK(start == 2 && end == 2);
        // Nothing there at all.
        CHECK(!OrgFindResultsBlock({"#+end_src", "prose"}, 1, &start, &end));
    }
    {
        // --- Splicing: replace/append/prepend/none.
        const Lines doc = {"#+end_src", "#+RESULTS:", ": old", "after"};
        const Lines block = {"#+RESULTS:", ": new"};
        CHECK((OrgSpliceResultsBlock(doc, 1, block, "replace") == Lines{"#+end_src", "#+RESULTS:", ": new", "after"}));
        CHECK((OrgSpliceResultsBlock(doc, 1, block, "append") ==
               Lines{"#+end_src", "#+RESULTS:", ": old", ": new", "after"}));
        CHECK((OrgSpliceResultsBlock(doc, 1, block, "prepend") ==
               Lines{"#+end_src", "#+RESULTS:", ": new", ": old", "after"}));
        CHECK(OrgSpliceResultsBlock(doc, 1, block, "none") == doc);
        CHECK(OrgSpliceResultsBlock(doc, 1, block, "silent") == doc);
        // With nothing there yet, every writing handling inserts.
        const Lines empty = {"#+end_src", "after"};
        CHECK((OrgSpliceResultsBlock(empty, 1, block, "append") ==
               Lines{"#+end_src", "#+RESULTS:", ": new", "after"}));
        // Appending to a bare keyword keeps the new body in order.
        const Lines bare = {"#+end_src", "#+RESULTS:", "after"};
        CHECK((OrgSpliceResultsBlock(bare, 1, {"#+RESULTS:", ": a", ": b"}, "append") ==
               Lines{"#+end_src", "#+RESULTS:", ": a", ": b", "after"}));
    }

    {
        // --- `:exports`, over a whole document.
        const Lines doc = {
            "#+TITLE: t",           // 1
            "* one",                // 2
            "#+begin_src python",   // 3
            "print(1)",             // 4
            "#+end_src",            // 5
            "#+RESULTS:",           // 6
            ": 1",                  // 7
            "* two",                // 8
            "#+NAME: hidden",       // 9
            "#+begin_src python :exports results",  // 10
            "print(2)",             // 11
            "#+end_src",            // 12
            "#+RESULTS: hidden",    // 13
            ": 2",                  // 14
            "* three",              // 15
            "#+begin_src python :exports none",  // 16
            "print(3)",             // 17
            "#+end_src",            // 18
            "#+RESULTS:",           // 19
            ": 3",                  // 20
            "tail",                 // 21
        };
        const Lines gated = OrgApplyExportGates(doc);
        // Default (`code`): the block stays, its results go.
        CHECK((std::find(gated.begin(), gated.end(), "print(1)") != gated.end()));
        CHECK((std::find(gated.begin(), gated.end(), ": 1") == gated.end()));
        // `results`: the results stay, and the code goes along with the
        // `#+NAME:` affiliated with it.
        CHECK((std::find(gated.begin(), gated.end(), "print(2)") == gated.end()));
        CHECK((std::find(gated.begin(), gated.end(), "#+NAME: hidden") == gated.end()));
        CHECK((std::find(gated.begin(), gated.end(), ": 2") != gated.end()));
        // `none`: neither.
        CHECK((std::find(gated.begin(), gated.end(), "print(3)") == gated.end()));
        CHECK((std::find(gated.begin(), gated.end(), ": 3") == gated.end()));
        // Everything that is not a block is untouched.
        CHECK(gated.front() == "#+TITLE: t");
        CHECK(gated.back() == "tail");
        CHECK((std::find(gated.begin(), gated.end(), "* three") != gated.end()));

        // A file-wide `#+PROPERTY:` reaches a block that sets nothing...
        const Lines prop = {"#+PROPERTY: header-args :exports none", "#+begin_src sh", "echo hi", "#+end_src"};
        CHECK(OrgApplyExportGates(prop).size() == 1);
        // ...and the block's own value still beats it.
        const Lines override = {"#+PROPERTY: header-args :exports none", "#+begin_src sh :exports code", "echo hi",
                                "#+end_src"};
        CHECK(OrgApplyExportGates(override).size() == 4);
        // As does a `#+HEADER:` line between them.
        const Lines hdr = {"#+PROPERTY: header-args :exports none", "#+HEADER: :exports code", "#+begin_src sh",
                           "echo hi", "#+end_src"};
        CHECK(OrgApplyExportGates(hdr).size() == 5);
        // A language-scoped property only applies to that language.
        const Lines scoped = {"#+PROPERTY: header-args:python :exports none", "#+begin_src sh", "echo hi",
                              "#+end_src"};
        CHECK(OrgApplyExportGates(scoped).size() == 4);
        // An unterminated block is left exactly as it stands.
        const Lines open_block = {"#+begin_src sh :exports none", "echo hi"};
        CHECK(OrgApplyExportGates(open_block) == open_block);
    }

    std::printf("org_doc_test: all checks passed\n");
    return 0;
}
