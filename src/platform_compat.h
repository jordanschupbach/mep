#pragma once

// Small POSIX-shaped shims for the Windows build, plus the handful of
// genuinely cross-platform helpers that exist because no single spelling
// works everywhere.
//
// Scope, deliberately narrow: this header covers C-library calls that
// Windows provides under a different name or a different signature --
// the mechanical cases, where a one-line adapter is the whole story and
// behaviour is identical on both sides. It is not a portability layer.
// Anything where the platforms genuinely differ in behaviour (how PATH is
// spelled and what counts as executable on it, process creation,
// pseudo-terminals, sockets) is handled at the call site, where the
// difference is visible to whoever reads it, rather than hidden behind a
// POSIX name that would quietly mean something else here.
//
// Include it from any translation unit that needs one of these. On every
// non-Windows platform the whole file is nearly empty, so an unconditional
// include costs nothing.

#include <ctime>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <direct.h>
#include <stdlib.h>
#include <windows.h>

// <windows.h> defines a family of function-like macros that rename an
// unsuffixed API to its A/W variant -- and several of those names are
// also member functions of mep's own gfx:: backend interfaces
// (gfx/backend.h). Any translation unit that sees both then has its
// `gfx::DrawTextEx(...)` call rewritten to `gfx::DrawTextExA(...)`, which
// fails to compile in a way that points at gfx rather than at Windows.
//
// Undefined here so that including this header is always safe, whatever
// else the file does. Nothing in mep calls the Win32 originals, so there
// is nothing to lose by removing the aliases; the suffixed names
// (DrawTextExW and friends) remain available to anyone who wants them.
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef GetObject
#undef PlaySound
#undef GetCurrentTime
#undef near
#undef far

// --- Reentrant time conversion ------------------------------------------
//
// MSVC has the same two functions under the _s names, with the arguments
// the other way round and an errno_t result. POSIX returns the tm pointer
// (null on failure); these preserve that, so a call site reads the same on
// both platforms.

inline std::tm *localtime_r(const std::time_t *time, std::tm *result) {
    return ::localtime_s(result, time) == 0 ? result : nullptr;
}

inline std::tm *gmtime_r(const std::time_t *time, std::tm *result) {
    return ::gmtime_s(result, time) == 0 ? result : nullptr;
}

// mktime's UTC counterpart. _mkgmtime is exactly that, under Microsoft's
// own name for it.
inline std::time_t timegm(std::tm *tm) { return ::_mkgmtime(tm); }

// --- Environment --------------------------------------------------------
//
// _putenv_s covers both: assigning an empty value is how Windows removes a
// variable. The `overwrite` parameter is accepted so the signature matches
// POSIX's, and honoured rather than ignored.

inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && std::getenv(name) != nullptr) return 0;
    return ::_putenv_s(name, value) == 0 ? 0 : -1;
}

inline int unsetenv(const char *name) { return ::_putenv_s(name, "") == 0 ? 0 : -1; }

// --- Processes and directories ------------------------------------------
//
// popen/pclose exist under the underscore names with identical semantics.

#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif

// Windows has no POSIX mode bits, so the mode argument is accepted and
// dropped: permissions on a created directory come from the parent's ACL.
// Every caller in this tree passes 0700 purely to keep a private directory
// private, which on Windows a per-user directory already is.
inline int mkdir(const char *path, int /*mode*/) { return ::_mkdir(path); }

#endif  // _WIN32

namespace mep::compat {

// Seconds east of UTC for the local time `tm` represents -- POSIX's
// non-standard `tm_gmtoff` field, which MSVC's struct tm does not have.
//
// The Windows implementation reconstructs it rather than reading a global:
// converting the same broken-down time both ways and taking the difference
// gives the offset actually in force at that instant, which is what the
// caller wants and what a fixed _timezone would get wrong for any date on
// the other side of a daylight-saving boundary.
inline long GmtOffsetSeconds(const std::tm &tm) {
#if defined(_WIN32)
    std::tm local = tm;
    local.tm_isdst = -1;
    const std::time_t as_local = std::mktime(&local);
    if (as_local == static_cast<std::time_t>(-1)) return 0;
    std::tm utc = tm;
    utc.tm_isdst = 0;
    const std::time_t as_utc = ::_mkgmtime(&utc);
    if (as_utc == static_cast<std::time_t>(-1)) return 0;
    return static_cast<long>(as_utc - as_local);
#else
    return static_cast<long>(tm.tm_gmtoff);
#endif
}

// Whether `program` resolves to an executable on PATH.
//
// Not a shim over a single call: the two platforms disagree on every part
// of the question. PATH is separated by ':' on POSIX and ';' on Windows; a
// POSIX executable is one with the execute bit, while Windows has no such
// bit and instead treats a file as executable when its extension is listed
// in PATHEXT; and the name a caller asks for ("yt-dlp") is spelled
// "yt-dlp.exe" on disk here. _access() cannot even express the POSIX
// query -- its mode argument has no execute bit, and passing one is an
// invalid-parameter error rather than a false answer.
bool ProgramOnPath(const char *program);

// Whether `path` names a file that can be run, for a caller that already
// has a path and so has nothing to search for.
//
// Same platform split as ProgramOnPath: on POSIX this is the execute
// permission bit (access(X_OK)), which is the only thing that makes a
// file runnable there. Windows has no such bit -- what it has instead is
// the PATHEXT convention, which applies to resolving a bare name and not
// to a path the caller spelled out -- so existence is the whole question.
bool IsExecutableFile(const std::string &path);

}  // namespace mep::compat
