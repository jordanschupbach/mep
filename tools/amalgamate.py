#!/usr/bin/env python3
"""Generates a single self-contained .cpp that combines every mep source
file (all core modules, the collaboration subsystem, and the embedded
font/query data headers -- with their upstream license text intact) into
one translation unit, for the `mep-amalgam` build target (see
CMakeLists.txt). This is the inverse of splitting the library apart: each
module here is still one file on disk (src/*.cpp, src/*.h) and still
builds independently as part of `mep_core` -- this script never edits
those files, it only concatenates them.

Usage: amalgamate.py <src_dir> <output_file>
"""
import re
import sys
import os

# Every module that used to be (and, in the normal build, still is) its
# own translation unit, in dependency order: each module's declarations
# must be visible before any later module's declarations or bodies. The
# rename list disambiguates symbol names that are only unique because
# each module is normally its own translation unit (e.g. two modules
# each privately declaring a local class Parser/Lexer) -- harmless
# normally, but a hard duplicate-definition error once every module
# shares one translation unit.
MODULES = [
    dict(name="vterm", header="vterm.h", cpps=["vterm.cpp"], renames=[]),
    dict(name="regex", header="regex.h", cpps=["regex.cpp"], renames=[
        ("Parser", "Parser_regex"),
    ]),
    dict(name="treesitter", header="treesitter.h", cpps=["treesitter.cpp"], renames=[]),
    dict(name="image_doc", header="image_doc.h", cpps=["image_doc.cpp"], renames=[]),
    dict(name="pdf_doc", header="pdf_doc.h", cpps=["pdf_doc.cpp"], renames=[]),
    dict(name="formula", header="formula.h", cpps=["formula.cpp"], renames=[
        ("Lexer", "Lexer_formula"),
        ("Parser", "Parser_formula"),
        ("Token", "Token_formula"),
        ("NodePtr", "NodePtr_formula"),
    ]),
    dict(name="html_doc", header="html_doc.h", cpps=["html_doc.cpp"], renames=[]),
    dict(name="job", header="job.h", cpps=["job.cpp"], renames=[]),
    dict(name="org_doc", header="org_doc.h", cpps=["org_doc.cpp"], renames=[
        ("EqualsIgnoreCase", "EqualsIgnoreCase_org_doc"),
    ]),
    dict(name="agent_rpc", header="agent_rpc.h", cpps=["agent_rpc.cpp"], renames=[]),
    dict(name="lua_env", header="lua_env.h", cpps=["lua_env.cpp"], renames=[
        ("ReplaceAllLiteral", "ReplaceAllLiteral_lua_env"),
    ]),
    dict(name="office_doc", header="office_doc.h", cpps=["office_doc.cpp", "office_odt.cpp"], renames=[
        ("LowerExt", "LowerExt_office_doc"),
    ]),
    dict(name="doc_export", header="doc_export.h", cpps=["doc_export.cpp"], renames=[
        ("LowerExt", "LowerExt_doc_export"),
    ]),
    dict(name="sheet_doc", header="sheet_doc.h", cpps=["sheet_doc.cpp", "sheet_xlsx.cpp", "sheet_ods.cpp"], renames=[
        ("CallFunction", "CallFunction_sheet_doc"),
        ("ToNumber", "ToNumber_sheet_doc"),
        ("LowerExt", "LowerExt_sheet_doc"),
    ]),
    dict(name="js_engine", header="js_engine.h", cpps=["js_engine.cpp"], renames=[
        ("CallFunction", "CallFunction_js_engine"),
        ("ToNumber", "ToNumber_js_engine"),
        ("Lexer", "Lexer_js_engine"),
        ("Parser", "Parser_js_engine"),
        ("Token", "Token_js_engine"),
        ("NodePtr", "NodePtr_js_engine"),
    ]),
    dict(name="workspace_git", header="workspace_git.h", cpps=["workspace_git.cpp"], renames=[]),
    dict(name="editor", header="editor.h", cpps=["editor.cpp"], renames=[
        ("EqualsIgnoreCase", "EqualsIgnoreCase_editor"),
        ("ReplaceAllLiteral", "ReplaceAllLiteral_editor"),
    ]),
]

# Small foundational headers with no module of their own (shared with
# mep-collabd / the collab test executables, which keep compiling them as
# separate translation units regardless of this amalgamation).
PLAIN_HEADERS = ["json.h", "persist.h", "rpc_framing.h"]

# The collaboration subsystem: also its own translation units in the
# normal build (linked a second time into mep-collabd and the collab
# test executables), embedded here too so mep-amalgam is fully
# self-contained.
COLLAB_HEADERS = ["collab_crdt.h", "collab_session.h", "collab_websocket.h"]
COLLAB_CPPS = ["collab_crdt.cpp", "collab_session.cpp", "collab_websocket.cpp"]

# Pure data tables (fonts, Treesitter highlight queries) -- each carries
# its own upstream license text (SPDX identifier + source URL, or a
# verbatim license header for vendored query files) directly in the
# comments preceding the data, which is preserved verbatim below.
DATA_HEADERS = [
    "font_data.h",
    "icon_font_data.h",
    "symbol_font_data.h",
    "office_font_data.h",
    "office_font_data_mono.h",
    "office_font_data_serif.h",
    "treesitter_queries.h",
]

ALL_LOCAL_HEADERS = set(
    [m["header"] for m in MODULES] + PLAIN_HEADERS + COLLAB_HEADERS + DATA_HEADERS
)

INCLUDE_RE = re.compile(r'^#include\s+"([^"]+)"\s*$')


def strip_local_includes(text):
    out = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        m = INCLUDE_RE.match(stripped)
        if m and m.group(1) in ALL_LOCAL_HEADERS:
            continue
        # Meaningless (and Wpragma-once-outside-header-noisy) once every
        # header shares one translation unit; the #ifndef/#define/#endif
        # guards the other headers use stay, doing no harm here.
        if stripped == "#pragma once":
            continue
        out.append(line)
    return "".join(out)


def apply_renames(text, renames):
    for old, new in renames:
        text = re.sub(r"\b" + re.escape(old) + r"\b", new, text)
    return text


def read(src_dir, name):
    with open(os.path.join(src_dir, name), encoding="utf-8") as f:
        return f.read()


def wrap(name, text, indent=""):
    return f"{indent}// {{{{ file: {name}\n{text.rstrip()}\n{indent}// }}}} file: {name}\n\n"


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    src_dir, out_path = sys.argv[1], sys.argv[2]

    out = []
    out.append(
        "// GENERATED FILE -- do not edit by hand.\n"
        "// Produced by tools/amalgamate.py from the separate module sources under\n"
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
        "// original header, immediately above the data it covers.\n\n"
    )

    out.append("// {{{ embedded headers and data\n\n")
    for name in PLAIN_HEADERS + COLLAB_HEADERS + DATA_HEADERS:
        out.append(wrap(name, strip_local_includes(read(src_dir, name))))
    out.append("// }}} embedded headers and data\n\n")

    out.append("// {{{ collaboration subsystem\n\n")
    for name in COLLAB_CPPS:
        out.append(wrap(name, strip_local_includes(read(src_dir, name))))
    out.append("// }}} collaboration subsystem\n\n")

    out.append("// {{{ declarations\n\n")
    for m in MODULES:
        text = strip_local_includes(read(src_dir, m["header"]))
        text = apply_renames(text, m["renames"])
        out.append(f"// {{{{ module: {m['name']} (declarations)\n")
        out.append(wrap(m["header"], text))
        out.append(f"// }}}} module: {m['name']} (declarations)\n\n")
    out.append("// }}} declarations\n\n")

    out.append("// {{{ implementation\n\n")
    for m in MODULES:
        out.append(f"// {{{{ module: {m['name']} (implementation)\n")
        for cpp in m["cpps"]:
            text = strip_local_includes(read(src_dir, cpp))
            text = apply_renames(text, m["renames"])
            out.append(wrap(cpp, text))
        out.append(f"// }}}} module: {m['name']} (implementation)\n\n")

    out.append("// {{{ module: app (implementation)\n")
    out.append(wrap("main.cpp", strip_local_includes(read(src_dir, "main.cpp"))))
    out.append("// }}} module: app (implementation)\n\n")
    out.append("// }}} implementation\n")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("".join(out))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
