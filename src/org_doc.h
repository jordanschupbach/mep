#ifndef MEP_ORG_DOC_H
#define MEP_ORG_DOC_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Deliberately raylib-free (same reasoning as sheet_doc.h/pdf_doc.h): the
// outline model and its parsing/rewrite helpers are pure CPU-side data
// structures operating on plain std::string lines, usable/testable without
// a GL context. main.cpp/editor.cpp own turning this into an interactive
// Kanban board / Gantt chart and splicing rewritten lines back into a
// Buffer's own `lines` (the org file's *actual* text -- there is no
// separate binary model to save, unlike Workbook/OfficeDoc; see the
// Kanban/Gantt plan's "Key architectural decision").
//
// A hand-rolled, intentionally partial org outline reader -- not a general
// org-mode parser. Scope, matching this repo's convention of naming
// exclusions rather than silently omitting them:
//   - Only the first "#+TODO:" line in a file is honored.
//   - A headline's SCHEDULED/DEADLINE must appear together on the single
//     planning line immediately following it (the normal org-export
//     convention); additional planning lines are not scanned.
//   - Timestamp repeaters/warning periods ("+1w", "-2d") and time RANGES
//     ("09:00-10:30") are not modeled -- only the first HH:MM, if any, is
//     read as a time-of-day. The rest of a timestamp's original text is
//     preserved verbatim by every rewrite helper below, which only ever
//     replaces the "YYYY-MM-DD Day" portion of a "<...>" token, never the
//     whole span.
//   - :EFFORT:, :ID:, :BLOCKER:, :ASSIGNEE:/ :TEAM:, and :PROGRESS: are read
//     from an immediately-following :PROPERTIES: drawer. BLOCKER is a
//     whitespace/comma-separated list of predecessor IDs, used by the Gantt
//     view to draw dependency arrows.

struct OrgTimestamp {
    bool present = false;
    int year = 0, month = 0, day = 0;
    bool has_time = false;
    int hour = 0, min = 0;
    // Full original "<2026-08-25 Tue +1w>"-style token, kept so rewrite
    // helpers can preserve everything after the weekday verbatim.
    std::string raw;
};

struct OrgHeadline {
    int level = 0;                 // count of leading '*'
    std::string todo_keyword;      // "" if the headline has none
    bool is_done_keyword = false;  // classified against the parsed TODO/DONE sequence
    char priority = 0;             // 'A'.. from "[#A]", or 0 if absent
    std::string title;             // after TODO keyword + priority, before tags
    std::vector<std::string> tags;
    OrgTimestamp scheduled, deadline;
    std::optional<std::string> effort;  // raw "H:MM" from a :EFFORT: property
    std::string id;                      // :ID: property, if any
    std::vector<std::string> blockers;   // :BLOCKER: predecessor IDs
    std::string assignee;                // :ASSIGNEE: or :TEAM: label
    int progress = 0;                    // :PROGRESS: percentage, clamped 0..100

    // Inclusive line-index range in the source `lines` this headline's
    // whole subtree (itself + every deeper-level line until the next
    // sibling-or-shallower headline) occupies. This is what a Kanban card
    // move/delete or a `line_start` retitle actually operates on.
    int line_start = -1, line_end = -1;
    int planning_line = -1;  // index of the SCHEDULED/DEADLINE line, -1 if none
    int parent_index = -1;  // index into OrgOutline::headlines, -1 if top-level
};

struct OrgOutline {
    std::vector<std::string> todo_keywords;  // from the first "#+TODO:" line; default {"TODO"}
    std::vector<std::string> done_keywords;  // default {"DONE"}
    std::vector<OrgHeadline> headlines;      // flattened, document order
};

// --- Display-side scans (Editor::OrgLinkScan / DrawPane, main.cpp) ---
// These answer "what should this line look like", not "what does this file
// mean", but they live here for the same reason everything else in this
// header does: they are pure functions over plain std::string lines, so
// they can be tested without a GL context (org_doc_test.cpp) -- which
// matters, because link syntax is exactly the kind of thing that breaks on
// the edge cases (`[[a]][[b]]` on one line, an empty target, a URL sitting
// inside a bracket link) rather than on the common path.

// Headline depth: the count of leading `*` when they start at column 0 and
// are followed by a space, else 0. Deliberately stricter than "starts with
// a star": `*bold*` opening a line and an indented `  * item` bullet are
// both not headlines.
/**
 * @brief Returns an org headline's depth (count of leading `*`), or 0 when the line isn't a headline.
 * @param line the line to measure
 * @return the headline level (1-based), or 0
 */
int OrgHeadlineLevel(const std::string &line);

// A headline's leading stars are markup like any other: what they say
// (this row's depth) the render already says with size and colour
// (kOrgHeadingStyles/OrgHeadlineLevel1..3, editor.h), so concealment
// hides them and indents the title one column per level below the first
// instead -- real org's own org-indent-mode look, and the reason a
// rendered outline reads as a document rather than as a run of asterisks.
//
// Two numbers, one definition, because two renderers have to agree on
// them (DrawPane's row loop and its closed-fold summary, main.cpp):
// OrgHeadlineStarHideLen is what the render drops from the front of the
// line -- the stars *and* the single space separating them from the
// title, so a level-1 headline's title starts at the margin -- and
// OrgHeadlineStarIndentCols is the indent drawn in its place. The
// difference between them is 2 at every depth, which is exactly how far
// left a headline's title slides when its stars are hidden.
/**
 * @brief Returns the bytes at the start of an org headline the render hides (stars plus their separating space), or 0 when the line isn't a headline.
 * @param line the line to measure
 * @return the prefix length to hide, or 0
 */
int OrgHeadlineStarHideLen(const std::string &line);
/**
 * @brief Returns the columns of indentation the render draws in place of a headline's hidden stars (level - 1), or 0 when the line isn't a headline.
 * @param line the line to measure
 * @return the indent width in columns, or 0
 */
int OrgHeadlineStarIndentCols(const std::string &line);

// --- Org emphasis marker boundaries -----------------------------------
//
// Org only lets `*bold*`, `/italic/`, `_under_`, `+strike+`, `=verbatim=`
// and `~code~` open after one of a *specific* set of characters and close
// before another (`org-emphasis-regexp-components`' PRE and POST classes,
// reproduced exactly): PRE is start-of-line, whitespace, `-`, `(`, `'`,
// `"` or `{`; POST is end-of-line, whitespace, `-`, `.`, `,`, `:`, `!`,
// `?`, `;`, `'`, `"`, `)`, `}` or `[`.
//
// The looser "any non-alphanumeric will do" rule these scanners used
// before let a marker open after punctuation that org never treats as a
// boundary, and `/` in a URL is the case that bites: in
// `see [[https://example.com][Site]] and https://example.org`, the second
// `/` of the first `//` opened an italic run that closed on the `/` of
// the *second* URL, so a whole stretch of prose (the bracket link's own
// concealed markup included) was drawn as concealed italic text painted
// over the link. Reported live as "funny rendering"; the same class of
// over-match as `$..$` math matching inside a code block.
/**
 * @brief Reports whether a character may immediately precede an opening org emphasis marker.
 * @param c the character before the marker, or '\0' for start-of-line
 * @return true if `c` is in org's PRE class (or is start-of-line)
 */
bool OrgEmphasisPreOk(char c);
/**
 * @brief Reports whether a character may immediately follow a closing org emphasis marker.
 * @param c the character after the marker, or '\0' for end-of-line
 * @return true if `c` is in org's POST class (or is end-of-line)
 */
bool OrgEmphasisPostOk(char c);
/**
 * @brief Reports whether a character is org emphasis "border" whitespace, which may not sit just inside a marker pair.
 * @param c the character to test
 * @return true if `c` is a space or tab
 */
bool OrgEmphasisBorderBlank(char c);

// One link found on a line. `col_start`/`col_end` bound the *raw markup*
// (half-open, byte offsets, the same convention Decoration uses);
// `display` is what should be drawn in its place when markup is concealed.
struct OrgLinkSpanInfo {
    int col_start = 0;
    int col_end = 0;
    std::string target;
    std::string display;
    // True for a `[[...]]` link, whose markup is hidden behind `display`.
    // False for a bare URL, which is already its own display text.
    bool bracketed = false;
};

// Every `[[target]]` / `[[target][description]]` and every bare
// `http(s)://...` on `line`, in column order. A bare URL that falls inside
// a bracket link's own span is not reported separately -- the bracket link
// already covers it, and two overlapping links on the same columns would
// give the renderer two conflicting things to draw and click.
//
// `display` drops the scheme noise org itself drops from a bare link's
// display: `file:`, `id:`, and a leading `*`/`#`. A link with a
// description always displays that description verbatim.
//
// Text inside a `=verbatim=` or `~code~` run is not scanned at all: org
// treats it as literal, and a help page writing `=[[file:x]]=` to *show*
// link syntax must not end up with a live link there.
//
// Scope, named rather than silently omitted: only the `http`/`https`
// schemes are recognized unbracketed. A bare `www.example.com`, `mailto:`
// or `ftp://` is left as plain text -- inside a bracket link every scheme
// works, and guessing at unbracketed ones turns ordinary prose (a
// sentence ending in a domain name) into a link.
/**
 * @brief Finds every org bracket link and bare http(s) URL on a line, in column order.
 * @param line the line to scan
 * @return the links found; empty when the line has none
 */
std::vector<OrgLinkSpanInfo> ScanOrgLinkSpans(const std::string &line);

// Parses every headline in `lines` into a flat, document-ordered outline.
// Never fails outright -- a file with zero headlines just yields an empty
// `headlines` vector with the default/parsed keyword sequence.
/**
 * @brief Parses an org file's lines into a flat, document-ordered outline of headlines.
 * @param lines the full text of the org file, one entry per line
 * @return the parsed OrgOutline (TODO/DONE keyword sequence plus every headline found)
 */
OrgOutline ParseOrgOutline(const std::vector<std::string> &lines);

// Returns a rewritten copy of `headline_line` with its TODO/DONE keyword
// token replaced by `new_keyword` (or `new_keyword` inserted right after
// the stars if the headline currently has none). `todo_keywords`/
// `done_keywords` disambiguate "the first word of the title happens to
// look like a keyword" from an actual keyword token, so callers must pass
// the same sequence `ParseOrgOutline` produced for this file.
/**
 * @brief Returns a copy of a headline line with its TODO/DONE keyword token replaced (or inserted if absent).
 * @param headline_line the original "*** KEYWORD [#A] Title :tags:" line
 * @param new_keyword the keyword token to place after the stars
 * @param todo_keywords the file's TODO-side keyword sequence, used to recognize the existing keyword token
 * @param done_keywords the file's DONE-side keyword sequence, used to recognize the existing keyword token
 * @return the rewritten headline line
 */
std::string RewriteHeadlineKeyword(const std::string &headline_line, const std::string &new_keyword,
                                    const std::vector<std::string> &todo_keywords,
                                    const std::vector<std::string> &done_keywords);

// Returns a rewritten copy of `planning_line` with the SCHEDULED (if
// `is_deadline` is false) or DEADLINE (if true) timestamp's date replaced
// by `new_ts`, leaving the other field on the same line (if present) and
// everything after that timestamp's weekday (time-of-day, repeater)
// untouched. A no-op (returns the input unchanged) if that keyword isn't
// present on the line.
/**
 * @brief Returns a copy of a SCHEDULED/DEADLINE planning line with that timestamp's date replaced.
 * @param planning_line the original planning line, e.g. "SCHEDULED: <2026-08-25 Tue>"
 * @param is_deadline true to rewrite the DEADLINE timestamp, false to rewrite the SCHEDULED timestamp
 * @param new_ts the replacement date (and time-of-day, if any) to substitute in
 * @return the rewritten planning line, or the input unchanged if the requested keyword isn't present
 */
std::string RewriteTimestampInLine(const std::string &planning_line, bool is_deadline, const OrgTimestamp &new_ts);

// Returns `ts` shifted by `delta_days` (may be negative), recomputing the
// weekday for the new date. `has_time`/`hour`/`min` are carried over
// unchanged. A no-op if `!ts.present`.
/**
 * @brief Returns a timestamp shifted by a number of days, recomputing its weekday.
 * @param ts the timestamp to shift
 * @param delta_days number of days to add (may be negative to subtract)
 * @return the shifted timestamp, with has_time/hour/min carried over unchanged, or `ts` unchanged if `!ts.present`
 */
OrgTimestamp ShiftTimestamp(const OrgTimestamp &ts, int delta_days);

// Formats a *freshly constructed* timestamp (year/month/day/has_time/
// hour/min only -- no repeater) as "<2026-08-25 Tue>" / "<2026-08-25 Tue
// 09:00>". Rewriting an *existing* timestamp in place should go through
// RewriteTimestampInLine instead, so any repeater/warning suffix survives.
/**
 * @brief Formats a freshly constructed timestamp as an org "<YYYY-MM-DD Day[ HH:MM]>" token.
 * @param ts the timestamp to format (year/month/day/has_time/hour/min only; no repeater is emitted)
 * @return the formatted token, or "" if `!ts.present`
 */
std::string FormatOrgTimestamp(const OrgTimestamp &ts);

// Whole-file-regenerating headline builder -- unlike RewriteHeadlineKeyword
// (a single-token surgical edit), this rebuilds an *entire* headline line
// from its parsed parts. Used for the two operations where the whole line
// legitimately IS just these parts with nothing else worth preserving: a
// Kanban title rename, and constructing a brand-new headline line for a
// new card. `level` is clamped to >= 1 (a stars count of 0 isn't a
// headline at all).
/**
 * @brief Builds an entire headline line from its parsed parts (stars, keyword, priority, title, tags).
 * @param level headline depth (number of leading '*'), clamped to >= 1
 * @param todo_keyword the TODO/DONE keyword to place after the stars, or "" to omit it
 * @param priority the priority letter to emit as "[#X]", or 0 to omit it
 * @param title the headline title text
 * @param tags trailing tags to append as ":tag1:tag2:", or empty to omit
 * @return the newly constructed headline line
 */
std::string FormatHeadlineLine(int level, const std::string &todo_keyword, char priority, const std::string &title,
                                const std::vector<std::string> &tags);

// Proleptic-Gregorian day-ordinal conversions (day 0 = 1970-01-01),
// exposed so main.cpp's Gantt renderer can turn a calendar date into an
// x-pixel offset (and a drag's pixel delta back into a day count) without
// duplicating the same civil-calendar arithmetic ParseOrgOutline/
// ShiftTimestamp already use internally.
/**
 * @brief Converts a proleptic-Gregorian calendar date to a day ordinal (day 0 = 1970-01-01).
 * @param year the calendar year
 * @param month the calendar month (1-12)
 * @param day the calendar day of month
 * @return the day ordinal
 */
long long OrgDayNumber(int year, int month, int day);
/**
 * @brief Converts a day ordinal (day 0 = 1970-01-01) back to a proleptic-Gregorian calendar date.
 * @param day_number the day ordinal to convert
 * @param year set to the resulting calendar year
 * @param month set to the resulting calendar month (1-12)
 * @param day set to the resulting calendar day of month
 */
void OrgDateFromDayNumber(long long day_number, int &year, int &month, int &day);

// Locates the first "#+TODO:" line (ParseOrgOutline only ever honors the
// first one -- see this header's top comment), or -1 if `lines` has none.
// Exposed so a Kanban column add/rename/delete can find the line to
// rewrite (or know it needs to insert one) without re-implementing
// ParseTodoLine's own prefix check.
/**
 * @brief Locates the first "#+TODO:" line in the file (the only one ParseOrgOutline honors).
 * @param lines the full text of the org file, one entry per line
 * @return the index of the first "#+TODO:" line, or -1 if `lines` has none
 */
int FindOrgTodoLineIndex(const std::vector<std::string> &lines);

// Inverse of the "#+TODO:" line parsing folded into ParseOrgOutline --
// builds "#+TODO: kw1 kw2 | kw3" from separate todo/done keyword lists.
// Deliberately never emits "(x)" fast-select hints: ParseTodoLine tolerates
// their absence, OrgOutline never retained them, and every rewrite helper
// above compares keyword tokens without them -- so a keyword list that came
// from a file using fast-select hints loses them once rewritten through
// here (a documented gap, not an oversight).
/**
 * @brief Builds a "#+TODO: kw1 kw2 | kw3" line from separate todo/done keyword lists.
 * @param todo_keywords the TODO-side keywords, emitted before the '|'
 * @param done_keywords the DONE-side keywords, emitted after the '|'
 * @return the formatted "#+TODO:" line (never includes "(x)" fast-select hints)
 */
std::string FormatTodoLine(const std::vector<std::string> &todo_keywords, const std::vector<std::string> &done_keywords);

// Activity-bar Todo panel <-> org file sync (kBuiltinActivityBar, main.cpp;
// Editor::ActivityTodoLoad/ActivityTodoSave in editor.cpp). The panel is a
// flat checklist; an org file is an outline. The mapping: every headline
// carrying a TODO/DONE keyword (any level) is one checklist item, in
// document order; headlines without a keyword (plain section headers) and
// all body text are invisible to the panel but preserved verbatim.
struct OrgTodoItem {
    bool done = false;    // classified against the file's TODO/DONE keyword split
    std::string text;     // the headline title (keyword, priority and tags stripped)
    int line = -1;        // 0-based index of the source headline line; -1 = not in the file yet
    int level = 1;        // headline depth, so the panel can indent subtasks
    std::string keyword;  // the headline's current keyword ("" for a not-yet-written item)
};

// Archived headlines -- an ":ARCHIVE:" tag on the headline itself or on
// any ancestor, org's own tag-inheritance rule for that tag -- are
// invisible to the panel exactly like plain (keywordless) headlines:
// OrgTodoListItems never lists them and OrgTodoListApply never touches
// them (so a checklist loaded without them can't delete them as
// "unreferenced" on the next save). The sidebar's 'A' key adds the tag
// through OrgTodoListArchive.
constexpr const char *kOrgArchiveTag = "ARCHIVE";

/**
 * @brief Lists every keyworded, non-archived headline in an org file as a flat checklist item.
 * @param lines the full text of the org file, one entry per line
 * @return one OrgTodoItem per headline with a TODO/DONE keyword outside any :ARCHIVE: subtree, in document order
 */
std::vector<OrgTodoItem> OrgTodoListItems(const std::vector<std::string> &lines);

// Writes a checklist back into the org text it was loaded from, as a set
// of surgical edits rather than a regeneration -- everything the panel
// can't represent (body text, properties, plain headlines, the "#+TODO:"
// line) survives untouched:
//   - an item whose `line` names a keyworded headline and whose `done`
//     differs from that headline's keyword gets its keyword token rewritten
//     (RewriteHeadlineKeyword) to the file's first DONE keyword (done) or
//     first TODO keyword (not done). A headline already on the right side
//     of the split keeps its exact keyword (a DOING item toggled "not
//     done" stays DOING).
//   - a keyworded headline no item refers to is removed along with its
//     whole subtree (body + child headlines), org's own subtree semantics:
//     "clear done" on a DONE parent takes its children with it.
//   - an item with `line == -1` becomes a new level-1 headline appended at
//     the end of the file, "* TODO text" (or the first DONE keyword if
//     `done` is already set).
//   - an item whose `line` no longer names a keyworded headline (the file
//     changed underneath the panel) is ignored rather than re-appended, so
//     a stale panel can't duplicate entries.
// Item `text` is never written back for existing headlines: the panel has
// no rename affordance, and retitling would discard priority cookies etc.
/**
 * @brief Applies a checklist's done-states/additions/removals to org text as minimal line edits.
 * @param lines the current full text of the org file
 * @param items the checklist to reconcile the file with (see the rules above)
 * @return the rewritten file lines (equal to `lines` when nothing changed)
 */
std::vector<std::string> OrgTodoListApply(const std::vector<std::string> &lines, const std::vector<OrgTodoItem> &items);

// The one write-back OrgTodoListApply deliberately never does: retitle
// the keyworded headline at 0-based index `line`, keeping its stars,
// keyword, priority cookie and tags (FormatHeadlineLine over the parsed
// parts). Used by the Todo sidebar's 'e' key. Returns `lines` unchanged
// when `line` doesn't name a keyworded headline (a stale panel, same rule
// as OrgTodoListApply) or `new_title` is empty.
/**
 * @brief Rewrites the title of the keyworded headline at a 0-based line index, preserving keyword, priority and tags.
 * @param lines the current full text of the org file
 * @param line the 0-based index of the headline to retitle
 * @param new_title the replacement title text
 * @return the rewritten file lines (equal to `lines` when `line` isn't a keyworded headline or `new_title` is empty)
 */
std::vector<std::string> OrgTodoListRetitle(const std::vector<std::string> &lines, int line, const std::string &new_title);

// The sidebar's 'A' key: tag the keyworded headline at 0-based index
// `line` with ":ARCHIVE:" (appended after any tags it already has, via
// FormatHeadlineLine over the parsed parts), which drops it -- and its
// whole subtree -- out of the checklist from the next OrgTodoListItems
// on, then relocates that whole subtree (headline, body, children) to
// the end of the file so archived items sink to the bottom instead of
// sitting where they were. Returns `lines` unchanged when `line` doesn't
// name a keyworded headline or it already carries the tag.
/**
 * @brief Adds the :ARCHIVE: tag to the keyworded headline at a 0-based line index and moves its whole subtree to the end of the file.
 * @param lines the current full text of the org file
 * @param line the 0-based index of the headline to archive
 * @return the rewritten file lines (equal to `lines` when `line` isn't a keyworded headline or is already archived)
 */
std::vector<std::string> OrgTodoListArchive(const std::vector<std::string> &lines, int line);

// Reorders the checklist: swaps the keyworded headline at 0-based index
// `line` with its previous (`delta < 0`) or next (`delta > 0`) sibling --
// the nearest other headline sharing the same parent, skipping over any
// deeper-level descendants in between (org's own "move subtree past a
// sibling" semantics; a headline never moves in or out of a different
// parent's children this way). Whole subtrees swap together: body,
// properties, clock history, children. Returns `lines` unchanged, with
// `*new_line` left at `line`, when there is no such sibling or `line`
// doesn't name a keyworded headline.
/**
 * @brief Swaps the keyworded headline at a 0-based line index with its previous or next sibling subtree.
 * @param lines the current full text of the org file
 * @param line the 0-based index of the headline to move
 * @param delta -1 to swap with the previous sibling, +1 for the next
 * @param new_line set to the moved headline's line index afterward (or left at `line` if it didn't move)
 * @return the rewritten file lines (equal to `lines` when nothing changed)
 */
std::vector<std::string> OrgTodoListMove(const std::vector<std::string> &lines, int line, int delta, int *new_line);

// --- Clocking (Todo sidebar's Enter = start/stop, kBuiltinActivityBar) ---
// Same on-disk convention as Editor::OrgClockIn/OrgClockOut (editor.cpp,
// the in-buffer :MepOrgClockIn/Out commands): an open clock is a
// "CLOCK: [YYYY-MM-DD Day HH:MM]" line with nothing after the bracket,
// inside a :LOGBOOK: drawer under its headline; stopping it appends
// "--[end] =>  H:MM". These operate on plain lines so the sidebar can
// clock a headline in a file that isn't the current buffer (or open at
// all). Timestamp bodies are the "YYYY-MM-DD Day HH:MM" text between the
// brackets, as editor.cpp's FormatOrgTimestampNow produces -- the weekday
// is ignored when parsing.
struct OrgOpenClock {
    int line = -1;           // 0-based index of the open CLOCK line, -1 if none
    int headline_line = -1;  // 0-based index of the nearest headline above it (-1 if none)
    std::string start_ts;    // the bracketed start timestamp body
};

/**
 * @brief Checks whether a line is an open (not-yet-closed) org CLOCK entry, i.e. `CLOCK: [timestamp]`.
 * @param line the line to check
 * @param start_ts if non-null, set to the bracketed timestamp body on success
 * @return true if the line is an open clock line
 */
bool OrgMatchOpenClockLine(const std::string &line, std::string *start_ts);

/**
 * @brief Parses a `YYYY-MM-DD Day HH:MM` org timestamp body (weekday ignored, unanchored scan).
 * @param s the text to scan
 * @param y set to the year on success
 * @param mo set to the month on success
 * @param d set to the day on success
 * @param hh set to the hour on success
 * @param mm set to the minute on success
 * @return true if a timestamp was found
 */
bool OrgParseClockTimestamp(const std::string &s, int *y, int *mo, int *d, int *hh, int *mm);

/**
 * @brief Finds the first open CLOCK line in an org file and the headline it belongs to.
 * @param lines the full text of the org file
 * @return the open clock (line == -1 when none is running)
 */
OrgOpenClock OrgFindOpenClock(const std::vector<std::string> &lines);

// --- Org tables: the rendered display layout (Editor::OrgTableWrapScan) ---
// A table is *rendered* narrower than it is stored, for either of two
// reasons: its aligned width runs past `:set textwidth`, so the columns
// are re-budgeted to fit the line width and any cell too long for its
// column wraps onto continuation lines (one stored row drawing as
// several); or its cells are simply narrower drawn than stored, because
// concealment stood their link markup down to a description, and laying
// the columns out on the rendered widths closes the gutter the file's
// own padding leaves behind. The caller decides which applies -- the
// planner reports the first as `wrapped` and the second falls out of
// comparing its layout against the stored rows. Display-only,
// deliberately:
// wrapping in the file would be valid org (a continuation row is an
// ordinary row with an empty first cell, which is what Emacs'
// `org-table-wrap-region` writes) but it is a *semantic* edit -- it adds
// rows, so spreadsheet references like `@3$2` shift under it, and a
// continuation row can never be reliably told back apart from a row whose
// first cell is genuinely empty, making the wrap one-way. Nothing here
// touches a buffer; it hands back strings for the renderer to draw.
//
// Pure functions over already-parsed cells (the `|`-splitting itself stays
// in editor.cpp, which had it first) so the whole width/wrap policy is
// testable without a GL context -- org_doc_test.cpp.

// One link inside a table cell, as it lands in the text the cell is
// *planned* from -- which is the cell as drawn, so a bracket link
// concealed down to its description spans the description and not the
// markup (byte offsets, half-open, the same convention OrgLinkSpanInfo
// uses).
struct OrgTableCellLink {
    int start = 0;
    int end = 0;
    std::string target;
    // True when the span is a description standing in for hidden markup,
    // false when it is the link's own raw text (a bare URL, or a bracket
    // link with concealment off). Editor::OrgLinkScan underlines the
    // second and not the first -- the underline is what tells you an
    // unconcealed run of text is a link -- and the table layout has to
    // make the same distinction or the two renderers disagree about what
    // the same link looks like.
    bool concealed = false;
};

// A table cell as it is drawn: every `[[target][description]]` on it
// stood down to `description` when `conceal` is set, and every bare
// `http(s)://...` left as its own text either way -- exactly what
// Editor::OrgLinkScan conceals and leaves alone on an ordinary row.
//
// This is what the wrap planner below has to be fed, and the reason this
// exists at all: a cell measured and wrapped as its raw markup is
// budgeted columns for characters that never reach the screen, so a
// table of `[[file:docs/lua-api.org][Lua API]]` links is re-budgeted as
// if its first column held fifty columns of text instead of the seven it
// draws as -- wrapping a table that fits, and hard-splitting the URL
// inside the markup across two rendered lines when it really doesn't.
// Link formatting comes first; the columns are then laid out around what
// it leaves.
/**
 * @brief Renders a table cell as drawn, standing each bracket link down to its description.
 * @param cell the cell's stored text, already trimmed
 * @param conceal true when markup concealment is on (Editor::OrgConcealVisible)
 * @param links optional; set to each link's span in the returned text, in column order
 * @return the cell's display text (`cell` itself when it holds no links)
 */
std::string OrgTableCellDisplayText(const std::string &cell, bool conceal, std::vector<OrgTableCellLink> *links);

// One table row as the planner sees it: either a `|---+---|` rule or a
// row of trimmed cell texts.
struct OrgTableCells {
    bool is_sep = false;
    std::vector<std::string> cells;
    // Links inside `cells`, one entry per cell, as spans into that
    // cell's own text (OrgTableCellDisplayText's output). Left short or
    // empty by a caller with no links to report -- a cell index past the
    // end of this simply carries none.
    std::vector<std::vector<OrgTableCellLink>> links;
};

// One link on a rendered line, in that line's own columns: the planner
// carries each cell's links through the wrap so the renderer has
// something to style and follow. A link whose description wraps onto two
// lines is reported once per line, over just the part that landed there.
struct OrgTableWrapLink {
    int col_start = 0;  // byte offset into the line's text
    int col_end = 0;
    std::string target;
    bool concealed = false;  // see OrgTableCellLink::concealed
};

// One line a stored row draws as: its text, plus the links on it. A
// wrapped row's own text never reaches the screen and its decorations
// are skipped (see Buffer::org_table_wrap_rows, editor.h), so anything
// the renderer needs to style has to arrive here.
struct OrgTableWrapLine {
    std::string text;
    std::vector<OrgTableWrapLink> links;
};

struct OrgTableWrapPlan {
    // True when the table did not fit the budget and its columns had to
    // be re-budgeted (so some cell wrapped onto continuation lines).
    // False does *not* mean the layout is unusable: `col_widths`/`rows`
    // always hold a complete one, at the table's natural rendered
    // widths, which is exactly what a caller wanting to close a
    // concealed table's gutter draws. It only means nothing had to give.
    bool wrapped = false;
    std::vector<int> col_widths;                      // content columns, excluding each cell's ` ` padding
    std::vector<std::vector<OrgTableWrapLine>> rows;  // per input row, the line(s) it draws as
};

/**
 * @brief Measures a string in display columns (one per codepoint), not bytes.
 * @param s the text to measure
 * @return the number of codepoints in `s`
 */
int OrgTableDisplayWidth(const std::string &s);

// Greedy word wrap at `width` display columns: breaks on spaces, and only
// splits a word mid-way when the word alone is wider than the column.
// Always returns at least one (possibly empty) line, so a blank cell still
// occupies its row.
/**
 * @brief Wraps text to a column width, breaking on spaces and splitting only over-wide words.
 * @param text the cell text to wrap
 * @param width the target width in display columns (values below 1 are treated as 1)
 * @return the wrapped lines, never empty
 */
std::vector<std::string> OrgTableWrapCell(const std::string &text, int width);

// Chooses column widths for a table rendered within `budget` display
// columns (`:set textwidth`), counting the `indent` the table's leading
// `|` sits at plus the `| ` / ` | ` chrome every row carries.
//
// Columns narrower than an equal share keep their natural width and only
// the wide ones give up columns (a water-filling split, so a table of one
// long prose column beside three short ones spends the budget on the
// prose rather than shaving all four evenly). A column is never widened
// past its own longest cell, and never shrunk below kOrgTableMinColWidth
// unless the budget leaves no choice.
/**
 * @brief Plans a table's rendered column widths and per-row wrapped lines for a line-width budget.
 * @param rows the table's parsed rows, in order
 * @param budget the total rendered width to fit, in display columns (`:set textwidth`)
 * @param indent the display column the table's leading `|` sits at
 * @return the plan; `wrapped` is false when the table already fits
 */
OrgTableWrapPlan PlanOrgTableWrap(const std::vector<OrgTableCells> &rows, int budget, int indent);

// The narrowest a column is squeezed to while any wider one still has
// columns to give up -- below this a prose cell wraps to one or two words
// per line and reads worse than a table running past the margin.
constexpr int kOrgTableMinColWidth = 6;

// Inserts "  CLOCK: [now_ts]" under the headline at 0-based `headline_line`:
// at the top of its existing :LOGBOOK: drawer, else in a new drawer right
// after the headline's planning line and :PROPERTIES: drawer (if any),
// org's own drawer order. Returns `lines` unchanged when `headline_line`
// isn't a headline or a clock is already open anywhere in the file (only
// one may run at a time, same rule as Editor::OrgClockIn).
/**
 * @brief Starts a clock under a headline by inserting an open CLOCK line into its LOGBOOK drawer.
 * @param lines the current full text of the org file
 * @param headline_line the 0-based index of the headline to clock
 * @param now_ts the start timestamp body to write, e.g. "2026-09-05 Sat 10:00"
 * @return the rewritten file lines (equal to `lines` if nothing could be started)
 */
std::vector<std::string> OrgClockStartLines(const std::vector<std::string> &lines, int headline_line,
                                            const std::string &now_ts);

// Closes the first open CLOCK line as "CLOCK: [start]--[now_ts] =>  H:MM";
// `minutes` (if non-null) receives the elapsed whole minutes (clamped at
// 0). Returns `lines` unchanged, with `minutes` = -1, when none is open.
/**
 * @brief Stops the first open clock in an org file, writing its end timestamp and duration.
 * @param lines the current full text of the org file
 * @param now_ts the end timestamp body to write
 * @param minutes if non-null, set to the elapsed whole minutes (-1 when no clock was open)
 * @return the rewritten file lines (equal to `lines` when no clock was open)
 */
std::vector<std::string> OrgClockStopLines(const std::vector<std::string> &lines, const std::string &now_ts,
                                           int *minutes);

// --- Org inline images: the drawn figure's geometry -------------------
// (Buffer::org_image_rows / Editor::OrgImagesVisible(), DrawPane in
// main.cpp)
//
// An inline image is laid out against org's own text column -- `:set
// textwidth`, the same measure a LaTeX export's \linewidth stands for
// and the same one an org block card already sizes itself to -- not
// against the pane, so widening a split doesn't blow every figure up to
// fill it. Within that column a figure is drawn at
// kOrgImageWidthFraction of it, or at its own native pixel size if that
// is smaller (an 80px icon stays an 80px icon rather than being
// upscaled into a blurry banner), and centered in it.
//
// The vertical room it claims follows from that drawn height rather than
// being a fixed budget the image is letterboxed inside -- a wide, short
// plot reserves a few line-heights, a tall portrait reserves many, and
// neither leaves empty rows above or below. That makes the slot count
// per-image data, which the four walkers that must agree on a row's
// height -- DrawPane's row loop, its block-card slot walk, its cursor-Y
// lookup (all main.cpp) and Editor::UpdateScrollForPane (editor.cpp) --
// all read back through Editor::OrgImageLayoutForRow, the same
// agreement org LaTeX fragments' own per-entry `slots` is under.

// The text width assumed when `:set textwidth` is off (0) -- org's own
// conventional measure, and this setting's own default.
constexpr int kOrgImageLineWidthChars = 80;
constexpr float kOrgImageWidthFraction = 0.8f;
// Ceiling on one figure's height, in line-heights: past this the image
// is scaled down further (not cropped or letterboxed, so "no dead space"
// still holds) so a single very tall portrait can't claim several
// screenfuls of scroll on its own.
constexpr int kOrgImageMaxSlots = 40;
// What a row claims when the image's own size isn't known yet (its
// header couldn't be sniffed -- see image_codec::Dimensions): enough to
// show something without punching a screen-tall hole in the buffer.
constexpr int kOrgImageUnknownSlots = 8;

// Where and how big one inline image is drawn, plus the vertical room it
// reserves. `offset_x` is measured from the text column's left edge (the
// same x a plain row's first character starts at).
struct OrgImageLayout {
    float width = 0.0f;
    float height = 0.0f;
    float offset_x = 0.0f;
    int slots = 1;
};

/**
 * @brief Lays out one org inline image: its drawn size, its centering offset, and the
 * line-heights it reserves.
 * @param px_w the image's native pixel width (<=0 if unknown)
 * @param px_h the image's native pixel height (<=0 if unknown)
 * @param char_width the renderer's monospace advance width, in pixels
 * @param line_height the renderer's line height, in pixels
 * @param avail_cols how many columns of text actually fit in the pane (Pane::text_cols)
 * @param text_cols org's text width in columns (`:set textwidth`; <=0 falls back to
 * kOrgImageLineWidthChars)
 * @return the layout; `slots` is what every slot-counting site must reserve for the row
 */
OrgImageLayout OrgImageLayoutFor(int px_w, int px_h, float char_width, float line_height, int avail_cols,
                                 int text_cols);

// --- Per-src-block language-server status (<leader>ots) ---
// One `#+begin_src` block's language-server state, rendered as a line of
// text along the bottom edge of the block's card (DrawPane, main.cpp),
// under a rule separating it from the code above.
//
// The facts themselves can only come from Lua: the org->LSP bridge
// (kBuiltinOrgPolyglot, main.cpp) owns the shadow files, the per-block
// clients and the server registry, none of which C++ has a view of. Lua
// reports them per block (mep.buf_set_org_lsp_status) and this pure half
// turns them into the one line actually drawn -- kept here rather than in
// the Lua chunk or inline in DrawPane so the wording, the pluralization
// and the severity ranking are testable without a GL context
// (org_doc_test.cpp).
enum class OrgLspState {
    // The block has no language tag, no org-babel entry, or no LSP server
    // registered for its language -- nothing will ever attach.
    kUnsupported,
    // A server is registered, but this block has no client yet. The bridge
    // attaches lazily (the first time an LSP feature runs inside the block),
    // so this is the resting state of an untouched block, not a failure.
    kIdle,
    // Client process spawned; its `initialize` response hasn't come back.
    kStarting,
    // Initialized and answering.
    kReady,
    // A client was started for this block and is gone now -- the process
    // exited, or the spawn itself failed.
    kExited,
};

struct OrgLspStatus {
    OrgLspState state = OrgLspState::kUnsupported;
    // `#+begin_src <lang>`'s language tag as written, "" when the block has
    // none (which is itself a reason for kUnsupported).
    std::string lang;
    // The mep.lsp_servers registry key that resolved for `lang`
    // ("pyright", "clangd", ...), "" when none did.
    std::string server;
    // Diagnostics whose range starts on one of the block's body rows,
    // already translated back to org line numbers by the bridge. `hints`
    // merges LSP severity 3 (Information) and 4 (Hint), which a one-line
    // status has no room to tell apart.
    int errors = 0;
    int warnings = 0;
    int hints = 0;
};

// How loudly the line is drawn. The counts outrank the state: a `ready`
// server reporting three errors is an error line, not a healthy one.
enum class OrgLspStatusTone { kMuted, kOk, kWarn, kError };

/**
 * @brief Renders one block's language-server status as the single line drawn under its code.
 * @param st The block's reported state, server and diagnostic counts.
 * @return The status line; never empty.
 */
std::string FormatOrgLspStatus(const OrgLspStatus &st);

/**
 * @brief Picks the color bucket a status line is drawn in.
 * @param st The block's reported state, server and diagnostic counts.
 * @return kError/kWarn when diagnostics or a dead client say so, kOk for a healthy attached server, kMuted otherwise.
 */
OrgLspStatusTone OrgLspStatusToneOf(const OrgLspStatus &st);

/**
 * @brief Maps the bridge's state name ("idle", "starting", "ready", "exited") to its enum.
 * @param name The state name as Lua reports it; anything unrecognized is kUnsupported.
 * @return The parsed state.
 */
OrgLspState OrgLspStateFromName(const std::string &name);

// The play button a `#+begin_src` card's title bar carries (DrawPane,
// main.cpp): clicking it runs that block through the same org-babel
// machinery C-c C-c / `:MepOrgBabelExecute` already drives.
//
// Whether the button is there at all -- and whether it is drawn live or
// muted -- is decided from the block's own header, which is the one part
// of this worth testing without a GL context: the rules are exactly the
// ones the *execution* path enforces (a language it can resolve, and org's
// own `:eval` gate), so the button must not promise a run that
// mep.org_babel_execute would then refuse.
enum class OrgBlockPlay {
    // Not a src block (`quote`, `example`, `export`, ...), or one still
    // being typed with no `#+end_src` yet -- there is nothing to run.
    kHidden,
    // A src block whose run would fail or be refused before it started:
    // no language tag, or `:eval no`/`:eval never`. Drawn muted, and still
    // clickable -- the click produces the warn toast that says which of
    // the two it is, which is more use than a control that does nothing.
    kDisabled,
    // Runnable as far as the header can tell. Whether the interpreter is
    // actually on $PATH is mep_org_babel_resolve_lang's business, at click
    // time; the card is drawn once per frame and must not shell out.
    kReady,
};

// One block's header as the play button reads it.
struct OrgBlockPlayInput {
    bool is_src = false;
    // A `#+end_src` was found for this block (OrgBlockCard::end_row >= 0).
    bool closed = false;
    // `#+begin_src <lang>`'s tag as written ("python", "C++"), "" when the
    // block has none.
    std::string lang;
    // The `:eval` header arg's value, "" when the block has no `:eval`.
    // Only `no`/`never` disable the button, matching the check in
    // mep.org_babel_execute: `no-export`/`query` and friends still run
    // when the user asks for this one block by hand.
    std::string eval_arg;
};

/**
 * @brief Decides whether a block card draws a play button, and whether it is live or muted.
 * @param in The block's header as parsed for its card.
 * @return kHidden for anything unrunnable by kind, kDisabled for a src block whose run would be refused, kReady otherwise.
 */
OrgBlockPlay OrgBlockPlayFor(const OrgBlockPlayInput &in);

/**
 * @brief Renders the play button's hover tooltip: what a click will do, or why it won't.
 * @param in The block's header as parsed for its card.
 * @return The tooltip text; "" when the block has no button (kHidden).
 */
std::string OrgBlockPlayHint(const OrgBlockPlayInput &in);


// --- Block settings (the card's gear button, Mode::OrgBlockSettings) ------
//
// The settings popup a block card's gear button opens: every header
// argument org honors for that block, each with the kind of input it
// actually takes, and the two pure operations the popup needs -- reading
// one option's current value out of a block's header, and writing a new
// one back into the line it lives on.
//
// All of it is here rather than in the editor for the usual reason the
// org stack splits this way: the catalog and the line rewriting are
// decidable from text alone, so they can be tested without a GL context
// (org_doc_test.cpp), leaving editor.cpp with the mode's state machine
// and main.cpp with the drawing.
enum class OrgHeaderArgKind {
    // One of a fixed list of words. The list always starts with "",
    // which means "not written on this block at all" -- clearing an
    // option and setting it to org's own default are different things
    // (`:cache no` is not the same as no `:cache` when a `#+PROPERTY:`
    // sets one file-wide), so the popup can express both.
    kChoice,
    // A number, stepped by `step` and clamped to [min_value, max_value].
    kNumber,
    // Free text (a filename, a session name, a compiler flag list).
    kText,
    // A bare `-n`/`+n`/`-r`/`-k` block switch rather than a `:key value`
    // argument: written in the switch region between the block's
    // language and its first `:key`, and either present or absent.
    kSwitch,
};

// One row of the settings popup: what to show, what kind of input it
// takes, and which header argument it writes.
struct OrgHeaderArgSpec {
    // Section heading drawn above this row, "" to continue the previous
    // section. Purely a display grouping -- navigation skips headings.
    std::string section;
    // The header-arg key without its colon ("results", "tangle"), or the
    // switch token itself ("-n") for kSwitch.
    std::string key;
    // The row's label, which is not always ":" + key: the four `:results`
    // rows all write the same key and say which part of it they own.
    std::string label;
    OrgHeaderArgKind kind = OrgHeaderArgKind::kText;
    // kChoice/kSwitch: the values offered, in cycle order, "" first.
    std::vector<std::string> choices;
    // kNumber only. `default_value` is where the first j/k press lands
    // when the option isn't written on the block yet -- the value org (or
    // the language's own plotting defaults) would have used anyway, so
    // stepping an unset option starts somewhere sensible instead of at 0.
    double min_value = 0.0;
    double max_value = 0.0;
    double step = 1.0;
    double default_value = 0.0;
    // For the four `:results` rows: which class of org's up-to-four
    // space-separated `:results` words this row owns ("collection",
    // "type", "format", "handling"). "" for every other spec, which owns
    // its key's whole value.
    std::string facet;
    // One line under the list explaining what the focused option does.
    std::string hint;
};

/**
 * @brief Reports whether a block kind has header arguments worth a settings popup.
 * @param block_kind The lowercased block word ("src", "example", "quote", ...).
 * @return True for the kinds org reads options from (`src` and `example`), false otherwise.
 */
bool OrgBlockHasSettings(const std::string &block_kind);

/**
 * @brief Builds the settings popup's rows for one block: the common header args, then the
 * ones only that language takes.
 * @param block_kind The lowercased block word ("src", "example", ...).
 * @param lang The block's language tag as written ("python", "C++"), "" when it has none.
 * @return The specs in display order; empty when the kind has no settings (OrgBlockHasSettings).
 */
std::vector<OrgHeaderArgSpec> OrgHeaderArgSpecsFor(const std::string &block_kind, const std::string &lang);

/**
 * @brief Classifies one `:results` word into the facet it belongs to.
 * @param word A single `:results` word ("output", "table", "raw", "silent").
 * @return "collection", "type", "format", "handling", or "" for a word org doesn't know.
 */
std::string OrgResultsFacetOf(const std::string &word);

/**
 * @brief Reads one facet out of a whole `:results` value.
 * @param results The block's `:results` value ("output table replace"), "" when unset.
 * @param facet The facet wanted ("collection", "type", "format", "handling").
 * @return That facet's word, or "" when the value names none.
 */
std::string OrgResultsFacetValue(const std::string &results, const std::string &facet);

/**
 * @brief Returns a `:results` value with one facet replaced (or removed), the other words kept in place.
 * @param results The current `:results` value, "" when unset.
 * @param facet The facet to write ("collection", "type", "format", "handling").
 * @param word The word to put there, or "" to drop that facet entirely.
 * @return The rewritten value; "" when nothing is left, which the caller writes as "remove :results".
 */
std::string OrgResultsWithFacet(const std::string &results, const std::string &facet, const std::string &word);

/**
 * @brief Quotes a header-arg value if it needs it, so it survives the reparse as one value.
 * @param value The raw value text.
 * @return The value, wrapped in double quotes when it contains a token-boundary colon that
 * would otherwise be read as the start of the next argument; unchanged otherwise.
 */
std::string OrgQuoteHeaderArgValue(const std::string &value);

/**
 * @brief Formats a number as a header-arg value, without a trailing ".0".
 * @param v The value.
 * @return Its shortest exact-enough spelling ("7", "6.5").
 */
std::string OrgFormatHeaderArgNumber(double v);

/**
 * @brief Steps a numeric option's value up or down, clamped to the spec's range.
 * @param spec The option's spec (supplies step/min/max and the starting point for an unset value).
 * @param value The current value as written, "" when the option is unset.
 * @param direction +1 to increment, -1 to decrement.
 * @return The new value, formatted the way it will be written.
 */
std::string OrgStepHeaderArgNumber(const OrgHeaderArgSpec &spec, const std::string &value, int direction);

/**
 * @brief Writes one `:key value` header argument into a block header line, in place.
 * @param line The `#+begin_...` or `#+HEADER:` line to rewrite.
 * @param key The header-arg key without its colon.
 * @param value The value to write, or "" to remove the argument entirely.
 * @return The rewritten line. An argument already on the line is replaced where it sits (the
 * rest of the line is left byte-for-byte alone); a new one is appended at the end.
 */
std::string OrgSetHeaderArgOnLine(const std::string &line, const std::string &key, const std::string &value);

/**
 * @brief Reports whether a block header line already carries a given bare switch.
 * @param line The `#+begin_...` line.
 * @param sw The switch token ("-n", "+n", "-r", "-k").
 * @return True when the switch is present in the line's switch region.
 */
bool OrgBlockLineHasSwitch(const std::string &line, const std::string &sw);

/**
 * @brief Adds or removes a bare block switch on a `#+begin_...` line.
 * @param line The line to rewrite.
 * @param sw The switch token ("-n", "+n", "-r", "-k").
 * @param on True to add it (if absent), false to remove it (if present).
 * @return The rewritten line; a switch is added after the block's language and before its
 * first `:key` argument, which is where org reads switches from.
 */
std::string OrgSetBlockSwitchOnLine(const std::string &line, const std::string &sw, bool on);

// --- Babel header arguments: the values the *execution* path reads -------
//
// The settings popup writes header arguments with the full org grammar
// (multi-word values, quoted values, `#+HEADER:` lines), but babel used
// to read them back with a single-whitespace-token matcher over the
// `#+begin_src` line alone -- so `:dir ~/my notes`, `:flags -O2 -Wall`
// and anything written onto a `#+HEADER:` line were silently ignored.
// These are the readers that close that gap; they take the *arguments*
// text (what follows the language tag), not a whole line, so the same
// code serves a `#+begin_src` line, a `#+HEADER:` line and a
// `#+PROPERTY: header-args` line.

/**
 * @brief Reads one header argument's value out of a header-args string.
 * @param args The arguments text (everything after the block's language tag).
 * @param key The key to read, without its leading colon, matched case-insensitively.
 * @return The value with surrounding whitespace and one layer of matching quotes removed;
 * "" both for an absent key and for a bare valueless one (use OrgHeaderArgPresent to tell them apart).
 */
std::string OrgHeaderArgValue(const std::string &args, const std::string &key);

/**
 * @brief Reports whether a header-args string mentions a key at all.
 * @param args The arguments text.
 * @param key The key to look for, without its leading colon.
 * @return True when the key is written, whatever its value.
 */
bool OrgHeaderArgPresent(const std::string &args, const std::string &key);

/**
 * @brief Splits every `:key value` pair out of a header-args string, in written order.
 * @param args The arguments text.
 * @return The pairs, keys lowercased and values unquoted; repeated keys are kept (`:var` is
 * accumulated by org, and the caller decides later-wins for the rest).
 */
std::vector<std::pair<std::string, std::string>> OrgHeaderArgPairs(const std::string &args);

/**
 * @brief Merges header-args strings in increasing precedence order into one args string.
 * @param layers The args texts, least-significant first (`#+PROPERTY:`, then `#+HEADER:`
 * lines in document order, then the `#+begin_src` line's own).
 * @return One args string carrying the winning value for every key, with every `:var`
 * from every layer preserved.
 */
std::string OrgMergeHeaderArgs(const std::vector<std::string> &layers);

// --- Shell expansion inside a header argument ----------------------------
//
// `:flags $(pkg-config --cflags datamunge)` is the only practical way to
// give a compiled block the include/library flags a real project is
// described by, so the compile/run argument lists (`:flags`, `:libs`,
// `:cmdline`) run their value through the expansion below before it is
// split into words. Org itself spells this with an Emacs-Lisp value; mep
// has no Emacs, so it spells it the way the shell does.
//
// What expands: `$(command)` and `` `command` `` (command substitution,
// nesting-aware for the former), `${NAME}` and `$NAME` (environment
// variables, from mep's own environment). A backslash before `$`, a
// backtick or another backslash escapes it, and nothing else is touched --
// quotes in particular are left exactly where they are, since a header
// argument's own quoting was already resolved by OrgHeaderArgValue.

/**
 * @brief The callback OrgExpandShellSubstitutions runs a `$(...)`/backtick command through.
 * @param command The command text between the delimiters, unexpanded.
 * @return The command's standard output; whatever a failed command produced is used as-is.
 */
using OrgShellRunner = std::function<std::string(const std::string &command)>;

/**
 * @brief Reports whether a header-argument value contains anything the shell expander would act on.
 * @param value The header argument's value.
 * @return True when a `$` or a backtick appears unescaped, i.e. when expanding could change the value.
 */
bool OrgValueNeedsShellExpansion(const std::string &value);

/**
 * @brief Expands command substitutions and environment variables in a header-argument value.
 * @param value The header argument's value, as OrgHeaderArgValue returned it.
 * @param run Runs one command and returns its standard output (trailing newlines are trimmed
 * and interior newlines/tabs become spaces, so the result stays one line of words).
 * @return The value with every `$(...)`, backtick and `$NAME` replaced; escapes resolved.
 */
std::string OrgExpandShellSubstitutions(const std::string &value, const OrgShellRunner &run);

// --- Results blocks: what a finished run actually writes back ------------
//
// Org's `:results` value is up to four independent words plus `:wrap`,
// and only two of them (`table`, `graphics`) used to reach the writer.
// This turns the whole set into the literal lines that follow a
// `#+RESULTS:` keyword.
struct OrgResultsOptions {
    // The four `:results` facets, as OrgResultsFacetValue reads them.
    std::string collection;  // "value" / "output" / ""
    std::string type;        // "table" / "list" / "scalar" / "verbatim" / "file" / ""
    std::string format;      // "raw" / "org" / "html" / "latex" / "code" / "pp" / "drawer" / "link" / "graphics" / ""
    std::string handling;    // "replace" / "silent" / "append" / "prepend" / "none" / ""
    // `:wrap`'s value: the block word (plus any arguments) the results
    // are fenced in, e.g. "example" or "src html". Beats `:format`.
    std::string wrap;
    // The block's own language, used as the fence language by
    // `:results code` when `:wrap` didn't name one.
    std::string lang;
    // `:sep`'s value: the field separator a `:results table` run's output
    // is split on. "" auto-detects tab, then comma, the way it always did.
    std::string sep;
    // `:colnames no` drops the `|---+---|` rule a table result otherwise
    // grows under its first row.
    bool colnames = true;
    // The output is already org markup that must not be fenced or
    // prefixed -- a `:file` block's own `[[file:...]]` link.
    bool file_link = false;
};

/**
 * @brief Reads the results options out of a block's header arguments.
 * @param args The block's merged header-args text.
 * @param lang The block's language tag, used as `:results code`'s fence language.
 * @return The options, with every facet defaulted to "" (org's own "not written") rather than guessed.
 */
OrgResultsOptions OrgResultsOptionsFrom(const std::string &args, const std::string &lang);

/**
 * @brief Turns a run's raw output lines into the lines written under `#+RESULTS:`.
 * @param out_lines The process's output, one line per entry.
 * @param opts The block's results options.
 * @return The literal lines to insert after the `#+RESULTS:` keyword.
 */
std::vector<std::string> OrgFormatResultsBody(const std::vector<std::string> &out_lines,
                                              const OrgResultsOptions &opts);

/**
 * @brief Reports whether a results body is raw org markup (inserted as-is) rather than a
 * plain `: ` example.
 * @param opts The block's results options.
 * @return True when OrgFormatResultsBody's output must not be re-fenced or re-prefixed by the caller.
 */
bool OrgResultsBodyIsRaw(const OrgResultsOptions &opts);

/**
 * @brief Formats output lines as an org table.
 * @param lines The raw output lines.
 * @param sep The field separator (`:sep`); "" auto-detects a tab, then a comma.
 * @param colnames True to write the `|---+---|` rule under the first row of a multi-row table.
 * @return The table's lines, or `lines` unchanged when no separator could be found.
 */
std::vector<std::string> OrgFormatResultsTable(const std::vector<std::string> &lines, const std::string &sep,
                                               bool colnames);

// --- Noweb references ----------------------------------------------------

/**
 * @brief Expands `<<name>>` noweb references in a block body.
 * @param body The block's body lines.
 * @param blocks Reference name -> the body text that name resolves to (already joined with newlines).
 * @param sep The separator `:noweb-sep` asks for between concatenated blocks; unused here
 * because `blocks` arrives pre-joined, kept so the caller's contract reads the same.
 * @param depth_limit How many levels of reference-inside-reference to follow before giving up.
 * @return The expanded body. A reference on its own line has its indentation applied to every
 * expanded line, the way org does it; an unresolved reference is left exactly as written.
 */
std::vector<std::string> OrgNowebExpand(const std::vector<std::string> &body,
                                        const std::map<std::string, std::string> &blocks, const std::string &sep,
                                        int depth_limit);

/**
 * @brief Decides whether a `:noweb` value asks for expansion in a given context.
 * @param noweb The `:noweb` value as written, "" when unset (org's default is "no").
 * @param context "eval" for an interactive/export run, "tangle" for tangling.
 * @return True when references must be expanded in that context.
 */
bool OrgNowebExpandsIn(const std::string &noweb, const std::string &context);

// --- Block switches: `-n` / `+n` / `-r` / `-k` ---------------------------

// What a block's display switches ask for, read off its `#+begin_` line.
struct OrgBlockSwitches {
    bool number = false;         // `-n` or `+n`
    bool continue_numbers = false;  // `+n`: keep counting from the previous block
    int start = 1;               // `-n 12` starts the count at 12
    bool strip_refs = true;      // `(ref:name)` labels are removed unless `-k` keeps them
};

/**
 * @brief Reads a `#+begin_...` line's display switches.
 * @param line The block's opening line.
 * @return The switches; `number` false when the line carries neither `-n` nor `+n`.
 */
OrgBlockSwitches OrgParseBlockSwitches(const std::string &line);

/**
 * @brief Applies a block's display switches to its body.
 * @param body The block's body lines.
 * @param sw The switches read from its opening line.
 * @param counter In/out: the running line number `+n` continues from, advanced past this block.
 * @return The body as it should be displayed: `(ref:name)` labels stripped unless `-k`, and
 * every line prefixed with a right-aligned number when `-n`/`+n` asked for one.
 */
std::vector<std::string> OrgApplyBlockSwitches(const std::vector<std::string> &body, const OrgBlockSwitches &sw,
                                               int *counter);

// --- Tangling ------------------------------------------------------------

// The tangle-side header arguments, which decide what the written file
// looks like around each block's body rather than what it contains.
struct OrgTangleOptions {
    std::string shebang;    // `:shebang`, written as the file's first line
    std::string mode;       // `:tangle-mode`, e.g. "o755"
    bool mkdirp = false;    // `:mkdirp yes`
    bool padline = true;    // `:padline no` packs blocks with no blank line between them
    std::string comments;   // `:comments` -- "link"/"yes"/"org"/"both"/"noweb"
};

/**
 * @brief Reads the tangle options out of a block's header arguments.
 * @param args The block's merged header-args text.
 * @return The options, defaulted to org's own defaults for anything unwritten.
 */
OrgTangleOptions OrgTangleOptionsFrom(const std::string &args);

/**
 * @brief Parses a `:tangle-mode` value into a filesystem mode.
 * @param mode The value as written ("o755", "#o755", "0755", "755").
 * @return The mode in octal, or -1 when the value isn't one org would accept.
 */
int OrgParseTangleMode(const std::string &mode);

/**
 * @brief Renders the comment lines that precede or follow one tangled block.
 * @param comments The `:comments` value.
 * @param comment_prefix The target language's line-comment marker ("# ", "// ").
 * @param org_file The org file's name, for a `link` comment's back-reference.
 * @param name The block's `#+NAME:`, "" when it has none.
 * @param start_row The block's `#+begin_src` row, 1-based, for a `link` comment.
 * @param opening True for the comment that opens the block, false for the one that closes it.
 * @return The comment lines, empty when `:comments` asks for none.
 */
std::vector<std::string> OrgTangleComment(const std::string &comments, const std::string &comment_prefix,
                                          const std::string &org_file, const std::string &name, int start_row,
                                          bool opening);

// --- `:exports` ----------------------------------------------------------

/**
 * @brief Reports whether an export includes a block's code.
 * @param exports The `:exports` value, "" when unset (org's default is "code").
 * @return True for "code"/"both"/"" , false for "results"/"none".
 */
bool OrgExportsCode(const std::string &exports);

/**
 * @brief Reports whether an export includes a block's results.
 * @param exports The `:exports` value, "" when unset.
 * @return True for "results"/"both", false for "code"/"none"/"".
 */
bool OrgExportsResults(const std::string &exports);

/**
 * @brief Finds the `#+RESULTS:` block already written under a src block.
 * @param lines The document's lines.
 * @param after_row The block's `#+end_src` row, 1-based; the results start on the row after it.
 * @param start Receives the `#+RESULTS:` row, 1-based.
 * @param end Receives the results' last row, 1-based (equal to `start` for a keyword with nothing under it).
 * @param raw_body True when the block writes an unquoted body (`:results raw`/`org`/`verbatim`), which carries no
 * per-line marker and so is read as an org paragraph -- up to the first blank line, keyword, or heading.
 * @return True when a results block follows the given row.
 */
bool OrgFindResultsBlock(const std::vector<std::string> &lines, int after_row, int *start, int *end,
                         bool raw_body = false);

/**
 * @brief Splices a formatted results block into a copy of a document.
 * @param lines The document's lines.
 * @param after_row The `#+end_src` row the results belong under, 1-based.
 * @param block The results block's lines, `#+RESULTS:` keyword included.
 * @param handling The `:results` handling word ("replace", "append", "prepend", "none", "silent").
 * @param raw_body True when the block writes an unquoted body -- see OrgFindResultsBlock.
 * @return The document with the results written; unchanged for a handling that writes nothing.
 */
std::vector<std::string> OrgSpliceResultsBlock(const std::vector<std::string> &lines, int after_row,
                                               const std::vector<std::string> &block, const std::string &handling,
                                               bool raw_body = false);

/**
 * @brief Applies every src block's `:exports` to a document, dropping what an export leaves out.
 * @param lines The document's lines, results blocks already spliced in.
 * @return The document with each block's code and/or results removed as its `:exports` asks.
 * A block's affiliated keyword lines go with its code; `#+PROPERTY: header-args` and the
 * block's own `#+HEADER:` lines are both read, in org's precedence order.
 */
std::vector<std::string> OrgApplyExportGates(const std::vector<std::string> &lines);

// --- LaTeX/math fragments (the in-buffer preview's own scanner) ----------
//
// Every LaTeX fragment in an org (or .tex) buffer, for the math preview
// Editor::OrgLatexScanFragments drives: `#+BEGIN_LATEX`/`#+BEGIN_SRC latex`
// blocks, the display-math environments (`\begin{align}` and friends), and
// the four delimiter pairs `$..$`, `$$..$$`, `\(..\)`, `\[..\]`.
//
// A fragment is reported one of two ways, because mep's row renderer draws
// one buffer row as one run of text and can neither reflow it nor place a
// texture mid-row by itself:
//   - `blocks` -- the fragment occupies its rows *entirely* (nothing but
//     whitespace outside it on the first and last row), so the preview can
//     replace those rows with the rendered image.
//   - `inlines` -- the fragment shares a row with prose ("the value $x^2$
//     matters here"), so the image is drawn into the fragment's own columns
//     with the text around it sliding against it. A fragment that crosses a
//     line break has one `parts` entry per row it touches: the first part
//     is where the image goes, and the rest exist to be concealed, since
//     their source text is the same fragment's continuation.
//
// Multi-row fragments are bounded the way real org-mode bounds them: a
// fragment is an *object* inside one element, so it can never cross a blank
// line or a headline. That is also what keeps a document with one unbalanced
// `$$` in it from swallowing everything below it into a single image -- and
// it is why `$..$`, whose delimiter is by far the most ambiguous, is capped
// tighter still, at org's own one-embedded-newline limit.
struct OrgLatexBlockFragment {
    int start_row = 0;  // 1-based
    int end_row = 0;    // 1-based, inclusive
    std::string body;   // the fragment's source, delimiters included
};

// One row's worth of an inline fragment's columns. Byte offsets used as
// columns, the same equivalence every decoration span in mep assumes.
struct OrgLatexInlinePart {
    int row = 0;        // 1-based
    int col_start = 0;  // 1-based, inclusive
    int col_end = 0;    // 1-based, exclusive
};

struct OrgLatexInlineFragment {
    std::string body;  // the fragment's source, delimiters and any newlines included
    std::vector<OrgLatexInlinePart> parts;
};

struct OrgLatexFragments {
    std::vector<OrgLatexBlockFragment> blocks;
    std::vector<OrgLatexInlineFragment> inlines;
};

/**
 * @brief Scans a document for every LaTeX/math fragment the in-buffer preview can render.
 * @param lines The document's lines.
 * @return The whole-row fragments and the prose-embedded ones, each in document order.
 */
OrgLatexFragments OrgLatexScan(const std::vector<std::string> &lines);

// --- Literal (non-prose) regions ----------------------------------------
//
// Where an org document stops being prose and starts being code, output or
// markup arguments. Spell checking is the consumer that made this worth
// naming: a squiggle under every identifier in a `#+begin_src` block, or
// under `=Petal.Length=`, is noise no dictionary can fix, and the words in
// those places are not words.
//
// What counts as literal, in org's own terms:
//   - every row of a `src`, `example` or `export` block, its `#+begin_`/
//     `#+end_` rows included (those carry a language and header arguments).
//     A `quote`, `verse` or other block is prose and is *not* listed, which
//     is the whole reason this is a block-type test rather than a blanket
//     "anything fenced" one;
//   - a fixed-width row (`: ` ...), org's literal-output element, which is
//     the shape a `#+RESULTS:` block takes;
//   - an inline `=verbatim=` or `~code~` span, org's two code objects.
// Everything else -- including a `#+title:`/`#+caption:` line, which is
// prose that deserves checking -- is left alone.
struct OrgLiteralSpan {
    int row = 0;        // 1-based
    int col_start = 0;  // 1-based, inclusive
    int col_end = 0;    // 1-based, exclusive
};

/**
 * @brief Finds every literal (non-prose) span in an org document.
 * @param lines The document's lines.
 * @return The spans, in document order; a whole literal row is one span covering it.
 */
std::vector<OrgLiteralSpan> OrgLiteralSpans(const std::vector<std::string> &lines);

#endif
