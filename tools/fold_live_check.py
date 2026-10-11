#!/usr/bin/env python3
"""Drive a real mep over its agent socket and check folding end to end.

The windowless harnesses (mep-fold-test, mep-editor-fold-test) cover the
fold *list*: the invariants, the shifter, the save/restore round trip
against an Editor built in-process. None of them covers what the user
actually reports -- "I open a Python file that has folds in it and
everything is offset" -- because that involves a real process, a real
session file written by one run and read by the next, and the renderer's
own row walk.

So this spawns a throwaway mep on its own Xvfb display with a private
HOME (so the session file is this run's, never the user's), folds a
Python file by keystroke, quits, relaunches the *same* home against the
*same* file, and asserts on what comes back -- fold ranges, the text
under them, the cursor, and the screen.

The gotchas it encodes (each fails silently rather than loudly) are the
same ones tools/pdf_live_check.py documents at its own head: the socket
path must stay under 108 bytes, replies must be matched on their id, the
frame loop idles so synthetic input needs the loop poked, Xvfb has no
window manager so input focus must be set by hand, and a fresh display
number per instance.

Usage: tools/fold_live_check.py [--build-dir build/native] [--keep] [--only NAME]
"""

import argparse
import ctypes
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --------------------------------------------------------------- plumbing


class Rpc:
    """One connection, with ids matched and notifications queued."""

    def __init__(self, path, timeout=20.0):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""
        self.next_id = 1
        self.events = []

    def _read_message(self):
        while True:
            head_end = self.buf.find(b"\r\n\r\n")
            if head_end >= 0:
                head = self.buf[:head_end].decode(errors="replace")
                length = None
                for line in head.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        length = int(line.split(":", 1)[1].strip())
                if length is not None and len(self.buf) >= head_end + 4 + length:
                    body = self.buf[head_end + 4 : head_end + 4 + length]
                    self.buf = self.buf[head_end + 4 + length :]
                    return json.loads(body.decode(errors="replace"))
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("agent socket closed")
            self.buf += chunk

    def call(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}).encode()
        self.s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        while True:
            msg = self._read_message()
            if msg.get("id") != rid:  # a notification, not this call's reply
                self.events.append(msg)
                continue
            if "error" in msg:
                raise RuntimeError("%s failed: %s" % (method, msg["error"]))
            return msg.get("result")

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def focus_largest_window(display, build_dir):
    """Give mep's window X input focus: Xvfb runs no window manager, so
    without this XTest keystrokes go nowhere. find_library returns a
    soname that cannot be dlopen'd under Nix, so the absolute store path
    comes from the binary's own ldd output."""
    lib = None
    try:
        out = subprocess.run(["ldd", os.path.join(build_dir, "mep")], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "libX11.so" in line and "=>" in line:
                lib = line.split("=>")[1].strip().split(" ")[0]
                break
    except OSError:
        pass
    if not lib or not os.path.exists(lib):
        return False
    x11 = ctypes.CDLL(lib)
    x11.XOpenDisplay.restype = ctypes.c_void_p
    dpy = x11.XOpenDisplay(display.encode())
    if not dpy:
        return False
    root = ctypes.c_ulong()
    parent = ctypes.c_ulong()
    children = ctypes.POINTER(ctypes.c_ulong)()
    n = ctypes.c_uint()
    scr_root = x11.XDefaultRootWindow(ctypes.c_void_p(dpy))
    x11.XQueryTree(ctypes.c_void_p(dpy), ctypes.c_ulong(scr_root), ctypes.byref(root), ctypes.byref(parent),
                   ctypes.byref(children), ctypes.byref(n))

    class XWA(ctypes.Structure):
        _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                    ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
                    ("root", ctypes.c_ulong), ("class_", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                    ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
                    ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                    ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                    ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
                    ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
                    ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]

    best, best_area = None, -1
    for i in range(n.value):
        w = children[i]
        a = XWA()
        if not x11.XGetWindowAttributes(ctypes.c_void_p(dpy), ctypes.c_ulong(w), ctypes.byref(a)):
            continue
        if a.map_state != 2:  # IsViewable
            continue
        area = a.width * a.height
        if area > best_area:
            best, best_area = w, area
    if best is None:
        return False
    x11.XSetInputFocus(ctypes.c_void_p(dpy), ctypes.c_ulong(best), 1, 0)
    x11.XFlush(ctypes.c_void_p(dpy))
    return True


class Xvfb:
    """One display, shared by however many mep runs a scenario needs."""

    def __init__(self):
        self.display = ":%d" % random.randint(300, 900)
        self.proc = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1600x1200x24"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


class Mep:
    """One mep process against a given HOME, so a second run of the same
    home reads the session file the first one wrote."""

    def __init__(self, build_dir, home, display, args, cwd=None):
        self.build_dir = build_dir
        self.home = home
        self.display = display
        self.logpath = os.path.join(home, "mep-%d.log" % int(time.time() * 1000 % 1e9))
        self.logf = open(self.logpath, "wb")
        env = dict(os.environ, DISPLAY=display, HOME=home,
                   XDG_DATA_HOME=os.path.join(home, "share"),
                   XDG_CONFIG_HOME=os.path.join(home, "config"))
        self.proc = subprocess.Popen([os.path.join(build_dir, "mep")] + list(args), env=env,
                                     stdout=self.logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     cwd=cwd or REPO)
        sock_dir = os.path.join(home, "share", "mep", "agent-sockets")
        self.sock_path = os.path.join(sock_dir, "%d.sock" % self.proc.pid)
        for _ in range(200):
            if os.path.exists(self.sock_path):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("mep exited early; log:\n" + self.log_tail())
            time.sleep(0.1)
        else:
            raise RuntimeError("agent socket never appeared (%s); log:\n%s" % (self.sock_path, self.log_tail()))
        self.rpc = Rpc(self.sock_path)
        for _ in range(50):
            try:
                info = self.rpc.call("session.info")
            except (OSError, RuntimeError):
                time.sleep(0.2)
                continue
            if info and info.get("pid") == self.proc.pid:
                break
            time.sleep(0.2)
        focus_largest_window(display, build_dir)
        self.pump(12)

    def log_tail(self, n=3000):
        try:
            with open(self.logpath, "rb") as fh:
                return fh.read()[-n:].decode(errors="replace")
        except OSError:
            return "(no log)"

    def pump(self, n=6):
        for _ in range(n):
            try:
                self.rpc.call("session.info")
            except RuntimeError:
                pass
            time.sleep(0.03)

    def cmd(self, c):
        r = self.rpc.call("command.run", {"cmd": c})
        self.pump(5)
        return r

    def key(self, name, times=1):
        for _ in range(times):
            self.rpc.call("ui.key_press", {"key": name})
            self.pump(4)

    def shift_key(self, name):
        self.rpc.call("ui.key_down", {"key": "Shift_L"})
        self.rpc.call("ui.key_press", {"key": name})
        self.rpc.call("ui.key_up", {"key": "Shift_L"})
        self.pump(6)

    def lua_value(self, expr):
        """Evaluate a Lua expression, read it back through a file (there
        is no RPC that returns a Lua value)."""
        out = os.path.join(self.home, "out.txt")
        if os.path.exists(out):
            os.remove(out)
        chunk = ("local ok, v = pcall(function() return %s end) "
                 "local f = io.open(%s, 'w') f:write(tostring(ok and v or ('ERR: ' .. tostring(v)))) f:close()"
                 % (expr, json.dumps(out)))
        self.cmd("lua " + chunk)
        for _ in range(25):
            if os.path.exists(out):
                with open(out) as fh:
                    return fh.read()
            self.pump(3)
        return None

    def folds(self):
        """The live fold list, rows 1-indexed (mep.fold_list)."""
        txt = self.lua_value(
            "(function() local s = '' for _, f in ipairs(mep.fold_list()) do "
            "s = s .. f.start_row .. ',' .. f.end_row .. ',' .. tostring(f.closed) .. ',' .. f.provider .. ';' "
            "end return s end)()")
        if txt is None or txt.startswith("ERR"):
            raise RuntimeError("mep.fold_list() unavailable: %r" % txt)
        out = []
        for part in txt.split(";"):
            if not part:
                continue
            a, b, c, p = part.split(",")
            out.append({"start": int(a), "end": int(b), "closed": c == "true", "provider": p})
        return out

    def line(self, row1):
        """One buffer line, by 1-indexed row."""
        return self.lua_value("mep.get_line(%d)" % row1)

    def cursor_row(self):
        # mep.cursor() returns (row, col), both 1-indexed; pcall in
        # lua_value keeps only the first, which is the row. (There is no
        # mep.cursor_row.)
        return int(self.lua_value("mep.cursor()"))

    def screenshot(self, dest):
        r = self.rpc.call("ui.screenshot")
        src = r.get("path") if isinstance(r, dict) else None
        if src and os.path.exists(src):
            shutil.copy(src, dest)
            return dest
        return None

    def quit(self, timeout=20):
        """`:qa` -- the path that writes the session file on the way out."""
        try:
            self.rpc.call("command.run", {"cmd": "qa"})
        except (RuntimeError, OSError):
            pass
        for _ in range(int(timeout * 10)):
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        else:
            self.kill()
        try:
            self.rpc.close()
        except Exception:
            pass
        self.logf.close()

    def kill(self):
        try:
            self.rpc.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(5)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        try:
            self.logf.close()
        except Exception:
            pass


# ------------------------------------------------------------------ fixture

# Every line says which line it is, so an offset is visible in the text
# itself rather than having to be inferred.
def python_source():
    rows = []
    rows.append("import os  # L1")
    rows.append("import sys  # L2")
    rows.append("")
    rows.append("")
    rows.append("def alpha(x):  # L5")
    rows.append("    total = 0  # L6")
    rows.append("    for i in range(x):  # L7")
    rows.append("        total += i  # L8")
    rows.append("    return total  # L9")
    rows.append("")
    rows.append("")
    rows.append("def beta(y):  # L12")
    rows.append("    if y > 0:  # L13")
    rows.append("        return y  # L14")
    rows.append("    return -y  # L15")
    rows.append("")
    rows.append("")
    rows.append("class Gamma:  # L18")
    rows.append("    def one(self):  # L19")
    rows.append("        return 1  # L20")
    rows.append("")
    rows.append("    def two(self):  # L22")
    rows.append("        return 2  # L23")
    rows.append("")
    rows.append("")
    rows.append("def delta(z):  # L26")
    rows.append("    acc = []  # L27")
    rows.append("    for q in z:  # L28")
    rows.append("        acc.append(q)  # L29")
    rows.append("    return acc  # L30")
    rows.append("")
    rows.append("")
    rows.append("print(alpha(3))  # L33")
    rows.append("print(beta(4))  # L34")
    rows.append("print(delta([1]))  # L35")
    return "\n".join(rows) + "\n"


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok), detail))
    print("   %-62s %s%s" % (name, "PASS" if ok else "FAIL", ("\n        " + detail) if detail and not ok else ""))


def fold_str(folds):
    return " ".join("[%d..%d %s %s]" % (f["start"], f["end"], "closed" if f["closed"] else "open", f["provider"])
                    for f in folds) or "(none)"


def session_folds(home):
    """Every (buffer, ranges) the session file holds, across workspaces."""
    d = os.path.join(home, "share", "mep", "workspaces")
    out = []
    if not os.path.isdir(d):
        return out
    for name in sorted(os.listdir(d)):
        try:
            doc = json.load(open(os.path.join(d, name)))
        except Exception:
            continue
        for w in doc.get("workspaces", []):
            for e in w.get("folds", []):
                out.append((name, w.get("name"), e.get("buffer"), e.get("ranges")))
    return out


# ------------------------------------------------------------------ checks


def scenario_manual_roundtrip(build_dir, xv, keep):
    """The reported case: fold a Python file by hand, quit, reopen it.

    Two manual folds made with `zf` over a Visual selection -- a function
    body and a class -- then `:qa` (which writes the session file) and a
    fresh process on the same home and the same file. What comes back must
    be the same two ranges over the same text, with no third fold and no
    drift."""
    print("== 1. zf folds survive quit+reopen on the same rows")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, ["--project", proj, path], cwd=proj)
    try:
        check("the file opened", (m.line(5) or "").startswith("def alpha"), repr(m.line(5)))
        # `def alpha` (rows 5..9) folded the way a user does it: cursor on
        # the def, V, four lines down, zf.
        m.cmd("5")
        m.shift_key("v")
        m.key("j", 4)
        m.key("z")
        m.key("f")
        # `class Gamma` (rows 18..23).
        m.cmd("18")
        m.shift_key("v")
        m.key("j", 5)
        m.key("z")
        m.key("f")
        made = m.folds()
        check("two folds were made", len(made) == 2, fold_str(made))
        check("they are 5..9 and 18..23",
              sorted((f["start"], f["end"]) for f in made) == [(5, 9), (18, 23)], fold_str(made))
        m.screenshot(os.path.join(home, "before.png"))
        # The debounced session save needs half a second of quiet; `:qa`
        # writes unconditionally anyway, but wait so both paths are real.
        time.sleep(1.0)
        m.pump(10)
        saved = session_folds(home)
        check("the session file holds the folds", any(r for _, _, _, r in saved), json.dumps(saved))
    finally:
        m.quit()

    m2 = Mep(build_dir, home, xv.display, ["--project", proj, path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        check("the same two folds came back",
              sorted((f["start"], f["end"]) for f in back) == [(5, 9), (18, 23)], fold_str(back))
        check("no extra folds appeared", len(back) == 2, fold_str(back))
        for f in back:
            head = m2.line(f["start"]) or ""
            check("fold starting at %d is still on its own text" % f["start"],
                  head.startswith("def alpha") or head.startswith("class Gamma"),
                  "row %d is %r" % (f["start"], head))
        # The file's own text must be untouched: an offset that is really
        # a shifted *buffer* would show up here.
        check("row 1 is still `import os`", (m2.line(1) or "").startswith("import os"), repr(m2.line(1)))
        check("row 35 is still the last print", "delta" in (m2.line(35) or ""), repr(m2.line(35)))
        m2.screenshot(os.path.join(home, "after.png"))
        # Open the first fold and check the text underneath is the text
        # that belongs to it.
        m2.cmd("5")
        m2.key("z")
        m2.key("o")
        check("opening the fold reveals its own body",
              (m2.line(6) or "").strip().startswith("total = 0"), repr(m2.line(6)))
        print("      screenshots: %s" % os.path.join(home, "{before,after}.png"))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_bare_open(build_dir, xv, keep):
    """The same thing without --project: `mep demo.py` in the project
    directory, which is how a file is usually opened."""
    print("== 2. `mep demo.py` (no --project) finds the folds it left")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        m.cmd("12")
        m.shift_key("v")
        m.key("j", 3)
        m.key("z")
        m.key("f")
        made = m.folds()
        check("the fold was made as 12..15", [(f["start"], f["end"]) for f in made] == [(12, 15)], fold_str(made))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        check("the fold came back as 12..15", [(f["start"], f["end"]) for f in back] == [(12, 15)], fold_str(back))
        check("row 12 is still `def beta`", (m2.line(12) or "").startswith("def beta"), repr(m2.line(12)))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_edit_then_reopen(build_dir, xv, keep):
    """Folds made, then the file edited above them and saved, then
    reopened. The ranges in the session file are rows; if the save
    records them before the edit shifted them (or the reopen applies them
    against the new text without checking), this is where the user's
    "everything is offset" comes from."""
    print("== 3. editing above a fold, saving and reopening keeps it on its text")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        m.cmd("18")
        m.shift_key("v")
        m.key("j", 5)
        m.key("z")
        m.key("f")
        check("fold made as 18..23", [(f["start"], f["end"]) for f in m.folds()] == [(18, 23)],
              fold_str(m.folds()))
        # Three lines inserted at the very top, then saved to disk.
        m.cmd("1")
        m.cmd("normal O# added one")
        m.cmd("1")
        m.cmd("normal O# added two")
        m.cmd("1")
        m.cmd("normal O# added three")
        m.cmd("w")
        after_edit = m.folds()
        check("the fold followed the insert to 21..26",
              [(f["start"], f["end"]) for f in after_edit] == [(21, 26)], fold_str(after_edit))
        check("row 21 is `class Gamma`", (m.line(21) or "").startswith("class Gamma"), repr(m.line(21)))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        check("the fold came back as 21..26", [(f["start"], f["end"]) for f in back] == [(21, 26)],
              fold_str(back))
        check("row 21 is `class Gamma` on reopen", (m2.line(21) or "").startswith("class Gamma"),
              repr(m2.line(21)))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_external_change(build_dir, xv, keep):
    """The folds are saved, and then the file changes *outside* mep (a
    git pull, another editor, a formatter) before it is reopened. Rows
    saved against the old text cannot be right for the new text; what
    must not happen is a fold left sitting on unrelated lines."""
    print("== 4. a file that changed on disk between sessions does not come back offset")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        m.cmd("18")
        m.shift_key("v")
        m.key("j", 5)
        m.key("z")
        m.key("f")
        check("fold made as 18..23", [(f["start"], f["end"]) for f in m.folds()] == [(18, 23)],
              fold_str(m.folds()))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    # Four lines added at the top by something else entirely.
    open(path, "w").write("# ext 1\n# ext 2\n# ext 3\n# ext 4\n" + python_source())

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        starts = [f["start"] for f in back]
        on_text = all((m2.line(s) or "").startswith(("def ", "class ")) for s in starts) if starts else True
        check("no fold is left on an unrelated line", on_text,
              fold_str(back) + "".join(" row %d=%r" % (s, m2.line(s)) for s in starts))
        # The four external lines pushed `class Gamma` from 18 to 22, and
        # the fold has to be there with it.
        check("the fold followed its text to 22..27",
              [(f["start"], f["end"]) for f in back] == [(22, 27)], fold_str(back))
        check("row 22 is `class Gamma`", (m2.line(22) or "").startswith("class Gamma"), repr(m2.line(22)))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_closed_fold_navigation(build_dir, xv, keep):
    """A reopened file whose folds are closed: j/k, :N and the screen all
    have to agree about which row is which. "Everything is offset" is
    what a disagreement here looks like."""
    print("== 5. with the restored folds closed, the cursor lands on the row it is told to")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        for start, down in ((5, 4), (18, 5), (26, 4)):
            m.cmd(str(start))
            m.shift_key("v")
            m.key("j", down)
            m.key("z")
            m.key("f")
        made = m.folds()
        check("three folds made", sorted((f["start"], f["end"]) for f in made) == [(5, 9), (18, 23), (26, 30)],
              fold_str(made))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        check("three folds came back", sorted((f["start"], f["end"]) for f in back) == [(5, 9), (18, 23), (26, 30)],
              fold_str(back))
        check("all three came back closed", all(f["closed"] for f in back), fold_str(back))
        # `:N` then read the cursor's own row back: the two must match,
        # and the line under it must be line N of the file.
        for want in (1, 5, 12, 18, 26, 33, 35):
            m2.cmd(str(want))
            got = m2.cursor_row()
            line = m2.line(got) or ""
            ok = got == want or "# L%d" % want in (m2.line(want) or "")
            check(":%d leaves the cursor on row %d" % (want, want), got == want,
                  "cursor is on %d (%r)" % (got, line))
        # j from a closed fold's summary row must land just past it.
        m2.cmd("5")
        m2.key("j")
        check("j from the summary row of 5..9 lands on row 10", m2.cursor_row() == 10,
              "landed on %d (%r)" % (m2.cursor_row(), m2.line(m2.cursor_row())))
        m2.key("k")
        check("k comes back to the summary row 5", m2.cursor_row() == 5,
              "landed on %d" % m2.cursor_row())
        m2.screenshot(os.path.join(home, "closed.png"))
        print("      screenshot: %s" % os.path.join(home, "closed.png"))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_treesitter_folds(build_dir, xv, keep):
    """:MepSyntaxFold builds every Python construct's fold from the
    Treesitter query, the user closes some, and the session has to bring
    exactly those back -- without the rebuild on the next run stacking a
    second copy of each."""
    print("== 6. :MepSyntaxFold folds close, persist and do not stack on reopen")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        m.cmd("MepSyntaxFold")
        built = m.folds()
        print("      built: %s" % fold_str(built))
        check("the provider built folds", len(built) >= 3, fold_str(built))
        check("they start on def/class rows",
              all((m.line(f["start"]) or "").lstrip().startswith(("def ", "class ", "for ", "if ", "import"))
                  for f in built), fold_str(built))
        # Close the one over `class Gamma`.
        m.cmd("18")
        m.key("z")
        m.key("c")
        closed = [f for f in m.folds() if f["closed"]]
        check("one fold is closed", len(closed) >= 1, fold_str(m.folds()))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        m2.cmd("MepSyntaxFold")  # as a config with syntax_fold_auto would
        back = m2.folds()
        print("      reopened+rebuilt: %s" % fold_str(back))
        ranges = [(f["start"], f["end"]) for f in back]
        check("no range is duplicated", len(ranges) == len(set(ranges)), fold_str(back))
        closed_back = [(f["start"], f["end"]) for f in back if f["closed"]]
        check("the closed fold came back closed", (18, 23) in closed_back or any(s == 18 for s, _ in closed_back),
              fold_str(back))
        m2.cmd("18")
        m2.key("z")
        m2.key("a")  # one za must open it
        after = [f for f in m2.folds() if f["start"] == 18]
        check("one za opens the restored fold", all(not f["closed"] for f in after), fold_str(m2.folds()))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_agent_rewrite(build_dir, xv, keep):
    """What actually happens on this machine: the file is edited on disk
    by something that is not mep -- an agent in a terminal pane, black,
    a git pull -- in several places at once, above and inside the folds.
    Every surviving fold has to come back on its own text; the one whose
    text was deleted has to come back not at all."""
    print("== 7. scattered on-disk edits: folds follow their text, or go")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        for start, down in ((5, 4), (18, 5), (26, 4)):
            m.cmd(str(start))
            m.shift_key("v")
            m.key("j", down)
            m.key("z")
            m.key("f")
        check("three folds made", sorted((f["start"], f["end"]) for f in m.folds()) ==
              [(5, 9), (18, 23), (26, 30)], fold_str(m.folds()))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    # The rewrite: two lines added at the top, `def beta` deleted
    # outright, one line added inside `class Gamma`, `def delta` left
    # alone but pushed around by all of it.
    lines = python_source().split("\n")
    body = [l for l in lines if l != ""] if False else lines
    out = []
    out.append("from typing import List  # new")
    out.append("")
    skip = False
    for ln in body:
        if ln.startswith("def beta"):
            skip = True
            continue
        if skip:
            if ln.startswith("def ") or ln.startswith("class "):
                skip = False
            else:
                continue
        out.append(ln)
        if ln.startswith("class Gamma"):
            out.append("    \"\"\"A docstring that was not here before.\"\"\"")
    open(path, "w").write("\n".join(out))

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        for f in back:
            head = m2.line(f["start"]) or ""
            tail = m2.line(f["end"]) or ""
            check("fold %d..%d sits on a def/class and its own last line" % (f["start"], f["end"]),
                  head.startswith(("def ", "class ")) and tail.strip().startswith("return"),
                  "head=%r tail=%r" % (head, tail))
        heads = sorted((m2.line(f["start"]) or "").split("(")[0] for f in back)
        check("alpha and delta came back, beta did not",
              heads == ["class Gamma:  # L18", "def alpha", "def delta"], str(heads))
        # The one whose body grew has to cover the extra line too.
        gamma = [f for f in back if (m2.line(f["start"]) or "").startswith("class Gamma")]
        check("the class fold grew with its new docstring line",
              len(gamma) == 1 and (m2.line(gamma[0]["end"]) or "").strip() == "return 2  # L23",
              fold_str(back))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_repeated_lines(build_dir, xv, keep):
    """A long file full of lines that repeat verbatim -- which real Python
    is: `    def __init__(self):`, `        return None`, twenty times
    over. Finding a fold by its text has to pick the occurrence the fold
    was actually on, not the first one in the file."""
    print("== 8. a long file of repeated lines: each fold keeps its own copy")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "many.py")
    # Twelve identical-looking classes; only the class line differs.
    blocks = []
    for i in range(12):
        blocks.append("class Thing%d:" % i)
        blocks.append("    def __init__(self):")
        blocks.append("        self.x = 0")
        blocks.append("        self.y = 0")
        blocks.append("        return None")
        blocks.append("")
    open(path, "w").write("\n".join(blocks) + "\n")
    # Rows are 1-indexed: class N starts at 6*N + 1, its __init__ at +2.
    init_of = lambda n: 6 * n + 2

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        # Fold the __init__ of the 3rd and 10th class -- both bodies are
        # byte-identical, so only the row tells them apart.
        for n in (3, 10):
            m.cmd(str(init_of(n)))
            m.shift_key("v")
            m.key("j", 3)
            m.key("z")
            m.key("f")
        made = sorted((f["start"], f["end"]) for f in m.folds())
        check("both folds made on the right copies",
              made == [(init_of(3), init_of(3) + 3), (init_of(10), init_of(10) + 3)], fold_str(m.folds()))
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    # Unchanged file first: the saved rows must simply come back.
    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = sorted((f["start"], f["end"]) for f in m2.folds())
        check("unchanged file: both folds come back on their own copies",
              back == [(init_of(3), init_of(3) + 3), (init_of(10), init_of(10) + 3)], fold_str(m2.folds()))
    finally:
        m2.quit()

    # Now five lines added at the top on disk: each fold must move by
    # exactly five, not collapse onto the first matching __init__.
    open(path, "w").write("# a\n# b\n# c\n# d\n# e\n" + "\n".join(blocks) + "\n")
    m3 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = sorted((f["start"], f["end"]) for f in m3.folds())
        want = [(init_of(3) + 5, init_of(3) + 8), (init_of(10) + 5, init_of(10) + 8)]
        check("after an insert above, each fold moved by five", back == want,
              "%s want %s" % (fold_str(m3.folds()), want))
        for f in m3.folds():
            check("fold %d..%d is on a `def __init__` line" % (f["start"], f["end"]),
                  (m3.line(f["start"]) or "").strip() == "def __init__(self):", repr(m3.line(f["start"])))
    finally:
        m3.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_agent_rpc_rewrite(build_dir, xv, keep):
    """An agent editing the open buffer the way MEP_AGENT_API.md tells it
    to -- `buffer.setLines` over the agent socket, not a disk write. That
    replaces the line vector under the folds, live, with no reload and no
    restart: if nothing tells the folds, every one of them is left on its
    old rows while the user watches."""
    print("== 9. an agent's buffer.setLines leaves the folds on their own text")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        for start, down in ((5, 4), (18, 5)):
            m.cmd(str(start))
            m.shift_key("v")
            m.key("j", down)
            m.key("z")
            m.key("f")
        check("two folds made", sorted((f["start"], f["end"]) for f in m.folds()) == [(5, 9), (18, 23)],
              fold_str(m.folds()))
        state = m.rpc.call("state.dump")
        buffers = state.get("buffers", []) if isinstance(state, dict) else []
        bid = None
        for b in buffers:
            if (b.get("filename") or "").endswith("demo.py"):
                bid = b.get("id")
        check("the buffer id was found", bid is not None, json.dumps(buffers)[:300])
        if bid is None:
            return
        # Two lines at the top and one inside the class: an ordinary agent edit.
        new_lines = ["from typing import List  # new", ""]
        for ln in python_source().split("\n")[:-1]:
            new_lines.append(ln)
            if ln.startswith("class Gamma"):
                new_lines.append('    """Docs."""')
        m.rpc.call("buffer.setLines", {"buffer_id": bid, "lines": new_lines})
        m.pump(8)
        back = m.folds()
        print("      after the rewrite: %s" % fold_str(back))
        check("def alpha's fold followed the insert to 7..11",
              any(f["start"] == 7 and f["end"] == 11 for f in back), fold_str(back))
        gamma = [f for f in back if (m.line(f["start"]) or "").startswith("class Gamma")]
        check("the class fold is still on `class Gamma`", len(gamma) == 1, fold_str(back))
        check("and still ends on its own last line",
              len(gamma) == 1 and (m.line(gamma[0]["end"]) or "").strip() == "return 2  # L23",
              fold_str(back) + " end=%r" % (m.line(gamma[0]["end"]) if gamma else None))
    finally:
        m.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_format_whole_file(build_dir, xv, keep):
    """`:Format` (and every Lua command built on mep.replace_lines)
    rewrites the whole file as one replacement covering every fold in it.
    The row arithmetic had no answer for that, so the fold set was simply
    emptied -- "I have to make all my folds again", from the other end."""
    print("== 10. a whole-file rewrite through mep.replace_lines keeps the folds")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        for start, down in ((5, 4), (18, 5)):
            m.cmd(str(start))
            m.shift_key("v")
            m.key("j", down)
            m.key("z")
            m.key("f")
        check("two folds made", sorted((f["start"], f["end"]) for f in m.folds()) == [(5, 9), (18, 23)],
              fold_str(m.folds()))
        # Every line read back and written straight out again, bar one
        # blank line dropped above `def beta` -- a formatter's shape.
        m.cmd("lua local out = {} for i = 1, mep.line_count() do if i ~= 11 then "
              "out[#out + 1] = mep.get_line(i) end end mep.replace_lines(1, mep.line_count() + 1, out)")
        m.pump(8)
        back = m.folds()
        print("      after the rewrite: %s" % fold_str(back))
        check("both folds survived the whole-file replace", len(back) == 2, fold_str(back))
        check("def alpha's fold stayed at 5..9", any(f["start"] == 5 and f["end"] == 9 for f in back),
              fold_str(back))
        check("class Gamma's came up a row, to 17..22",
              any(f["start"] == 17 and f["end"] == 22 for f in back), fold_str(back))
        check("row 17 really is `class Gamma`", (m.line(17) or "").startswith("class Gamma"),
              repr(m.line(17)))
    finally:
        m.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


def scenario_nested(build_dir, xv, keep):
    """Folds inside folds, which is what a folded Python file is: the
    class, and a fold per method inside it. They have to come back nested
    after a restart, open one level per keypress, and -- the case that
    breaks if each fold is re-found on its own -- a method must not escape
    its class and drag the class wider when the file changes underneath."""
    print("== 11. nested folds: still nested after a restart, and za opens one level")
    home = tempfile.mkdtemp(prefix="/tmp/mepf-")
    proj = os.path.join(home, "proj")
    os.makedirs(proj)
    path = os.path.join(proj, "demo.py")
    open(path, "w").write(python_source())

    m = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        # The class (18..23), then each of its methods inside it.
        m.cmd("18")
        m.shift_key("v")
        m.key("j", 5)
        m.key("z")
        m.key("f")
        # zf on a closed fold's summary row would act on the fold, so open
        # the class first, fold its methods, then close it again.
        m.cmd("18")
        m.key("z")
        m.key("o")
        for start, down in ((19, 1), (22, 1)):
            m.cmd(str(start))
            m.shift_key("v")
            m.key("j", down)
            m.key("z")
            m.key("f")
        made = sorted((f["start"], f["end"]) for f in m.folds())
        check("three nested folds made", made == [(18, 23), (19, 20), (22, 23)], fold_str(m.folds()))
        m.cmd("18")
        m.key("z")
        m.key("c")  # close the class over them
        time.sleep(1.0)
        m.pump(10)
    finally:
        m.quit()

    # Four lines added at the top on disk between the sessions.
    open(path, "w").write("# e1\n# e2\n# e3\n# e4\n" + python_source())

    m2 = Mep(build_dir, home, xv.display, [path], cwd=proj)
    try:
        back = m2.folds()
        print("      reopened with: %s" % fold_str(back))
        got = sorted((f["start"], f["end"]) for f in back)
        check("all three came back, each moved by four", got == [(22, 27), (23, 24), (26, 27)], fold_str(back))
        outer = [f for f in back if f["start"] == 22]
        inner = [f for f in back if f["start"] != 22]
        check("the class fold is the outer one, and closed",
              len(outer) == 1 and outer[0]["closed"], fold_str(back))
        check("both methods are inside it",
              all(f["start"] >= 22 and f["end"] <= 27 for f in inner), fold_str(back))
        check("row 22 is `class Gamma`", (m2.line(22) or "").startswith("class Gamma"), repr(m2.line(22)))
        # One za opens the class and reveals the methods' own summary rows;
        # the methods stay as they were (vim opens one level per press).
        m2.cmd("22")
        m2.key("z")
        m2.key("a")
        after = [f for f in m2.folds() if f["start"] == 22]
        check("one za opens the class", len(after) == 1 and not after[0]["closed"], fold_str(m2.folds()))
        # And the cursor can now reach a method's own row.
        m2.cmd("23")
        check("the cursor reaches `def one` at row 23", m2.cursor_row() == 23,
              "landed on %d (%r)" % (m2.cursor_row(), m2.line(m2.cursor_row())))
    finally:
        m2.quit()
    if keep:
        print("      (kept %s)" % home)
    else:
        shutil.rmtree(home, ignore_errors=True)


SCENARIOS = {
    "nested": scenario_nested,
    "agentrpc": scenario_agent_rpc_rewrite,
    "format": scenario_format_whole_file,
    "agent": scenario_agent_rewrite,
    "repeated": scenario_repeated_lines,
    "manual": scenario_manual_roundtrip,
    "bare": scenario_bare_open,
    "edit": scenario_edit_then_reopen,
    "external": scenario_external_change,
    "nav": scenario_closed_fold_navigation,
    "treesitter": scenario_treesitter_folds,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--only", action="append", default=None, choices=sorted(SCENARIOS))
    args = ap.parse_args()
    if not os.path.exists(os.path.join(args.build_dir, "mep")):
        sys.exit("no mep binary in %s" % args.build_dir)
    xv = Xvfb()
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
