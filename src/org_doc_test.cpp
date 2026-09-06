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
    //     tags; the archived headline and its whole subtree vanish from
    //     the checklist, but a save of that checklist leaves them in the
    //     file (they're never "unreferenced" -- see OrgTodoListApply). A
    //     plain headline, a stale line or an already-archived one is a
    //     no-op.
    {
        Lines doc = {"#+TODO: TODO | DONE", "* TODO [#A] Parent :work:", "** TODO Child", "   body",
                     "* DONE Sibling", "* TODO Other"};
        Lines out = OrgTodoListArchive(doc, 1);
        CHECK(out[1] == "* TODO [#A] Parent :work:ARCHIVE:");
        CHECK(out[2] == "** TODO Child");  // the child keeps its own line untouched...
        std::vector<OrgTodoItem> items = OrgTodoListItems(out);
        CHECK(items.size() == 2);  // ...but is hidden with its parent
        CHECK(items[0].text == "Sibling" && items[0].line == 4);
        CHECK(items[1].text == "Other" && items[1].line == 5);
        // Saving the (archived-free) checklist back must not drop the
        // archived subtree; toggling a visible sibling still works.
        items[1].done = true;
        Lines saved = OrgTodoListApply(out, items);
        CHECK(saved.size() == out.size());
        CHECK(saved[1] == "* TODO [#A] Parent :work:ARCHIVE:");
        CHECK(saved[2] == "** TODO Child");
        CHECK(saved[3] == "   body");
        CHECK(saved[5] == "* DONE Other");
        // Retitling the visible sibling doesn't disturb the archived rows either.
        CHECK(OrgTodoListRetitle(saved, 5, "Renamed")[1] == "* TODO [#A] Parent :work:ARCHIVE:");
        // No-ops.
        CHECK(OrgTodoListArchive(out, 1) == out);    // already archived
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

    std::printf("org_doc_test: all checks passed\n");
    return 0;
}
