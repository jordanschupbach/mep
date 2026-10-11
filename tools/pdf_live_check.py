#!/usr/bin/env python3
"""Drive a real mep over its agent socket and check PDF selection end to end.

The unit tests cover pdftext's pure functions and tools/pdf_accuracy.py
covers whole-file extraction, but neither touches the layer the user
actually operates: annotate mode's caret, `v` visual selection, `y`, and
the passage that reaches an AI prompt. That layer had no test at all --
no pdf_caret_test, and the AI path is Lua embedded in main.cpp reachable
only from a running editor.

So this spawns a throwaway mep on its own Xvfb display, opens a fixture,
and asserts on what the editor itself produces.

Gotchas this encodes, each of which fails silently rather than loudly:
  * The agent socket path must be short. sockaddr_un.sun_path is 108
    bytes; mep logs "agent socket path too long" and binds nothing, so a
    long HOME means waiting forever for a socket that never existed.
  * The socket carries event.* notifications as well as replies, so a
    reply has to be matched on the request's own id.
  * mep's frame loop idles, and ui.* queues one synthetic-input step per
    rendered frame, so anything driven by keystrokes needs the loop
    poked repeatedly before the effect is observable.
  * Xvfb has no window manager, so nothing assigns X input focus and
    XTest keystrokes land nowhere until XSetInputFocus is called by hand.
  * A fresh display number per run: a stale /tmp/.X<n>-lock stops the
    next Xvfb, and mep then aborts with "XOpenDisplay failed", which
    reads as a mep crash.

Usage: tools/pdf_live_check.py [--build-dir build/native-dev] [--keep]
"""

import argparse
import ctypes
import http.server
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
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
            # A notification has no id; queue it rather than mistaking it
            # for this call's reply, which would shift every later result.
            if msg.get("id") != rid:
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
    """Give mep's window X input focus. Xvfb runs no window manager, so
    without this XTest keystrokes are delivered nowhere. find_library
    returns a soname that cannot be dlopen'd under Nix, so the absolute
    store path is taken from the binary's own ldd output."""
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
    best, best_area = None, -1

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


class Mep:
    def __init__(self, build_dir, first_file, keep=False):
        self.keep = keep
        self.display = ":%d" % random.randint(300, 900)
        # Short, because sun_path is 108 bytes and the socket lives under
        # $XDG_DATA_HOME/mep/agent-sockets/<pid>.sock.
        self.home = tempfile.mkdtemp(prefix="/tmp/mepl-")
        self.logf = open(os.path.join(self.home, "mep.log"), "wb")
        self.xvfb = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1600x1200x24"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        env = dict(os.environ, DISPLAY=self.display, HOME=self.home,
                   XDG_DATA_HOME=os.path.join(self.home, "share"),
                   XDG_CONFIG_HOME=os.path.join(self.home, "config"),
                   # Makes the render worker log each page's pixel
                   # dimensions, which is the only way from outside to
                   # see what scale it actually rendered at.
                   MEP_PDF_PROF="1")
        self.build_dir = build_dir
        self.proc = subprocess.Popen([os.path.join(build_dir, "mep"), "--no-session", first_file], env=env,
                                     stdout=self.logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=REPO)
        sock_dir = os.path.join(self.home, "share", "mep", "agent-sockets")
        self.sock_path = os.path.join(sock_dir, "%d.sock" % self.proc.pid)
        for _ in range(150):
            if os.path.exists(self.sock_path):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("mep exited early; log:\n" + self._log_tail())
            time.sleep(0.1)
        else:
            raise RuntimeError("agent socket never appeared (%s); log:\n%s" % (self.sock_path, self._log_tail()))
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
        focus_largest_window(self.display, build_dir)
        self.pump(10)

    def render_sizes(self):
        """Every (w, h) a render worker reported, in order."""
        import re as _re
        try:
            with open(os.path.join(self.home, "mep.log"), "rb") as fh:
                txt = fh.read().decode(errors="replace")
        except OSError:
            return []
        return [(int(a), int(b)) for a, b in _re.findall(r"RenderPage\(\d+\) worker: [0-9.]+ ms (\d+)x(\d+)", txt)]

    def _log_tail(self):
        try:
            with open(os.path.join(self.home, "mep.log"), "rb") as fh:
                return fh.read()[-2000:].decode(errors="replace")
        except OSError:
            return "(no log)"

    def pump(self, n=6):
        """Advance mep's frame loop. It idles, and ui.* input is drained
        one step per rendered frame, so an effect appears only after the
        loop has actually run -- every RPC call wakes it."""
        for _ in range(n):
            try:
                self.rpc.call("session.info")
            except RuntimeError:
                pass
            time.sleep(0.03)

    def cmd(self, c):
        r = self.rpc.call("command.run", {"cmd": c})
        self.pump(4)
        return r

    def key(self, name):
        self.rpc.call("ui.key_press", {"key": name})
        self.pump(5)

    def shift_key(self, name):
        """A shifted key needs Shift genuinely held: ui.key_press goes
        straight to KeyEvent with no shift handling, so "plus" resolves
        to the +/= keycode and arrives as '=' -- which in a PDF pane is
        fit-page, not zoom-in."""
        self.rpc.call("ui.key_down", {"key": "Shift_L"})
        self.rpc.call("ui.key_press", {"key": name})
        self.rpc.call("ui.key_up", {"key": "Shift_L"})
        self.pump(6)

    def lua_value(self, expr):
        """Evaluate a Lua expression and read it back through a file --
        there is no RPC that returns a Lua value, and `:lua` prints
        nowhere observable."""
        out = os.path.join(self.home, "out.txt")
        if os.path.exists(out):
            os.remove(out)
        chunk = ("local ok, v = pcall(function() return %s end) "
                 "local f = io.open(%s, 'w') f:write(tostring(ok and v or ('ERR: ' .. tostring(v)))) f:close()"
                 % (expr, json.dumps(out)))
        self.cmd("lua " + chunk)
        for _ in range(20):
            if os.path.exists(out):
                with open(out) as fh:
                    return fh.read()
            self.pump(3)
        return None

    def close(self):
        try:
            self.rpc.close()
        except Exception:
            pass
        for p in (self.proc, self.xvfb):
            try:
                p.terminate()
                p.wait(5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass
        self.logf.close()
        if self.keep:
            print("   (kept %s)" % self.home)
        else:
            shutil.rmtree(self.home, ignore_errors=True)


class WireSink:
    """A local HTTP server standing in for the model API.

    mep's AI client shells out to `curl -X POST <base_url>/...`, so
    pointing its base URL here captures the exact JSON that would have
    gone to the provider -- which is the only way to check the image
    content blocks are spelled the way each API wants them. No key, no
    network, no tokens.

    Replies with a minimal SSE stream so the client's own parser is
    exercised rather than erroring out."""

    def __init__(self):
        self.bodies = []
        sink = self

        class H(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(n)
                sink.bodies.append((self.path, raw))
                # Both providers stream `data: {...}` lines; these are
                # the shapes mep's own parser looks for.
                if "messages" in self.path:
                    body = (b'data: {"type":"content_block_delta","delta":'
                            b'{"type":"text_delta","text":"ok"}}\n\n'
                            b'data: {"type":"message_stop"}\n\n')
                else:
                    body = (b'data: {"choices":[{"delta":{"content":"ok"}}]}\n\n'
                            b'data: [DONE]\n\n')
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def last_json(self):
        if not self.bodies:
            return None, None
        path, raw = self.bodies[-1]
        try:
            return path, json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            return path, None

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def find_image_block(msg):
    """The image part of one wire message, in either provider's spelling."""
    c = msg.get("content")
    if not isinstance(c, list):
        return None
    for part in c:
        if not isinstance(part, dict):
            continue
        if part.get("type") == "image" and isinstance(part.get("source"), dict):
            return ("anthropic", part["source"].get("media_type"), part["source"].get("data", ""))
        if part.get("type") == "image_url":
            url = (part.get("image_url") or {}).get("url", "")
            if url.startswith("data:"):
                head, _, data = url.partition(",")
                return ("openai", head[5:].split(";")[0], data)
    return None


# ------------------------------------------------------------------ checks

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("   %-52s %s%s" % (name, "PASS" if ok else "FAIL", ("  " + detail) if detail and not ok else ""))


def run_checks(build_dir, keep):
    fixtures = os.path.join(REPO, "test", "pdf_fixtures")
    bullets = os.path.join(fixtures, "bullets_cm.pdf")
    math = os.path.join(fixtures, "display_math.pdf")
    for f in (bullets, math):
        if not os.path.exists(f):
            sys.exit("missing fixture %s -- run tools/pdf_make_fixtures.sh" % f)

    m = Mep(build_dir, bullets, keep=keep)
    try:
        print("== bullets_cm.pdf: a bulleted item must yank as its text")
        pages = m.lua_value("mep.pdf_page_count and mep.pdf_page_count() or 'n/a'")
        text = m.lua_value("string.sub(mep.pdf_page_text(1), 1, 400)")
        check("page text reaches Lua", bool(text) and "ERR" not in (text or ""), repr(text)[:160])
        if text:
            check("bulleted item body text present in page text",
                  "nested list follows this item" in text.replace("\n", " "), repr(text)[:200])

        # Annotate mode, then a visual selection over the first rows.
        m.cmd("pdfannotate")
        m.pump(6)
        m.key("j")
        m.key("j")
        m.key("j")
        m.key("v")
        for _ in range(6):
            m.key("l")
        # mep.pdf_yank() is the same Editor::PdfYankSelection the `y` key
        # runs; it syncs the unnamed register to the system clipboard,
        # which is the only way to read a register back from Lua.
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(8)
        reg = m.lua_value("string.gsub(tostring(mep.clipboard_get() or 'n/a'), '\\n', ' | ')")
        check("yank puts the selected passage in the register", bool(reg) and reg not in ("", "n/a", "nil") and "ERR" not in reg,
              repr(reg)[:200])
        if reg and "ERR" not in reg and reg != "n/a":
            print("      register: %s" % repr(reg)[:140])

        sel = m.lua_value("string.gsub(tostring((mep.pdf_selection() or {}).text or 'none'), '\\n', ' | ')")
        check("pdf_selection() returns the passage an AI prompt would get",
              bool(sel) and sel not in ("none", "", "nil") and "ERR" not in sel, repr(sel)[:200])
        if sel and sel not in ("none", "nil") and "ERR" not in sel:
            print("      passage:  %s" % repr(sel)[:140])
    finally:
        m.close()

    # The equation case: the thing that was unreachable before.
    m = Mep(build_dir, math, keep=keep)
    try:
        print("== display_math.pdf: a displayed equation must be reachable")
        page = m.lua_value("string.sub(mep.pdf_page_text(1), 1, 1200)")
        has_eq = bool(page) and "lim" in page
        check("equation text present in page text", has_eq, repr(page)[:200])
        if page:
            # The operator and its limit as separate words, which is what
            # makes a quoted equation readable rather than run together.
            check("limit reads as 'lim' plus its own limit, not run together",
                  "lim " in page or "lim\n" in page, repr(page)[-200:])

        m.cmd("pdfannotate")
        m.pump(6)
        # Walk down far enough to be inside the equation block, select a
        # line, and yank it.
        for _ in range(8):
            m.key("j")
        m.key("v")
        for _ in range(10):
            m.key("l")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(8)
        reg = m.lua_value("string.gsub(tostring(mep.clipboard_get() or 'n/a'), '\\n', ' | ')")
        got = bool(reg) and reg not in ("", "n/a", "nil") and "ERR" not in reg
        check("an equation row can be visually selected and yanked", got, repr(reg)[:200])
        if got:
            print("      register: %s" % repr(reg)[:140])
    finally:
        m.close()

    # Zooming must re-render at the new scale, not magnify the cached
    # raster. That is the whole of the "maths looks rasterized" fix: a
    # 144dpi raster shown at 2x carries about a third of the detail a
    # native render at 2x does.
    m = Mep(build_dir, math, keep=keep)
    try:
        print("== display_math.pdf: zooming must re-render, not magnify")
        m.pump(25)
        before = m.render_sizes()
        base = max((w for w, _ in before), default=0)
        check("page rendered at all", base > 0, str(before[:4]))
        # Three zoom-in steps, then sit still long enough for the
        # debounced re-render to fire.
        for _ in range(3):
            m.shift_key("equal")
        for _ in range(12):
            m.pump(6)
            time.sleep(0.1)
        after = m.render_sizes()
        grew = max((w for w, _ in after), default=0)
        check("re-rendered at a larger scale after zooming", grew > base * 1.3,
              "widest before=%d after=%d" % (base, grew))
        if grew > base:
            print("      widest raster: %dpx -> %dpx (%.2fx)" % (base, grew, grew / base))
        # ...and the debounce must coalesce: three zoom steps should not
        # mean three full re-render passes of the whole page window.
        passes_at_new_scale = len([1 for w, _ in after if w > base * 1.3])
        check("debounce coalesced the zoom steps", passes_at_new_scale <= 8,
              "%d renders above the old scale" % passes_at_new_scale)
    finally:
        m.close()

    # Vim motions in annotate mode: counts, and gg/G as caret motions.
    m = Mep(build_dir, math, keep=keep)
    try:
        print("== display_math.pdf: counts and gg/G must work on the caret")
        m.cmd("pdfannotate")
        m.pump(8)

        def caret():
            return m.lua_value("tostring(mep.pdf_selection and 'ok' or 'ok')")

        # G to the last row, then gg back to the first: yanking after each
        # is how the position is observed, since there is no caret getter.
        m.shift_key("g")
        m.pump(6)
        m.key("v")
        for _ in range(6):
            m.key("l")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(6)
        last = m.lua_value("string.gsub(tostring(mep.clipboard_get() or ''), '\\n', ' ')")
        m.key("Escape")
        m.pump(4)
        m.cmd("lua mep.clipboard_set('')")
        m.key("g")
        m.key("g")
        m.pump(6)
        m.key("v")
        for _ in range(6):
            m.key("l")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(6)
        first = m.lua_value("string.gsub(tostring(mep.clipboard_get() or ''), '\\n', ' ')")
        check("G and gg land on different rows", bool(first) and bool(last) and first != last,
              "gg=%r G=%r" % (first, last))
        print("      gg -> %r" % (first or "")[:48])
        print("      G  -> %r" % (last or "")[:48])
        # The page's first row is its heading, which is what gg must find.
        check("gg reaches the top of the page", bool(first) and first.startswith("Display"),
              repr(first)[:120])

        # A count on a caret motion: 3w must not move one word.
        m.key("Escape")
        m.pump(4)
        m.key("g")
        m.key("g")
        m.pump(4)
        m.key("w")
        m.key("v")
        for _ in range(5):
            m.key("l")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(6)
        one_w = m.lua_value("tostring(mep.clipboard_get() or '')")
        m.key("Escape")
        m.pump(4)
        m.key("g")
        m.key("g")
        m.pump(4)
        m.key("3")
        m.key("w")
        m.key("v")
        for _ in range(5):
            m.key("l")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(6)
        three_w = m.lua_value("tostring(mep.clipboard_get() or '')")
        check("a count moves the caret further than no count", one_w != three_w,
              "w=%r 3w=%r" % (one_w, three_w))
        print("      w  -> %r   3w -> %r" % ((one_w or "")[:20], (three_w or "")[:20]))
    finally:
        m.close()

    # A visual selection must be able to cross a page break: a
    # paragraph continuing overleaf is one passage.
    twocol = os.path.join(fixtures, "twocolumn_header.pdf")
    multi = twocol if os.path.exists(twocol) else math
    m = Mep(build_dir, multi, keep=keep)
    try:
        print("== a visual selection must cross a page break")
        m.cmd("pdfannotate")
        m.pump(8)
        m.key("g")
        m.key("g")
        m.pump(4)
        # Select from near the end of page 1 onward, then push past the
        # bottom so the caret moves to page 2 with visual still live.
        m.shift_key("g")      # last row of this page
        m.pump(4)
        m.key("v")
        for _ in range(14):   # j past the end -> next page
            m.key("j")
        m.pump(10)
        # pdf_current_page takes a buffer id; calling it bare throws.
        page_now = m.lua_value("tostring(mep.pdf_current_page(mep.current_buffer()))")
        # The fixture has two pages; starting on the first, the caret
        # must be on the second.
        check("the caret crossed to page 2 with visual live", page_now == "2", "page=%s" % page_now)
        m.cmd("lua mep.clipboard_set('')")
        m.lua_value("tostring(mep.pdf_yank())")
        m.pump(8)
        got = m.lua_value("tostring(mep.clipboard_get() or '')")
        check("the cross-page selection yields text", bool(got) and len(got) > 20, repr(got)[:120])
        if got:
            print("      %d chars, %d lines" % (len(got), got.count("\\n") + 1))
    finally:
        m.close()

    # The AI path, for both providers, against a local sink -- so the
    # actual wire JSON is checked rather than just mep's own message
    # table. A page with no text layer must arrive as an image.
    scan = os.path.join(fixtures, "scan_bitonal.pdf")
    if not os.path.exists(scan):
        check("scan fixture present", False, "run tools/pdf_make_fixtures.sh")
    else:
        for provider in ("openai", "anthropic"):
            sink = WireSink()
            m = Mep(build_dir, scan, keep=keep)
            try:
                print("== scan_bitonal.pdf + %s: no text layer must be sent as an image" % provider)
                base = "http://127.0.0.1:%d" % sink.port
                # A key is set so the client never stops to prompt for one.
                m.cmd("lua mep.ai_api_key = 'test-key'")
                m.cmd("lua mep.ai_provider = %s" % json.dumps(provider))
                m.cmd("lua mep.ai_base_url = %s" % json.dumps(base + "/v1"))
                m.cmd("lua mep.ai_anthropic_base_url = %s" % json.dumps(base))
                m.cmd("lua mep.ai_http_enabled = true")
                page_text = m.lua_value("string.format('%d', #(mep.pdf_page_text(1) or ''))")
                check("the fixture really has no text layer", page_text in ("0", "", None),
                      "page text length = %s" % page_text)
                # K is the ask key; it needs Shift genuinely held.
                m.shift_key("k")
                for _ in range(25):
                    m.pump(6)
                    time.sleep(0.12)
                    if sink.bodies:
                        break
                path, body = sink.last_json()
                check("a request reached the API (%s)" % provider, body is not None,
                      "captured %d bodies" % len(sink.bodies))
                if body:
                    msgs = body.get("messages") or []
                    found = find_image_block(msgs[0]) if msgs else None
                    check("the page arrived as an image block (%s)" % provider, found is not None,
                          json.dumps(body)[:220])
                    if found:
                        spelling, media, data = found
                        expect = "anthropic" if provider == "anthropic" else "openai"
                        check("image used %s's own spelling" % provider, spelling == expect,
                              "got %s" % spelling)
                        check("media type is image/png", media == "image/png", str(media))
                        check("image payload is substantial base64", len(data) > 2000,
                              "%d chars" % len(data))
                        print("      %s: %s block, %s, %d chars of base64"
                              % (provider, spelling, media, len(data)))
                    # The text half must still be there, asking about the page.
                    txt = json.dumps(msgs[0].get("content"))
                    check("the question travelled with it (%s)" % provider,
                          "no extractable text" in txt, txt[:200])
            finally:
                m.close()
                sink.close()

        # A page whose text extracts as nonsense must also go as an
        # image -- the TAOCP case. Unlike the scan, this page HAS text
        # and a selection is possible, so it exercises the quality
        # verdict rather than the empty-passage path.
        degen = os.path.join(fixtures, "degenerate_text.pdf")
        if not os.path.exists(degen):
            check("degenerate fixture present", False, "run tools/pdf_make_fixtures.sh")
        else:
            sink = WireSink()
            m = Mep(build_dir, degen, keep=keep)
            try:
                print("== degenerate_text.pdf: nonsense text must be sent as an image")
                base = "http://127.0.0.1:%d" % sink.port
                m.cmd("lua mep.ai_api_key = 'test-key'")
                m.cmd("lua mep.ai_provider = 'openai'")
                m.cmd("lua mep.ai_base_url = %s" % json.dumps(base + "/v1"))
                m.cmd("lua mep.ai_http_enabled = true")
                q = m.lua_value("(function() local d, c, dis = mep.pdf_page_text_quality(1) "
                                "return tostring(d) .. '|chars=' .. c .. '|distinct=' .. dis end)()")
                check("the page's text is judged degenerate",
                      bool(q) and q.startswith("true|"), repr(q)[:160])
                if q:
                    print("      %s" % q)
                # It has text, so a selection is possible -- and must
                # still not be what gets sent.
                m.cmd("pdfannotate")
                m.pump(6)
                for _ in range(3):
                    m.key("j")
                m.key("v")
                for _ in range(8):
                    m.key("l")
                m.shift_key("k")
                for _ in range(25):
                    m.pump(6)
                    time.sleep(0.12)
                    if sink.bodies:
                        break
                path, body = sink.last_json()
                check("a request reached the API", body is not None, "captured %d" % len(sink.bodies))
                if body:
                    msgs = body.get("messages") or []
                    found = find_image_block(msgs[0]) if msgs else None
                    check("the page went as an image despite having text",
                          found is not None, json.dumps(body)[:200])
                    txt = json.dumps(msgs[0].get("content")) if msgs else ""
                    check("the nonsense text was NOT quoted to the model",
                          "DBBB" not in txt, txt[:200])
                    if found:
                        print("      image attached, %d chars; no nonsense in the prompt" % len(found[2]))
            finally:
                m.close()
                sink.close()

        # A page that renders blank must be refused, not sent: an
        # unsupported image format (JPEG 2000) leaves a white rectangle,
        # and a model would faithfully describe an empty page.
        m = Mep(build_dir, scan, keep=keep)
        try:
            print("== a blank-rendering page must be refused, not sent")
            # The fixture renders fine, so blankness is forced by asking
            # for a region that lies in the page's own empty margin.
            # lua_value takes an EXPRESSION, so statements go inside an
            # immediately-invoked function.
            got = m.lua_value("(function() local b, w = mep.pdf_page_png(1, 2.0, 0, 0, 20, 20) "
                              "return tostring(b == nil) .. '|' .. tostring(w) end)()")
            check("a blank region returns nil plus a reason",
                  bool(got) and got.startswith("true|") and "blank" in got,
                  repr(got)[:200])
            if got:
                print("      %s" % got[:150])
            # ...and a region with content still returns an image.
            ok2 = m.lua_value("(function() local b = mep.pdf_page_png(1, 2.0, 60, 600, 500, 700) "
                              "return tostring(b ~= nil and #b > 500) end)()")
            check("a region with content still renders", ok2 == "true", repr(ok2))
        finally:
            m.close()

        # ...and a page that DOES have text must not be sent as an image.
        sink = WireSink()
        m = Mep(build_dir, math, keep=keep)
        try:
            print("== display_math.pdf: a page with text must stay text-only")
            base = "http://127.0.0.1:%d" % sink.port
            m.cmd("lua mep.ai_api_key = 'test-key'")
            m.cmd("lua mep.ai_provider = 'openai'")
            m.cmd("lua mep.ai_base_url = %s" % json.dumps(base + "/v1"))
            m.cmd("lua mep.ai_http_enabled = true")
            m.cmd("pdfannotate")
            m.pump(6)
            for _ in range(4):
                m.key("j")
            m.key("v")
            for _ in range(8):
                m.key("l")
            m.shift_key("k")
            for _ in range(25):
                m.pump(6)
                time.sleep(0.12)
                if sink.bodies:
                    break
            path, body = sink.last_json()
            check("a request reached the API", body is not None, "captured %d" % len(sink.bodies))
            if body:
                msgs = body.get("messages") or []
                found = find_image_block(msgs[0]) if msgs else None
                check("no image attached when the text extracted", found is None,
                      json.dumps(body)[:200])
                txt = json.dumps(msgs[0].get("content")) if msgs else ""
                check("the selected passage travelled as text", "From " in txt, txt[:200])
                if msgs:
                    print("      content: %s" % txt[:150])
        finally:
            m.close()
            sink.close()

    print()
    bad = [r for r in RESULTS if not r[1]]
    print("%d/%d checks passed" % (len(RESULTS) - len(bad), len(RESULTS)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native-dev"))
    ap.add_argument("--keep", action="store_true", help="keep the throwaway HOME for inspection")
    a = ap.parse_args()
    if shutil.which("Xvfb") is None:
        sys.exit("Xvfb not on PATH")
    sys.exit(run_checks(a.build_dir, a.keep))


if __name__ == "__main__":
    main()
