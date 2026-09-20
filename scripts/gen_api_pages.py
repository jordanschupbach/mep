#!/usr/bin/env python3
"""Generate the Lua API reference pages of the help manual.

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


def main():
    text = SOURCE.read_text(errors="replace")
    names = sorted(set(bindings(text)))
    if not names:
        print("gen_api_pages: found no bindings -- has lua_env.cpp changed shape?",
              file=sys.stderr)
        return 1
    desc = briefs(text)

    claimed, total = set(), 0
    for slug, title, order, intro, pattern in FAMILIES:
        entries = [(n, first_sentence(desc.get(n, "")))
                   for n in names if n not in claimed and re.match(pattern, n)]
        claimed.update(n for n, _ in entries)
        total += page(slug, title, order, intro, entries)
    undocumented = sum(1 for n in names if not desc.get(n))
    print(f"gen_api_pages: {total} bindings across {len(FAMILIES)} pages "
          f"({undocumented} with no doc comment in lua_env.cpp)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
