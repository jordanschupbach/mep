#!/usr/bin/env python3
"""Golden captures of how mepml renders (plans/MEPML_STYLE_PLAN.md, Step 0).

Captures, for a fixed snapshot of the fixture documents:
  - the editor: a throwaway mep on a private Xvfb display pages through
    each document (the cursor's row shows its source, the rest rendered),
    then once more with concealment off, one PNG per stop;
  - the exports: `mep-mepml convert` to every text and office format.

  tools/mepml_golden.py snapshot DIR          copy the fixtures into DIR/fixtures
  tools/mepml_golden.py capture DIR NAME      capture into DIR/NAME
  tools/mepml_golden.py compare DIR A B       diff two captures

A snapshot is taken once and every capture reads it, so a fixture edited
in the working tree between two captures cannot show up as a difference.
`compare` is exact: pixels for the editor, bytes for text exports, the
unzipped members for docx/odt/pptx/odp (minus their timestamps).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (path in the repo, what to copy beside it)
FIXTURES = [
    ("test.mepml", ["opts.mepml", "refs.mepml", "test.mepss"]),
    ("tmp/regression.mepml", ["tmp/figs", "tmp/js"]),
    ("tmp/style.mepml", []),
    ("examples/mepl/iris_r/iris_r.mepml", ["examples/mepl/iris_r"]),
    ("examples/mepl/slides/slides.mepml", ["examples/mepl/slides"]),
]
EXPORTS = ["html", "md", "org", "rtf", "txt", "tex", "docx", "odt", "pptx", "odp"]
ZIPPED = {"docx", "odt", "pptx", "odp"}
STEP_ROWS = 8  # rows between stops: less than a screen, so stops overlap
# The pane's own area of a shot (the tab bar above and the status lines
# below hold a clock and messages that are not the document's rendering).
PANE_CROP = "1000x480+0+85"


def rpc(sock_path, method, params=None, timeout=20.0):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(timeout)
    s.connect(sock_path)
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}).encode()
    s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    data = b""
    try:
        while b'"id":1' not in data:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data.decode(errors="replace")


def clean_env():
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)  # (an alsa-lib built for a newer glibc)
    return env


class Mep:
    def __init__(self, build_dir, display, home, cache, path, log):
        env = dict(clean_env(), DISPLAY=display, HOME=home, XDG_DATA_HOME=os.path.join(home, ".local/share"),
                   XDG_CONFIG_HOME=os.path.join(home, ".config"), XDG_CACHE_HOME=cache)
        env.pop("MEP_AGENT_SOCKET", None)
        self.home = home
        self.proc = subprocess.Popen([os.path.join(build_dir, "mep"), "--no-session", path], env=env, stdout=log,
                                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=os.path.dirname(path))
        self.sock = os.path.join(home, ".local/share/mep/agent-sockets", "%d.sock" % self.proc.pid)
        for _ in range(150):
            if os.path.exists(self.sock):
                break
            time.sleep(0.1)
        else:
            self.close()
            raise RuntimeError("mep's agent socket never appeared (%s)" % self.sock)
        info = ""
        for _ in range(50):
            try:
                info = rpc(self.sock, "session.info", timeout=2.0)
            except OSError:
                info = ""
            if '"pid":%d' % self.proc.pid in info:
                return
            time.sleep(0.2)
        self.close()
        raise RuntimeError("socket answered for another process: " + info[:200])

    def cmd(self, cmd):
        return rpc(self.sock, "command.run", {"cmd": cmd})

    def lua(self, code):
        return self.cmd("lua " + code)

    def pump(self, seconds):
        """Frames only happen while something moves: keep the loop turning."""
        end = time.time() + seconds
        x = 5
        while time.time() < end:
            x = 6 if x == 5 else 5
            rpc(self.sock, "ui.mouse_move", {"x": x, "y": 5}, timeout=2.0)
            time.sleep(0.05)

    def screenshot(self, dest):
        out = rpc(self.sock, "ui.screenshot")
        m = re.search(r'"path":"([^"]+)"', out)
        if not m:
            raise RuntimeError("screenshot failed: " + out[:200])
        shutil.move(m.group(1), dest)

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def snapshot(root):
    fx = os.path.join(root, "fixtures")
    if os.path.exists(fx):
        sys.exit("%s already exists: a snapshot is taken once" % fx)
    for path, extras in FIXTURES:
        for rel in [path] + extras:
            src, dst = os.path.join(REPO, rel), os.path.join(fx, rel)
            if not os.path.exists(src):
                print("  (missing: %s)" % rel)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isdir(src):
                shutil.copytree(src, dst, dirs_exist_ok=True)
            else:
                shutil.copy(src, dst)
    shutil.copytree(fx, fx + ".pristine", symlinks=True)
    print("snapshot in", fx)


def stable_shot(mep, dest, settle=0.2, same=4, tries=40):
    """Screenshot once `same` in a row are the same picture.

    Maths and images load late, and the preview of the formula under the
    cursor later still (it is rendered again when the cursor arrives), so
    two equal shots are not yet a settled pane.
    """
    last, run = None, 0
    for _ in range(tries):
        mep.pump(settle)
        mep.screenshot(dest)
        # (Only the pane: the tab bar has an animated icon.)
        pixels = subprocess.run(["magick", dest, "-crop", PANE_CROP, "+repage", "rgb:-"], capture_output=True).stdout
        digest = hashlib.sha256(pixels).hexdigest()
        run = run + 1 if digest == last else 1
        last = digest
        if run >= same:
            return True
    return False


def capture_editor(args, fx_root, out_root, display):
    cache = os.path.join(args.dir, "cache")  # tectonic's renders, shared by every capture
    os.makedirs(cache, exist_ok=True)
    real_cache = os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache"))
    # tectonic's own bundle is big and already downloaded: reuse it.
    for name in ("Tectonic",):
        if os.path.isdir(os.path.join(real_cache, name)) and not os.path.exists(os.path.join(cache, name)):
            os.symlink(os.path.join(real_cache, name), os.path.join(cache, name))
    log = open(os.path.join(out_root, "mep.log"), "w")
    unstable = []
    for path, _ in FIXTURES:
        if args.only and args.only not in path:
            continue
        full = os.path.join(fx_root, path)
        if not os.path.exists(full):
            continue
        stem = path.replace("/", "_").replace(".mepml", "")
        dest = os.path.join(out_root, "editor", stem)
        os.makedirs(dest, exist_ok=True)
        n = len(open(full).read().split("\n"))
        slides = sum(1 for l in open(full).read().split("\n") if l.startswith("\\slide("))
        # Two passes, each in a mep of its own: the document as it is
        # edited, then the presentation view. Presenting runs a deck's live
        # blocks (they rewrite their results and pictures), so it comes
        # last and never shares an instance with the document's stops.
        passes = []
        if not args.present_only:
            passes.append("document")
        if slides and not args.no_present:
            passes.append("present")
        for which in passes:
            home = tempfile.mkdtemp(prefix="mepg-")  # short: the agent socket's path is capped
            mep = Mep(args.build_dir, display, home, cache, full, log)
            try:
                mep.lua("mep.set_cursor(1, 1)")
                mep.pump(args.warm)  # every fragment's maths rendered before the first stop
                if which == "present":
                    # (A slide stays blank until its maths is in, so a page
                    # is given longer to settle.)
                    mep.lua("mep.mepml_present('fill')")
                    mep.pump(2.0)
                    for page in range(1, slides + 2):
                        mep.lua("mep.mepml_present_goto(%d)" % page)
                        shot = os.path.join(dest, "present-%03d.png" % page)
                        if not stable_shot(mep, shot, same=8, tries=60):
                            unstable.append(shot)
                    continue
                for mode in ("rendered", "raw"):
                    if mode == "raw":
                        mep.lua("if mep.org_conceal_visible() then mep.org_conceal_toggle() end")
                        mep.pump(1.0)
                    rows = list(range(1, n + 1, STEP_ROWS))
                    if mode == "raw":
                        rows = rows[: args.raw_stops]
                    for row in rows:
                        mep.lua("mep.set_cursor(%d, 1)" % row)
                        shot = os.path.join(dest, "%s-%05d.png" % (mode, row))
                        if not stable_shot(mep, shot):
                            unstable.append(shot)
            finally:
                mep.close()
                shutil.rmtree(home, ignore_errors=True)
        print("  editor: %s (%d lines)" % (path, n))
    if unstable:
        print("  never settled: " + ", ".join(os.path.relpath(u, out_root) for u in unstable))


def capture_exports(args, fx_root, out_root):
    tool = os.path.join(args.build_dir, "mep-mepml")
    for path, _ in FIXTURES:
        if args.only and args.only not in path:
            continue
        full = os.path.join(fx_root, path)
        if not os.path.exists(full):
            continue
        stem = path.replace("/", "_").replace(".mepml", "")
        dest = os.path.join(out_root, "export", stem)
        os.makedirs(dest, exist_ok=True)
        failed = []
        for fmt in EXPORTS:
            out = os.path.join(dest, "out." + fmt)
            r = subprocess.run([tool, "convert", full, out], capture_output=True, text=True, env=clean_env(),
                               cwd=os.path.dirname(full))
            with open(out + ".log", "w") as f:
                f.write("exit %d\n%s%s" % (r.returncode, r.stdout, r.stderr))
            if r.returncode != 0:
                failed.append(fmt)
        print("  export: %s%s" % (path, "  (failed: %s)" % " ".join(failed) if failed else ""))


def capture(args):
    fx_root = os.path.join(args.dir, "fixtures")
    if not os.path.isdir(fx_root):
        sys.exit("no snapshot: run `snapshot %s` first" % args.dir)
    # Every capture starts from the snapshot as it was taken: presenting a
    # deck starts its live blocks, which rewrite their own result pictures.
    pristine = fx_root + ".pristine"
    if os.path.isdir(pristine):
        shutil.rmtree(fx_root)
        shutil.copytree(pristine, fx_root, symlinks=True)
    out_root = os.path.join(args.dir, args.name)
    if os.path.exists(out_root):
        shutil.rmtree(out_root)
    os.makedirs(out_root)
    if not args.no_exports and not args.present_only:
        capture_exports(args, fx_root, out_root)
    if not args.no_editor:
        xvfb = subprocess.Popen(["Xvfb", args.display, "-screen", "0", "1600x1000x24"], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        try:
            capture_editor(args, fx_root, out_root, args.display)
        finally:
            xvfb.terminate()
    print("captured", out_root)


def zip_members(path):
    out = {}
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            data = z.read(name)
            # Creation times differ run to run.
            data = re.sub(rb"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z?", b"<time>", data)
            out[name] = data
    return out


def files_under(root):
    out = []
    for d, _, names in os.walk(root):
        for n in names:
            out.append(os.path.relpath(os.path.join(d, n), root))
    return sorted(out)


def png_differs(a, b, diff_path):
    """Pixel-exact comparison; writes a picture of where two shots differ."""
    if open(a, "rb").read() == open(b, "rb").read():
        return 0
    crops = []
    for src in (a, b):
        out = diff_path + (".a.png" if src == a else ".b.png")
        subprocess.run(["magick", src, "-crop", PANE_CROP, "+repage", out], check=True)
        crops.append(out)
    r = subprocess.run(["magick", "compare", "-metric", "AE", crops[0], crops[1], diff_path], capture_output=True,
                       text=True)
    m = re.match(r"\s*([0-9.e+]+)", r.stderr)
    count = int(float(m.group(1))) if m else -1
    if count == 0:
        for f in crops + [diff_path]:
            if os.path.exists(f):
                os.remove(f)
    return count


def compare(args):
    a_root, b_root = os.path.join(args.dir, args.a), os.path.join(args.dir, args.b)
    diff_root = os.path.join(args.dir, "diff-%s-%s" % (args.a, args.b))
    shutil.rmtree(diff_root, ignore_errors=True)
    a_files, b_files = set(files_under(a_root)), set(files_under(b_root))
    problems = []
    for rel in sorted(a_files | b_files):
        if rel.endswith(".log") and not rel.startswith("export"):
            continue
        if args.common and (rel not in a_files or rel not in b_files):
            continue
        if rel not in a_files or rel not in b_files:
            problems.append("%s: only in %s" % (rel, args.a if rel in a_files else args.b))
            continue
        a, b = os.path.join(a_root, rel), os.path.join(b_root, rel)
        ext = rel.rsplit(".", 1)[-1]
        if ext == "png":
            diff = os.path.join(diff_root, rel)
            os.makedirs(os.path.dirname(diff), exist_ok=True)
            count = png_differs(a, b, diff)
            if count:
                problems.append("%s: %d pixels differ (%s)" % (rel, count, diff))
        elif ext in ZIPPED:
            ma, mb = zip_members(a), zip_members(b)
            for name in sorted(set(ma) | set(mb)):
                if ma.get(name) != mb.get(name):
                    problems.append("%s!%s differs" % (rel, name))
        else:
            da, db = open(a, "rb").read(), open(b, "rb").read()
            if da != db:
                problems.append("%s differs" % rel)
    total = len(a_files | b_files)
    if problems:
        print("\n".join(problems))
        print("%d of %d files differ" % (len(problems), total))
        return 1
    print("identical: %d files" % total)
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("dir")
    c = sub.add_parser("capture")
    c.add_argument("dir")
    c.add_argument("name")
    c.add_argument("--build-dir", default=os.path.join(REPO, "build/native"))
    c.add_argument("--display", default=":93")
    c.add_argument("--only", default="", help="only fixtures whose path contains this")
    c.add_argument("--warm", type=float, default=25.0, help="seconds to let maths render before the first stop")
    c.add_argument("--raw-stops", type=int, default=6, help="stops captured with concealment off")
    c.add_argument("--no-editor", action="store_true")
    c.add_argument("--no-present", action="store_true", help="skip the presentation view's pages")
    c.add_argument("--present-only", action="store_true", help="only the presentation view's pages")
    c.add_argument("--no-exports", action="store_true")
    d = sub.add_parser("compare")
    d.add_argument("dir")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--common", action="store_true", help="compare only what both captures have")
    args = ap.parse_args()
    args.dir = os.path.abspath(args.dir)
    if args.cmd == "snapshot":
        snapshot(args.dir)
    elif args.cmd == "capture":
        capture(args)
    else:
        sys.exit(compare(args))


if __name__ == "__main__":
    main()
