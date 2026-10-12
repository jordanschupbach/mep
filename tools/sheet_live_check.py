#!/usr/bin/env python3
"""Drive a real mep over its agent socket and check the spreadsheet pane.

The windowless harness (mep-sheet-view-test) covers the model: the display
order, the sort and filter predicates, the geometry arithmetic, the stats,
the apply-to-data rewrite. None of it covers what the user actually does --
press Alt-Right and watch a column widen, click a cell, drag a column edge,
open the header menu -- because that needs a real window, real keystrokes,
a real font to measure for autofit, and the renderer's own slot walk.

So this spawns a throwaway mep on its own Xvfb display with a private HOME,
opens a CSV, and asserts on what comes back through mep.sheet_info().

The plumbing (Rpc, Xvfb, Mep, focus_largest_window) is imported from
tools/fold_live_check.py rather than copied: it is the same socket, the
same framing, the same "the frame loop idles so poke it" problem, and a
third copy would be a third thing to keep in step. Everything that file
documents at its own head applies here -- the socket path must stay under
108 bytes, replies are matched on their id, Xvfb has no window manager so
input focus is set by hand, and each instance gets a fresh display number.

CSV only: xlsx and ods open in the same grid but are deliberately out of
scope for this phase.

Usage: tools/sheet_live_check.py [--build-dir build/native] [--keep] [--only NAME]
"""

import argparse
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fold_live_check as F  # noqa: E402  (path has to be set first)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print("   %-62s %s%s" % (name, "PASS" if ok else "FAIL", ("\n        " + detail) if detail and not ok else ""))


# Mixed on purpose: repeated sort keys, mixed case, a text value and a
# blank in a numeric column, and a column worth filtering on.
FIXTURE = """region,rep,units,revenue,status
West,Alice,120,15400.50,closed
East,bob,45,5120.00,open
West,Carol,330,41250.75,closed
North,dave,,0,stalled
South,Erin,78,9870.25,open
East,Frank,210,26500.00,closed
North,Grace,15,1800.00,open
West,alice,95,12100.40,open
South,Heidi,410,52300.90,closed
East,Ivan,,0,stalled
North,Judy,180,22900.10,closed
South,Ken,62,7640.00,open
"""


class Sheet:
    """A mep instance opened on a CSV, with the sheet-specific inputs."""

    def __init__(self, build_dir, xv, home, text=FIXTURE, name="demo.csv"):
        self.home = home
        self.proj = os.path.join(home, "proj")
        os.makedirs(self.proj, exist_ok=True)
        self.path = os.path.join(self.proj, name)
        with open(self.path, "w") as fh:
            fh.write(text)
        self.m = F.Mep(build_dir, home, xv.display, [self.path], cwd=self.proj)
        self.m.pump(12)

    # -- input ----------------------------------------------------------
    def key(self, name, times=1):
        self.m.key(name, times)

    def mod_key(self, mod, name, times=1):
        """A chord. The modifier has to be genuinely held: ui.key_press of
        a shifted keysym on its own does not deliver one."""
        for _ in range(times):
            self.m.rpc.call("ui.key_down", {"key": mod})
            self.m.rpc.call("ui.key_press", {"key": name})
            self.m.rpc.call("ui.key_up", {"key": mod})
            self.m.pump(5)

    def plus(self, times=1):
        # '+' is Shift+= on the layout Xvfb hands us.
        self.mod_key("Shift_L", "equal", times)

    def cmd(self, c):
        return self.m.cmd(c)

    def click(self, x, y):
        self.m.rpc.call("ui.mouse_click", {"x": int(x), "y": int(y)})
        self.m.pump(10)

    def drag(self, x0, y0, x1, y1):
        self.m.rpc.call("ui.mouse_drag", {"x1": int(x0), "y1": int(y0), "x2": int(x1), "y2": int(y1)})
        self.m.pump(12)

    def shift_key(self, name, times=1):
        """A capital. ui.key_press of the shifted keysym alone does not
        deliver one -- Shift has to be genuinely held."""
        self.mod_key("Shift_L", name, times)

    # -- state ----------------------------------------------------------
    def info(self, *fields):
        """mep.sheet_info(), read back as a `k=v;` string -- the same
        serialize-and-read-a-file idiom fold_live_check.folds() uses,
        since no RPC returns a Lua value."""
        expr = ("(function() local i = mep.sheet_info() local s = '' "
                "for _, k in ipairs({%s}) do s = s .. k .. '=' .. tostring(i[k]) .. ';' end return s end)()"
                % ",".join("'%s'" % f for f in fields))
        txt = self.m.lua_value(expr)
        if txt is None or txt.startswith("ERR"):
            raise RuntimeError("mep.sheet_info() unavailable: %r" % txt)
        out = {}
        for part in txt.split(";"):
            if part:
                k, v = part.split("=", 1)
                out[k] = v
        return out

    def num(self, field):
        return float(self.info(field)[field])

    def display_rows(self, count=8):
        txt = self.m.lua_value(
            "(function() local i = mep.sheet_info() local s = '' "
            "for n = 1, math.min(%d, #i.display_rows) do s = s .. i.display_rows[n] .. ',' end return s end)()" % count)
        return [int(p) for p in (txt or "").split(",") if p]

    def cell(self, row, col):
        return self.m.lua_value("mep.sheet_cell(%d, %d)" % (row, col))

    def file_bytes(self):
        with open(self.path, "rb") as fh:
            return fh.read()

    def quit(self):
        self.m.quit()


def scenario_zoom(build_dir, xv, keep):
    print("== zoom: +/-/= and Ctrl+wheel scale the grid")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        check("opens at 100%", abs(sh.num("zoom") - 1.0) < 0.001, str(sh.num("zoom")))
        sh.plus(3)
        zoomed = sh.num("zoom")
        check("'+' three times zooms in", zoomed > 1.3, str(zoomed))
        sh.key("minus", 2)
        check("'-' zooms back out", sh.num("zoom") < zoomed, str(sh.num("zoom")))
        sh.key("equal")
        check("'=' resets to 100%", abs(sh.num("zoom") - 1.0) < 0.001, str(sh.num("zoom")))
        sh.cmd("MepSheetZoom 250")
        check(":MepSheetZoom 250 sets 2.5x", abs(sh.num("zoom") - 2.5) < 0.01, str(sh.num("zoom")))
        # Clamped, never applied raw.
        sh.cmd("MepSheetZoom 9000")
        check("an absurd zoom is clamped", sh.num("zoom") <= 4.0, str(sh.num("zoom")))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_sizing(build_dir, xv, keep):
    print("== sizing: Alt+arrows resize the cursor's column and row")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        before = sh.info("col_width", "row_height", "cursor_col", "cursor_row")
        sh.mod_key("Alt_L", "Right", 5)
        after = sh.info("col_width", "cursor_col")
        check("Alt+Right widens the column by five steps",
              int(after["col_width"]) == int(before["col_width"]) + 50,
              "%s -> %s" % (before["col_width"], after["col_width"]))
        # The regression this whole binding is most likely to cause: the
        # bare-arrow motion firing as well.
        check("Alt+Right does NOT move the cursor", after["cursor_col"] == before["cursor_col"],
              "%s -> %s" % (before["cursor_col"], after["cursor_col"]))
        sh.mod_key("Alt_L", "Left", 2)
        check("Alt+Left narrows it again", int(sh.info("col_width")["col_width"]) == int(before["col_width"]) + 30,
              sh.info("col_width")["col_width"])
        sh.mod_key("Alt_L", "Down", 4)
        rh = sh.info("row_height", "cursor_row")
        check("Alt+Down makes the row taller", int(rh["row_height"]) > int(before["row_height"]),
              "%s -> %s" % (before["row_height"], rh["row_height"]))
        check("Alt+Down does NOT move the cursor", rh["cursor_row"] == before["cursor_row"],
              "%s -> %s" % (before["cursor_row"], rh["cursor_row"]))
        # A width below the minimum would make the column impossible to
        # grab again.
        sh.cmd("MepSheetWidth A 1")
        check("a width is clamped to a grabbable minimum", int(sh.info("col_width")["col_width"]) >= 24,
              sh.info("col_width")["col_width"])
        sh.cmd("MepSheetWidth A 200")
        check(":MepSheetWidth sets an exact width", int(sh.info("col_width")["col_width"]) == 200,
              sh.info("col_width")["col_width"])
        sh.cmd("MepSheetWidth A +30")
        check(":MepSheetWidth +30 adjusts", int(sh.info("col_width")["col_width"]) == 230,
              sh.info("col_width")["col_width"])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_autofit(build_dir, xv, keep):
    print("== autofit: a column sizes itself to its widest value")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    text = "short,long\na,%s\nb,x\n" % ("w" * 60)
    sh = Sheet(build_dir, xv, home, text=text)
    try:
        sh.cmd("MepSheetAutofit B")
        sh.m.pump(12)
        wide = int(sh.m.lua_value("mep.sheet_col_width(2)"))
        narrow_before = int(sh.m.lua_value("mep.sheet_col_width(1)"))
        check("the long column grew past the default", wide > 120, str(wide))
        sh.cmd("MepSheetAutofit A")
        sh.m.pump(12)
        narrow = int(sh.m.lua_value("mep.sheet_col_width(1)"))
        check("the short column shrank", narrow < narrow_before, "%d -> %d" % (narrow_before, narrow))
        check("but stayed grabbable", narrow >= 24, str(narrow))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_sort(build_dir, xv, keep):
    print("== sort: the view reorders, the file does not")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        sh.cmd("w")
        baseline = sh.file_bytes()
        sh.cmd("MepSheetFreeze 1 0")
        sh.cmd("MepSheetSort C desc num")
        order = sh.display_rows(5)
        # units descending: 410 (row 10), 330 (row 4), 210 (row 7), 180
        # (row 12) -- after the frozen header row 1.
        check("sorted descending by units", order == [1, 10, 4, 7, 12], str(order))
        check("the header row stayed first", order[0] == 1, str(order))
        check("the row numbers are still the sheet's", sh.cell(10, 2) == "Heidi", sh.cell(10, 2))
        sh.cmd("w")
        check("a sorted :w is byte-identical to an unsorted one", sh.file_bytes() == baseline,
              "the sort leaked into the file")
        sh.cmd("MepSheetSortClear")
        check("clearing the sort restores the file's order", sh.display_rows(4) == [1, 2, 3, 4],
              str(sh.display_rows(4)))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_apply(build_dir, xv, keep):
    print("== apply: :MepSheetApply turns the view into the file")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        sh.cmd("MepSheetFreeze 1 0")
        sh.cmd("MepSheetSort C desc num")
        sh.cmd("MepSheetApply")
        sh.cmd("w")
        lines = sh.file_bytes().decode().splitlines()
        check("the header is still the header", lines[0].startswith("region,"), lines[0])
        check("the file itself is now sorted", lines[1].startswith("South,Heidi,410"), lines[1])
        check("and the next row follows it", lines[2].startswith("West,Carol,330"), lines[2])
        sh.key("u")
        sh.cmd("w")
        back = sh.file_bytes().decode().splitlines()
        check("`u` undoes the apply", back[1].startswith("West,Alice,120"), back[1])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_filter(build_dir, xv, keep):
    print("== filter: rows hide, motions skip them, the file is untouched")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        sh.cmd("w")
        baseline = sh.file_bytes()
        sh.cmd("MepSheetFreeze 1 0")
        sh.cmd("MepSheetFilter A = west")
        info = sh.info("visible_rows", "total_rows", "filters")
        check("three West rows plus the frozen header are visible", info["visible_rows"] == "4",
              str(info))
        check("the total is still the whole file", info["total_rows"] == "13", str(info))
        check("the header row survives a filter it does not match", sh.display_rows(1) == [1],
              str(sh.display_rows(1)))
        # Motions must step over the hidden rows, not onto them.
        sh.cmd("MepSheetFind Carol")
        after_find = sh.info("cursor_row")["cursor_row"]
        check("find lands on a visible row", after_find == "4", after_find)
        sh.key("j")
        check("`j` skips the hidden rows", sh.info("cursor_row")["cursor_row"] == "9",
              sh.info("cursor_row")["cursor_row"])
        sh.shift_key("g")
        check("`G` goes to the last VISIBLE row", sh.info("cursor_row")["cursor_row"] == "9",
              sh.info("cursor_row")["cursor_row"])
        sh.cmd("w")
        check("a filtered :w is byte-identical", sh.file_bytes() == baseline, "the filter leaked into the file")
        sh.cmd("MepSheetFilterClear")
        check("clearing restores every row", sh.info("visible_rows")["visible_rows"] == "13",
              sh.info("visible_rows")["visible_rows"])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_cursor_resnap(build_dir, xv, keep):
    print("== the cursor never parks on a row the filter is hiding")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        # Put the cursor on an East row, then hide every East row.
        sh.cmd("MepSheetFind bob")
        check("the cursor is on the East row", sh.info("cursor_row")["cursor_row"] == "3",
              sh.info("cursor_row")["cursor_row"])
        sh.cmd("MepSheetFilter A = west")
        row = sh.info("cursor_row")["cursor_row"]
        check("the cursor moved to a visible row", row in ("2", "4", "9"), row)
        # And editing there edits the row you can see, not the hidden one.
        sh.key("i")
        sh.m.rpc.call("ui.type_text", {"text": "CHECKED"})
        sh.m.pump(25)
        sh.key("Escape")
        sh.cmd("MepSheetFilterClear")
        sh.cmd("w")
        body = sh.file_bytes().decode()
        check("the edit landed on a visible row", "CHECKED" in body, body[:120])
        check("and not on the hidden East row", "CHECKED,bob" not in body, body[:200])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_freeze(build_dir, xv, keep):
    print("== freeze: the header stays put while the body scrolls")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    rows = ["h1,h2"] + ["r%d,v%d" % (i, i) for i in range(1, 200)]
    sh = Sheet(build_dir, xv, home, text="\n".join(rows) + "\n")
    try:
        sh.cmd("MepSheetFreeze 1 1")
        info = sh.info("freeze_rows", "freeze_cols")
        check("one row and one column are frozen", info == {"freeze_rows": "1", "freeze_cols": "1"}, str(info))
        sh.shift_key("g")
        scrolled = sh.info("scroll_display", "cursor_row")
        check("G scrolled the body down", int(scrolled["scroll_display"]) > 1, str(scrolled))
        # The scroll origin must never move into the frozen band, or the
        # pinned rows would be drawn twice.
        check("the scroll origin stays below the frozen band", int(scrolled["scroll_display"]) >= 2,
              str(scrolled))
        sh.cmd("MepSheetUnfreeze")
        check("unfreezing clears both", sh.info("freeze_rows", "freeze_cols") ==
              {"freeze_rows": "0", "freeze_cols": "0"}, str(sh.info("freeze_rows", "freeze_cols")))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_stats(build_dir, xv, keep):
    print("== stats: the numbers describe what is on screen")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        sh.cmd("MepSheetFreeze 1 0")
        got = sh.m.lua_value("(function() local s = mep.sheet_stats(3) "
                             "return s.count .. ',' .. s.nonempty .. ',' .. s.numeric .. ',' .. s.sum .. "
                             "',' .. s.min .. ',' .. s.max end)()")
        # units: 12 rows, 10 non-empty, all numeric, 1545 total, 15..410
        check("units totals correctly", got == "12,10,10,1545.0,15.0,410.0", str(got))
        sh.cmd("MepSheetFilter A = west")
        filtered = sh.m.lua_value("(function() local s = mep.sheet_stats(3) return s.count .. ',' .. s.sum end)()")
        check("a filtered column reports the filtered numbers", filtered == "3,545.0", str(filtered))
        # A text column must not invent arithmetic for itself.
        sh.cmd("MepSheetFilterClear")
        text_col = sh.m.lua_value("(function() local s = mep.sheet_stats(1) "
                                  "return s.numeric .. ',' .. s.sum .. ',' .. s.distinct end)()")
        check("a text column reports counts, not sums", text_col == "0,0.0,4", str(text_col))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_find(build_dir, xv, keep):
    print("== find: / and n walk the grid")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        sh.cmd("MepSheetFind closed")
        first = sh.info("cursor_row", "cursor_col")
        check("the first match is E2", first == {"cursor_row": "2", "cursor_col": "5"}, str(first))
        sh.key("n")
        check("`n` goes to the next one", sh.info("cursor_row")["cursor_row"] == "4",
              sh.info("cursor_row")["cursor_row"])
        sh.shift_key("n")
        check("`N` goes back", sh.info("cursor_row")["cursor_row"] == "2",
              sh.info("cursor_row")["cursor_row"])
        # Case folding follows :set ignorecase, the same setting buffer
        # search obeys -- not a rule of its own.
        check("case-sensitive by default", sh.m.lua_value("tostring(mep.sheet_find('HEIDI', true))") == "false")
        sh.cmd("set ignorecase")
        check("and case-insensitive with :set ignorecase",
              sh.m.lua_value("tostring(mep.sheet_find('HEIDI', true))") == "true")
        sh.cmd("set noignorecase")
        missing = sh.m.lua_value("tostring(mep.sheet_find('no such value', true))")
        check("a miss reports a miss", missing == "false", str(missing))
        # The `/` key itself, through the prompt it opens -- the route a
        # user actually takes, and the one that would break if the key
        # stopped reaching the handler.
        sh.cmd("MepSheetFind region")  # park the cursor somewhere known
        sh.key("slash")
        sh.m.pump(8)
        sh.m.rpc.call("ui.type_text", {"text": "Erin"})
        sh.m.pump(30)
        sh.key("Return")
        sh.m.pump(12)
        at = sh.info("cursor_row", "cursor_col")
        check("`/` opens the prompt and jumps to the match",
              at == {"cursor_row": "6", "cursor_col": "2"}, str(at))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_mouse(build_dir, xv, keep):
    print("== mouse: click a cell, drag a column edge, open the header menu")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        # A body click puts the cursor on the cell under the pointer.
        sh.click(300, 270)
        clicked = sh.info("cursor_row", "cursor_col")
        check("a body click moves the cursor", clicked["cursor_row"] != "1" and clicked["cursor_col"] != "1",
              str(clicked))
        # Clicking a column letter opens that column's menu.
        sh.click(200, 130)
        check("a header click opens the column menu", sh.info("menu_col")["menu_col"] != "0",
              sh.info("menu_col")["menu_col"])
        # Clicking in the body closes it again.
        sh.click(300, 300)
        check("clicking away closes it", sh.info("menu_col")["menu_col"] == "0",
              sh.info("menu_col")["menu_col"])
        # Dragging a column-header boundary resizes that column. The edge
        # is computed from the grid rectangle mep reports, not guessed at:
        # a drag four pixels off lands on nothing and the test would be
        # checking its own arithmetic instead of the feature.
        sh.cmd("MepSheetWidth A 100")
        geom = sh.info("grid_x", "grid_y", "header_w", "col_header_h")
        edge_x = int(geom["grid_x"]) + int(geom["header_w"]) + 100
        header_y = int(geom["grid_y"]) + int(geom["col_header_h"]) // 2
        width_before = int(sh.m.lua_value("mep.sheet_col_width(1)"))
        sh.drag(edge_x, header_y, edge_x + 80, header_y)
        width_after = int(sh.m.lua_value("mep.sheet_col_width(1)"))
        check("dragging the header edge widens the column", width_after > width_before + 60,
              "%d -> %d (edge at x=%d, y=%d)" % (width_before, width_after, edge_x, header_y))
        sh.drag(edge_x + 80, header_y, edge_x + 30, header_y)
        check("and dragging it back narrows it", int(sh.m.lua_value("mep.sheet_col_width(1)")) < width_after,
              sh.m.lua_value("mep.sheet_col_width(1)"))
        # A double-click on the same edge fits the column to its content.
        sh.click(edge_x + 30, header_y)
        sh.click(edge_x + 30, header_y)
        sh.m.pump(12)
        check("a double-click on the edge autofits", int(sh.m.lua_value("mep.sheet_col_width(1)")) < 120,
              sh.m.lua_value("mep.sheet_col_width(1)"))
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_menu_filter(build_dir, xv, keep):
    print("== the header menu's value checklist filters the column")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        # The menu is a second route to the same operations, so driving it
        # from Lua is enough to prove the plumbing: what the mouse adds
        # (that the rows are clickable) scenario_mouse covers.
        sh.m.lua_value("tostring(mep.sheet_filter(1, 'oneof', {'West', 'South'}))")
        sh.m.pump(8)
        info = sh.info("visible_rows", "filters")
        check("an oneof filter keeps just those values", info["visible_rows"] == "6", str(info))
        sh.m.lua_value("tostring(mep.sheet_filter_clear())")
        sh.m.pump(6)
        check("clearing restores them", sh.info("visible_rows")["visible_rows"] == "13",
              sh.info("visible_rows")["visible_rows"])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


def scenario_visual_range(build_dir, xv, keep):
    print("== a Visual range over a filtered view covers only what is visible")
    home = tempfile.mkdtemp(prefix="/tmp/meps-")
    sh = Sheet(build_dir, xv, home)
    try:
        # Drag-select first: a body drag has to become a Visual range.
        geom = sh.info("grid_x", "grid_y", "header_w", "col_header_h")
        x0 = int(geom["grid_x"]) + int(geom["header_w"]) + 20
        y0 = int(geom["grid_y"]) + int(geom["col_header_h"]) + 30
        sh.drag(x0, y0, x0 + 100, y0 + 70)
        mode = sh.m.lua_value("tostring(mep.mode and mep.mode() or 'n/a')")
        cur = sh.info("cursor_row", "cursor_col")
        check("a body drag extended the cursor across cells",
              int(cur["cursor_row"]) > 1 and int(cur["cursor_col"]) > 1, "%s (mode %s)" % (cur, mode))
        sh.key("Escape")

        # The data-safety one. Filter to the West rows, select from the
        # first visible row to the last, and clear it: the East/North/South
        # rows that merely lie BETWEEN them by row number are not part of
        # what the user selected and must survive.
        sh.cmd("MepSheetFreeze 1 0")
        sh.cmd("MepSheetFilter A = west")
        sh.cmd("MepSheetFind Alice")   # row 2, the first visible data row
        sh.key("v")
        sh.shift_key("g")              # extend to the last visible row (9)
        sh.key("x")
        sh.cmd("MepSheetFilterClear")
        sh.cmd("w")
        body = sh.file_bytes().decode()
        check("the hidden East row was not cleared", "East,bob,45" in body, body[:200])
        check("the hidden North row was not cleared", "North,dave" in body, body[:200])
        check("the hidden South row was not cleared", "South,Erin,78" in body, body[:200])
        check("the visible West rows were cleared", "West,Alice,120" not in body and "West,Carol,330" not in body,
              body[:200])
    finally:
        sh.quit()
        if not keep:
            shutil.rmtree(home, ignore_errors=True)


SCENARIOS = {
    "visual": scenario_visual_range,
    "zoom": scenario_zoom,
    "sizing": scenario_sizing,
    "autofit": scenario_autofit,
    "sort": scenario_sort,
    "apply": scenario_apply,
    "filter": scenario_filter,
    "resnap": scenario_cursor_resnap,
    "freeze": scenario_freeze,
    "stats": scenario_stats,
    "find": scenario_find,
    "mouse": scenario_mouse,
    "menu": scenario_menu_filter,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--only", action="append", default=None, choices=sorted(SCENARIOS))
    args = ap.parse_args()
    if not os.path.exists(os.path.join(args.build_dir, "mep")):
        sys.exit("no mep binary in %s" % args.build_dir)
    xv = F.Xvfb()
    try:
        for name in (args.only or sorted(SCENARIOS)):
            try:
                SCENARIOS[name](args.build_dir, xv, args.keep)
            except Exception as exc:  # a harness failure is not a pass
                check("scenario %s ran" % name, False, "%s: %s" % (type(exc).__name__, exc))
    finally:
        xv.close()
    bad = [r for r in RESULTS if not r[1]]
    print("\n---- %d checks, %d failures ----" % (len(RESULTS), len(bad)))
    for name, _, detail in bad:
        print("  FAIL %s\n       %s" % (name, detail))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
