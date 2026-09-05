#include "org_doc.h"

#include <algorithm>
#include <cctype>
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

std::vector<OrgTodoItem> OrgTodoListItems(const std::vector<std::string> &lines) {
    std::vector<OrgTodoItem> items;
    OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        if (h.todo_keyword.empty()) continue;
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

    // Keyworded headlines by their line index -- the identity the panel
    // hands back in OrgTodoItem::line.
    std::vector<const OrgHeadline *> by_line(lines.size(), nullptr);
    for (const OrgHeadline &h : outline.headlines) {
        if (h.todo_keyword.empty()) continue;
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
