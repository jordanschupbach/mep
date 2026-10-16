// Generates a single self-contained .cpp that combines every mep source
// file (all core modules, the collaboration subsystem, and the embedded
// font/query data headers -- with their upstream license text intact) into
// one translation unit, for the `mep-amalgam` build target (see
// CMakeLists.txt). This is the inverse of splitting the library apart: each
// module here is still one file on disk (src/*.cpp, src/*.h) and still
// builds independently as part of `mep_core` -- this tool never edits
// those files, it only concatenates them.
//
// Built as a host tool (the `mep-amalgamate` target) and run by the
// `mep-amalgam` target's custom command; replaces tools/amalgamate.py and
// produces byte-identical output to it.
//
// Usage: mep-amalgamate <src_dir> <output_file>

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "py_text.h"

namespace {

using pytext::U;

struct Module {
    std::string name;
    std::string header;
    std::vector<std::string> cpps;
    std::vector<std::pair<std::string, std::string>> renames;
};

// Every module that used to be (and, in the normal build, still is) its
// own translation unit, in dependency order: each module's declarations
// must be visible before any later module's declarations or bodies. The
// rename list disambiguates symbol names that are only unique because
// each module is normally its own translation unit (e.g. two modules
// each privately declaring a local class Parser/Lexer) -- harmless
// normally, but a hard duplicate-definition error once every module
// shares one translation unit.
const std::vector<Module> &Modules() {
    static const std::vector<Module> kModules = {
        {"vterm", "vterm.h", {"vterm.cpp"}, {}},
        {"regex", "regex.h", {"regex.cpp"}, {{"Parser", "Parser_regex"}}},
        {"treesitter", "treesitter.h", {"treesitter.cpp"}, {}},
        {"image_doc", "image_doc.h", {"image_doc.cpp"}, {}},
        {"pdf_doc", "pdf_doc.h", {"pdf_doc.cpp"}, {}},
        {"formula",
         "formula.h",
         {"formula.cpp"},
         {{"Lexer", "Lexer_formula"},
          {"Parser", "Parser_formula"},
          {"Token", "Token_formula"},
          {"NodePtr", "NodePtr_formula"}}},
        {"html_doc", "html_doc.h", {"html_doc.cpp"}, {}},
        {"job", "job.h", {"job.cpp"}, {}},
        {"project_search", "project_search.h", {"project_search.cpp"}, {}},
        {"org_doc", "org_doc.h", {"org_doc.cpp"}, {{"EqualsIgnoreCase", "EqualsIgnoreCase_org_doc"}}},
        {"agent_rpc", "agent_rpc.h", {"agent_rpc.cpp"}, {}},
        {"lua_env", "lua_env.h", {"lua_env.cpp"}, {{"ReplaceAllLiteral", "ReplaceAllLiteral_lua_env"}}},
        {"office_doc", "office_doc.h", {"office_doc.cpp", "office_odt.cpp"}, {{"LowerExt", "LowerExt_office_doc"}}},
        {"doc_export", "doc_export.h", {"doc_export.cpp"}, {{"LowerExt", "LowerExt_doc_export"}}},
        {"sheet_doc",
         "sheet_doc.h",
         {"sheet_doc.cpp", "sheet_xlsx.cpp", "sheet_ods.cpp"},
         {{"CallFunction", "CallFunction_sheet_doc"},
          {"ToNumber", "ToNumber_sheet_doc"},
          {"LowerExt", "LowerExt_sheet_doc"}}},
        {"js_engine",
         "js_engine.h",
         {"js_engine.cpp"},
         {{"CallFunction", "CallFunction_js_engine"},
          {"ToNumber", "ToNumber_js_engine"},
          {"Lexer", "Lexer_js_engine"},
          {"Parser", "Parser_js_engine"},
          {"Token", "Token_js_engine"},
          {"NodePtr", "NodePtr_js_engine"}}},
        {"workspace_git", "workspace_git.h", {"workspace_git.cpp"}, {}},
        {"editor",
         "editor.h",
         {"editor.cpp"},
         {{"EqualsIgnoreCase", "EqualsIgnoreCase_editor"}, {"ReplaceAllLiteral", "ReplaceAllLiteral_editor"}}},
    };
    return kModules;
}

// Small foundational headers with no module of their own (shared with
// mep-collabd / the collab test executables, which keep compiling them as
// separate translation units regardless of this amalgamation).
const std::vector<std::string> kPlainHeaders = {"json.h", "persist.h", "rpc_framing.h"};

// The collaboration subsystem: also its own translation units in the
// normal build, embedded here too so mep-amalgam is fully self-contained.
const std::vector<std::string> kCollabHeaders = {"collab_crdt.h", "collab_session.h", "collab_websocket.h"};
const std::vector<std::string> kCollabCpps = {"collab_crdt.cpp", "collab_session.cpp", "collab_websocket.cpp"};

// Pure data tables (fonts, Treesitter highlight queries) -- each carries
// its own upstream license text in the comments preceding the data, which
// is preserved verbatim below.
const std::vector<std::string> kDataHeaders = {
    "font_data.h",           "icon_font_data.h",       "symbol_font_data.h",    "office_font_data.h",
    "office_font_data_mono.h", "office_font_data_serif.h", "treesitter_queries.h",
};

std::set<std::u32string> AllLocalHeaders() {
    std::set<std::u32string> out;
    for (const Module &m : Modules()) out.insert(U(m.header));
    for (const auto *list : {&kPlainHeaders, &kCollabHeaders, &kDataHeaders}) {
        for (const std::string &h : *list) out.insert(U(h));
    }
    return out;
}

// `^#include\s+"([^"]+)"\s*$` against an already-stripped line.
bool LocalInclude(const std::u32string &line, const std::set<std::u32string> &headers) {
    const std::u32string kw = U"#include";
    if (!pytext::At(line, 0, kw)) return false;
    size_t i = kw.size();
    const size_t ws = i;
    while (i < line.size() && pytext::IsSpace(line[i])) ++i;
    if (i == ws || i >= line.size() || line[i] != U'"') return false;
    const size_t name_start = ++i;
    while (i < line.size() && line[i] != U'"') ++i;
    if (i >= line.size() || i == name_start) return false;
    const std::u32string name = line.substr(name_start, i - name_start);
    ++i;
    while (i < line.size() && pytext::IsSpace(line[i])) ++i;
    if (i != line.size()) return false;
    return headers.count(name) > 0;
}

std::u32string StripLocalIncludes(const std::u32string &text, const std::set<std::u32string> &headers) {
    std::u32string out;
    out.reserve(text.size());
    for (const std::u32string &line : pytext::SplitLinesKeepEnds(text)) {
        const std::u32string stripped = pytext::Strip(line);
        if (LocalInclude(stripped, headers)) continue;
        // Meaningless (and Wpragma-once-outside-header-noisy) once every
        // header shares one translation unit; the #ifndef/#define/#endif
        // guards the other headers use stay, doing no harm here.
        if (stripped == U"#pragma once") continue;
        out += line;
    }
    return out;
}

// re.sub(r"\b" + old + r"\b", new, text) for each rename in turn.
std::u32string ApplyRenames(std::u32string text, const std::vector<std::pair<std::string, std::string>> &renames) {
    for (const auto &[old_ascii, new_ascii] : renames) {
        const std::u32string old_name = U(old_ascii);
        const std::u32string new_name = U(new_ascii);
        std::u32string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            const size_t end = i + old_name.size();
            if (pytext::At(text, i, old_name) && (i == 0 || !pytext::IsWord(text[i - 1])) &&
                (end >= text.size() || !pytext::IsWord(text[end]))) {
                out += new_name;
                i = end;
            } else {
                out.push_back(text[i]);
                ++i;
            }
        }
        text = std::move(out);
    }
    return text;
}

bool Read(const std::string &src_dir, const std::string &name, std::u32string *out) {
    const std::string path = src_dir + "/" + name;
    std::string bytes;
    if (!pytext::ReadBytes(path, &bytes)) {
        std::fprintf(stderr, "amalgamate: cannot read %s\n", path.c_str());
        return false;
    }
    *out = pytext::UniversalNewlines(pytext::Decode(bytes));
    return true;
}

std::u32string Wrap(const std::string &name, const std::u32string &text) {
    return U("// {{ file: " + name + "\n") + pytext::RStrip(text) + U("\n// }} file: " + name + "\n\n");
}

const char kBanner[] =
    "// GENERATED FILE -- do not edit by hand.\n"
    "// Produced by tools/amalgamate.cpp from the separate module sources under\n"
    "// src/ for the `mep-amalgam` single-translation-unit build (see\n"
    "// CMakeLists.txt). The normal `mep` target builds those same files as a\n"
    "// static library (mep_core) instead; both build the same application.\n"
    "//\n"
    "// Every module's declarations are folded in dependency order first, then\n"
    "// every module's implementation, then main.cpp's own driver code, so a\n"
    "// class is always fully declared before any code that uses it -- the same\n"
    "// structure src/main.cpp itself used before the library split. A handful\n"
    "// of identifiers (Parser, Lexer, Token, NodePtr, ToNumber, LowerExt,\n"
    "// EqualsIgnoreCase, ReplaceAllLiteral, CallFunction) are given a\n"
    "// module-name suffix below where two modules each privately declare one\n"
    "// with the same name -- harmless while each is its own translation unit,\n"
    "// a duplicate-definition error once they share this one.\n"
    "//\n"
    "// Embedded data (fonts, Treesitter highlight queries) keeps its own\n"
    "// upstream license text -- an SPDX identifier + source URL, or a verbatim\n"
    "// license header for vendored query files -- exactly as it appears in the\n"
    "// original header, immediately above the data it covers.\n\n";

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: mep-amalgamate <src_dir> <output_file>\n");
        return 1;
    }
    const std::string src_dir = argv[1];
    const std::string out_path = argv[2];
    const std::set<std::u32string> headers = AllLocalHeaders();

    std::u32string out = U(kBanner);
    std::u32string text;

    out += U"// {{{ embedded headers and data\n\n";
    for (const auto *list : {&kPlainHeaders, &kCollabHeaders, &kDataHeaders}) {
        for (const std::string &name : *list) {
            if (!Read(src_dir, name, &text)) return 1;
            out += Wrap(name, StripLocalIncludes(text, headers));
        }
    }
    out += U"// }}} embedded headers and data\n\n";

    out += U"// {{{ collaboration subsystem\n\n";
    for (const std::string &name : kCollabCpps) {
        if (!Read(src_dir, name, &text)) return 1;
        out += Wrap(name, StripLocalIncludes(text, headers));
    }
    out += U"// }}} collaboration subsystem\n\n";

    out += U"// {{{ declarations\n\n";
    for (const Module &m : Modules()) {
        if (!Read(src_dir, m.header, &text)) return 1;
        text = ApplyRenames(StripLocalIncludes(text, headers), m.renames);
        out += U("// {{ module: " + m.name + " (declarations)\n");
        out += Wrap(m.header, text);
        out += U("// }} module: " + m.name + " (declarations)\n\n");
    }
    out += U"// }}} declarations\n\n";

    out += U"// {{{ implementation\n\n";
    for (const Module &m : Modules()) {
        out += U("// {{ module: " + m.name + " (implementation)\n");
        for (const std::string &cpp : m.cpps) {
            if (!Read(src_dir, cpp, &text)) return 1;
            out += Wrap(cpp, ApplyRenames(StripLocalIncludes(text, headers), m.renames));
        }
        out += U("// }} module: " + m.name + " (implementation)\n\n");
    }

    out += U"// {{{ module: app (implementation)\n";
    if (!Read(src_dir, "main.cpp", &text)) return 1;
    out += Wrap("main.cpp", StripLocalIncludes(text, headers));
    out += U"// }}} module: app (implementation)\n\n";
    out += U"// }}} implementation\n";

    if (!pytext::WriteBytes(out_path, pytext::Encode(out))) {
        std::fprintf(stderr, "amalgamate: cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out_path.c_str());
    return 0;
}
