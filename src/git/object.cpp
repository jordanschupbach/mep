// Object ids, object (de)serialization, signatures and dates.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "git/git.h"
#include "git/sha1.h"

namespace mep::git {

bool Oid::IsNull() const {
    for (uint8_t c : b)
        if (c) return false;
    return true;
}

std::string Oid::Hex() const {
    static const char *kHex = "0123456789abcdef";
    std::string s(40, '0');
    for (size_t i = 0; i < 20; i++) {
        s[2 * i] = kHex[b[i] >> 4];
        s[2 * i + 1] = kHex[b[i] & 15];
    }
    return s;
}

namespace {
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool Oid::FromHex(std::string_view hex, Oid &out) {
    if (hex.size() != 40) return false;
    for (size_t i = 0; i < 20; i++) {
        int hi = HexVal(hex[2 * i]), lo = HexVal(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.b[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

const char *TypeName(ObjType t) {
    switch (t) {
        case ObjType::Commit: return "commit";
        case ObjType::Tree: return "tree";
        case ObjType::Blob: return "blob";
        case ObjType::Tag: return "tag";
        default: return "";
    }
}

ObjType TypeFromName(std::string_view name) {
    if (name == "commit") return ObjType::Commit;
    if (name == "tree") return ObjType::Tree;
    if (name == "blob") return ObjType::Blob;
    if (name == "tag") return ObjType::Tag;
    return ObjType::None;
}

Oid HashObject(ObjType t, std::string_view data) {
    Sha1 h;
    std::string hdr = std::string(TypeName(t)) + " " + std::to_string(data.size());
    h.Update(hdr.data(), hdr.size() + 1);  // including the NUL
    h.Update(data);
    Oid o;
    o.b = h.Final();
    return o;
}

// ------------------------------------------------------------ signatures

std::string FormatTz(int minutes) {
    char buf[16];
    int a = minutes < 0 ? -minutes : minutes;
    std::snprintf(buf, sizeof buf, "%c%02d%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
    return buf;
}

bool ParseSignature(std::string_view line, Signature &out) {
    size_t lt = line.find('<');
    size_t gt = line.rfind('>');
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt < lt) return false;
    std::string_view name = line.substr(0, lt);
    while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
    out.name = std::string(name);
    out.email = std::string(line.substr(lt + 1, gt - lt - 1));
    std::string_view rest = line.substr(gt + 1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    out.time = 0;
    out.tz = 0;
    out.tz_str = "+0000";
    size_t i = 0;
    while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) {
        out.time = out.time * 10 + (rest[i] - '0');
        i++;
    }
    while (i < rest.size() && rest[i] == ' ') i++;
    if (i < rest.size() && (rest[i] == '+' || rest[i] == '-')) {
        std::string_view tz = rest.substr(i);
        size_t e = 1;
        while (e < tz.size() && std::isdigit(static_cast<unsigned char>(tz[e]))) e++;
        out.tz_str = std::string(tz.substr(0, e));
        int v = std::atoi(out.tz_str.c_str() + 1);
        int m = (v / 100) * 60 + v % 100;
        out.tz = tz[0] == '-' ? -m : m;
    }
    return true;
}

std::string FormatSignature(const Signature &s) {
    return s.name + " <" + s.email + "> " + std::to_string(s.time) + " " +
           (s.tz_str.empty() ? FormatTz(s.tz) : s.tz_str);
}

// ------------------------------------------------------------ commits

bool ParseCommit(std::string_view data, Commit &out) {
    out = Commit{};
    size_t pos = 0;
    bool have_tree = false;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string_view::npos) nl = data.size();
        std::string_view line = data.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            out.message = std::string(data.substr(std::min(pos, data.size())));
            return have_tree;
        }
        if (line[0] == ' ') continue;  // continuation of a multi-line header (gpgsig, mergetag)
        size_t sp = line.find(' ');
        std::string_view key = line.substr(0, sp);
        std::string_view val = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        if (key == "tree") {
            have_tree = Oid::FromHex(val, out.tree);
        } else if (key == "parent") {
            Oid p;
            if (Oid::FromHex(val, p)) out.parents.push_back(p);
        } else if (key == "author") {
            ParseSignature(val, out.author);
        } else if (key == "committer") {
            ParseSignature(val, out.committer);
        } else if (key == "encoding") {
            out.encoding = std::string(val);
        }
    }
    return have_tree;
}

std::string SerializeCommit(const Commit &c) {
    std::string s = "tree " + c.tree.Hex() + "\n";
    for (const Oid &p : c.parents) s += "parent " + p.Hex() + "\n";
    s += "author " + FormatSignature(c.author) + "\n";
    s += "committer " + FormatSignature(c.committer) + "\n";
    if (!c.encoding.empty()) s += "encoding " + c.encoding + "\n";
    s += "\n";
    s += c.message;
    return s;
}

namespace {
// Length of `line` with trailing whitespace removed (git's is_blank_line).
size_t TrimmedLen(std::string_view line) {
    size_t n = line.size();
    while (n && std::isspace(static_cast<unsigned char>(line[n - 1]))) n--;
    return n;
}
size_t SkipBlankLines(std::string_view msg, size_t pos) {
    while (pos < msg.size()) {
        size_t nl = msg.find('\n', pos);
        size_t end = nl == std::string_view::npos ? msg.size() : nl + 1;
        if (TrimmedLen(msg.substr(pos, end - pos)) != 0) break;
        pos = end;
    }
    return pos;
}
}  // namespace

std::string CommitSubject(const std::string &message) {
    std::string_view msg = message;
    size_t pos = SkipBlankLines(msg, 0);
    std::string out;
    bool first = true;
    while (pos < msg.size()) {
        size_t nl = msg.find('\n', pos);
        size_t end = nl == std::string_view::npos ? msg.size() : nl + 1;
        std::string_view line = msg.substr(pos, end - pos);
        size_t len = TrimmedLen(line);
        if (len == 0) break;
        if (!first) out += ' ';
        out.append(line.substr(0, len));
        first = false;
        pos = end;
    }
    return out;
}

std::string CommitBody(const std::string &message) {
    std::string_view msg = message;
    size_t pos = SkipBlankLines(msg, 0);
    while (pos < msg.size()) {
        size_t nl = msg.find('\n', pos);
        size_t end = nl == std::string_view::npos ? msg.size() : nl + 1;
        if (TrimmedLen(msg.substr(pos, end - pos)) == 0) break;
        pos = end;
    }
    pos = SkipBlankLines(msg, pos);
    return std::string(msg.substr(pos));
}

// ------------------------------------------------------------ trees

bool ParseTree(std::string_view data, std::vector<TreeEntry> &out) {
    out.clear();
    size_t pos = 0;
    while (pos < data.size()) {
        size_t sp = data.find(' ', pos);
        if (sp == std::string_view::npos) return false;
        uint32_t mode = 0;
        for (size_t i = pos; i < sp; i++) {
            if (data[i] < '0' || data[i] > '7') return false;
            mode = mode * 8 + static_cast<uint32_t>(data[i] - '0');
        }
        size_t nul = data.find('\0', sp + 1);
        if (nul == std::string_view::npos || nul + 21 > data.size()) return false;
        TreeEntry e;
        e.mode = mode;
        e.name = std::string(data.substr(sp + 1, nul - sp - 1));
        std::memcpy(e.oid.b.data(), data.data() + nul + 1, 20);
        out.push_back(std::move(e));
        pos = nul + 21;
    }
    return true;
}

namespace {
bool TreeLess(const TreeEntry &a, const TreeEntry &b) {
    std::string ka = a.name, kb = b.name;
    if (a.IsTree()) ka += '/';
    if (b.IsTree()) kb += '/';
    return ka < kb;
}
}  // namespace

std::string SerializeTree(std::vector<TreeEntry> entries) {
    std::sort(entries.begin(), entries.end(), TreeLess);
    std::string s;
    for (const TreeEntry &e : entries) {
        char mode[16];
        std::snprintf(mode, sizeof mode, "%o", e.mode);
        s += mode;
        s += ' ';
        s += e.name;
        s += '\0';
        s.append(reinterpret_cast<const char *>(e.oid.b.data()), 20);
    }
    return s;
}

bool ParseTag(std::string_view data, Tag &out) {
    out = Tag{};
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string_view::npos) nl = data.size();
        std::string_view line = data.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) {
            out.message = std::string(data.substr(std::min(pos, data.size())));
            break;
        }
        size_t sp = line.find(' ');
        std::string_view key = line.substr(0, sp);
        std::string_view val = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        if (key == "object") Oid::FromHex(val, out.object);
        else if (key == "type") out.type = TypeFromName(val);
        else if (key == "tag") out.name = std::string(val);
        else if (key == "tagger") out.has_tagger = ParseSignature(val, out.tagger);
    }
    return !out.object.IsNull();
}

// ------------------------------------------------------------ dates

namespace {
struct Tm {
    int year, mon, mday, hour, min, sec, wday;
};
Tm Broken(int64_t t, int tz) {
    int64_t s = t + static_cast<int64_t>(tz) * 60;
    int64_t days = s / 86400;
    int64_t rem = s % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    Tm tm{};
    tm.hour = static_cast<int>(rem / 3600);
    tm.min = static_cast<int>((rem % 3600) / 60);
    tm.sec = static_cast<int>(rem % 60);
    tm.wday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
    // Civil-from-days (Howard Hinnant).
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    tm.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    tm.mon = static_cast<int>(m);
    tm.mday = static_cast<int>(d);
    return tm;
}
int64_t DaysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
const char *kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}  // namespace

std::string FormatDateShort(int64_t t, int tz) {
    Tm tm = Broken(t, tz);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.year, tm.mon, tm.mday);
    return buf;
}

std::string FormatDateNormal(int64_t t, int tz) {
    Tm tm = Broken(t, tz);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s %s %d %02d:%02d:%02d %d %s", kWeekdays[tm.wday], kMonths[tm.mon - 1], tm.mday,
                  tm.hour, tm.min, tm.sec, tm.year, FormatTz(tz).c_str());
    return buf;
}

std::string FormatDateIso(int64_t t, int tz) {
    Tm tm = Broken(t, tz);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d %s", tm.year, tm.mon, tm.mday, tm.hour, tm.min,
                  tm.sec, FormatTz(tz).c_str());
    return buf;
}

// git's show_date_relative, word for word.
std::string FormatDateRelative(int64_t t, int64_t now) {
    auto plural = [](int64_t n, const char *unit) {
        return std::to_string(n) + " " + unit + (n == 1 ? "" : "s");
    };
    if (now < t) return "in the future";
    int64_t diff = now - t;
    if (diff < 90) return plural(diff, "second") + " ago";
    diff = (diff + 30) / 60;
    if (diff < 90) return plural(diff, "minute") + " ago";
    diff = (diff + 30) / 60;
    if (diff < 36) return plural(diff, "hour") + " ago";
    diff = (diff + 12) / 24;
    if (diff < 14) return plural(diff, "day") + " ago";
    if (diff < 70) return plural((diff + 3) / 7, "week") + " ago";
    if (diff < 365) return plural((diff + 15) / 30, "month") + " ago";
    if (diff < 1825) {
        int64_t totalmonths = (diff * 12 * 2 + 365) / (365 * 2);
        int64_t years = totalmonths / 12;
        int64_t months = totalmonths % 12;
        if (months) return plural(years, "year") + ", " + plural(months, "month") + " ago";
        return plural(years, "year") + " ago";
    }
    return plural((diff + 183) / 365, "year") + " ago";
}

bool ParseDate(std::string_view s, int64_t &t, int &tz) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return false;
    auto parse_tz = [&](std::string_view z) -> bool {
        while (!z.empty() && z.front() == ' ') z.remove_prefix(1);
        if (z.empty()) {
            tz = 0;
            return true;
        }
        if (z == "Z" || z == "UTC" || z == "GMT") {
            tz = 0;
            return true;
        }
        if (z[0] != '+' && z[0] != '-') return false;
        std::string digits;
        for (char c : z.substr(1))
            if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        if (digits.size() != 4 && digits.size() != 2) return false;
        int hh = std::atoi(digits.substr(0, 2).c_str());
        int mm = digits.size() == 4 ? std::atoi(digits.substr(2).c_str()) : 0;
        tz = (hh * 60 + mm) * (z[0] == '-' ? -1 : 1);
        return true;
    };
    if (s[0] == '@' || std::all_of(s.begin(), s.begin() + static_cast<long>(std::min(s.find(' '), s.size())),
                                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        std::string_view num = s[0] == '@' ? s.substr(1) : s;
        size_t sp = num.find(' ');
        std::string n(num.substr(0, sp));
        if (n.empty() || n.size() < 6) {
            // A short all-digit string is not a unix timestamp; fall through.
        } else {
            t = std::strtoll(n.c_str(), nullptr, 10);
            return parse_tz(sp == std::string_view::npos ? std::string_view() : num.substr(sp + 1));
        }
    }
    // YYYY-MM-DD[T ]HH:MM:SS [tz]
    int y, mo, d, h = 0, mi = 0, se = 0;
    std::string str(s);
    char sep = 0;
    int consumed = 0;
    if (std::sscanf(str.c_str(), "%d-%d-%d%c%d:%d:%d%n", &y, &mo, &d, &sep, &h, &mi, &se, &consumed) >= 3 &&
        consumed > 0) {
        int64_t days = DaysFromCivil(y, mo, d);
        int64_t local = days * 86400 + h * 3600 + mi * 60 + se;
        if (!parse_tz(std::string_view(str).substr(static_cast<size_t>(consumed)))) return false;
        t = local - static_cast<int64_t>(tz) * 60;
        return true;
    }
    // RFC 2822: "Thu, 07 Apr 2005 22:13:13 +0200"
    char wd[8] = {0}, mon[8] = {0};
    char tzs[16] = {0};
    if (std::sscanf(str.c_str(), "%3s, %d %3s %d %d:%d:%d %15s", wd, &d, mon, &y, &h, &mi, &se, tzs) == 8) {
        int m = 0;
        for (int i = 0; i < 12; i++)
            if (strcmp(kMonths[i], mon) == 0) m = i + 1;
        if (!m) return false;
        if (!parse_tz(tzs)) return false;
        t = DaysFromCivil(y, m, d) * 86400 + h * 3600 + mi * 60 + se - static_cast<int64_t>(tz) * 60;
        return true;
    }
    return false;
}

}  // namespace mep::git
