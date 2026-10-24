#!/usr/bin/env python3
"""Drive a real mep and check that the incremental LaTeX scan keeps what it should.

mep.org_latex_scan used to throw away every inline maths render in the
buffer and register them all again on each edit; it now keeps each one
whose fragment is still exactly where its render is registered
(Editor::OrgLatexScanDelta), and line edits carry the registry with them
(Editor::ShiftDecorationsForLineEdit). Both are only right if, after any
edit, what was kept is *exactly* what a scan from nothing produces -- a
span left behind at a stale column draws a formula over the wrong text,
and one missing leaves raw TeX on screen. Nothing windowless can see
that: it takes the real scan hook, real renders and real keystrokes.

So for each edit this asserts:
  - the registry, once the renders are in, equals a from-scratch scan's;
  - a second scan has nothing left to do (every fragment kept);
  - the edit re-registered only a few spans, not every one in the file.

Renders need mep's TeX bundle: $MEP_TEX_BUNDLE, else the newest one in
the Nix store. Without one there is nothing to check, and it says so.

Usage: tools/latex_scan_live_check.py [--build-dir build/native] [--keep]
"""

import argparse
import glob
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pdf_live_check import Mep  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Prose, a multi-line fragment, a display block and a table of formulas:
# every kind of span the registry holds.
FIXTURE = """* Notes
Some prose with $a^2$ and $b_{1}$ in it.

A fragment $x +
y$ across two lines.

\\[ \\sum_{k=1}^{n} k \\]

| Term | Meaning |
| $w_{1}$ | weight $h_{1}$ from $x_{1}$ |
| $w_{2}$ | weight $h_{2}$ from $x_{2}$ |
| $w_{3}$ | weight $h_{3}$ from $x_{3}$ |
| $w_{4}$ | weight $h_{4}$ from $x_{4}$ |

Closing prose $z$.
"""

SNAPSHOT = ("(function() local t = {} for _, s in ipairs(mep.buf_latex_inline_list()) do "
            "t[#t + 1] = s.row .. ':' .. s.col_start .. ':' .. s.col_end .. ':' .. s.first_row .. ':' .. s.last_row "
            ".. ':' .. (s.path:match('[^/]*$') or '') end return table.concat(t, ';') end)()")

failures = []


def check(name, ok, detail=""):
    print("   %-62s %s%s" % (name, "PASS" if ok else "FAIL", ("\n        " + detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)


def find_bundle():
    env = os.environ.get("MEP_TEX_BUNDLE")
    if env and os.path.isfile(env):
        return env
    found = glob.glob("/nix/store/*-mep-*/share/mep/tex/tex-bundle.mtb")
    return max(found, key=os.path.getmtime) if found else None


def settle(m, timeout=60.0):
    """The registry once it stops changing: renders arrive over a few frames."""
    last, same, t0 = None, 0, time.time()
    while time.time() - t0 < timeout:
        m.pump(6)
        snap = m.lua_value(SNAPSHOT)
        same = same + 1 if snap == last else 0
        last = snap
        if same >= 4 and snap:
            return snap
        time.sleep(0.3)
    return last


def leave(m):
    """Normal mode, cursor on the heading: no fragment is deferred for it."""
    m.key("Escape")
    m.cmd("1")
    m.pump(10)


def scratch_snapshot(m):
    m.cmd("lua mep.buf_clear_latex_inline() mep.org_latex_scan()")
    return settle(m)


def adds(m):
    return int(m.lua_value("LATEX_ADDS") or -1)


def run(build_dir, keep):
    bundle = find_bundle()
    if not bundle:
        print("no TeX bundle (set MEP_TEX_BUNDLE): nothing to check")
        return 0
    os.environ["MEP_TEX_BUNDLE"] = bundle
    work = tempfile.mkdtemp(prefix="/tmp/meplsc-")
    path = os.path.join(work, "scan.org")
    with open(path, "w") as fh:
        fh.write(FIXTURE)
    m = Mep(build_dir, path, keep=keep)
    try:
        # Counts registrations: mep_org_latex_register_inline looks
        # mep.buf_add_latex_inline up at each call.
        m.cmd("lua LATEX_ADDS = 0 local f = mep.buf_add_latex_inline "
              "LATEX_LOG = {} mep.buf_add_latex_inline = function(r, a, ...) LATEX_ADDS = LATEX_ADDS + 1 "
              "LATEX_LOG[#LATEX_LOG + 1] = r .. ':' .. a return f(r, a, ...) end")
        leave(m)
        first = settle(m, timeout=120.0)
        total = len(first.split(";")) if first else 0
        print("== initial scan (%d spans)" % total)
        check("every fragment has a render", total >= 14, "spans: %r" % first)

        def scenario(name, do, max_adds):
            print("== " + name)
            m.cmd("lua LATEX_ADDS = 0 LATEX_LOG = {}")
            do()
            leave(m)
            kept = settle(m)
            n = adds(m)
            log = m.lua_value("table.concat(LATEX_LOG, ' ')")
            # A second scan, with everything in: nothing to register.
            m.cmd("lua LATEX_ADDS = 0 mep.org_latex_scan()")
            m.pump(10)
            again = adds(m)
            fresh = scratch_snapshot(m)
            check("kept registry equals a from-scratch scan", kept == fresh,
                  "kept:  %s\n        fresh: %s" % (kept, fresh))
            check("a second scan registers nothing", again == 0, "it registered %d" % again)
            check("re-registered at most %d of %d spans" % (max_adds, total), 0 <= n <= max_adds, "it registered %d: %s" % (n, log))

        table_row = FIXTURE.split("\n").index("| $w_{2}$ | weight $h_{2}$ from $x_{2}$ |") + 1

        def type_in_cell():
            m.cmd(str(table_row))
            m.cmd("lua mep.set_cursor(%d, 3) mep.enter_insert() mep.insert_text('abc')" % table_row)
        # Leaving the table realigns it (org's own behaviour): the widened
        # first column moves the second on all four rows -- eight formulas
        # really are somewhere else -- plus the edited row's own.
        scenario("typing in a table cell before its formulas", type_in_cell, 9)

        def edit_formula():
            m.cmd("lua local l = mep.get_line(%d) mep.set_line(%d, (l:gsub('h_{2}', 'h_{22}')))" % (table_row, table_row))
        scenario("editing a formula in place", edit_formula, 3)

        def open_line():
            m.cmd("2")
            m.rpc.call("ui.key_down", {"key": "Shift_L"})
            m.rpc.call("ui.key_press", {"key": "o"})
            m.rpc.call("ui.key_up", {"key": "Shift_L"})
            m.pump(6)
            m.cmd("lua mep.insert_text('new line')")
        scenario("opening a line above everything", open_line, 2)

        def delete_line():
            m.cmd("2")
            m.key("d")
            m.key("d")
        scenario("deleting a line above everything", delete_line, 2)

        def undo():
            m.key("u")
            m.pump(6)
            m.key("u")
        scenario("undoing", undo, 14)

        def delete_formula():
            m.cmd("lua local l = mep.get_line(2) mep.set_line(2, (l:gsub(' and %$b_{1}%$', '')))")
        scenario("deleting a formula", delete_formula, 2)
    finally:
        m.close()
    print("\n%d failed" % len(failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    sys.exit(run(os.path.abspath(args.build_dir), args.keep))


if __name__ == "__main__":
    main()
