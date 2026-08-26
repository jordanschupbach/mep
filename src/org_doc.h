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
OrgOutline ParseOrgOutline(const std::vector<std::string> &lines);

// Returns a rewritten copy of `headline_line` with its TODO/DONE keyword
// token replaced by `new_keyword` (or `new_keyword` inserted right after
// the stars if the headline currently has none). `todo_keywords`/
// `done_keywords` disambiguate "the first word of the title happens to
// look like a keyword" from an actual keyword token, so callers must pass
// the same sequence `ParseOrgOutline` produced for this file.
std::string RewriteHeadlineKeyword(const std::string &headline_line, const std::string &new_keyword,
                                    const std::vector<std::string> &todo_keywords,
                                    const std::vector<std::string> &done_keywords);

// Returns a rewritten copy of `planning_line` with the SCHEDULED (if
// `is_deadline` is false) or DEADLINE (if true) timestamp's date replaced
// by `new_ts`, leaving the other field on the same line (if present) and
// everything after that timestamp's weekday (time-of-day, repeater)
// untouched. A no-op (returns the input unchanged) if that keyword isn't
// present on the line.
std::string RewriteTimestampInLine(const std::string &planning_line, bool is_deadline, const OrgTimestamp &new_ts);

// Returns `ts` shifted by `delta_days` (may be negative), recomputing the
// weekday for the new date. `has_time`/`hour`/`min` are carried over
// unchanged. A no-op if `!ts.present`.
OrgTimestamp ShiftTimestamp(const OrgTimestamp &ts, int delta_days);

// Formats a *freshly constructed* timestamp (year/month/day/has_time/
// hour/min only -- no repeater) as "<2026-08-25 Tue>" / "<2026-08-25 Tue
// 09:00>". Rewriting an *existing* timestamp in place should go through
// RewriteTimestampInLine instead, so any repeater/warning suffix survives.
std::string FormatOrgTimestamp(const OrgTimestamp &ts);

// Whole-file-regenerating headline builder -- unlike RewriteHeadlineKeyword
// (a single-token surgical edit), this rebuilds an *entire* headline line
// from its parsed parts. Used for the two operations where the whole line
// legitimately IS just these parts with nothing else worth preserving: a
// Kanban title rename, and constructing a brand-new headline line for a
// new card. `level` is clamped to >= 1 (a stars count of 0 isn't a
// headline at all).
std::string FormatHeadlineLine(int level, const std::string &todo_keyword, char priority, const std::string &title,
                                const std::vector<std::string> &tags);

// Proleptic-Gregorian day-ordinal conversions (day 0 = 1970-01-01),
// exposed so main.cpp's Gantt renderer can turn a calendar date into an
// x-pixel offset (and a drag's pixel delta back into a day count) without
// duplicating the same civil-calendar arithmetic ParseOrgOutline/
// ShiftTimestamp already use internally.
long long OrgDayNumber(int year, int month, int day);
void OrgDateFromDayNumber(long long day_number, int &year, int &month, int &day);

// Locates the first "#+TODO:" line (ParseOrgOutline only ever honors the
// first one -- see this header's top comment), or -1 if `lines` has none.
// Exposed so a Kanban column add/rename/delete can find the line to
// rewrite (or know it needs to insert one) without re-implementing
// ParseTodoLine's own prefix check.
int FindOrgTodoLineIndex(const std::vector<std::string> &lines);

// Inverse of the "#+TODO:" line parsing folded into ParseOrgOutline --
// builds "#+TODO: kw1 kw2 | kw3" from separate todo/done keyword lists.
// Deliberately never emits "(x)" fast-select hints: ParseTodoLine tolerates
// their absence, OrgOutline never retained them, and every rewrite helper
// above compares keyword tokens without them -- so a keyword list that came
// from a file using fast-select hints loses them once rewritten through
// here (a documented gap, not an oversight).
std::string FormatTodoLine(const std::vector<std::string> &todo_keywords, const std::vector<std::string> &done_keywords);

#endif
