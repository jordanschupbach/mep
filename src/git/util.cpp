#include "git/util.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mep::git {

bool ReadFile(const std::string &path, std::string &out) {
#if !defined(_WIN32)
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        ::close(fd);
        return false;
    }
    out.clear();
    out.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = ::read(fd, out.data() + got, out.size() - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    out.resize(got);
    // A file that grew while we read it: take the rest too.
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
#endif
}

bool WriteFile(const std::string &path, std::string_view data, bool exec) {
#if !defined(_WIN32)
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, exec ? 0777 : 0666);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return ::close(fd) == 0;
#else
    (void)exec;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
#endif
}

bool WriteFileAtomic(const std::string &path, std::string_view data, std::string *err) {
    std::string lock = path + ".lock";
#if !defined(_WIN32)
    int fd = ::open(lock.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        if (err) {
            if (errno == EEXIST)
                *err = "Unable to create '" + lock +
                       "': File exists.\n\nAnother git process seems to be running in this repository.";
            else
                *err = "Unable to create '" + lock + "': " + std::strerror(errno);
        }
        return false;
    }
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(lock.c_str());
            if (err) *err = "write error on " + lock;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    if (::close(fd) != 0 || ::rename(lock.c_str(), path.c_str()) != 0) {
        ::unlink(lock.c_str());
        if (err) *err = "unable to write " + path;
        return false;
    }
    return true;
#else
    if (FileExists(lock)) {
        if (err) *err = "Unable to create '" + lock + "': File exists.";
        return false;
    }
    if (!WriteFile(lock, data)) {
        if (err) *err = "unable to write " + lock;
        return false;
    }
    std::error_code ec;
    fs::rename(lock, path, ec);
    if (ec) {
        fs::remove(lock, ec);
        if (err) *err = "unable to write " + path;
        return false;
    }
    return true;
#endif
}

bool FileExists(const std::string &path) {
#if !defined(_WIN32)
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
#else
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
#endif
}

bool IsDirectory(const std::string &path) {
#if !defined(_WIN32)
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#else
    std::error_code ec;
    return fs::is_directory(path, ec);
#endif
}

bool MakeDirs(const std::string &path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return IsDirectory(path);
}

bool RemoveFile(const std::string &path) {
    std::error_code ec;
    return fs::remove(path, ec);
}

bool RemoveAll(const std::string &path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

void RemoveEmptyParents(const std::string &path, const std::string &stop) {
    std::string dir = DirName(path);
    while (dir.size() > stop.size() && StartsWith(dir, stop)) {
#if !defined(_WIN32)
        if (::rmdir(dir.c_str()) != 0) break;
#else
        std::error_code ec;
        if (!fs::is_empty(dir, ec) || !fs::remove(dir, ec)) break;
#endif
        dir = DirName(dir);
    }
}

std::string DirName(const std::string &path) {
    size_t s = path.find_last_of('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return path.substr(0, s);
}

std::string BaseName(const std::string &path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string JoinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string NormalizePath(const std::string &path) {
    bool abs = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t s = path.find('/', pos);
        if (s == std::string::npos) s = path.size();
        std::string part = path.substr(pos, s - pos);
        pos = s + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }
    std::string out = abs ? "/" : "";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    if (out.empty()) out = abs ? "/" : ".";
    return out;
}

std::string AbsolutePath(const std::string &path, const std::string &base) {
    if (!path.empty() && path[0] == '/') return NormalizePath(path);
    return NormalizePath(JoinPath(base, path));
}

std::string RealPath(const std::string &path) {
#if !defined(_WIN32)
    char *r = ::realpath(path.c_str(), nullptr);
    if (!r) return NormalizePath(path);
    std::string out = r;
    std::free(r);
    return out;
#else
    std::error_code ec;
    fs::path p = fs::canonical(path, ec);
    return ec ? NormalizePath(path) : p.generic_string();
#endif
}

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool ListDir(const std::string &path, std::vector<DirEntry> &out) {
    out.clear();
#if !defined(_WIN32)
    DIR *d = ::opendir(path.c_str());
    if (!d) return false;
    while (struct dirent *e = ::readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
        DirEntry de;
        de.name = n;
#if defined(DT_DIR)
        switch (e->d_type) {
            case DT_REG: de.kind = DirEntry::File; break;
            case DT_DIR: de.kind = DirEntry::Dir; break;
            case DT_LNK: de.kind = DirEntry::Link; break;
            case DT_UNKNOWN: de.kind = DirEntry::Unknown; break;
            default: de.kind = DirEntry::Other; break;
        }
#endif
        if (de.kind == DirEntry::Unknown) {
            struct stat st;
            std::string full = path + "/" + de.name;
            if (::lstat(full.c_str(), &st) == 0) {
                if (S_ISREG(st.st_mode)) de.kind = DirEntry::File;
                else if (S_ISDIR(st.st_mode)) de.kind = DirEntry::Dir;
                else if (S_ISLNK(st.st_mode)) de.kind = DirEntry::Link;
                else de.kind = DirEntry::Other;
            }
        }
        out.push_back(std::move(de));
    }
    ::closedir(d);
    return true;
#else
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(path, ec)) {
        DirEntry de;
        de.name = e.path().filename().generic_string();
        if (e.is_symlink(ec)) de.kind = DirEntry::Link;
        else if (e.is_directory(ec)) de.kind = DirEntry::Dir;
        else if (e.is_regular_file(ec)) de.kind = DirEntry::File;
        else de.kind = DirEntry::Other;
        out.push_back(std::move(de));
    }
    return !ec;
#endif
}

// ------------------------------------------------------------ wildmatch

namespace {
constexpr int kMatch = 0, kNoMatch = 1, kAbortAll = -1, kAbortToStarStar = -2;

bool IsGlobSpecial(unsigned char c) { return c == '*' || c == '?' || c == '[' || c == '\\'; }

bool ClassMatch(const unsigned char *s, long len, unsigned char t, bool &known) {
    auto eq = [&](const char *name) { return static_cast<long>(std::strlen(name)) == len && std::memcmp(s, name, static_cast<size_t>(len)) == 0; };
    known = true;
    if (eq("alnum")) return std::isalnum(t);
    if (eq("alpha")) return std::isalpha(t);
    if (eq("blank")) return t == ' ' || t == '\t';
    if (eq("cntrl")) return std::iscntrl(t);
    if (eq("digit")) return std::isdigit(t);
    if (eq("graph")) return std::isgraph(t);
    if (eq("lower")) return std::islower(t);
    if (eq("print")) return std::isprint(t);
    if (eq("punct")) return std::ispunct(t);
    if (eq("space")) return std::isspace(t);
    if (eq("upper")) return std::isupper(t);
    if (eq("xdigit")) return std::isxdigit(t);
    known = false;
    return false;
}

int DoWild(const unsigned char *p, const unsigned char *text, bool pathname, bool icase) {
    const unsigned char *pattern = p;
    unsigned char p_ch;
    for (; (p_ch = *p) != '\0'; text++, p++) {
        int matched;
        bool match_slash;
        unsigned char t_ch, prev_ch;
        if ((t_ch = *text) == '\0' && p_ch != '*') return kAbortAll;
        if (icase && std::isupper(t_ch)) t_ch = static_cast<unsigned char>(std::tolower(t_ch));
        if (icase && std::isupper(p_ch)) p_ch = static_cast<unsigned char>(std::tolower(p_ch));
        switch (p_ch) {
            case '\\':
                p_ch = *++p;
                if (icase && std::isupper(p_ch)) p_ch = static_cast<unsigned char>(std::tolower(p_ch));
                if (t_ch != p_ch) return kNoMatch;
                continue;
            default:
                if (t_ch != p_ch) return kNoMatch;
                continue;
            case '?':
                if (pathname && t_ch == '/') return kNoMatch;
                continue;
            case '*': {
                if (*++p == '*') {
                    const unsigned char *prev_p = p - 2;
                    while (*++p == '*') {
                    }
                    if ((prev_p < pattern || *prev_p == '/') &&
                        (*p == '\0' || *p == '/' || (p[0] == '\\' && p[1] == '/'))) {
                        if (p[0] == '/' && DoWild(p + 1, text, pathname, icase) == kMatch) return kMatch;
                        match_slash = true;
                    } else {
                        match_slash = false;
                    }
                } else {
                    match_slash = !pathname;
                }
                if (*p == '\0') {
                    if (!match_slash && std::strchr(reinterpret_cast<const char *>(text), '/')) return kNoMatch;
                    return kMatch;
                } else if (!match_slash && *p == '/') {
                    const char *slash = std::strchr(reinterpret_cast<const char *>(text), '/');
                    if (!slash) return kNoMatch;
                    text = reinterpret_cast<const unsigned char *>(slash);
                    break;
                }
                for (;;) {
                    if (t_ch == '\0') break;
                    if (!IsGlobSpecial(*p)) {
                        p_ch = *p;
                        if (icase && std::isupper(p_ch)) p_ch = static_cast<unsigned char>(std::tolower(p_ch));
                        while ((t_ch = *text) != '\0' && (match_slash || t_ch != '/')) {
                            if (icase && std::isupper(t_ch)) t_ch = static_cast<unsigned char>(std::tolower(t_ch));
                            if (t_ch == p_ch) break;
                            text++;
                        }
                        if (t_ch != p_ch) return kNoMatch;
                    }
                    if ((matched = DoWild(p, text, pathname, icase)) != kNoMatch) {
                        if (!match_slash || matched != kAbortToStarStar) return matched;
                    } else if (!match_slash && t_ch == '/') {
                        return kAbortToStarStar;
                    }
                    t_ch = *++text;
                }
                return kAbortAll;
            }
            case '[': {
                p_ch = *++p;
                if (p_ch == '^') p_ch = '!';
                bool negated = p_ch == '!';
                if (negated) p_ch = *++p;
                prev_ch = 0;
                matched = 0;
                do {
                    if (!p_ch) return kAbortAll;
                    if (p_ch == '\\') {
                        p_ch = *++p;
                        if (!p_ch) return kAbortAll;
                        if (t_ch == p_ch) matched = 1;
                    } else if (p_ch == '-' && prev_ch && p[1] && p[1] != ']') {
                        p_ch = *++p;
                        if (p_ch == '\\') {
                            p_ch = *++p;
                            if (!p_ch) return kAbortAll;
                        }
                        if (t_ch <= p_ch && t_ch >= prev_ch) matched = 1;
                        else if (icase && std::islower(t_ch)) {
                            unsigned char up = static_cast<unsigned char>(std::toupper(t_ch));
                            if (up <= p_ch && up >= prev_ch) matched = 1;
                        }
                        p_ch = 0;
                    } else if (p_ch == '[' && p[1] == ':') {
                        const unsigned char *s = p += 2;
                        for (; (p_ch = *p) && p_ch != ']'; p++) {
                        }
                        if (!p_ch) return kAbortAll;
                        long i = static_cast<long>(p - s) - 1;
                        if (i < 0 || p[-1] != ':') {
                            p = s - 2;
                            p_ch = '[';
                            if (t_ch == p_ch) matched = 1;
                            continue;
                        }
                        bool known = false;
                        if (ClassMatch(s, i, t_ch, known)) matched = 1;
                        else if (known && icase && std::string_view(reinterpret_cast<const char *>(s), static_cast<size_t>(i)) == "upper" && std::islower(t_ch)) matched = 1;
                        if (!known) return kAbortAll;
                        p_ch = 0;
                    } else if (t_ch == p_ch) {
                        matched = 1;
                    }
                } while (prev_ch = p_ch, (p_ch = *++p) != ']');
                if (matched == (negated ? 1 : 0) || (pathname && t_ch == '/')) return kNoMatch;
                continue;
            }
        }
    }
    return *text ? kNoMatch : kMatch;
}
}  // namespace

bool WildMatch(std::string_view pattern, std::string_view text, bool pathname, bool icase) {
    std::string p(pattern), t(text);
    return DoWild(reinterpret_cast<const unsigned char *>(p.c_str()), reinterpret_cast<const unsigned char *>(t.c_str()),
                  pathname, icase) == kMatch;
}

std::string QuotePath(std::string_view path, bool quote_non_ascii, bool quote_space) {
    bool need = false;
    for (char ch0 : path) {
        unsigned char c = static_cast<unsigned char>(ch0);
        if (c < 0x20 || c == '"' || c == '\\' || c == 0x7f || (quote_non_ascii && c >= 0x80) || (quote_space && c == ' ')) need = true;
    }
    if (!need) return std::string(path);
    std::string out = "\"";
    for (char ch1 : path) {
        unsigned char c = static_cast<unsigned char>(ch1);
        switch (c) {
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\v': out += "\\v"; break;
            case '\f': out += "\\f"; break;
            case '\r': out += "\\r"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c < 0x20 || c == 0x7f || (quote_non_ascii && c >= 0x80)) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\%03o", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int LocalTzMinutes(int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &tt);
    std::tm utc{};
    gmtime_s(&utc, &tt);
    utc.tm_isdst = local.tm_isdst;
    return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)) / 60);
#else
    localtime_r(&tt, &local);
    return static_cast<int>(local.tm_gmtoff / 60);
#endif
}

}  // namespace mep::git
