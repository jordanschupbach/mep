#ifndef MEP_ORG_DOC_H
#define MEP_ORG_DOC_H

#include <optional>
#include <string>
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
// on. Returns `lines` unchanged when `line` doesn't name a keyworded
// headline or it already carries the tag.
/**
 * @brief Adds the :ARCHIVE: tag to the keyworded headline at a 0-based line index, preserving keyword, priority, title and other tags.
 * @param lines the current full text of the org file
 * @param line the 0-based index of the headline to archive
 * @return the rewritten file lines (equal to `lines` when `line` isn't a keyworded headline or is already archived)
 */
std::vector<std::string> OrgTodoListArchive(const std::vector<std::string> &lines, int line);

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

#endif
