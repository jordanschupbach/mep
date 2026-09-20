#!/usr/bin/env python3
"""Generate the reference pages of the help manual.

Three kinds: the Lua API pages, the command index and the key index.

mep exposes ~445 Lua bindings. Hand-writing a reference for that many, and
then keeping it correct as they change, is not realistic -- so these six
pages are generated from `src/lua_env.cpp`, which is where the bindings and
their doc comments already live.

Each binding is registered as `{"name", l_name}` and documented either by a
Doxygen `@brief` on its `l_name` implementation or by a plain `// mep.name(...)`
comment above it. Both forms are read here.

Run by `just help`, before the Org sources are exported. The generated
help/api-*.org files are committed like any other page; regenerating them is
part of the normal docs build, and `just help-check` fails if a committed
page has drifted from what its source produces.
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SOURCE = REPO / "src" / "lua_env.cpp"
HELP = REPO / "help"

# Which page each binding lands on. Order matters: the first pattern that
# matches wins, so the more specific families come first.
FAMILIES = [
    ("api-docs", "Documents and formats", 50, r"""
        Org, Markdown, HTML, PDF, office, spreadsheet, notebook, image, 3D
        model and learning-deck bindings -- everything that drives one of
        mep's document viewers or editors.""",
     r"^(org_|md_|html_|pdf_|office_|sheet_|notebook_|model_|image_|buf_|learn_"
     r"|doc_export|ansi_render|babel_|roam_graph|is_image_path|is_pdf_buffer)"),

    ("api-lang", "Language tooling", 40, r"""
        Language servers, completion, snippets, syntax, spell checking,
        formatting, debugging and git.""",
     r"^(lsp_|completion_|snippet_|spell_|syntax_|dap_|git_|docs_|colorize"
     r"|accept_inline|clear_inline|inline_suggestion|set_inline|set_completion"
     r"|set_insert_tab_hook|leetcode_|diff_lines|ts_)"),

    ("api-ui", "Panes, panels and pickers", 20, r"""
        Everything that puts something on screen: panes and tabs, sidebars,
        pickers, floats, hovers, notifications, themes and icons.""",
     r"^(pane_|tab_new|sidebar_|picker_|float_|hover_|notify|layout"
     r"|set_statusline|set_winbar_click|icon_for_file|hl_for_file|colorscheme"
     r"|current_theme|theme_names|font_size|hint_jump|quick_jump|pick_pane_open"
     r"|resize_pane|nav_pane|focus_top_left_pane|deco_add|ns_)"),

    ("api-workspace", "Workspaces, files and processes", 30, r"""
        Workspaces and projects, the filesystem, subprocesses, HTTP and the
        environment mep runs in.""",
     r"^(workspace_|project_|fs_|list_dir|getcwd|chdir|setenv|unsetenv|platform"
     r"|job_|http_|run_config|direnv|participant|bundled_help_root"
     r"|list_urls_scan|fuzzy_score|read_lines|open$|scratch|quit|now)"),

    ("api-events", "Commands, keys and events", 60, r"""
        Defining commands and key bindings, and reacting to what happens in
        the editor.""",
     r"^(on_|command|map|map_|leader_|mapping_descriptions|set_leader|set_mod1"
     r"|cmd|enter_insert|enter_normal|is_insert_mode|is_terminal_buffer"
     r"|terminal_|active_todo_set|activity_|stt_|set_on_directory_open"
     r"|set_completion_accept_hook|set_completion_resolve_hook)"),

    ("api-editing", "Buffers, text and the cursor", 10, r"""
        Reading and changing buffer contents, moving the cursor, the
        clipboard, folds and undo.""",
     r"."),  # catch-all, so nothing is dropped
]


def bindings(text):
    """Every `{"name", l_name}` registration, in source order."""
    return [m.group(1) for m in re.finditer(r'\{"([a-z_0-9]+)",\s*l_[a-z_0-9]+\}', text)]


def briefs(text):
    """name -> one-line description, from @brief or a `// mep.name(...)` comment."""
    out = {}
    for m in re.finditer(r"@brief\s+(.*?)(?:\n\s*\*\s*@|\n\s*\*/)", text, re.S):
        blurb = " ".join(part.strip(" *") for part in m.group(1).split("\n")).strip()
        impl = re.search(r"Implements mep\.([a-z_0-9]+)", blurb)
        if not impl:
            continue
        desc = re.sub(r"^Implements mep\.[a-z_0-9]+(\([^)]*\))?:?\s*", "", blurb)
        out.setdefault(impl.group(1), desc.rstrip("."))
    for m in re.finditer(r"^// mep\.([a-z_0-9]+)\(([^)]*)\)([^\n]*)", text, re.M):
        out.setdefault(m.group(1), (m.group(3) or "").lstrip(" .->:").rstrip("."))
    return out


def first_sentence(text, limit=150):
    """Keep the reference scannable: one clause per binding, not a paragraph."""
    text = " ".join(text.split())
    text = re.split(r"(?<=[a-z0-9)])\.\s+(?=[A-Z])", text)[0]
    if len(text) > limit:
        cut = text[:limit].rsplit(" ", 1)[0]
        text = cut + "..."
    # `|` would split an Org table cell, and `=` pairs would read as markup.
    return text.replace("|", "/").replace("=", "")


def page(slug, title, order, intro, entries):
    lines = [
        f"#+TITLE: {title}",
        '#+HTML_HEAD: <meta name="help-section" content="Lua API">',
        f'#+HTML_HEAD: <meta name="help-order" content="{order}">',
        '#+HTML_HEAD: <link rel="stylesheet" href="help.css">',
        "",
        "* What is here",
        "",
        " ".join(intro.split()),
        "",
        "This page is generated from mep's own binding table, so it lists what",
        "the running editor actually exposes. Call any of these from",
        "=init.lua=, from =:lua=, or from a =mep-lua= org block.",
        "",
        f"* The bindings ({len(entries)})",
        "",
        "A dash means that binding has no doc comment in the source yet, not",
        "that it does nothing.",
        "",
        "| Function | Does |",
        "|----------+------|",
    ]
    for name, desc in entries:
        lines.append(f"| =mep.{name}= | {desc or '--'} |")
    lines += [
        "",
        "* See also",
        "",
        "- [[file:config.html][Configuration]] -- where to call these from.",
        "- [[file:recipes.html][Configuration recipes]] -- worked examples.",
        "",
    ]
    (HELP / f"{slug}.org").write_text("\n".join(lines))
    return len(entries)


MAIN = REPO / "src" / "main.cpp"
EDITOR = REPO / "src" / "editor.cpp"


def documented_in():
    """command-or-key -> the page that mentions it.

    Doubles the index as a coverage map: a row with no page is a thing mep
    has that the manual has not got round to explaining.
    """
    where = {}
    for src in sorted(HELP.glob("*.org")):
        if src.name.startswith(("_", "api-", "command-index", "key-index")):
            continue
        title = re.search(r"#\+TITLE:\s*(.+)", src.read_text(errors="replace"))
        where[src.stem] = (title.group(1).strip() if title else src.stem,
                           src.read_text(errors="replace"))
    return where


def page_for(token, where, leader=False):
    needle = f"<leader>{token}" if leader else token
    for stem, (title, body) in sorted(where.items()):
        if needle in body:
            return f"[[file:{stem}.html][{title}]]"
    return "--"


def gen_command_index(where):
    main_text = MAIN.read_text(errors="replace")
    editor_text = EDITOR.read_text(errors="replace")
    lua_cmds = set(re.findall(r"mep\.command\('([A-Za-z0-9_]+)'", main_text))
    native = re.search(r"static const std::vector<std::string> kNames = \{(.*?)\};",
                       editor_text, re.S)
    native_cmds = set(re.findall(r'"([^"]+)"', native.group(1))) if native else set()
    rows = []
    for cmd in sorted(lua_cmds | native_cmds, key=str.lower):
        rows.append(f"| =:{cmd}= | {page_for(cmd, where)} |")
    lines = [
        "#+TITLE: Command index",
        '#+HTML_HEAD: <meta name="help-section" content="Reference">',
        '#+HTML_HEAD: <meta name="help-order" content="10">',
        '#+HTML_HEAD: <link rel="stylesheet" href="help.css">',
        "", "* Every command", "",
        f"All {len(lua_cmds | native_cmds)} ex commands mep defines: "
        f"{len(native_cmds)} built in and {len(lua_cmds)} registered from Lua.",
        "",
        "=Tab= at the =:= prompt completes these, and =<leader>hk= lists the key",
        "bindings instead. A dash means no page covers that command yet.",
        "", "| Command | Documented in |", "|---------+---------------|",
    ] + rows + ["", "* See also", "",
                "- [[file:key-index.html][Key index]] -- the same, for keys.",
                "- [[file:command-line.html][The command line]] -- how to run one.", ""]
    (HELP / "command-index.org").write_text("\n".join(lines))
    return len(rows)


def gen_key_index(where):
    main_text = MAIN.read_text(errors="replace")
    rows = []
    for seq, desc in sorted(set(re.findall(r"mep\.leader_map\('([^']+)'\s*,\s*'([^']*)'", main_text))):
        rows.append(f"| =<leader>{seq}= | {desc} | {page_for(seq, where, leader=True)} |")
    lines = [
        "#+TITLE: Key index",
        '#+HTML_HEAD: <meta name="help-section" content="Reference">',
        '#+HTML_HEAD: <meta name="help-order" content="20">',
        '#+HTML_HEAD: <link rel="stylesheet" href="help.css">',
        "", "* Leader bindings", "",
        f"All {len(rows)} =<leader>= sequences registered at startup. Press",
        "=<Space>= for the same list as a popup, or =<leader>hk= for every",
        "binding including your own.",
        "",
        "Motions, operators and the other Normal-mode keys are on the pages in",
        "the Editing section; this index covers the leader layer, which is where",
        "the features live.",
        "", "| Key | Does | Documented in |", "|-----+------+---------------|",
    ] + rows + ["", "* See also", "",
                "- [[file:command-index.html][Command index]] -- the same, for commands.",
                "- [[file:which-key.html][Leader and which-key]] -- the groups.",
                "- [[file:keymaps.html][Remapping keys]] -- changing them.", ""]
    (HELP / "key-index.org").write_text("\n".join(lines))
    return len(rows)


def main():
    text = SOURCE.read_text(errors="replace")
    names = sorted(set(bindings(text)))
    if not names:
        print("gen_reference_pages: found no bindings -- has lua_env.cpp changed shape?",
              file=sys.stderr)
        return 1
    desc = briefs(text)

    claimed, total = set(), 0
    for slug, title, order, intro, pattern in FAMILIES:
        entries = [(n, first_sentence(desc.get(n, "")))
                   for n in names if n not in claimed and re.match(pattern, n)]
        claimed.update(n for n, _ in entries)
        total += page(slug, title, order, intro, entries)
    where = documented_in()
    n_cmds = gen_command_index(where)
    n_keys = gen_key_index(where)
    print(f"gen_reference_pages: {n_cmds} commands, {n_keys} leader keys indexed")
    undocumented = sum(1 for n in names if not desc.get(n))
    print(f"gen_reference_pages: {total} bindings across {len(FAMILIES)} pages "
          f"({undocumented} with no doc comment in lua_env.cpp)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
