#!/usr/bin/env python3
"""Drive a real mep and check hand-typed LaTeX math in an Org buffer.

The acceptance checks of plans/LATEX_IN_ORG_TYPING_SPEC.md (section 10),
plus the pieces that section leans on: math autosnippets that fire the
instant their trigger is typed, but only inside math; tab stops whose
placeholder is replaced by typing and that follow the text as it grows;
Tab that walks a snippet's stops (even with the completion menu up) and
leaves past its closing brace; nested and mirrored snippets; choice
fields; menu-expanded org snippets; the delimiter-balance linter.

Everything is typed as real keystrokes, one character per frame or so --
the autosnippet poller only sees what the frame loop sees, so typing a
whole word in one go would test something no user can do. The plumbing
(Xvfb, focus, the agent socket) is tools/fold_live_check.py's.

Usage: tools/latex_typing_live_check.py [--build-dir build/native] [--keep] [--only N]
"""

import argparse
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fold_live_check import RESULTS, Mep, Xvfb, check  # noqa: E402

# X keysym names for the unshifted keys, and the base key of each shifted
# symbol on a US layout. Shifted keys need Shift genuinely held (see
# tools/pdf_live_check.py), so they are typed as Shift_L + base.
KEYSYM = {" ": "space", "\\": "backslash", "/": "slash", ";": "semicolon", "=": "equal", "-": "minus",
          ".": "period", ",": "comma", "[": "bracketleft", "]": "bracketright", "'": "apostrophe",
          "`": "grave"}
SHIFTED = {"$": "4", "(": "9", ")": "0", "{": "bracketleft", "}": "bracketright", "^": "6", "_": "minus",
           ">": "period", "<": "comma", "!": "1", "+": "equal", ":": "semicolon", "~": "grave", "|": "backslash",
           "*": "8", "&": "7", "?": "slash", "@": "2", "#": "3", "%": "5", '"': "apostrophe"}


class Typist:
    def __init__(self, mep):
        self.m = mep

    def char(self, c):
        r = self.m.rpc
        if c in SHIFTED or c.isupper():
            base = SHIFTED.get(c, c.lower())
            r.call("ui.key_down", {"key": "Shift_L"})
            r.call("ui.key_press", {"key": KEYSYM.get(base, base)})
            r.call("ui.key_up", {"key": "Shift_L"})
        else:
            r.call("ui.key_press", {"key": KEYSYM.get(c, c)})
        self.m.pump(3)

    def text(self, s):
        for c in s:
            self.char(c)
        self.m.pump(4)

    def key(self, name):
        self.m.rpc.call("ui.key_press", {"key": name})
        self.m.pump(5)

    def chord(self, mod, name):
        r = self.m.rpc
        r.call("ui.key_down", {"key": mod})
        r.call("ui.key_press", {"key": name})
        r.call("ui.key_up", {"key": mod})
        self.m.pump(5)

    def tab(self):
        self.key("Tab")

    def shift_tab(self):
        self.chord("Shift_L", "Tab")

    def ctrl(self, name):
        self.chord("Control_L", name)


def lua_str(s):
    return json.dumps(s)


class Note:
    """The org buffer under test, reset between checks."""

    def __init__(self, mep):
        self.m = mep
        self.t = Typist(mep)

    def reset(self, lines, row, col):
        """Normal mode, buffer = lines, cursor at (row, col) 1-indexed, then Insert there."""
        self.t.key("Escape")
        self.t.key("Escape")
        arr = "{" + ",".join(lua_str(l) for l in lines) + "}"
        # Insert first: Normal mode would clamp an end-of-line column back
        # onto the last character.
        self.m.cmd("lua mep.buffer_set_lines(mep.current_buffer(), %s) mep.enter_insert() mep.set_cursor(%d, %d)"
                   % (arr, row, col))
        self.m.pump(6)

    def lines(self):
        txt = self.m.lua_value("(function() local t = {} for r = 1, mep.line_count() do t[r] = mep.get_line(r) end "
                               "return table.concat(t, '\\n') end)()")
        return (txt or "").split("\n")

    def line(self, row):
        return self.lines()[row - 1]

    def cursor(self):
        txt = self.m.lua_value("(function() local r, c = mep.cursor() return r .. ',' .. c end)()")
        r, c = txt.split(",")
        return int(r), int(c)

    def snip(self):
        txt = self.m.lua_value("(function() local i = mep.snippet_info() return i.depth .. ',' .. i.num .. ',' .. "
                               "i.s_row .. ',' .. i.s_col .. ',' .. i.e_row .. ',' .. i.e_col .. ',' .. "
                               "tostring(i.selected) end)()")
        d, n, sr, sc, er, ec, sel = txt.split(",")
        return {"depth": int(d), "num": int(n), "s": (int(sr), int(sc)), "e": (int(er), int(ec)),
                "selected": sel == "true"}

    def selected_text(self):
        s = self.snip()
        if not s["selected"] or s["s"][0] != s["e"][0]:
            return None
        return self.line(s["s"][0])[s["s"][1] - 1:s["e"][1] - 1]

    def menu(self):
        txt = self.m.lua_value("(function() local s = mep.completion_state() return tostring(s.open) .. '|' .. "
                               "table.concat(s.items, ' ') end)()")
        o, items = txt.split("|", 1)
        return o == "true", items.split()


# ----------------------------------------------------------------- checks


def run_checks(n, only):
    m, t = n.m, n.t

    def want(k):
        return only is None or k in only

    if want(1):
        print("== 1. prose never expands")
        n.reset([""], 1, 1)
        t.text("the sum is in a set")
        check("prose line unchanged", n.line(1) == "the sum is in a set", repr(n.line(1)))

    if want(2):
        print("== 2. mk, //, typing replaces placeholders, Tab leaves past }")
        n.reset([""], 1, 1)
        t.text("mk")
        check("mk -> $|$", n.line(1) == "$$" and n.cursor() == (1, 2), "%r %r" % (n.line(1), n.cursor()))
        t.text("//")
        check("// -> \\frac{num}{den} inside $", n.line(1) == "$\\frac{num}{den}$", repr(n.line(1)))
        check("num is selected", n.selected_text() == "num", repr(n.snip()))
        t.text("a")
        t.tab()
        check("den selected after Tab", n.selected_text() == "den", repr(n.snip()))
        t.text("b")
        t.tab()
        check("$\\frac{a}{b}|$", n.line(1) == "$\\frac{a}{b}$" and n.cursor() == (1, 13),
              "%r %r" % (n.line(1), n.cursor()))
        t.tab()
        check("Tab once more -> past the closing $", n.cursor() == (1, 14), repr(n.cursor()))
        t.text(" x")
        check("Tab after the last exit is ordinary typing", n.line(1) == "$\\frac{a}{b}$ x", repr(n.line(1)))

    if want(3):
        print("== 3. letter+digit auto-subscript")
        n.reset(["$$"], 1, 2)
        t.text("x1+y2")
        check("x1+y2 -> x_{1}+y_{2}", n.line(1) == "$x_{1}+y_{2}$", repr(n.line(1)))
        t.text("+z12")
        check("z12 -> z_{12} (second digit joins)", n.line(1) == "$x_{1}+y_{2}+z_{12}$", repr(n.line(1)))

    if want(4):
        print("== 4. Greek and arrows")
        n.reset(["$$"], 1, 2)
        t.text(";a->;b")
        check(";a->;b -> \\alpha\\to\\beta", n.line(1) == "$\\alpha\\to\\beta$", repr(n.line(1)))

    if want(5):
        print("== 5. int / inf after in -- one backslash")
        n.reset(["$$"], 1, 2)
        t.text("int")
        check("int -> \\int_{a}^{b} f(x) \\, dx", n.line(1) == "$\\int_{a}^{b} f(x) \\, dx$", repr(n.line(1)))
        check("a is selected", n.selected_text() == "a", repr(n.snip()))
        n.reset(["$$"], 1, 2)
        t.text("inf")
        check("inf -> \\infty", n.line(1) == "$\\infty$", repr(n.line(1)))
        n.reset(["$$"], 1, 2)
        t.text("X indep Y")
        check("indep -> \\perp\\!\\!\\!\\perp", n.line(1) == "$X \\perp\\!\\!\\!\\perp Y$", repr(n.line(1)))

    if want(6):
        print("== 6. ;ph ;ps ;Ph ;Ps reachable")
        for typed, out in ((";ph", "\\phi"), (";ps", "\\psi"), (";Ph", "\\Phi"), (";Ps", "\\Psi"), (";p", "\\pi")):
            n.reset(["$$"], 1, 2)
            t.text(typed)
            check("%s -> %s" % (typed, out), n.line(1) == "$" + out + "$", repr(n.line(1)))

    if want(7):
        print("== 7. word boundary: nsum, argmin")
        n.reset(["$$"], 1, 2)
        t.text("nsum")
        check("nsum -> \\sum_{i=1}^{n}", n.line(1) == "$\\sum_{i=1}^{n}$", repr(n.line(1)))
        n.reset(["$$"], 1, 2)
        t.text("argmin")
        check("argmin -> \\underset{\\theta}{\\arg\\min}\\;", n.line(1) == "$\\underset{\\theta}{\\arg\\min}\\;$",
              repr(n.line(1)))

    if want(8):
        print("== 8. ck from prose, ali nested inside")
        n.reset([""], 1, 1)
        t.text("ck")
        check("ck -> \\[ / indented body / \\]", n.lines() == ["\\[", "  ", "\\]"] and n.cursor() == (2, 3),
              "%r %r" % (n.lines(), n.cursor()))
        t.text("ali")
        want_lines = ["\\[", "  \\begin{align*}", "    a &= b", "  \\end{align*}", "\\]"]
        check("ali nests, indented under the body", n.lines() == want_lines, repr(n.lines()))
        check("a &= b is selected", n.selected_text() == "a &= b", repr(n.snip()))
        t.text("x &= y")
        t.tab()
        check("Tab leaves align* (row 4, after })", n.cursor() == (4, 15) and n.snip()["depth"] == 1,
              "%r %r" % (n.cursor(), n.snip()))
        t.tab()
        check("Tab again leaves \\[ \\]", n.cursor() == (5, 3) and n.snip()["depth"] == 0,
              "%r %r" % (n.cursor(), n.snip()))

    if want(9):
        print("== 9. beg mirrors the environment name")
        n.reset(["\\[", "  ", "\\]"], 2, 3)
        t.text("beg")
        check("beg -> \\begin{env} ... \\end{env}", n.lines()[1:4] == ["  \\begin{env}", "    body", "  \\end{env}"],
              repr(n.lines()))
        t.text("matr")
        check("\\end{} follows mid-word", n.line(4) == "  \\end{matr}", repr(n.lines()))
        t.text("ix")
        check("\\end{matrix}", n.line(2) == "  \\begin{matrix}" and n.line(4) == "  \\end{matrix}", repr(n.lines()))
        t.tab()
        t.text("1 & 0")
        check("body typed", n.line(3) == "    1 & 0", repr(n.lines()))

    if want(10):
        print("== 10. a stray $ heals at the paragraph break")
        n.reset(["costs $5 a stray dollar", "", ""], 3, 1)
        t.text("the sum is in")
        check("next paragraph is prose", n.line(3) == "the sum is in", repr(n.lines()))
        n.reset(["costs $5 a stray dollar sum", "", ""], 1, 28)
        t.text(" in")
        check("same paragraph still sees math (in -> \\in)", n.line(1).endswith(" \\in"), repr(n.line(1)))

    if want(11):
        print("== 11. \\\\ opens no symbol menu")
        n.reset(["\\begin{cases}", "  a & b ", "\\end{cases}"], 2, 9)
        t.text("\\\\")
        is_open, items = n.menu()
        check("after \\\\ the menu stays shut", n.line(2) == "  a & b \\\\" and not is_open, "%r %r" % (n.line(2), items))
        n.reset(["$$"], 1, 2)
        t.text("\\al")
        is_open, items = n.menu()
        check("\\al offers alpha (control)", is_open and "alpha" in items, repr(items[:10]))

    if want(12):
        print("== 12. Tab: literal without a snippet, jump even with the menu up")
        n.reset(["x"], 1, 2)
        t.tab()
        check("Tab inserts two spaces in org", n.line(1) == "x  ", repr(n.line(1)))
        n.reset(["$$"], 1, 2)
        t.text("//")
        t.text("s")
        is_open, items = n.menu()
        check("menu is up after 's'", is_open, repr(items))
        t.tab()
        check("Tab jumped to den, not into the menu", n.line(1) == "$\\frac{s}{den}$" and n.selected_text() == "den",
              "%r %r" % (n.line(1), n.snip()))

    if want(13):
        print("== 13. Tab keeps placeholder defaults")
        n.reset(["$$"], 1, 2)
        t.text("lim")
        t.tab()
        t.tab()
        t.tab()
        check("\\lim_{n \\to \\infty} a_n|", n.line(1) == "$\\lim_{n \\to \\infty} a_n$" and n.cursor() == (1, 25),
              "%r %r" % (n.line(1), n.cursor()))

    if want(14):
        print("== 14. no auto-pairing")
        n.reset([""], 1, 1)
        t.text("({[")
        check("({[ inserts only those", n.line(1) == "({[", repr(n.line(1)))

    if want(15):
        print("== 15. nested // inside //")
        n.reset(["$$"], 1, 2)
        t.text("////")
        check("inner frac in outer numerator", n.line(1) == "$\\frac{\\frac{num}{den}}{den}$", repr(n.line(1)))
        t.text("a")
        t.tab()
        t.text("b")
        t.tab()
        t.tab()
        t.text("c")
        t.tab()
        check("\\frac{\\frac{a}{b}}{c}|", n.line(1) == "$\\frac{\\frac{a}{b}}{c}$" and n.cursor() == (1, 23),
              "%r %r" % (n.line(1), n.cursor()))

    if want(16):
        print("== 16. org menu snippets: accept expands, choice cycles, cbl is prose-only")
        n.reset([""], 1, 1)
        t.text("src")
        is_open, items = n.menu()
        check("src offered in the menu", is_open and "src" in items, repr(items))
        t.ctrl("y")
        check("C-y expands src", n.lines() == ["#+begin_src python", "", "#+end_src"], repr(n.lines()))
        t.ctrl("j")
        check("C-j cycles the language", n.line(1) == "#+begin_src lua", repr(n.lines()))
        t.ctrl("k")
        t.ctrl("k")
        check("C-k cycles back (wraps to the empty choice)", n.line(1) == "#+begin_src ", repr(n.lines()))
        t.ctrl("j")
        t.tab()
        t.text("print(1)")
        check("Tab into the body", n.lines() == ["#+begin_src python", "print(1)", "#+end_src"], repr(n.lines()))
        n.reset([""], 1, 1)
        t.text("quote")
        t.key("Return")
        check("Enter on an untouched menu is a newline", n.lines() == ["quote", ""], repr(n.lines()))
        n.reset(["$$"], 1, 2)
        t.text("cb")
        is_open, items = n.menu()
        check("cbl not offered in math", "cbl" not in items, repr(items))
        n.reset([""], 1, 1)
        t.text("cb")
        is_open, items = n.menu()
        check("cbl offered in prose", "cbl" in items, repr(items))

    if want(17):
        print("== 17. undo restores the trigger")
        n.reset(["$$"], 1, 2)
        t.text("sum")
        t.key("Escape")
        t.key("u")
        check("u after sum gives the typed trigger back", n.line(1) == "$sum$", repr(n.line(1)))

    if want(18):
        print("== 18. delimiter-balance linter")
        n.reset(["ok $x$ and \\(y\\)", "stray $ here", "", "\\begin{align*}", "a \\\\ b", "\\end{align}", ""], 1, 1)
        t.key("Escape")
        txt = m.lua_value("(function() local o = {} for _, p in ipairs(mep.math_lint()) do "
                          "o[#o + 1] = p.row .. ':' .. p.col end return table.concat(o, ' ') end)()")
        check("stray $ and mismatched \\end reported", txt == "2:7 4:1 6:1", repr(txt))

    if want(19):
        print("== 19. prefix-collision audit")
        txt = m.lua_value("(function() local o = {} for _, a in ipairs(mep.autosnippet_prefix_audit('org')) do "
                          "o[#o + 1] = a.trig .. '<' .. a.prefix .. '=' .. #a.forms end return table.concat(o, ' ') "
                          "end)()")
        print("      " + txt)
        pairs = dict((p.split("<")[0], p) for p in txt.split())
        check("every colliding trigger has an absorb form",
              all(int(p.split("=")[1]) > 1 for p in txt.split()), txt)
        check("int inf indep ;ph ;ps ;Ph ;Ps are the collisions",
              sorted(pairs) == sorted(["int", "inf", "indep", ";ph", ";ps", ";Ph", ";Ps"]), txt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build/native")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--only", type=int, action="append")
    args = ap.parse_args()
    home = tempfile.mkdtemp(prefix="mepl-", dir="/tmp")
    notes = os.path.join(home, "notes")
    os.makedirs(notes)
    path = os.path.join(notes, "lecture.org")
    with open(path, "w") as fh:
        fh.write("\n")
    xv = Xvfb()
    m = None
    try:
        m = Mep(args.build_dir, home, xv.display, ["--no-session", path], cwd=notes)
        run_checks(Note(m), set(args.only) if args.only else None)
    finally:
        if m:
            m.kill()
        xv.close()
        if not args.keep:
            shutil.rmtree(home, ignore_errors=True)
        else:
            print("kept", home)
    failed = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed" % (len(RESULTS), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
