#ifndef MEP_PERSIST_H
#define MEP_PERSIST_H

// Small persistence helpers (NVIM_PARITY_PLAN.md Part I Phase 2): a
// per-user data directory convention (mirrors Neovim's `stdpath('data')`)
// plus JSON read/write for the small state files several later phases
// need (project list, activity-bar todos, flashcards SM-2 state, ...).
// Native-only -- the wasm/browser build has no real filesystem to persist
// to, matching how file I/O elsewhere in this codebase is already gated.

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "json.h"

#if !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#endif

// Returns (creating if needed) mep's per-user data directory:
// `$XDG_DATA_HOME/mep` if set, else `$HOME/.local/share/mep` on Linux,
// `$HOME/Library/Application Support/mep` on macOS. Empty string if no
// home directory can be determined, or on the wasm build.
inline std::string MepDataDir() {
#if defined(__EMSCRIPTEN__)
    return "";
#else
    const char *xdg = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    std::string base;
    if (xdg && *xdg) {
        base = std::string(xdg) + "/mep";
    } else if (home && *home) {
#if defined(__APPLE__)
        base = std::string(home) + "/Library/Application Support/mep";
#else
        base = std::string(home) + "/.local/share/mep";
#endif
    } else {
        return "";
    }
    // mkdir -p, one path component at a time -- no dependency on a
    // recursive-mkdir library function being available everywhere.
    std::string partial;
    size_t start = base[0] == '/' ? 1 : 0;
    partial = base[0] == '/' ? "/" : "";
    size_t pos = start;
    while (pos <= base.size()) {
        size_t slash = base.find('/', pos);
        if (slash == std::string::npos) slash = base.size();
        partial += base.substr(pos, slash - pos);
        if (!partial.empty()) mkdir(partial.c_str(), 0755);
        partial += "/";
        pos = slash + 1;
    }
    return base;
#endif
}

// Per-user directory holding one Unix-domain-socket file per running
// native mep instance (see agent_rpc.h) -- a sibling of MepDataDir()
// itself rather than a whole separate directory-resolution convention.
// Empty string (nothing created) if MepDataDir() itself is empty.
inline std::string MepAgentSocketDir() {
#if defined(__EMSCRIPTEN__)
    return "";
#else
    std::string base = MepDataDir();
    if (base.empty()) return "";
    std::string dir = base + "/agent-sockets";
    mkdir(dir.c_str(), 0700);
    return dir;
#endif
}

// Reads and parses a JSON file. Returns false (out untouched) if the file
// doesn't exist or doesn't parse -- callers should treat that as "no
// persisted state yet", not an error.
inline bool ReadJsonFile(const std::string &path, Json *out) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)out;
    return false;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return Json::Parse(ss.str(), out);
#endif
}

// Writes `value` as JSON to `path`. Returns false on failure to open the
// file for writing.
inline bool WriteJsonFile(const std::string &path, const Json &value) {
#if defined(__EMSCRIPTEN__)
    (void)path;
    (void)value;
    return false;
#else
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << value.dump();
    return true;
#endif
}

#endif  // MEP_PERSIST_H
