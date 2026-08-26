#include "org_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string LStrip(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

// 0 = Sunday .. 6 = Saturday, via Sakamoto's algorithm.
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
long long DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
void CivilFromDays(long long z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : 9 - 12);  // mp<10 -> m=mp+3, else m=mp-9
    y = static_cast<int>(yy + (m <= 2));
}

// "#+TODO: TODO(t) IN-PROGRESS(i) | DONE(d)" -- case-insensitive keyword,
// '(' fast-select suffixes stripped, keywords before '|' -> todo_out,
// after -> done_out. Without a '|' at all, the last token is treated as
// the done keyword (matching real org's own default when no separator is
// given), unless there's only one token.
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
        if (paren != std::string::npos) tok = tok.substr(0, paren);
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

bool ParsePlanningLine(const std::string &line, std::string &sched_raw, std::string &deadline_raw) {
    std::string trimmed = LStrip(line);
    if (trimmed.compare(0, 9, "SCHEDULED") != 0 && trimmed.compare(0, 8, "DEADLINE") != 0) return false;
    size_t pos = 0;
    sched_raw = ExtractTimestampAfterKeyword(line, "SCHEDULED", pos);
    pos = 0;
    deadline_raw = ExtractTimestampAfterKeyword(line, "DEADLINE", pos);
    return !sched_raw.empty() || !deadline_raw.empty();
}

bool IsDrawerLine(const std::string &line, const std::string &name) {
    return EqualsIgnoreCase(LStrip(line), ":" + name + ":");
}

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
                out.headlines[this_index].planning_line = static_cast<int>(pl);
                if (!sched_raw.empty()) out.headlines[this_index].scheduled = ParseTimestamp(sched_raw);
                if (!deadline_raw.empty()) out.headlines[this_index].deadline = ParseTimestamp(deadline_raw);
                pl++;
            }
        }
        if (pl < lines.size() && IsDrawerLine(lines[pl], "PROPERTIES")) {
            size_t d = pl + 1;
            while (d < lines.size() && !IsDrawerLine(lines[d], "END")) {
                std::string key, val;
                if (ParsePropertyLine(lines[d], key, val)) {
                    if (EqualsIgnoreCase(key, "EFFORT")) out.headlines[this_index].effort = val;
                    else if (EqualsIgnoreCase(key, "ID")) out.headlines[this_index].id = val;
                    else if (EqualsIgnoreCase(key, "BLOCKER")) out.headlines[this_index].blockers = ParseDependencyIds(val);
                    else if (EqualsIgnoreCase(key, "ASSIGNEE") || EqualsIgnoreCase(key, "TEAM")) {
                        out.headlines[this_index].assignee = val;
                    } else if (EqualsIgnoreCase(key, "PROGRESS")) {
                        int progress = 0;
                        if (std::sscanf(val.c_str(), "%d", &progress) == 1) {
                            out.headlines[this_index].progress = std::clamp(progress, 0, 100);
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
