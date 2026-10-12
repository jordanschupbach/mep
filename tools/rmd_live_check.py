#!/usr/bin/env python3
"""End-to-end check of R Markdown notebooks in a real mep (Xvfb + the
agent socket, the same harness as tools/fold_live_check.py, whose Mep /
Xvfb helpers this reuses).

What it drives, through real keystrokes:
  - an .Rmd opens as a notebook (mep.notebook_format() == 'rmd')
  - Shift+Enter inside a chunk runs it; a later chunk sees its variables
    (one persistent R session)
  - C-c C-c runs a chunk; a plot comes back as an image output
  - Enter / Shift+Enter in the prose run nothing
  - :w writes the document byte-for-byte as it was (outputs never go in it)
  - after quitting and reopening, the outputs come back from the cache
Screenshots land next to --shots (default: a temp dir) for a visual check.
Skips (exit 0) when Rscript is not on PATH.

    python3 tools/rmd_live_check.py [--build-dir build/native] [--keep]
"""
import argparse
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fold_live_check import RESULTS, Mep, Xvfb, check  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DOC = """---
title: "Live check"
---

Some prose with `inline` code.

```{r first}
x <- 20
cat("set\\n")
```

More prose.

```{r second}
x + 22
```

```{r plot, fig.width=5}
plot(1:3)
```

```{r slow}
Sys.sleep(30)
```
"""
# 1-indexed rows used below (the editor's own :N numbering).
ROW_FIRST_BODY = 8     # x <- 20
ROW_SECOND_BODY = 15   # x + 22
ROW_PLOT_BODY = 19     # plot(1:3)
ROW_PROSE = 5          # Some prose ...
ROW_SLOW_BODY = 23     # Sys.sleep(30)
CELL_FIRST, CELL_SECOND, CELL_PLOT, CELL_SLOW = 1, 3, 4, 5   # 0-based cell indices (prose spans count)


def cell_state(m, idx):
    expr = ("(function() local o = mep.notebook_cell_outputs(%d) if not o then return 'nil' end "
            "local t = '' for _, x in ipairs(o.outputs) do "
            "t = t .. x.kind .. ':' .. (x.text or '') .. (x.has_image and '[img]' or '') .. '|' end "
            "return o.run_state .. '#' .. tostring(o.execution_count) .. '#' .. t end)()" % idx)
    return m.lua_value(expr) or ""


def wait_ran(m, idx, timeout=40):
    end = time.time() + timeout
    st = ""
    while time.time() < end:
        st = cell_state(m, idx)
        parts = st.split("#", 2)
        if len(parts) == 3 and parts[0] == "idle" and parts[1] not in ("-1", "nil"):
            return st
        m.pump(10)
        time.sleep(0.3)
    return st


def shift_enter(m):
    m.rpc.call("ui.key_down", {"key": "Shift_L"})
    m.rpc.call("ui.key_press", {"key": "Return"})
    m.rpc.call("ui.key_up", {"key": "Shift_L"})
    m.pump(8)


def ctrl_c_ctrl_c(m):
    m.rpc.call("ui.key_down", {"key": "Control_L"})
    m.rpc.call("ui.key_press", {"key": "c"})
    m.pump(2)
    m.rpc.call("ui.key_press", {"key": "c"})
    m.rpc.call("ui.key_up", {"key": "Control_L"})
    m.pump(8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native"))
    ap.add_argument("--shots", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not shutil.which("Rscript"):
        print("Rscript not on PATH -- skipping")
        return 0
    home = tempfile.mkdtemp(prefix="/tmp/mepr-")
    shots = args.shots or home
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "report.Rmd")
    with open(path, "w") as fh:
        fh.write(DOC)
    xv = Xvfb()
    try:
        print("== 1. run chunks in a fresh session")
        m = Mep(args.build_dir, home, xv.display, [path], cwd=proj)
        try:
            check("an .Rmd opens as an rmd notebook", m.lua_value("mep.notebook_format()") == "rmd",
                  str(m.lua_value("mep.notebook_format()")))
            check("the buffer is the file's own text", m.line(7) == "```{r first}", repr(m.line(7)))

            m.cmd(str(ROW_FIRST_BODY))
            shift_enter(m)
            st = wait_ran(m, CELL_FIRST)
            check("Shift+Enter in a chunk runs it", "stream:set" in st, str(st))

            m.cmd(str(ROW_SECOND_BODY))
            ctrl_c_ctrl_c(m)
            st = wait_ran(m, CELL_SECOND)
            check("C-c C-c runs a chunk, and it sees x (one R session)", "[1] 42" in st, str(st))

            m.cmd(str(ROW_PLOT_BODY))
            shift_enter(m)
            st = wait_ran(m, CELL_PLOT)
            check("a plot comes back as an image output", "[img]" in st, str(st))
            m.screenshot(os.path.join(shots, "rmd-1-ran.png"))

            # Interrupt: the running chunk stops, R (and its state) lives on.
            m.cmd(str(ROW_SLOW_BODY))
            shift_enter(m)
            time.sleep(2.0)
            m.pump(10)
            m.cmd("NotebookInterrupt")
            st = wait_ran(m, CELL_SLOW, timeout=15)
            check("Interrupt stops a running chunk", "Interrupted" in st, str(st))
            m.cmd(str(ROW_SECOND_BODY))
            shift_enter(m)
            st = wait_ran(m, CELL_SECOND)
            check("...and the R session survives with its state", "[1] 42" in st, str(st))

            # Prose: plain Enter moves down, Shift+Enter runs nothing.
            m.cmd(str(ROW_PROSE))
            before = m.lua_value("mep.notebook_cell_outputs(0).run_state")
            # (Normal-mode Enter has no motion of its own in mep; what
            # matters is that it no longer runs anything here.)
            m.key("Return")
            m.pump(10)
            check("plain Enter in prose runs nothing",
                  all(cell_state(m, i).startswith("idle") for i in range(6)), str([cell_state(m, i) for i in range(6)]))
            m.cmd(str(ROW_PROSE))
            shift_enter(m)
            m.pump(10)
            check("Shift+Enter in prose runs nothing",
                  before == "idle" and m.lua_value("mep.notebook_cell_outputs(0).run_state") == "idle"
                  and "stream" not in cell_state(m, 0), cell_state(m, 0))

            # The R language server sees the chunks: the cursor in an R
            # chunk resolves to a polyglot shadow .R document.
            m.cmd(str(ROW_SECOND_BODY))
            uri = m.lua_value("(function() local c = mep_polyglot_context_at_cursor() "
                              "return c and c.uri or 'nil' end)()") or ""
            check("an R chunk is served by an R language server (.R shadow)", uri.endswith(".R"), uri)
            m.cmd(str(ROW_PROSE))
            uri = m.lua_value("(function() local c = mep_polyglot_context_at_cursor() "
                              "return c and c.uri or 'nil' end)()") or ""
            check("prose has no language-server context", uri == "nil", uri)

            m.cmd("w")
            m.pump(10)
            with open(path) as fh:
                on_disk = fh.read()
            check(":w leaves the document byte-identical", on_disk == DOC, repr(on_disk[:200]))
            m.pump(10)
        finally:
            m.quit()

        cache_dir = os.path.join(home, "share", "mep", "notebook-cache")
        caches = os.listdir(cache_dir) if os.path.isdir(cache_dir) else []
        check("an output cache was written outside the document", len(caches) == 1, str(caches))

        print("== 2. reopen: outputs come back from the cache")
        m2 = Mep(args.build_dir, home, xv.display, [path], cwd=proj)
        try:
            st2 = cell_state(m2, CELL_SECOND)
            check("chunk 'second' has its [1] 42 back", "[1] 42" in st2, st2)
            check("the plot output is back", "[img]" in cell_state(m2, CELL_PLOT), cell_state(m2, CELL_PLOT))
            m2.screenshot(os.path.join(shots, "rmd-2-reopened.png"))
            # Edit a chunk and run it again: new output replaces the cached one.
            m2.cmd(str(ROW_SECOND_BODY))
            m2.cmd("s/22/100/")
            shift_enter(m2)
            st3 = wait_ran(m2, CELL_SECOND)
            check("re-running after an edit replaces the output (fresh session: x is unset)",
                  "object 'x' not found" in st3 or "[1] 120" in st3, st3)
            m2.cmd("e!")
            m2.pump(10)
        finally:
            m2.quit()
    finally:
        xv.close()
    bad = [r for r in RESULTS if not r[1]]
    print("\n---- %d checks, %d failures ----" % (len(RESULTS), len(bad)))
    for name, _, detail in bad:
        print("  FAIL %s\n       %s" % (name, detail))
    print("screenshots: %s" % shots)
    if not args.keep and not args.shots and not bad:
        shutil.rmtree(home, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
