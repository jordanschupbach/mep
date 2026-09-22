#include "org_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

/**
 * @brief Compares two strings for equality ignoring ASCII case.
 * @param a the first string
 * @param b the second string
 * @return true if `a` and `b` are the same length and equal case-insensitively
 */
bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

/**
 * @brief Strips leading ASCII space characters from a string.
 * @param s the string to strip
 * @return `s` with any leading ' ' characters removed
 */
std::string LStrip(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

// 0 = Sunday .. 6 = Saturday, via Sakamoto's algorithm.
/**
 * @brief Computes the day of week for a Gregorian calendar date via Sakamoto's algorithm.
 * @param y the calendar year
 * @param m the calendar month (1-12)
 * @param d the calendar day of month
 * @return the day of week, 0 = Sunday .. 6 = Saturday
 */
int DayOfWeek(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
const char *kWeekdayAbbrev[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Howard Hinnant's days_from_civil/civil_from_days (public-domain,
// proleptic Gregorian, day 0 = 1970-01-01) -- used only to add/subtract a
// whole number of days from a date without touching wall-clock time
// zones or libc's mktime range limits.
/**
 * @brief Converts a proleptic-Gregorian civil date to a day count from the epoch (day 0 = 1970-01-01).
 * @param y the calendar year
 * @param m the calendar month (1-12)
 * @param d the calendar day of month
 * @return the day count from the epoch
 */
long long DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const int doy_numerator = 153 * (static_cast<int>(m) + (m > 2 ? -3 : 9)) + 2;
    const unsigned doy = static_cast<unsigned>(doy_numerator) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
/**
 * @brief Converts a day count from the epoch (day 0 = 1970-01-01) back to a proleptic-Gregorian civil date.
 * @param z the day count from the epoch
 * @param y set to the resulting calendar year
 * @param m set to the resulting calendar month (1-12)
 * @param d set to the resulting calendar day of month
 */
void CivilFromDays(long long z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = static_cast<unsigned>(static_cast<int>(mp) + (mp < 10 ? 3 : 9 - 12));  // mp<10 -> m=mp+3, else m=mp-9
    y = static_cast<int>(yy + (m <= 2));
}

// "#+TODO: TODO(t) IN-PROGRESS(i) | DONE(d)" -- case-insensitive keyword,
// '(' fast-select suffixes stripped, keywords before '|' -> todo_out,
// after -> done_out. Without a '|' at all, the last token is treated as
// the done keyword (matching real org's own default when no separator is
// given), unless there's only one token.
/**
 * @brief Parses a "#+TODO: ..." line into separate TODO-side and DONE-side keyword lists.
 * @param line the candidate line to parse
 * @param todo_out appended with the TODO-side keyword tokens (fast-select "(x)" suffixes stripped)
 * @param done_out appended with the DONE-side keyword tokens (fast-select "(x)" suffixes stripped)
 * @return true if `line` was a valid "#+TODO:" line and at least one keyword was parsed
 */
bool ParseTodoLine(const std::string &line, std::vector<std::string> &todo_out, std::vector<std::string> &done_out) {
    std::string trimmed = LStrip(line);
    const std::string prefix = "#+TODO:";
    if (trimmed.size() < prefix.size()) return false;
    if (!EqualsIgnoreCase(trimmed.substr(0, prefix.size()), prefix)) return false;
    std::string rest = trimmed.substr(prefix.size());

    std::vector<std::string> tokens;
    bool pipe_seen = false;
    std::vector<bool> is_pipe;
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == ' ') i++;
        size_t start = i;
        while (i < rest.size() && rest[i] != ' ') i++;
        if (start == i) break;
        std::string tok = rest.substr(start, i - start);
        if (tok == "|") {
            pipe_seen = true;
            continue;
        }
        size_t paren = tok.find('(');
        if (paren != std::string::npos) tok.resize(paren);
        if (tok.empty()) continue;
        tokens.push_back(tok);
        is_pipe.push_back(pipe_seen);
    }
    if (tokens.empty()) return false;

    bool has_pipe = false;
    for (size_t k = 0; k < tokens.size(); k++) {
        // is_pipe[k] is true only for tokens *seen after* a pipe; recompute
        // whether a pipe appeared at all by checking if any of is_pipe is set.
        if (is_pipe[k]) has_pipe = true;
    }
    if (has_pipe) {
        for (size_t k = 0; k < tokens.size(); k++) {
            if (is_pipe[k]) done_out.push_back(tokens[k]);
            else todo_out.push_back(tokens[k]);
        }
    } else if (tokens.size() == 1) {
        todo_out.push_back(tokens[0]);
    } else {
        for (size_t k = 0; k + 1 < tokens.size(); k++) todo_out.push_back(tokens[k]);
        done_out.push_back(tokens.back());
    }
    return true;
}

// Splits a trailing " :tag1:tag2:" block off `rest` (already right-
// trimmed), if one validly appears (a run of ':'-separated non-empty
// alnum/_/@/% tokens immediately preceded by whitespace or start-of-
// string). Otherwise `title` is the whole of `rest` and `tags` is empty.
/**
 * @brief Splits a trailing " :tag1:tag2:" block off a headline's remaining text, if one validly appears.
 * @param rest the right-trimmed text remaining after stars/keyword/priority have been stripped
 * @param title set to the title text with any trailing tags block removed
 * @param tags set to the parsed tag tokens, or cleared if no valid tags block was found
 */
void ExtractTrailingTags(const std::string &rest, std::string &title, std::vector<std::string> &tags) {
    tags.clear();
    if (rest.empty() || rest.back() != ':') {
        title = rest;
        return;
    }
    size_t i = rest.size();
    while (i > 0) {
        char c = rest[i - 1];
        bool allowed = c == ':' || std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '@' || c == '%';
        if (!allowed) break;
        i--;
    }
    if (i > 0 && rest[i - 1] != ' ') {
        title = rest;
        return;
    }
    std::string block = rest.substr(i);
    if (block.size() < 2 || block.front() != ':') {
        title = rest;
        return;
    }
    std::vector<std::string> parts;
    std::string cur;
    for (char c : block) {
        if (c == ':') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    if (parts.size() < 3 || !parts.front().empty() || !parts.back().empty()) {
        title = rest;
        return;
    }
    std::vector<std::string> candidate;
    for (size_t k = 1; k + 1 < parts.size(); k++) {
        if (parts[k].empty()) {
            title = rest;
            return;
        }
        candidate.push_back(parts[k]);
    }
    tags = candidate;
    size_t title_end = i;
    while (title_end > 0 && rest[title_end - 1] == ' ') title_end--;
    title = rest.substr(0, title_end);
}

// Locates the keyword token (if any) right after a headline's stars, using
// the same tokenizing rule ParseHeadlineLine and RewriteHeadlineKeyword
// both rely on. Returns the [start,end) byte range of that token within
// `line` when it matches a known keyword, or a zero-length range at the
// position a keyword would start otherwise.
struct KeywordSpan {
    size_t start, end;
    bool matched;
    bool is_done;
};
/**
 * @brief Locates the keyword token (if any) right after a headline's stars.
 * @param line the full headline line
 * @param after_stars byte offset into `line` just past the stars and following space(s)
 * @param todo_kw the file's TODO-side keyword sequence to match against
 * @param done_kw the file's DONE-side keyword sequence to match against
 * @return the [start,end) byte range of the token, with `matched`/`is_done` set; on no match, a zero-length range at the token's start position
 */
KeywordSpan FindKeywordSpan(const std::string &line, size_t after_stars, const std::vector<std::string> &todo_kw,
                            const std::vector<std::string> &done_kw) {
    size_t i = after_stars;
    size_t tok_start = i;
    while (i < line.size() && line[i] != ' ') i++;
    std::string tok = line.substr(tok_start, i - tok_start);
    for (const auto &k : todo_kw) {
        if (k == tok) return {tok_start, i, true, false};
    }
    for (const auto &k : done_kw) {
        if (k == tok) return {tok_start, i, true, true};
    }
    return {tok_start, tok_start, false, false};
}

/**
 * @brief Parses a single "*** KEYWORD [#A] Title :tags:" headline line into its component fields.
 * @param line the candidate line to parse
 * @param todo_kw the file's TODO-side keyword sequence, used to recognize a keyword token
 * @param done_kw the file's DONE-side keyword sequence, used to recognize a keyword token
 * @param out set with the parsed level/keyword/priority/title/tags (planning/properties are not filled in here)
 * @return true if `line` is a valid headline line (starts with '*'s followed by a space)
 */
bool ParseHeadlineLine(const std::string &line, const std::vector<std::string> &todo_kw,
                       const std::vector<std::string> &done_kw, OrgHeadline &out) {
    size_t i = 0, n = line.size();
    while (i < n && line[i] == '*') i++;
    if (i == 0 || i >= n || line[i] != ' ') return false;
    out.level = static_cast<int>(i);
    i++;
    while (i < n && line[i] == ' ') i++;

    KeywordSpan kw = FindKeywordSpan(line, i, todo_kw, done_kw);
    out.todo_keyword.clear();
    out.is_done_keyword = false;
    if (kw.matched) {
        out.todo_keyword = line.substr(kw.start, kw.end - kw.start);
        out.is_done_keyword = kw.is_done;
        i = kw.end;
        while (i < n && line[i] == ' ') i++;
    }

    out.priority = 0;
    if (i + 3 < n && line[i] == '[' && line[i + 1] == '#' && std::isupper(static_cast<unsigned char>(line[i + 2])) &&
        line[i + 3] == ']') {
        out.priority = line[i + 2];
        i += 4;
        while (i < n && line[i] == ' ') i++;
    }

    std::string rest = line.substr(i);
    size_t end = rest.find_last_not_of(' ');
    rest = (end == std::string::npos) ? std::string() : rest.substr(0, end + 1);
    ExtractTrailingTags(rest, out.title, out.tags);
    return true;
}

// Extracts a single "<...>" token starting at or after `from`, if the
// keyword ("SCHEDULED"/"DEADLINE") appears there followed by ':' and a
// '<'-opened timestamp; advances `from` past it. Returns "" if absent.
/**
 * @brief Extracts a single "<...>" timestamp token that immediately follows a keyword (e.g. "SCHEDULED:").
 * @param line the planning line to scan
 * @param keyword the keyword to look for ("SCHEDULED" or "DEADLINE")
 * @param from byte offset to start searching from; advanced past the extracted token on success
 * @return the "<...>" token text, or "" if the keyword/colon/timestamp isn't found there
 */
std::string ExtractTimestampAfterKeyword(const std::string &line, const std::string &keyword, size_t &from) {
    size_t kw_pos = line.find(keyword, from);
    if (kw_pos == std::string::npos) return "";
    size_t i = kw_pos + keyword.size();
    while (i < line.size() && line[i] == ' ') i++;
    if (i >= line.size() || line[i] != ':') return "";
    i++;
    while (i < line.size() && line[i] == ' ') i++;
    if (i >= line.size() || line[i] != '<') return "";
    size_t close = line.find('>', i);
    if (close == std::string::npos) return "";
    from = close + 1;
    return line.substr(i, close - i + 1);
}

/**
 * @brief Parses a SCHEDULED/DEADLINE planning line, extracting either or both raw timestamp tokens.
 * @param line the candidate line to parse
 * @param sched_raw set to the raw "<...>" SCHEDULED timestamp token, or "" if absent
 * @param deadline_raw set to the raw "<...>" DEADLINE timestamp token, or "" if absent
 * @return true if `line` starts with "SCHEDULED"/"DEADLINE" and at least one timestamp was extracted
 */
bool ParsePlanningLine(const std::string &line, std::string &sched_raw, std::string &deadline_raw) {
    std::string trimmed = LStrip(line);
    if (trimmed.compare(0, 9, "SCHEDULED") != 0 && trimmed.compare(0, 8, "DEADLINE") != 0) return false;
    size_t pos = 0;
    sched_raw = ExtractTimestampAfterKeyword(line, "SCHEDULED", pos);
    pos = 0;
    deadline_raw = ExtractTimestampAfterKeyword(line, "DEADLINE", pos);
    return !sched_raw.empty() || !deadline_raw.empty();
}

/**
 * @brief Checks whether a line is exactly a ":NAME:" drawer marker (case-insensitive, ignoring leading spaces).
 * @param line the candidate line to check
 * @param name the drawer name to match, without surrounding colons (e.g. "PROPERTIES")
 * @return true if `line`, left-stripped, equals ":name:" case-insensitively
 */
bool IsDrawerLine(const std::string &line, const std::string &name) {
    return EqualsIgnoreCase(LStrip(line), ":" + name + ":");
}

/**
 * @brief Parses a ":KEY: value" property-drawer line into its key and value.
 * @param line the candidate line to parse
 * @param key set to the property key (text between the two leading colons)
 * @param val set to the trimmed value text following the second colon
 * @return true if `line`, left-stripped, starts with ':' and contains a closing ':' for the key
 */
bool ParsePropertyLine(const std::string &line, std::string &key, std::string &val) {
    std::string t = LStrip(line);
    if (t.empty() || t[0] != ':') return false;
    size_t colon2 = t.find(':', 1);
    if (colon2 == std::string::npos) return false;
    key = t.substr(1, colon2 - 1);
    size_t v = colon2 + 1;
    while (v < t.size() && t[v] == ' ') v++;
    size_t vend = t.find_last_not_of(' ');
    val = (vend == std::string::npos || vend < v) ? std::string() : t.substr(v, vend - v + 1);
    return true;
}

/**
 * @brief Splits a :BLOCKER: property value into individual predecessor IDs.
 * @param value the raw property value, IDs separated by whitespace and/or commas
 * @return the parsed list of non-empty ID tokens
 */
std::vector<std::string> ParseDependencyIds(const std::string &value) {
    std::vector<std::string> ids;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (std::isspace(static_cast<unsigned char>(value[pos])) || value[pos] == ',')) pos++;
        size_t end = pos;
        while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end])) && value[end] != ',') end++;
        if (end > pos) ids.push_back(value.substr(pos, end - pos));
        pos = end;
    }
    return ids;
}

/**
 * @brief Parses a full "<YYYY-MM-DD Day[ HH:MM...]>" org timestamp token into its date/time fields.
 * @param raw the raw "<...>" timestamp token text
 * @return the parsed OrgTimestamp (year/month/day/has_time/hour/min, with `raw` preserved verbatim); `present` is false if `raw` doesn't start with a valid date
 */
OrgTimestamp ParseTimestamp(const std::string &raw) {
    OrgTimestamp ts;
    ts.raw = raw;
    if (raw.size() < 11 || raw.front() != '<') return ts;
    int y, mo, d;
    if (std::sscanf(raw.c_str() + 1, "%d-%d-%d", &y, &mo, &d) != 3) return ts;
    ts.year = y;
    ts.month = mo;
    ts.day = d;
    ts.present = true;

    size_t space_after_date = raw.find(' ', 1);
    if (space_after_date == std::string::npos) return ts;
    size_t wd_end = raw.find(' ', space_after_date + 1);
    if (wd_end == std::string::npos) return ts;
    size_t maybe_time = wd_end + 1;
    if (maybe_time < raw.size() && std::isdigit(static_cast<unsigned char>(raw[maybe_time]))) {
        int hh, mm;
        if (std::sscanf(raw.c_str() + maybe_time, "%d:%d", &hh, &mm) == 2) {
            ts.has_time = true;
            ts.hour = hh;
            ts.min = mm;
        }
    }
    return ts;
}

// Replaces just the "YYYY-MM-DD Day" portion of a full "<...>" token with
// the one computed from `new_date`, leaving anything after the weekday
// (time-of-day, repeater, warning period) byte-for-byte untouched.
/**
 * @brief Replaces the "YYYY-MM-DD Day" portion of a "<...>" timestamp token, preserving everything after it verbatim.
 * @param raw the original "<...>" timestamp token text
 * @param new_date the date (year/month/day) to substitute in; its weekday is recomputed
 * @return the rewritten token, or `raw` unchanged if it isn't a well-formed "<...>" token
 */
std::string RewriteTimestampDate(const std::string &raw, const OrgTimestamp &new_date) {
    if (raw.size() < 11 || raw.front() != '<') return raw;
    size_t space_after_date = raw.find(' ', 1);
    if (space_after_date == std::string::npos) return raw;
    size_t wd_end = raw.find(' ', space_after_date + 1);
    if (wd_end == std::string::npos) wd_end = raw.size() - 1;  // "<date Wed>", nothing follows
    char buf[24];
    int dow = DayOfWeek(new_date.year, new_date.month, new_date.day);
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", new_date.year, new_date.month, new_date.day,
                  kWeekdayAbbrev[dow]);
    return raw.substr(0, 1) + std::string(buf) + raw.substr(wd_end);
}

}  // namespace

OrgOutline ParseOrgOutline(const std::vector<std::string> &lines) {
    OrgOutline out;
    for (const auto &l : lines) {
        std::vector<std::string> todo_kw, done_kw;
        if (ParseTodoLine(l, todo_kw, done_kw)) {
            out.todo_keywords = todo_kw;
            out.done_keywords = done_kw;
            break;
        }
    }
    if (out.todo_keywords.empty() && out.done_keywords.empty()) {
        out.todo_keywords = {"TODO"};
        out.done_keywords = {"DONE"};
    }

    std::vector<int> stack_level;
    std::vector<int> stack_index;
    for (size_t i = 0; i < lines.size(); i++) {
        OrgHeadline h;
        if (!ParseHeadlineLine(lines[i], out.todo_keywords, out.done_keywords, h)) continue;
        h.line_start = static_cast<int>(i);
        h.line_end = static_cast<int>(i);  // fixed up below

        while (!stack_level.empty() && stack_level.back() >= h.level) {
            stack_level.pop_back();
            stack_index.pop_back();
        }
        h.parent_index = stack_index.empty() ? -1 : stack_index.back();

        int this_index = static_cast<int>(out.headlines.size());
        out.headlines.push_back(h);
        stack_level.push_back(h.level);
        stack_index.push_back(this_index);

        size_t pl = i + 1;
        if (pl < lines.size()) {
            std::string sched_raw, deadline_raw;
            if (ParsePlanningLine(lines[pl], sched_raw, deadline_raw)) {
                out.headlines[static_cast<size_t>(this_index)].planning_line = static_cast<int>(pl);
                if (!sched_raw.empty()) out.headlines[static_cast<size_t>(this_index)].scheduled = ParseTimestamp(sched_raw);
                if (!deadline_raw.empty()) out.headlines[static_cast<size_t>(this_index)].deadline = ParseTimestamp(deadline_raw);
                pl++;
            }
        }
        if (pl < lines.size() && IsDrawerLine(lines[pl], "PROPERTIES")) {
            size_t d = pl + 1;
            while (d < lines.size() && !IsDrawerLine(lines[d], "END")) {
                std::string key, val;
                if (ParsePropertyLine(lines[d], key, val)) {
                    if (EqualsIgnoreCase(key, "EFFORT")) out.headlines[static_cast<size_t>(this_index)].effort = val;
                    else if (EqualsIgnoreCase(key, "ID")) out.headlines[static_cast<size_t>(this_index)].id = val;
                    else if (EqualsIgnoreCase(key, "BLOCKER")) out.headlines[static_cast<size_t>(this_index)].blockers = ParseDependencyIds(val);
                    else if (EqualsIgnoreCase(key, "ASSIGNEE") || EqualsIgnoreCase(key, "TEAM")) {
                        out.headlines[static_cast<size_t>(this_index)].assignee = val;
                    } else if (EqualsIgnoreCase(key, "PROGRESS")) {
                        int progress = 0;
                        if (std::sscanf(val.c_str(), "%d", &progress) == 1) {
                            out.headlines[static_cast<size_t>(this_index)].progress = std::clamp(progress, 0, 100);
                        }
                    }
                }
                d++;
            }
        }
    }

    for (size_t hi = 0; hi < out.headlines.size(); hi++) {
        int this_level = out.headlines[hi].level;
        int end = static_cast<int>(lines.size()) - 1;
        for (size_t hj = hi + 1; hj < out.headlines.size(); hj++) {
            if (out.headlines[hj].level <= this_level) {
                end = out.headlines[hj].line_start - 1;
                break;
            }
        }
        out.headlines[hi].line_end = end;
    }
    return out;
}

std::string RewriteHeadlineKeyword(const std::string &headline_line, const std::string &new_keyword,
                                    const std::vector<std::string> &todo_keywords,
                                    const std::vector<std::string> &done_keywords) {
    size_t i = 0, n = headline_line.size();
    while (i < n && headline_line[i] == '*') i++;
    if (i == 0 || i >= n || headline_line[i] != ' ') return headline_line;
    i++;
    while (i < n && headline_line[i] == ' ') i++;

    KeywordSpan kw = FindKeywordSpan(headline_line, i, todo_keywords, done_keywords);
    if (kw.matched) {
        return headline_line.substr(0, kw.start) + new_keyword + headline_line.substr(kw.end);
    }
    return headline_line.substr(0, kw.start) + new_keyword + " " + headline_line.substr(kw.start);
}

std::string RewriteTimestampInLine(const std::string &planning_line, bool is_deadline, const OrgTimestamp &new_ts) {
    const std::string kw = is_deadline ? "DEADLINE" : "SCHEDULED";
    size_t kw_pos = planning_line.find(kw);
    if (kw_pos == std::string::npos) return planning_line;
    size_t lt = planning_line.find('<', kw_pos);
    if (lt == std::string::npos) return planning_line;
    size_t gt = planning_line.find('>', lt);
    if (gt == std::string::npos) return planning_line;
    std::string raw = planning_line.substr(lt, gt - lt + 1);
    std::string new_raw = RewriteTimestampDate(raw, new_ts);
    return planning_line.substr(0, lt) + new_raw + planning_line.substr(gt + 1);
}

OrgTimestamp ShiftTimestamp(const OrgTimestamp &ts, int delta_days) {
    OrgTimestamp out = ts;
    if (!ts.present) return out;
    long long dn = DaysFromCivil(ts.year, static_cast<unsigned>(ts.month), static_cast<unsigned>(ts.day)) + delta_days;
    int y;
    unsigned m, d;
    CivilFromDays(dn, y, m, d);
    out.year = y;
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(d);
    return out;
}

std::string FormatOrgTimestamp(const OrgTimestamp &ts) {
    if (!ts.present) return "";
    char buf[40];
    int dow = DayOfWeek(ts.year, ts.month, ts.day);
    if (ts.has_time) {
        std::snprintf(buf, sizeof(buf), "<%04d-%02d-%02d %s %02d:%02d>", ts.year, ts.month, ts.day,
                      kWeekdayAbbrev[dow], ts.hour, ts.min);
    } else {
        std::snprintf(buf, sizeof(buf), "<%04d-%02d-%02d %s>", ts.year, ts.month, ts.day, kWeekdayAbbrev[dow]);
    }
    return std::string(buf);
}

std::string FormatHeadlineLine(int level, const std::string &todo_keyword, char priority, const std::string &title,
                                const std::vector<std::string> &tags) {
    std::string line(static_cast<size_t>(std::max(1, level)), '*');
    line += ' ';
    if (!todo_keyword.empty()) {
        line += todo_keyword;
        line += ' ';
    }
    if (priority) {
        line += "[#";
        line += priority;
        line += "] ";
    }
    line += title;
    if (!tags.empty()) {
        line += " :";
        for (const auto &t : tags) {
            line += t;
            line += ':';
        }
    }
    return line;
}

long long OrgDayNumber(int year, int month, int day) {
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

void OrgDateFromDayNumber(long long day_number, int &year, int &month, int &day) {
    unsigned m, d;
    CivilFromDays(day_number, year, m, d);
    month = static_cast<int>(m);
    day = static_cast<int>(d);
}

int FindOrgTodoLineIndex(const std::vector<std::string> &lines) {
    for (size_t i = 0; i < lines.size(); i++) {
        std::vector<std::string> todo_out, done_out;
        if (ParseTodoLine(lines[i], todo_out, done_out)) return static_cast<int>(i);
    }
    return -1;
}

std::string FormatTodoLine(const std::vector<std::string> &todo_keywords, const std::vector<std::string> &done_keywords) {
    std::string line = "#+TODO:";
    for (const auto &kw : todo_keywords) {
        line += " ";
        line += kw;
    }
    line += " |";
    for (const auto &kw : done_keywords) {
        line += " ";
        line += kw;
    }
    return line;
}

namespace {
/**
 * @brief Marks which headlines of an outline sit in an :ARCHIVE: subtree (the tag on themselves or an ancestor).
 * @param outline the parsed outline, headlines in document order
 * @return one flag per outline.headlines entry, true when that headline is archived
 */
std::vector<bool> OrgArchivedHeadlines(const OrgOutline &outline) {
    std::vector<bool> archived(outline.headlines.size(), false);
    // Ancestor chain as (level, archived) pairs, in document order:
    // a headline pops everything at its own level or deeper, so the
    // top is always its nearest ancestor.
    std::vector<std::pair<int, bool>> stack;
    for (size_t i = 0; i < outline.headlines.size(); i++) {
        const OrgHeadline &h = outline.headlines[i];
        while (!stack.empty() && stack.back().first >= h.level) stack.pop_back();
        bool self = std::find(h.tags.begin(), h.tags.end(), kOrgArchiveTag) != h.tags.end();
        bool is_archived = self || (!stack.empty() && stack.back().second);
        archived[i] = is_archived;
        stack.emplace_back(h.level, is_archived);
    }
    return archived;
}
}  // namespace

std::vector<OrgTodoItem> OrgTodoListItems(const std::vector<std::string> &lines) {
    std::vector<OrgTodoItem> items;
    OrgOutline outline = ParseOrgOutline(lines);
    const std::vector<bool> archived = OrgArchivedHeadlines(outline);
    for (size_t hi = 0; hi < outline.headlines.size(); hi++) {
        const OrgHeadline &h = outline.headlines[hi];
        if (h.todo_keyword.empty() || archived[hi]) continue;
        OrgTodoItem it;
        it.done = h.is_done_keyword;
        it.text = h.title;
        it.line = h.line_start;
        it.level = h.level;
        it.keyword = h.todo_keyword;
        items.push_back(std::move(it));
    }
    return items;
}

std::vector<std::string> OrgTodoListApply(const std::vector<std::string> &lines, const std::vector<OrgTodoItem> &items) {
    OrgOutline outline = ParseOrgOutline(lines);
    const std::string todo_kw = outline.todo_keywords.empty() ? std::string("TODO") : outline.todo_keywords[0];
    const std::string done_kw = outline.done_keywords.empty() ? std::string("DONE") : outline.done_keywords[0];

    // Keyworded, non-archived headlines by their line index -- the
    // identity the panel hands back in OrgTodoItem::line. An archived
    // one is left out so that, never being "referenced", it also can't
    // be dropped below.
    const std::vector<bool> archived = OrgArchivedHeadlines(outline);
    std::vector<const OrgHeadline *> by_line(lines.size(), nullptr);
    for (size_t hi = 0; hi < outline.headlines.size(); hi++) {
        const OrgHeadline &h = outline.headlines[hi];
        if (h.todo_keyword.empty() || archived[hi]) continue;
        if (h.line_start >= 0 && h.line_start < static_cast<int>(lines.size())) {
            by_line[static_cast<size_t>(h.line_start)] = &h;
        }
    }

    std::vector<std::string> out = lines;
    std::vector<bool> referenced(lines.size(), false);
    std::vector<const OrgTodoItem *> fresh;
    for (const OrgTodoItem &item : items) {
        if (item.line < 0) {
            fresh.push_back(&item);
            continue;
        }
        if (item.line >= static_cast<int>(lines.size())) continue;  // stale: ignored, see the header
        const OrgHeadline *h = by_line[static_cast<size_t>(item.line)];
        if (!h || referenced[static_cast<size_t>(item.line)]) continue;
        referenced[static_cast<size_t>(item.line)] = true;
        if (item.done != h->is_done_keyword) {
            out[static_cast<size_t>(item.line)] = RewriteHeadlineKeyword(lines[static_cast<size_t>(item.line)],
                                                                         item.done ? done_kw : todo_kw,
                                                                         outline.todo_keywords, outline.done_keywords);
        }
    }

    // Unreferenced keyworded headlines go, whole subtree each.
    std::vector<bool> drop(lines.size(), false);
    for (size_t i = 0; i < by_line.size(); i++) {
        const OrgHeadline *h = by_line[i];
        if (!h || referenced[i]) continue;
        int end = std::min(h->line_end, static_cast<int>(lines.size()) - 1);
        for (int k = h->line_start; k <= end; k++) drop[static_cast<size_t>(k)] = true;
    }

    std::vector<std::string> result;
    result.reserve(out.size() + fresh.size());
    for (size_t i = 0; i < out.size(); i++) {
        if (!drop[i]) result.push_back(std::move(out[i]));
    }
    if (!fresh.empty()) {
        // A buffer's "empty file" is one empty line -- don't leave that as a
        // stray blank first line above the first appended headline.
        if (result.size() == 1 && result[0].empty()) result.clear();
        for (const OrgTodoItem *item : fresh) {
            result.push_back(FormatHeadlineLine(1, item->done ? done_kw : todo_kw, 0, item->text, {}));
        }
    }
    return result;
}

std::vector<std::string> OrgTodoListArchive(const std::vector<std::string> &lines, int line) {
    if (line < 0 || line >= static_cast<int>(lines.size())) return lines;
    OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        if (h.line_start != line) continue;
        if (h.todo_keyword.empty()) break;
        if (std::find(h.tags.begin(), h.tags.end(), kOrgArchiveTag) != h.tags.end()) break;
        std::vector<std::string> tags = h.tags;
        tags.emplace_back(kOrgArchiveTag);

        // The tagged subtree (headline + body + children), relocated to
        // the very end of the file so archived items sink out of the way
        // instead of cluttering the spot they were working in.
        const int end = std::min(h.line_end, static_cast<int>(lines.size()) - 1);
        std::vector<std::string> subtree;
        subtree.reserve(static_cast<size_t>(end - h.line_start + 1));
        subtree.push_back(FormatHeadlineLine(h.level, h.todo_keyword, h.priority, h.title, tags));
        for (int i = h.line_start + 1; i <= end; i++) subtree.push_back(lines[static_cast<size_t>(i)]);

        std::vector<std::string> out;
        out.reserve(lines.size());
        for (int i = 0; i < h.line_start; i++) out.push_back(lines[static_cast<size_t>(i)]);
        for (int i = end + 1; i < static_cast<int>(lines.size()); i++) out.push_back(lines[static_cast<size_t>(i)]);
        for (std::string &l : subtree) out.push_back(std::move(l));
        return out;
    }
    return lines;
}

std::vector<std::string> OrgTodoListRetitle(const std::vector<std::string> &lines, int line, const std::string &new_title) {
    if (new_title.empty() || line < 0 || line >= static_cast<int>(lines.size())) return lines;
    OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        if (h.line_start != line) continue;
        if (h.todo_keyword.empty()) break;
        std::vector<std::string> out = lines;
        out[static_cast<size_t>(line)] = FormatHeadlineLine(h.level, h.todo_keyword, h.priority, new_title, h.tags);
        return out;
    }
    return lines;
}

std::vector<std::string> OrgTodoListMove(const std::vector<std::string> &lines, int line, int delta, int *new_line) {
    if (new_line) *new_line = line;
    if ((delta != -1 && delta != 1) || line < 0 || line >= static_cast<int>(lines.size())) return lines;
    OrgOutline outline = ParseOrgOutline(lines);
    int hi = -1;
    for (size_t i = 0; i < outline.headlines.size(); i++) {
        if (outline.headlines[i].line_start == line) {
            hi = static_cast<int>(i);
            break;
        }
    }
    if (hi < 0 || outline.headlines[static_cast<size_t>(hi)].todo_keyword.empty()) return lines;
    const int parent = outline.headlines[static_cast<size_t>(hi)].parent_index;
    int sib = -1;
    if (delta < 0) {
        for (int i = hi - 1; i >= 0; i--) {
            if (outline.headlines[static_cast<size_t>(i)].parent_index == parent) {
                sib = i;
                break;
            }
        }
    } else {
        for (size_t i = static_cast<size_t>(hi) + 1; i < outline.headlines.size(); i++) {
            if (outline.headlines[i].parent_index == parent) {
                sib = static_cast<int>(i);
                break;
            }
        }
    }
    if (sib < 0) return lines;

    // The earlier of the two subtrees in document order, and the later
    // one -- contiguous, since nothing but `hi`'s own deeper-level
    // descendants can sit between siblings sharing `parent`.
    const OrgHeadline &earlier = delta < 0 ? outline.headlines[static_cast<size_t>(sib)]
                                            : outline.headlines[static_cast<size_t>(hi)];
    const OrgHeadline &later = delta < 0 ? outline.headlines[static_cast<size_t>(hi)]
                                          : outline.headlines[static_cast<size_t>(sib)];
    if (earlier.line_end + 1 != later.line_start) return lines;  // not actually contiguous -- shouldn't happen

    std::vector<std::string> out;
    out.reserve(lines.size());
    for (int i = 0; i < earlier.line_start; i++) out.push_back(lines[static_cast<size_t>(i)]);
    for (int i = later.line_start; i <= later.line_end; i++) out.push_back(lines[static_cast<size_t>(i)]);
    for (int i = earlier.line_start; i <= earlier.line_end; i++) out.push_back(lines[static_cast<size_t>(i)]);
    for (int i = later.line_end + 1; i < static_cast<int>(lines.size()); i++) out.push_back(lines[static_cast<size_t>(i)]);

    // Moving up: `hi` (the later block) lands first, at `earlier`'s old
    // start. Moving down: `hi` (the earlier block) lands second, shifted
    // by how long the (now-preceding) `later` block is.
    if (new_line) *new_line = delta < 0 ? earlier.line_start : earlier.line_start + (later.line_end - later.line_start + 1);
    return out;
}

namespace {
/**
 * @brief Returns the index of the first non-whitespace character at or after `pos`.
 * @param s the string to scan
 * @param pos the offset to start from
 * @return the index of the first non-space character, or s.size()
 */
size_t SkipSpaces(const std::string &s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
    return pos;
}

/**
 * @brief Checks whether a line is an org headline of any level ("*"+ followed by a space).
 * @param line the line to check
 * @return true if the line starts with one or more '*' followed by a space
 */
bool IsAnyHeadlineLine(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && line[i] == '*') i++;
    return i > 0 && i < line.size() && line[i] == ' ';
}

/**
 * @brief Parses exactly `count` consecutive decimal digits starting at a position.
 * @param s the string to read from
 * @param pos the offset to start reading at
 * @param count the exact number of digit characters required
 * @param out set to the parsed integer value on success
 * @return true if `count` digit characters were present at `pos`
 */
bool ReadDigits(const std::string &s, size_t pos, int count, int *out) {
    if (pos + static_cast<size_t>(count) > s.size()) return false;
    int v = 0;
    for (int i = 0; i < count; i++) {
        char c = s[pos + static_cast<size_t>(i)];
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}
}  // namespace

bool OrgMatchOpenClockLine(const std::string &line, std::string *start_ts) {
    size_t i = SkipSpaces(line, 0);
    static const std::string kPrefix = "CLOCK:";
    if (line.compare(i, kPrefix.size(), kPrefix) != 0) return false;
    i = SkipSpaces(line, i + kPrefix.size());
    if (i >= line.size() || line[i] != '[') return false;
    size_t open = i + 1;
    size_t close = line.find(']', open);
    if (close == std::string::npos) return false;
    if (SkipSpaces(line, close + 1) != line.size()) return false;  // "--[...] => H:MM" tail -> already closed
    if (start_ts) *start_ts = line.substr(open, close - open);
    return true;
}

bool OrgParseClockTimestamp(const std::string &s, int *y, int *mo, int *d, int *hh, int *mm) {
    // "YYYY-MM-DD <weekday> HH:MM" -- the weekday token is any run of
    // non-space characters (locale names differ), the original Lua
    // pattern's `%d%d%d%d%-%d%d%-%d%d %S+ %d%d:%d%d`.
    for (size_t i = 0; i + 16 <= s.size(); i++) {
        int yy = 0, mmo = 0, dd = 0, h = 0, m = 0;
        if (!ReadDigits(s, i, 4, &yy) || s[i + 4] != '-' || !ReadDigits(s, i + 5, 2, &mmo) || s[i + 7] != '-' ||
            !ReadDigits(s, i + 8, 2, &dd) || s[i + 10] != ' ') {
            continue;
        }
        size_t j = i + 11;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
        if (j == i + 11 || j >= s.size() || s[j] != ' ') continue;
        j++;
        if (!ReadDigits(s, j, 2, &h) || j + 2 >= s.size() || s[j + 2] != ':' || !ReadDigits(s, j + 3, 2, &m)) continue;
        *y = yy;
        *mo = mmo;
        *d = dd;
        *hh = h;
        *mm = m;
        return true;
    }
    return false;
}

OrgOpenClock OrgFindOpenClock(const std::vector<std::string> &lines) {
    OrgOpenClock out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!OrgMatchOpenClockLine(lines[i], &out.start_ts)) continue;
        out.line = static_cast<int>(i);
        for (int k = static_cast<int>(i) - 1; k >= 0; k--) {
            if (IsAnyHeadlineLine(lines[static_cast<size_t>(k)])) {
                out.headline_line = k;
                break;
            }
        }
        return out;
    }
    return out;
}

std::vector<std::string> OrgClockStartLines(const std::vector<std::string> &lines, int headline_line,
                                            const std::string &now_ts) {
    if (headline_line < 0 || headline_line >= static_cast<int>(lines.size())) return lines;
    if (!IsAnyHeadlineLine(lines[static_cast<size_t>(headline_line)])) return lines;
    if (OrgFindOpenClock(lines).line >= 0) return lines;
    // The headline's own body only (up to its first child headline, not
    // the whole subtree): a child's :LOGBOOK: is the child's.
    int subtree_end = static_cast<int>(lines.size()) - 1;
    for (int i = headline_line + 1; i < static_cast<int>(lines.size()); i++) {
        if (IsAnyHeadlineLine(lines[static_cast<size_t>(i)])) {
            subtree_end = i - 1;
            break;
        }
    }
    const std::string entry = "  CLOCK: [" + now_ts + "]";
    std::vector<std::string> out = lines;
    // An existing :LOGBOOK: drawer in that body: newest entry
    // first, org's own convention (and Editor::OrgClockIn's).
    for (int i = headline_line + 1; i <= subtree_end; i++) {
        if (IsDrawerLine(lines[static_cast<size_t>(i)], "LOGBOOK")) {
            out.insert(out.begin() + i + 1, entry);
            return out;
        }
    }
    // No drawer yet: a new one after the planning line and :PROPERTIES:
    // drawer, if the headline has them, so those keep their org-mandated
    // position directly under the headline.
    int insert_at = headline_line + 1;
    if (insert_at <= subtree_end) {
        std::string sched, dead;
        if (ParsePlanningLine(lines[static_cast<size_t>(insert_at)], sched, dead)) insert_at++;
    }
    if (insert_at <= subtree_end && IsDrawerLine(lines[static_cast<size_t>(insert_at)], "PROPERTIES")) {
        for (int i = insert_at + 1; i <= subtree_end; i++) {
            if (IsDrawerLine(lines[static_cast<size_t>(i)], "END")) {
                insert_at = i + 1;
                break;
            }
        }
    }
    out.insert(out.begin() + insert_at, {"  :LOGBOOK:", entry, "  :END:"});
    return out;
}

std::vector<std::string> OrgClockStopLines(const std::vector<std::string> &lines, const std::string &now_ts,
                                           int *minutes) {
    if (minutes) *minutes = -1;
    OrgOpenClock clock = OrgFindOpenClock(lines);
    if (clock.line < 0) return lines;
    long long mins = 0;
    int y1 = 0, mo1 = 0, d1 = 0, h1 = 0, m1 = 0, y2 = 0, mo2 = 0, d2 = 0, h2 = 0, m2 = 0;
    if (OrgParseClockTimestamp(clock.start_ts, &y1, &mo1, &d1, &h1, &m1) &&
        OrgParseClockTimestamp(now_ts, &y2, &mo2, &d2, &h2, &m2)) {
        mins = (OrgDayNumber(y2, mo2, d2) - OrgDayNumber(y1, mo1, d1)) * 1440 + (h2 * 60 + m2) - (h1 * 60 + m1);
        if (mins < 0) mins = 0;
    }
    char durbuf[32];
    std::snprintf(durbuf, sizeof(durbuf), "%lld:%02lld", mins / 60, mins % 60);
    std::vector<std::string> out = lines;
    out[static_cast<size_t>(clock.line)] = "  CLOCK: [" + clock.start_ts + "]--[" + now_ts + "] =>  " + durbuf;
    if (minutes) *minutes = static_cast<int>(mins);
    return out;
}

// --- Display-side scans (see org_doc.h) ---

int OrgHeadlineLevel(const std::string &line) {
    size_t stars = 0;
    while (stars < line.size() && line[stars] == '*') stars++;
    if (stars == 0 || stars >= line.size() || line[stars] != ' ') return 0;
    return static_cast<int>(stars);
}

int OrgHeadlineStarHideLen(const std::string &line) {
    const int level = OrgHeadlineLevel(line);
    return level > 0 ? level + 1 : 0;  // the stars, plus the space that ends them
}

int OrgHeadlineStarIndentCols(const std::string &line) {
    const int level = OrgHeadlineLevel(line);
    return level > 1 ? level - 1 : 0;  // level 1 sits at the margin
}

bool OrgEmphasisPreOk(char c) {
    // '\0' is how both callers spell "there is no character here", i.e.
    // start-of-line, which org's own regexp allows via its `^` branch.
    if (c == '\0') return true;
    switch (c) {
        case ' ': case '\t': case '-': case '(': case '\'': case '"': case '{':
            return true;
        default:
            return false;
    }
}

bool OrgEmphasisPostOk(char c) {
    if (c == '\0') return true;  // end-of-line, org's `$` branch
    switch (c) {
        case ' ': case '\t': case '-': case '.': case ',': case ':': case '!':
        case '?': case ';': case '\'': case '"': case ')': case '}': case '[':
            return true;
        default:
            return false;
    }
}

bool OrgEmphasisBorderBlank(char c) { return c == ' ' || c == '\t'; }

namespace {

// The characters org's own link syntax lets a bare URL run over --
// mep.nvim's MEP_URL_PATTERN body class, minus nothing. A trailing `.`
// or `,` IS in this set, which is deliberate: it is part of plenty of
// real URLs, and org's answer to "the sentence's full stop got eaten" is
// to bracket the link, not to guess.
/**
 * @brief Reports whether a character may appear in the body of a bare URL.
 * @param c the character to test
 * @return true if `c` is a URL body character
 */
bool IsUrlBody(unsigned char c) {
    if (std::isalnum(c) != 0) return true;
    switch (c) {
        case '-': case '.': case '_': case '~': case ':': case '/': case '?':
        case '#': case '[': case ']': case '@': case '!': case '$': case '&':
        case '\'': case '(': case ')': case '*': case '+': case ',': case ';':
        case '=': case '%':
            return true;
        default:
            return false;
    }
}

// The `target][description` split inside a `[[...]]`. A `]` that isn't
// followed by `[` is part of the target, so `[[a]b][c]]` targets `a]b`.
/**
 * @brief Splits an org link's inner text into a target and an optional description.
 * @param inner the text between the outer `[[` and `]]`
 * @param target set to the target portion
 * @param desc set to the description, or left empty when there is none
 */
void SplitOrgLinkInner(const std::string &inner, std::string *target, std::string *desc) {
    size_t rb = inner.find("][");
    if (rb == std::string::npos) {
        *target = inner;
        desc->clear();
        return;
    }
    // rb == 0 leaves an empty target, which ScanOrgLinkSpans rejects --
    // `[[][desc]]` has nothing to follow, so it isn't a link yet.
    *target = inner.substr(0, rb);
    *desc = inner.substr(rb + 2);
}

// What a concealed link should read as when it has no description of its
// own: the target with the scheme noise org also drops from a bare link's
// display. An `id:` target's raw uuid says nothing, so it shows unprefixed
// too -- there is nothing better to show, and the prefix is pure noise.
/**
 * @brief Chooses the text a concealed org link displays in place of its raw markup.
 * @param target the link's target
 * @param desc the link's description, or "" when it has none
 * @return the display text
 */
std::string OrgLinkDisplay(const std::string &target, const std::string &desc) {
    if (!desc.empty()) return desc;
    if (target.compare(0, 5, "file:") == 0) return target.substr(5);
    if (target.compare(0, 3, "id:") == 0) return target.substr(3);
    if (!target.empty() && (target[0] == '*' || target[0] == '#')) return target.substr(1);
    return target;
}

// Columns covered by a `=verbatim=` or `~code~` run, using org's own
// marker-boundary rules (a marker only opens after line-start or a
// non-word character and before a non-space, and only closes after a
// non-space and before line-end or a non-word character). Text inside one
// is literal: org does not linkify it, and neither should this -- this
// repo's own help/*.org writes `=[[file:x]]=` precisely to *show* link
// syntax, and treating that as a live link would both conceal the example
// and leave a stretch of prose clickable.
/**
 * @brief Marks the columns of a line that sit inside a `=verbatim=` or `~code~` run.
 * @param line the line to scan
 * @return one flag per byte of `line`, true where that byte is inside such a run
 */
std::vector<bool> OrgLiteralColumns(const std::string &line) {
    std::vector<bool> literal(line.size(), false);
    const int len = static_cast<int>(line.size());
    /**
     * @brief Fetches the character at `idx`, or NUL when out of bounds.
     * @param idx the index to read
     * @return the character, or '\0'
     */
    auto at = [&](int idx) -> char { return (idx >= 0 && idx < len) ? line[static_cast<size_t>(idx)] : '\0'; };
    int i = 0;
    while (i < len) {
        const char ch = line[static_cast<size_t>(i)];
        if (ch != '=' && ch != '~') {
            i++;
            continue;
        }
        const char pre = at(i - 1), nxt = at(i + 1);
        if (!(OrgEmphasisPreOk(pre) && nxt != '\0' && !OrgEmphasisBorderBlank(nxt) && nxt != ch)) {
            i++;
            continue;
        }
        int close = -1;
        for (int k = i + 1; k < len; k++) {
            if (line[static_cast<size_t>(k)] != ch) continue;
            if (!OrgEmphasisBorderBlank(at(k - 1)) && at(k - 1) != ch && OrgEmphasisPostOk(at(k + 1))) {
                close = k;
                break;
            }
        }
        if (close < 0) {
            i++;
            continue;
        }
        for (int c = i; c <= close; c++) literal[static_cast<size_t>(c)] = true;
        i = close + 1;
    }
    return literal;
}

}  // namespace

std::vector<OrgLinkSpanInfo> ScanOrgLinkSpans(const std::string &line) {
    std::vector<OrgLinkSpanInfo> spans;
    const int len = static_cast<int>(line.size());
    const std::vector<bool> literal = OrgLiteralColumns(line);
    // Columns a bracket link already covers, so the bare-URL pass below
    // doesn't also report the `https://...` sitting inside one.
    std::vector<bool> claimed(line.size(), false);

    size_t pos = 0;
    while (true) {
        size_t open = line.find("[[", pos);
        if (open == std::string::npos) break;
        size_t close = line.find("]]", open + 2);
        if (close == std::string::npos) break;
        const std::string inner = line.substr(open + 2, close - (open + 2));
        pos = close + 2;
        std::string target, desc;
        SplitOrgLinkInner(inner, &target, &desc);
        // `[[]]` and `[[][desc]]` are not links: with no target there is
        // nothing to follow, and concealing them would hide the very
        // markup someone is in the middle of typing.
        if (target.empty()) continue;
        if (literal[open]) continue;  // inside `=...=`/`~...~`: shown, not followed
        OrgLinkSpanInfo sp;
        sp.col_start = static_cast<int>(open);
        sp.col_end = static_cast<int>(pos);
        sp.target = target;
        sp.display = OrgLinkDisplay(target, desc);
        sp.bracketed = true;
        for (int c = sp.col_start; c < sp.col_end; c++) claimed[static_cast<size_t>(c)] = true;
        spans.push_back(std::move(sp));
    }

    int i = 0;
    while (i < len) {
        if (line.compare(static_cast<size_t>(i), 4, "http") != 0) {
            i++;
            continue;
        }
        int j = i + 4;
        if (j < len && line[static_cast<size_t>(j)] == 's') j++;
        if (line.compare(static_cast<size_t>(j), 3, "://") != 0) {
            i++;
            continue;
        }
        int body_start = j + 3;
        int k = body_start;
        while (k < len && IsUrlBody(static_cast<unsigned char>(line[static_cast<size_t>(k)]))) k++;
        if (k == body_start) {
            i++;
            continue;
        }
        if (!claimed[static_cast<size_t>(i)] && !literal[static_cast<size_t>(i)]) {
            OrgLinkSpanInfo sp;
            sp.col_start = i;
            sp.col_end = k;
            sp.target = line.substr(static_cast<size_t>(i), static_cast<size_t>(k - i));
            sp.display = sp.target;
            sp.bracketed = false;
            spans.push_back(std::move(sp));
        }
        i = k;
    }

    std::sort(spans.begin(), spans.end(),
              [](const OrgLinkSpanInfo &a, const OrgLinkSpanInfo &b) { return a.col_start < b.col_start; });
    return spans;
}

// --- Org tables: the wrapped display layout (org_doc.h's own section) ---

namespace {

// Byte length of the UTF-8 codepoint starting at `i`, clamped so a
// truncated or invalid sequence still advances by one byte rather than
// running off the end (the parsers here take whatever is in the buffer,
// which may be mid-edit and not yet valid UTF-8).
/**
 * @brief Returns the byte length of the UTF-8 codepoint starting at an offset, at least 1.
 * @param s the string to read
 * @param i the byte offset of the codepoint's first byte
 * @return the codepoint's length in bytes, clamped to the remaining string
 */
int OrgUtf8Len(const std::string &s, size_t i) {
    if (i >= s.size()) return 1;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = 1;
    if ((c & 0x80) == 0x00) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (i + static_cast<size_t>(len) > s.size()) len = static_cast<int>(s.size() - i);
    return len < 1 ? 1 : len;
}

// The first `width` display columns of `s`, on a codepoint boundary.
/**
 * @brief Takes a prefix of a string measured in display columns rather than bytes.
 * @param s the string to cut
 * @param width the number of codepoints to keep
 * @return the prefix, cut on a codepoint boundary
 */
std::string OrgTakeCols(const std::string &s, int width) {
    int seen = 0;
    size_t i = 0;
    while (i < s.size() && seen < width) {
        i += static_cast<size_t>(OrgUtf8Len(s, i));
        seen++;
    }
    return s.substr(0, i);
}

// Pads `s` out to `width` display columns with spaces. A cell already at
// or past its width is returned untouched -- over-wide cells are the
// wrapper's problem, not the padder's.
/**
 * @brief Right-pads a string with spaces to a given display width.
 * @param s the string to pad
 * @param width the target width in display columns
 * @return `s` padded to `width` columns
 */
std::string OrgPadCols(const std::string &s, int width) {
    const int have = OrgTableDisplayWidth(s);
    if (have >= width) return s;
    return s + std::string(static_cast<size_t>(width - have), ' ');
}

// --- Wrapping a cell while keeping track of where its text came from ---
// The wrap drops the spaces it breaks on and hard-splits an over-wide
// word, so an offset into a wrapped line is not an offset into the cell.
// A link's span is in the cell's coordinates, though, which leaves the
// planner needing the map back: each word is recorded as one chunk of
// `len` bytes drawn at `out` in its line and taken from `src` in the
// cell, and a link span intersected against those chunks comes out as
// the columns it occupies on each line it reaches.
struct OrgCellChunk {
    int out = 0;  // byte offset into the wrapped line
    int src = 0;  // byte offset into the cell's text
    int len = 0;
};

struct OrgCellLine {
    std::string text;
    std::vector<OrgCellChunk> chunks;
};

// The wrap itself: greedy at `width` display columns, breaking on spaces
// and splitting a word only when the word alone is wider than the
// column. OrgTableWrapCell is this with the chunks dropped, and the two
// must stay one algorithm -- a cell whose text and whose link columns
// disagreed would paint a link face over the wrong characters.
/**
 * @brief Word-wraps a cell to a column width, recording where each placed word came from.
 * @param text the cell's text
 * @param width the target width in display columns (values below 1 are treated as 1)
 * @param out_lines the wrapped lines with their source chunks, never empty
 */
void WrapCellTracked(const std::string &text, int width, std::vector<OrgCellLine> *out_lines) {
    const int w = std::max(1, width);
    std::vector<OrgCellLine> &out = *out_lines;
    out.clear();
    // Split on runs of spaces: the cell text arrives already trimmed, and
    // interior runs collapse to one space, which is what makes a wrapped
    // cell read as prose instead of keeping the column padding of
    // whatever the author happened to type.
    struct Word {
        std::string text;
        int src = 0;
    };
    std::vector<Word> words;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\t') {
            i++;
            continue;
        }
        size_t k = i;
        while (k < text.size() && text[k] != ' ' && text[k] != '\t') k++;
        words.push_back(Word{text.substr(i, k - i), static_cast<int>(i)});
        i = k;
    }
    OrgCellLine cur;
    for (const Word &word : words) {
        std::string piece = word.text;
        int piece_src = word.src;
        // A word wider than the whole column can't be placed by breaking
        // on spaces: flush what we have and hard-split it across as many
        // lines as it needs (a long URL, a path, a chemical name).
        if (OrgTableDisplayWidth(piece) > w) {
            if (!cur.text.empty()) {
                out.push_back(std::move(cur));
                cur = OrgCellLine();
            }
            while (OrgTableDisplayWidth(piece) > w) {
                const std::string head = OrgTakeCols(piece, w);
                OrgCellLine line;
                line.chunks.push_back(OrgCellChunk{0, piece_src, static_cast<int>(head.size())});
                line.text = head;
                out.push_back(std::move(line));
                piece_src += static_cast<int>(head.size());
                piece = piece.substr(head.size());
            }
            if (!piece.empty()) {
                cur.chunks.push_back(OrgCellChunk{0, piece_src, static_cast<int>(piece.size())});
                cur.text = piece;
            }
            continue;
        }
        const int extra = cur.text.empty() ? 0 : 1;
        if (OrgTableDisplayWidth(cur.text) + extra + OrgTableDisplayWidth(piece) > w) {
            out.push_back(std::move(cur));
            cur = OrgCellLine();
            cur.chunks.push_back(OrgCellChunk{0, piece_src, static_cast<int>(piece.size())});
            cur.text = piece;
        } else {
            if (!cur.text.empty()) cur.text += " ";
            cur.chunks.push_back(
                OrgCellChunk{static_cast<int>(cur.text.size()), piece_src, static_cast<int>(piece.size())});
            cur.text += piece;
        }
    }
    if (!cur.text.empty() || out.empty()) out.push_back(std::move(cur));
}

// Where `link` lands on one wrapped line, appended to `into` with
// `base` (the cell's own offset within the assembled line) added on.
// One chunk per word, so a description of three words comes back as
// three spans separated by the single space the wrap joined them with;
// the caller merges those back together.
/**
 * @brief Appends the spans a cell link occupies on one wrapped line.
 * @param line the wrapped line and its source chunks
 * @param link the link's span in the cell's own text
 * @param base the byte offset the cell's text starts at in the assembled line
 * @param into the span list to append to
 */
void AppendCellLinkSpans(const OrgCellLine &line, const OrgTableCellLink &link, int base,
                         std::vector<OrgTableWrapLink> *into) {
    for (const OrgCellChunk &ch : line.chunks) {
        const int lo = std::max(link.start, ch.src);
        const int hi = std::min(link.end, ch.src + ch.len);
        if (hi <= lo) continue;
        into->push_back(OrgTableWrapLink{base + ch.out + (lo - ch.src), base + ch.out + (hi - ch.src), link.target,
                                        link.concealed});
    }
}

// Sorts one line's link spans into column order and joins the ones a
// word break split, so a multi-word description underlines as one run
// instead of a dotted sequence with a gap at every space.
/**
 * @brief Sorts a line's link spans and merges the pieces of a single link back together.
 * @param links the spans to normalize, in place
 */
void MergeWrapLinks(std::vector<OrgTableWrapLink> *links) {
    std::sort(links->begin(), links->end(), [](const OrgTableWrapLink &a, const OrgTableWrapLink &b) {
        return a.col_start < b.col_start;
    });
    size_t kept = 0;
    for (size_t i = 0; i < links->size(); i++) {
        OrgTableWrapLink &cand = (*links)[i];
        if (kept > 0) {
            OrgTableWrapLink &prev = (*links)[kept - 1];
            // `+ 1` is the joining space the wrap put between two words
            // of the same description; anything further apart is a
            // genuine gap and stays one.
            if (prev.target == cand.target && prev.concealed == cand.concealed &&
                cand.col_start <= prev.col_end + 1) {
                prev.col_end = std::max(prev.col_end, cand.col_end);
                continue;
            }
        }
        // `kept == i` until the first merge, and moving an element onto
        // itself would leave its target an empty string -- which then
        // matches nothing and defeats every later merge.
        if (kept != i) (*links)[kept] = std::move(cand);
        kept++;
    }
    links->resize(kept);
}

}  // namespace

int OrgTableDisplayWidth(const std::string &s) {
    int width = 0;
    for (char c : s) {
        // Every byte that isn't a UTF-8 continuation byte starts a new
        // codepoint, so this counts codepoints without decoding them.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) width++;
    }
    return width;
}

std::vector<std::string> OrgTableWrapCell(const std::string &text, int width) {
    std::vector<OrgCellLine> lines;
    WrapCellTracked(text, width, &lines);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (OrgCellLine &l : lines) out.push_back(std::move(l.text));
    return out;
}

std::string OrgTableCellDisplayText(const std::string &cell, bool conceal, std::vector<OrgTableCellLink> *links) {
    if (links) links->clear();
    const std::vector<OrgLinkSpanInfo> spans = ScanOrgLinkSpans(cell);
    if (spans.empty()) return cell;
    std::string out;
    size_t pos = 0;
    for (const OrgLinkSpanInfo &sp : spans) {
        const size_t a = std::min(static_cast<size_t>(std::max(0, sp.col_start)), cell.size());
        const size_t b = std::min(static_cast<size_t>(std::max(0, sp.col_end)), cell.size());
        // ScanOrgLinkSpans hands them back in column order and never
        // overlapping, so a span that walks backwards can only be a
        // clamped, degenerate one -- skipped rather than allowed to
        // scramble the output.
        if (b <= a || a < pos) continue;
        out.append(cell, pos, a - pos);
        // A bare URL's own `display` is the URL itself, so this stands
        // only bracket links down -- and only while concealment is on,
        // since with it off the renderer shows the markup and the
        // columns have to be budgeted for it.
        const bool concealed = conceal && sp.bracketed;
        const std::string shown = concealed ? sp.display : cell.substr(a, b - a);
        if (links && !shown.empty()) {
            links->push_back(OrgTableCellLink{static_cast<int>(out.size()),
                                              static_cast<int>(out.size() + shown.size()), sp.target, concealed});
        }
        out += shown;
        pos = b;
    }
    out.append(cell, pos, std::string::npos);
    return out;
}

OrgTableWrapPlan PlanOrgTableWrap(const std::vector<OrgTableCells> &rows, int budget, int indent) {
    OrgTableWrapPlan plan;
    size_t cols = 0;
    for (const OrgTableCells &r : rows) {
        if (!r.is_sep) cols = std::max(cols, r.cells.size());
    }
    if (cols == 0) return plan;

    // Natural width: the widest cell in each column, i.e. exactly the
    // widths :MepOrgTableAlign would write into the file.
    std::vector<int> natural(cols, 0);
    for (const OrgTableCells &r : rows) {
        if (r.is_sep) continue;
        for (size_t c = 0; c < r.cells.size(); c++) {
            natural[c] = std::max(natural[c], OrgTableDisplayWidth(r.cells[c]));
        }
    }

    // Every row spends `indent` columns of leading whitespace, one column
    // per `|` (cols + 1 of them) and two spaces of padding per cell, so
    // only what's left is available to the cell text itself.
    const int chrome = indent + static_cast<int>(cols) + 1 + 2 * static_cast<int>(cols);
    const int avail = budget - chrome;
    int natural_total = 0;
    for (int n : natural) natural_total += n;

    std::vector<int> widths = natural;
    if (avail > 0 && natural_total > avail) {
        plan.wrapped = true;
        // Water-filling: columns that already fit an equal share of the
        // budget keep their natural width and leave the rest of the
        // budget to the columns that don't, repeated until no column
        // fits its share. The remainder is then split between the
        // over-wide columns in proportion to how much text they hold.
        std::vector<bool> fixed(cols, false);
        int remaining = avail;
        size_t unfixed = cols;
        bool progress = true;
        while (progress && unfixed > 0) {
            progress = false;
            const int fair = remaining / static_cast<int>(unfixed);
            for (size_t c = 0; c < cols; c++) {
                if (fixed[c] || natural[c] > fair) continue;
                fixed[c] = true;
                widths[c] = natural[c];
                remaining -= natural[c];
                unfixed--;
                progress = true;
            }
            if (unfixed == 0) break;
        }
        if (unfixed > 0) {
            int share_total = 0;
            for (size_t c = 0; c < cols; c++) {
                if (!fixed[c]) share_total += natural[c];
            }
            for (size_t c = 0; c < cols; c++) {
                if (fixed[c]) continue;
                const int share = share_total > 0
                                      ? static_cast<int>(static_cast<long long>(remaining) * natural[c] / share_total)
                                      : remaining / static_cast<int>(unfixed);
                widths[c] = std::max(std::min(natural[c], kOrgTableMinColWidth), share);
            }
        }
        // The proportional split floors, and the minimum-width clamp can
        // push back over the budget, so settle the difference a column at
        // a time: shave the widest column while over, and feed the column
        // furthest short of its own content while under.
        /**
         * @brief Sums the planned column widths.
         * @return the total content width across every column
         */
        auto total = [&] {
            int t = 0;
            for (int wv : widths) t += wv;
            return t;
        };
        while (total() > avail) {
            size_t pick = cols;
            for (size_t c = 0; c < cols; c++) {
                if (widths[c] > 1 && (pick == cols || widths[c] > widths[pick])) pick = c;
            }
            if (pick == cols) break;
            widths[pick]--;
        }
        while (total() < avail) {
            size_t pick = cols;
            for (size_t c = 0; c < cols; c++) {
                if (widths[c] >= natural[c]) continue;
                if (pick == cols || natural[c] - widths[c] > natural[pick] - widths[pick]) pick = c;
            }
            if (pick == cols) break;
            widths[pick]++;
        }
    }
    for (int &wv : widths) wv = std::max(1, wv);
    plan.col_widths = widths;

    const std::string lead(static_cast<size_t>(std::max(0, indent)), ' ');
    plan.rows.reserve(rows.size());
    static const std::vector<OrgTableCellLink> kNoCellLinks;
    for (const OrgTableCells &r : rows) {
        std::vector<OrgTableWrapLine> out;
        if (r.is_sep) {
            OrgTableWrapLine line;
            line.text = lead + "|";
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) line.text += "+";
                line.text += std::string(static_cast<size_t>(widths[c] + 2), '-');
            }
            line.text += "|";
            out.push_back(std::move(line));
            plan.rows.push_back(std::move(out));
            continue;
        }
        // Wrap every cell first: the row draws as however many lines its
        // tallest cell needs, with the shorter cells blank underneath.
        std::vector<std::vector<OrgCellLine>> cell_lines(cols);
        size_t height = 1;
        for (size_t c = 0; c < cols; c++) {
            const std::string &txt = c < r.cells.size() ? r.cells[c] : std::string();
            WrapCellTracked(txt, widths[c], &cell_lines[c]);
            height = std::max(height, cell_lines[c].size());
        }
        for (size_t l = 0; l < height; l++) {
            OrgTableWrapLine line;
            line.text = lead + "|";
            for (size_t c = 0; c < cols; c++) {
                if (c > 0) line.text += "|";
                line.text += " ";
                // Where this cell's text starts in the assembled line,
                // which is what a link span inside it has to be offset
                // by to come out in the line's own columns.
                const int cell_base = static_cast<int>(line.text.size());
                if (l < cell_lines[c].size()) {
                    const OrgCellLine &cl = cell_lines[c][l];
                    const std::vector<OrgTableCellLink> &cell_links =
                        c < r.links.size() ? r.links[c] : kNoCellLinks;
                    for (const OrgTableCellLink &lk : cell_links) {
                        AppendCellLinkSpans(cl, lk, cell_base, &line.links);
                    }
                    line.text += OrgPadCols(cl.text, widths[c]);
                } else {
                    line.text += OrgPadCols(std::string(), widths[c]);
                }
                line.text += " ";
            }
            line.text += "|";
            MergeWrapLinks(&line.links);
            out.push_back(std::move(line));
        }
        plan.rows.push_back(std::move(out));
    }
    return plan;
}

// --- Org inline images: the drawn figure's geometry (org_doc.h) -------

OrgImageLayout OrgImageLayoutFor(int px_w, int px_h, float char_width, float line_height, int avail_cols,
                                 int text_cols) {
    OrgImageLayout out;
    if (char_width <= 0.0f) char_width = 1.0f;
    if (line_height <= 0.0f) line_height = 1.0f;
    // `:set textwidth=0` (wrapping off) still needs a measure to lay a
    // figure out against; org's own conventional 80 is it.
    if (text_cols <= 0) text_cols = kOrgImageLineWidthChars;
    // A pane that hasn't reported its width yet (Pane::text_cols is 0
    // until first drawn) is measured as if it were exactly as wide as
    // the text column -- the ordinary case, and the one that makes the
    // very first frame agree with every later one.
    if (avail_cols <= 0) avail_cols = text_cols;

    // The column the figure is centered in: the text width, or the pane
    // if that is narrower.
    const float box_w = static_cast<float>(avail_cols < text_cols ? avail_cols : text_cols) * char_width;
    // ...and the widest it may be drawn within that column.
    const float target_cols =
        std::min(static_cast<float>(avail_cols), static_cast<float>(text_cols) * kOrgImageWidthFraction);
    const float target_w = target_cols * char_width;

    if (px_w <= 0 || px_h <= 0) {
        out.width = target_w;
        out.slots = kOrgImageUnknownSlots;
        out.height = static_cast<float>(out.slots) * line_height;
        out.offset_x = std::max(0.0f, (box_w - out.width) * 0.5f);
        return out;
    }

    // Downscale to the target width, but never *up*: past its native
    // size an image only gets blurrier, and a small figure sitting at
    // its own size reads as deliberate rather than broken.
    float scale = std::min(1.0f, target_w / static_cast<float>(px_w));
    // A portrait tall enough to run past the height ceiling shrinks the
    // rest of the way instead of being cropped or letterboxed.
    scale = std::min(scale, static_cast<float>(kOrgImageMaxSlots) * line_height / static_cast<float>(px_h));

    out.width = static_cast<float>(px_w) * scale;
    out.height = static_cast<float>(px_h) * scale;
    // Round the reserved height up to whole line-heights -- the row grid
    // is the only granularity a slot count has. The epsilon keeps an
    // image whose height lands exactly on a multiple (the scaled-to-the-
    // ceiling case above, most visibly) from tipping into one extra,
    // empty, slot on a float hair.
    const float rows = out.height / line_height;
    out.slots = std::max(1, static_cast<int>(std::ceil(rows - 0.001f)));
    out.offset_x = std::max(0.0f, (box_w - out.width) * 0.5f);
    return out;
}

// --- Per-src-block language-server status (<leader>ots) ---

namespace {

/**
 * @brief Renders a count with a singular/plural noun ("1 error", "2 errors").
 * @param n The count.
 * @param noun The singular noun; an "s" is appended for any other count.
 * @return The formatted phrase.
 */
std::string CountPhrase(int n, const char *noun) {
    std::string out = std::to_string(n) + " " + noun;
    if (n != 1) out += "s";
    return out;
}

}  // namespace

std::string FormatOrgLspStatus(const OrgLspStatus &st) {
    // A block with nothing to attach says why in plain words rather than
    // naming a server that doesn't exist. The language is only worth
    // repeating here in the case the title bar can't already show it --
    // a block with no language tag at all has an "src" chip up there and
    // nothing else, so "no language set" is the whole answer.
    if (st.state == OrgLspState::kUnsupported || st.server.empty()) {
        if (st.lang.empty()) return "LSP: no language set";
        return "LSP: no server for " + st.lang;
    }
    // Everywhere else the language is already on the card's title bar, so
    // the line leads with the one thing that bar doesn't carry: which
    // server, and what it is doing.
    std::string out = st.server + ": ";
    switch (st.state) {
        case OrgLspState::kIdle:
            // Not "off": the bridge attaches the first time an LSP
            // feature actually runs inside the block (hover, completion,
            // goto-definition -- mep_polyglot_context_at_cursor's callers),
            // and saying so is the difference between an idle block and a
            // broken one.
            return out + "idle (attaches on first use)";
        case OrgLspState::kStarting:
            return out + "starting...";
        case OrgLspState::kExited:
            return out + "not running";
        case OrgLspState::kReady:
            break;
        case OrgLspState::kUnsupported:
            break;
    }
    out += "ready";
    std::vector<std::string> counts;
    if (st.errors > 0) counts.push_back(CountPhrase(st.errors, "error"));
    if (st.warnings > 0) counts.push_back(CountPhrase(st.warnings, "warning"));
    if (st.hints > 0) counts.push_back(CountPhrase(st.hints, "hint"));
    if (counts.empty()) return out + ", no diagnostics";
    out += ", ";
    for (size_t i = 0; i < counts.size(); i++) {
        if (i > 0) out += ", ";
        out += counts[i];
    }
    return out;
}

OrgLspStatusTone OrgLspStatusToneOf(const OrgLspStatus &st) {
    // Counts first: what the server found matters more than the fact that
    // it is running, and a block whose code is broken should read as
    // broken even though its client is perfectly healthy.
    if (st.errors > 0) return OrgLspStatusTone::kError;
    if (st.warnings > 0) return OrgLspStatusTone::kWarn;
    // A client that was started and is gone is a real fault (a crashed
    // server, a failed spawn) -- unlike kIdle, which is the normal
    // resting state of a block the cursor hasn't visited.
    if (st.state == OrgLspState::kExited) return OrgLspStatusTone::kWarn;
    if (st.state == OrgLspState::kReady) return OrgLspStatusTone::kOk;
    return OrgLspStatusTone::kMuted;
}

OrgLspState OrgLspStateFromName(const std::string &name) {
    if (name == "idle") return OrgLspState::kIdle;
    if (name == "starting") return OrgLspState::kStarting;
    if (name == "ready") return OrgLspState::kReady;
    if (name == "exited") return OrgLspState::kExited;
    return OrgLspState::kUnsupported;
}

OrgBlockPlay OrgBlockPlayFor(const OrgBlockPlayInput &in) {
    // Only src blocks run, and only once their closer exists: a block
    // still being typed has no body for Editor::OrgSrcBlockAt to find, so
    // a button on it could only ever produce "Not in a src block".
    if (!in.is_src || !in.closed) return OrgBlockPlay::kHidden;
    if (in.lang.empty()) return OrgBlockPlay::kDisabled;
    // Exactly the two values mep.org_babel_execute itself refuses, spelled
    // the same way (lowercase, no trimming beyond the header parse) -- the
    // button's job is to predict that path's answer, so matching it
    // loosely here would only let the two disagree.
    if (in.eval_arg == "no" || in.eval_arg == "never") return OrgBlockPlay::kDisabled;
    return OrgBlockPlay::kReady;
}

std::string OrgBlockPlayHint(const OrgBlockPlayInput &in) {
    switch (OrgBlockPlayFor(in)) {
        case OrgBlockPlay::kHidden:
            return "";
        case OrgBlockPlay::kDisabled:
            // Which of the two reasons it is: the `:eval` gate is a
            // deliberate choice someone made about this block, and saying
            // so is the difference between "you told me not to" and "I
            // can't tell what this is".
            if (in.lang.empty()) return "No language on this block -- nothing to run";
            return "Blocked by :eval " + in.eval_arg;
        case OrgBlockPlay::kReady:
            break;
    }
    return "Run this " + in.lang + " block (C-c C-c)";
}
