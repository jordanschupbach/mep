#pragma once

// Small filesystem/string helpers shared by the mep::git sources.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mep::git {

bool ReadFile(const std::string &path, std::string &out);
bool WriteFile(const std::string &path, std::string_view data, bool exec = false);
// Writes `<path>.lock` then renames it over `path` (git's lockfile dance).
// Fails if the lock already exists (another git process holds it).
bool WriteFileAtomic(const std::string &path, std::string_view data, std::string *err);
bool FileExists(const std::string &path);
bool IsDirectory(const std::string &path);
bool MakeDirs(const std::string &path);
bool RemoveFile(const std::string &path);
// Removes `path` (file or directory tree). Never follows symlinks.
bool RemoveAll(const std::string &path);
// Removes now-empty parent directories of `path` up to (not including) `stop`.
void RemoveEmptyParents(const std::string &path, const std::string &stop);
std::string DirName(const std::string &path);
std::string BaseName(const std::string &path);
std::string JoinPath(const std::string &a, const std::string &b);
std::string AbsolutePath(const std::string &path, const std::string &base);
std::string RealPath(const std::string &path);
std::string NormalizePath(const std::string &path);  // collapses ., .., //
std::string Trim(std::string_view s);
bool StartsWith(std::string_view s, std::string_view p);
bool EndsWith(std::string_view s, std::string_view p);

struct DirEntry {
    std::string name;
    enum Kind { Unknown, File, Dir, Link, Other } kind = Unknown;
};
bool ListDir(const std::string &path, std::vector<DirEntry> &out);

// git's wildmatch(): `*`/`?` stop at '/' when `pathname`, `**` crosses
// directories, [...] classes (incl. [:alpha:]), backslash escapes.
bool WildMatch(std::string_view pattern, std::string_view text, bool pathname = true, bool icase = false);

// git's quote_c_style (core.quotePath on): "..." with C escapes and octal
// for control/non-ASCII bytes, or the path unchanged when nothing needs it.
std::string QuotePath(std::string_view path, bool quote_non_ascii = true, bool quote_space = false);

int64_t NowSeconds();
int LocalTzMinutes(int64_t t);

}  // namespace mep::git
