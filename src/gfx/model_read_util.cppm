module;

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

export module mep.gfx.model_read_util;

// Shared by the model loaders under gfx/backend_native_model_*.cpp, which
// each used to carry their own identical copy of these -- harmless until
// BUILD_PERFORMANCE_PLAN.md Round 2 Phase C's unity build merges several
// .cpp files into one translation unit, where duplicate same-named
// anonymous-namespace helpers collide (`-Werror=unused-function`/
// redefinition on whichever copy loses name lookup). Converted from a
// plain `inline` header into a real C++20 module in Phase D, alongside
// `mep.diff`/`mep.path_util`. Unlike those two, this module needs its own
// FILE_SET entry on *five* separate targets (mep_core, the three
// mep_add_gfx_native_smoke()-generated smoke tests, and
// mep-model3d-doc-test) rather than just one or two -- every one of them
// compiles the gfx model loaders directly instead of linking mep_core
// (the same "compiled into multiple targets" duplication ccache's own
// CMakeLists.txt comment already calls out), and unlike a plain header,
// a module's interface unit has to be listed on each target that
// compiles a consumer of it, not just included for free.
export namespace gfx {

/**
 * @brief Reads a little-endian uint32_t out of `buf` at `offset`.
 * @param buf The byte buffer to read from.
 * @param offset Byte offset to read at.
 * @return The value at `offset`.
 */
inline uint32_t ReadU32(const std::vector<char> &buf, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}

/**
 * @brief Reads a little-endian uint16_t out of `buf` at `offset`.
 * @param buf The byte buffer to read from.
 * @param offset Byte offset to read at.
 * @return The value at `offset`.
 */
inline uint16_t ReadU16(const std::vector<char> &buf, size_t offset) {
    uint16_t v = 0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}

/**
 * @brief Reads a little-endian float out of `buf` at `offset`.
 * @param buf The byte buffer to read from.
 * @param offset Byte offset to read at.
 * @return The value at `offset`.
 */
inline float ReadF32(const std::vector<char> &buf, size_t offset) {
    float v = 0.0f;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}

/**
 * @brief Returns the directory portion of `path` (up to and including the last slash).
 * @param path The path to split.
 * @return The directory portion, or an empty string if `path` has no slash.
 */
inline std::string DirOf(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

}  // namespace gfx
