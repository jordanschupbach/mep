#include "platform_compat.h"

#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mep::compat {

namespace {

// Splits a PATH-shaped variable on the platform's own separator. Empty
// entries are dropped rather than treated as "the current directory",
// which is the POSIX reading of them but a well-known way to pick up the
// wrong binary.
std::vector<std::string> SplitSearchPath(const char *value, char separator) {
    std::vector<std::string> out;
    if (value == nullptr) return out;
    const std::string text = value;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t sep = text.find(separator, start);
        const size_t end = sep == std::string::npos ? text.size() : sep;
        if (end > start) out.push_back(text.substr(start, end - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return out;
}

}  // namespace

#if defined(_WIN32)

bool ProgramOnPath(const char *program) {
    if (program == nullptr || *program == '\0') return false;

    // PATHEXT is the list of extensions the shell will append when
    // resolving a bare command name; ".COM;.EXE;.BAT;.CMD" is the
    // long-standing default if it is somehow unset. The empty string is
    // tried first so a name given with its extension already attached
    // ("ffmpeg.exe") still resolves.
    std::vector<std::string> extensions{""};
    const char *pathext = std::getenv("PATHEXT");
    for (const std::string &ext : SplitSearchPath(pathext != nullptr ? pathext : ".COM;.EXE;.BAT;.CMD", ';')) {
        extensions.push_back(ext);
    }

    for (const std::string &dir : SplitSearchPath(std::getenv("PATH"), ';')) {
        for (const std::string &ext : extensions) {
            const std::string candidate = dir + "\\" + program + ext;
            // Mode 0 is "does it exist", the only question worth asking:
            // Windows has no execute permission bit, so a file found under
            // a PATHEXT extension is by definition runnable.
            if (::_access(candidate.c_str(), 0) == 0) return true;
        }
    }
    return false;
}

bool IsExecutableFile(const std::string &path) { return !path.empty() && ::_access(path.c_str(), 0) == 0; }

#else

bool ProgramOnPath(const char *program) {
    if (program == nullptr || *program == '\0') return false;
    for (const std::string &dir : SplitSearchPath(std::getenv("PATH"), ':')) {
        if (::access((dir + "/" + program).c_str(), X_OK) == 0) return true;
    }
    return false;
}

bool IsExecutableFile(const std::string &path) { return !path.empty() && ::access(path.c_str(), X_OK) == 0; }

#endif

}  // namespace mep::compat
