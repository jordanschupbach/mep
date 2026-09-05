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

    std::printf("org_doc_test: all checks passed\n");
    return 0;
}
