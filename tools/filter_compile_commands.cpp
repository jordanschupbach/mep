// Filter compile_commands.json down to mep's own top-level src/*.{c,cpp}
// entries, dropping every vendored/fetched translation unit (third_party/,
// build/*/_deps/, generated test/example scaffolding). Cppcheck's own -i
// path-ignore flag is silently a no-op in --project mode (verified against
// cppcheck 2.18.3 -- it still analyzes ignored paths), so this exists as the
// actual filtering mechanism `just lint-cppcheck` feeds cppcheck instead.
//
// Built as the `mep-filter-compile-commands` host tool; replaces
// scripts/filter_compile_commands.py. The output is serialized the way
// Python's json.dump() wrote it (", "/": " separators, non-ASCII escaped).
//
// Usage: mep-filter-compile-commands <compile_commands.json> <output.json>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/json.h"
#include "py_text.h"

#ifndef MEP_SOURCE_DIR
#define MEP_SOURCE_DIR ""
#endif

namespace {

void DumpString(const std::string &s, std::string &out) {
    out += '"';
    for (char32_t c : pytext::Decode(s)) {
        char buf[16];
        switch (c) {
            case U'"': out += "\\\""; break;
            case U'\\': out += "\\\\"; break;
            case U'\n': out += "\\n"; break;
            case U'\r': out += "\\r"; break;
            case U'\t': out += "\\t"; break;
            case U'\b': out += "\\b"; break;
            case U'\f': out += "\\f"; break;
            default:
                if (c >= 0x20 && c < 0x7F) {
                    out += static_cast<char>(c);
                } else if (c < 0x10000) {
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    const char32_t v = c - 0x10000;
                    std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", static_cast<unsigned>(0xD800 + (v >> 10)),
                                  static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
                    out += buf;
                }
        }
    }
    out += '"';
}

// json.dumps() with its default separators.
void Dump(const Json &v, std::string &out) {
    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
    } else if (v.is_number()) {
        const double d = v.as_double();
        char buf[32];
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", d);
        }
        out += buf;
    } else if (v.is_string()) {
        DumpString(v.as_string(), out);
    } else if (v.is_array()) {
        out += '[';
        bool first = true;
        for (const Json &item : v.items()) {
            if (!first) out += ", ";
            first = false;
            Dump(item, out);
        }
        out += ']';
    } else {
        out += '{';
        bool first = true;
        for (const auto &[key, value] : v.fields()) {
            if (!first) out += ", ";
            first = false;
            DumpString(key, out);
            out += ": ";
            Dump(value, out);
        }
        out += '}';
    }
}

bool EndsWithSource(const std::string &file, size_t name_start) {
    // `[^/]+\.(c|cpp)$` from name_start.
    const std::string name = file.substr(name_start);
    if (name.find('/') != std::string::npos) return false;
    for (const char *ext : {".c", ".cpp"}) {
        const std::string e = ext;
        if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

// The script's `/mep/src/[^/]+\.(c|cpp)$`, which assumed the checkout is
// a directory named `mep`; also accepted: this source tree's own src/, so
// the filter works from a checkout (or git worktree) of any name.
bool IsOwnSource(const std::string &file) {
    for (size_t pos = file.find("/mep/src/"); pos != std::string::npos; pos = file.find("/mep/src/", pos + 1)) {
        if (EndsWithSource(file, pos + 9)) return true;
    }
    const std::string root = std::string(MEP_SOURCE_DIR) + "/src/";
    return root.size() > 5 && file.rfind(root, 0) == 0 && EndsWithSource(file, root.size());
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: mep-filter-compile-commands <compile_commands.json> <output.json>\n");
        return 2;
    }
    std::string text;
    if (!pytext::ReadBytes(argv[1], &text)) {
        std::fprintf(stderr, "filter_compile_commands: cannot read %s\n", argv[1]);
        return 1;
    }
    Json entries;
    if (!Json::Parse(text, &entries) || !entries.is_array()) {
        std::fprintf(stderr, "filter_compile_commands: %s is not a JSON array\n", argv[1]);
        return 1;
    }
    Json own = Json::Array();
    for (const Json &e : entries.items()) {
        if (IsOwnSource(e.get("file").as_string(""))) own.push_back(e);
    }
    std::string out;
    Dump(own, out);
    if (!pytext::WriteBytes(argv[2], out)) {
        std::fprintf(stderr, "filter_compile_commands: cannot write %s\n", argv[2]);
        return 1;
    }
    std::fprintf(stderr, "%zu of %zu compile-db entries are mep's own src/\n", own.items().size(),
                 entries.items().size());
    return 0;
}
