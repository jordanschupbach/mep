#ifndef MEP_PERSIST_H
#define MEP_PERSIST_H

// Small persistence helpers (NVIM_PARITY_PLAN.md Part I Phase 2): a
// per-user data directory convention (mirrors Neovim's `stdpath('data')`)
// plus JSON read/write for the small state files several later phases
// need (project list, activity-bar todos, flashcards SM-2 state, ...).
// Native-only -- the wasm/browser build has no real filesystem to persist
// to, matching how file I/O elsewhere in this codebase is already gated.

#include <cmath>
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
/** @brief Returns (creating if needed) mep's per-user data directory.
 *  @return the resolved data directory path, or an empty string if no home directory can be determined, or on the wasm build. */
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
/** @brief Returns (creating if needed) the per-user directory holding one Unix-domain-socket file per running native mep instance.
 *  @return the resolved agent-sockets directory path, or an empty string if MepDataDir() is empty. */
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
/** @brief Reads and parses a JSON file.
 *  @param path filesystem path of the JSON file to read.
 *  @param out receives the parsed value on success; left untouched on failure.
 *  @return true if the file existed and parsed successfully, false otherwise (treated by callers as "no persisted state yet"). */
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
/** @brief Writes `value` as JSON to `path`.
 *  @param path filesystem path to write the JSON document to (truncated if it already exists).
 *  @param value the JSON value to serialize and write.
 *  @return true on success, false if the file could not be opened for writing. */
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

// --- Window state ----------------------------------------------------------

// The application window as persisted between runs (the file below): its
// size, whether it was maximized, and the interface zoom (the global font
// size everything is measured in -- Ctrl+Shift+=/- in a running mep).
//
// `width`/`height` are deliberately the last size the window had while *not*
// maximized: a maximized window's size is the screen's, and reopening a plain
// window that big -- or un-maximizing to it -- is not the size the user
// actually chose. So the pair means "the size to open at, and to fall back to
// when the window is un-maximized", which is what a window manager itself
// calls the restore geometry.
//
// One struct and one file for all three because they share a lifetime and a
// scope: they are the window's, not any project's, and mep has to settle all
// of them before it knows which project the window will end up showing.
struct WindowState {
    int width = 0;
    int height = 0;
    bool maximized = false;
    // 0 means "nothing saved, use the build's default" -- the caller's own
    // default font size, not a zoom level this file can name.
    float font_size = 0.0f;
};

// Sizes outside this range are not a window the user picked: a window
// manager reports 0x0 for a window it has not mapped yet, and anything past
// the upper bound is a corrupt or hand-edited file rather than a display.
// Restoring either would open mep at a size it could not be resized out of.
constexpr int kMinWindowDimension = 240;
constexpr int kMaxWindowDimension = 32000;

/** @brief Whether a width/height pair is a plausible window size to save or restore.
 *  @param width candidate width in pixels.
 *  @param height candidate height in pixels.
 *  @return true when both fall within [kMinWindowDimension, kMaxWindowDimension]. */
inline bool ValidWindowSize(int width, int height) {
    return width >= kMinWindowDimension && width <= kMaxWindowDimension && height >= kMinWindowDimension &&
           height <= kMaxWindowDimension;
}

// A deliberately wide sanity range rather than a second copy of the real zoom
// limits: main.cpp owns those (kMinFontSize/kMaxFontSize) and ApplyFontSize
// clamps every size to them anyway, including one restored from this file.
// What this rejects is a value that isn't a font size at all -- absent, zero,
// negative, or a NaN/inf that would survive the clamp and poison every
// measurement made from it.
constexpr float kMaxPersistedFontSize = 1000.0f;

/** @brief Whether a persisted zoom value is a usable font size at all (the caller still clamps it to its own limits).
 *  @param font_size candidate size in points.
 *  @return true when finite and within (0, kMaxPersistedFontSize]. */
inline bool ValidPersistedFontSize(float font_size) {
    return std::isfinite(font_size) && font_size > 0.0f && font_size <= kMaxPersistedFontSize;
}

// `<data_dir>/window.json`. One file for the whole application, not one per
// project: the window is the application's, and mep has no way to know which
// project a window will end up showing before it has to open that window.
/** @brief Returns the path of the persisted window-geometry file within `data_dir`.
 *  @param data_dir mep's per-user data directory (see MepDataDir()).
 *  @return `<data_dir>/window.json`. */
inline std::string WindowStatePath(const std::string &data_dir) { return data_dir + "/window.json"; }

/** @brief Reads the persisted window state.
 *  @param data_dir mep's per-user data directory; an empty string reads nothing.
 *  @param out receives the state on success; untouched otherwise. `font_size` comes back 0 when the file holds no usable zoom, leaving the caller's own default in place.
 *  @return true only when the file existed, parsed, and held a plausible size (ValidWindowSize) -- callers treat false as "nothing saved, use the defaults". */
inline bool ReadWindowState(const std::string &data_dir, WindowState *out) {
    if (data_dir.empty() || out == nullptr) return false;
    Json doc;
    if (!ReadJsonFile(WindowStatePath(data_dir), &doc) || !doc.is_object()) return false;
    const int width = doc.get("width").as_int(0);
    const int height = doc.get("height").as_int(0);
    if (!ValidWindowSize(width, height)) return false;
    out->width = width;
    out->height = height;
    out->maximized = doc.get("maximized").as_bool(false);
    // The zoom is reported separately from the size rather than failing the
    // whole read with it: a file written by a build that didn't save zoom yet
    // still has a window size worth honouring.
    const float font_size = static_cast<float>(doc.get("font_size").as_double(0.0));
    out->font_size = ValidPersistedFontSize(font_size) ? font_size : 0.0f;
    return true;
}

/** @brief Writes the window state for the next run.
 *  @param data_dir mep's per-user data directory; an empty string writes nothing.
 *  @param state the state to persist; an implausible size is not written rather than being written and ignored on the way back in, and an implausible zoom is omitted rather than blocking the size.
 *  @return true when the file was written. */
inline bool WriteWindowState(const std::string &data_dir, const WindowState &state) {
    if (data_dir.empty() || !ValidWindowSize(state.width, state.height)) return false;
    Json doc = Json::Object();
    doc["width"] = state.width;
    doc["height"] = state.height;
    doc["maximized"] = state.maximized;
    if (ValidPersistedFontSize(state.font_size)) doc["font_size"] = static_cast<double>(state.font_size);
    return WriteJsonFile(WindowStatePath(data_dir), doc);
}

#endif  // MEP_PERSIST_H
