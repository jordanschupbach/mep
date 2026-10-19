#!/usr/bin/env python3
"""The live-editor tier of mep's benchmark suite (plans/PERFORMANCE_PLAN.md).

For each fixture, starts a throwaway mep with its own data and config
directories (so neither the user's init.lua nor their session is
involved, and nothing the run does lands in their real data dir), times
how long it takes to answer on its agent socket, runs
tools/bench_live.lua inside it, and records:

  live/startup/to_socket            spawn -> agent socket answering
  live/<fixture>/open_cmd           `:e` of the fixture
  live/<fixture>/hook_*             the per-edit hooks, called directly
  live/<fixture>/frame_*            per-frame CPU work (median; p95/max/hooks/draw alongside)
  live/<fixture>/memory_mb          memory at the end of the run: physical footprint / VmRSS (n = MB, ms = 0)

Linux: runs on a private Xvfb display when Xvfb exists (never takes focus).
macOS: opens a real window for the duration of each fixture (a few
seconds); keep your hands off the keyboard while it runs.

  tools/bench_live.py [--list] [--filter PAT]... [--quick] [--no-record] [--json PATH] [--build-dir DIR]

Profiling: `--profile FIXTURE:SCENARIO [--seconds N]` (SCENARIO one of idle,
scroll, cursor_down, typing, highlight) opens the fixture, repeats that
scenario every frame for N seconds (default 8) and samples mep meanwhile --
macOS `sample` (call tree) or Linux `perf record` -- writing the report to
bench_results/profiles/ (gitignored scratch) and printing the heaviest frames.
"""

import argparse
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_common as bc  # noqa: E402

BINARY = "live_bench"


def gen_fixtures(work):
    """name -> (path, lines). Real files from this checkout grown to size."""
    def grow(src, lines, name):
        with open(os.path.join(bc.REPO, src), encoding="utf-8", errors="replace") as f:
            base = f.read().split("\n")
        out = []
        while len(out) < lines:
            out.extend(base[: lines - len(out)])
        path = os.path.join(work, name)
        with open(path, "w") as f:
            f.write("\n".join(out) + "\n")
        return path

    return {
        "cpp_2k": lambda: grow("src/editor.cpp", 2000, "cpp_2k.cpp"),
        "cpp_60k": lambda: grow("src/editor.cpp", 60000, "cpp_60k.cpp"),
        "py_20k": lambda: grow("tools/mepml_editor_bench.py", 20000, "py_20k.py"),
        "md_20k": lambda: grow("plans/CAD_FEM_PLAN.md", 20000, "md_20k.md"),
        "org_20k": lambda: grow("README.org", 20000, "org_20k.org"),
        # test.mepml uses every mepml construct; its imports and exec
        # blocks stay inert (nothing runs until asked).
        "mepml_20k": lambda: grow("test.mepml", 20000, "mepml_20k.mepml"),
        # A minified JSON document on one 1 MB line: what wrapping, drawing
        # and highlighting a single enormous line costs.
        "longline_1mb": lambda: longline(os.path.join(work, "longline_1mb.json")),
    }


def longline(path, size=1 << 20):
    parts = []
    total = 0
    i = 0
    while total < size:
        item = '{"id":%d,"name":"item_%d","tags":["alpha","beta"],"value":%d.%d}' % (i, i, i * 7, i % 10)
        parts.append(item)
        total += len(item) + 1
        i += 1
    with open(path, "w") as f:
        f.write("[" + ",".join(parts) + "]\n")
    return path


def rpc(sock_path, method, params=None, timeout=10.0):
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


class Mep:
    """A throwaway mep reachable over its agent socket."""

    def __init__(self, build_dir, home, first_file, log, display=None):
        data = os.path.join(home, "d")
        env = dict(os.environ, XDG_DATA_HOME=data, XDG_CONFIG_HOME=os.path.join(home, "c"),
                   XDG_STATE_HOME=os.path.join(home, "s"),
                   # macOS: no App Nap / background QoS for the benchmark window (backend_native_macos.mm).
                   MEP_BENCH_NO_THROTTLE="1")
        if display:
            env["DISPLAY"] = display
        self.sock_dir = os.path.join(data, "mep", "agent-sockets")
        t0 = time.monotonic()
        self.proc = subprocess.Popen([os.path.join(os.path.abspath(build_dir), "mep"), "--no-session", first_file], env=env,
                                     stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=home)
        self.sock = os.path.join(self.sock_dir, "%d.sock" % self.proc.pid)
        if len(self.sock) > 100:
            self.close()
            raise RuntimeError("agent socket path too long for a Unix socket: " + self.sock)
        deadline = time.monotonic() + 30
        info = ""
        while time.monotonic() < deadline:
            if os.path.exists(self.sock):
                try:
                    info = rpc(self.sock, "session.info", timeout=2.0)
                except OSError:
                    info = ""
                if '"pid":%d' % self.proc.pid in info:
                    break
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        if '"pid":%d' % self.proc.pid not in info:
            self.close()
            raise RuntimeError("mep never answered on its agent socket (%s)" % self.sock)
        self.startup_ms = (time.monotonic() - t0) * 1000.0

    def cmd(self, cmd):
        return rpc(self.sock, "command.run", {"cmd": cmd})

    def cpu_seconds(self):
        """Total CPU time (user + system, all threads) the process has used."""
        out = subprocess.run(["ps", "-o", "time=", "-p", str(self.proc.pid)], capture_output=True, text=True).stdout
        total = 0.0
        for part in out.strip().replace("-", ":").split(":"):
            total = total * 60 + float(part or 0)
        return total

    def memory_mb(self):
        """Memory this process owns: macOS's physical footprint (what Activity Monitor shows -- RSS would
        count the shared system libraries, ~0.9 GB of it), Linux's VmRSS."""
        if platform.system() == "Darwin":
            out = subprocess.run(["vmmap", "--summary", str(self.proc.pid)], capture_output=True, text=True).stdout
            for line in out.splitlines():
                if line.startswith("Physical footprint:"):
                    v = line.split(":", 1)[1].strip()
                    num = float(v[:-1])
                    return num * {"K": 1 / 1024, "M": 1, "G": 1024}.get(v[-1], 1)
            return 0.0
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) / 1024.0
        except OSError:
            pass
        return 0.0

    def close(self):
        try:
            self.cmd("qa!")
            self.proc.wait(5)
        except (OSError, subprocess.TimeoutExpired):
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


WAIT_SYMBOLS = ("__workq_kernreturn", "poll  ", "mach_msg2_trap", "__psynch_cvwait", "kevent", "semaphore_wait",
                "__select", "__semwait", "start_wqthread", "__ulock_wait", "_kernelrpc_")


def summarize_sample(path, top=30):
    """The heaviest self-time functions of a macOS `sample` report, idle waits left out."""
    try:
        text = open(path, errors="replace").read()
    except OSError:
        return ""
    i = text.find("Sort by top of stack, same collapsed")
    if i < 0:
        return ""
    body = text[i:].split("Binary Images:")[0].splitlines()[1:]
    rows = [l.rstrip() for l in body if l.strip() and not any(w in l for w in WAIT_SYMBOLS)]
    return "\n".join(rows[:top])


def profile(args):
    fixture, _, scenario = args.profile.partition(":")
    fixtures = gen_fixtures("/nonexistent")
    if fixture not in fixtures or not scenario:
        print("--profile wants FIXTURE:SCENARIO with FIXTURE one of %s" % ", ".join(fixtures), file=sys.stderr)
        return 2
    work = tempfile.mkdtemp(prefix="ml", dir="/tmp" if os.path.isdir("/tmp") else None)
    path = gen_fixtures(work)[fixture]()
    home = tempfile.mkdtemp(prefix="h", dir=work)
    empty = os.path.join(home, "start.txt")
    open(empty, "w").close()
    outdir = os.path.join(bc.REPO, "bench_results", "profiles")
    os.makedirs(outdir, exist_ok=True)
    report = os.path.join(outdir, "%s_%s.txt" % (fixture, scenario))
    log = open(os.path.join(work, "mep.log"), "w")
    m = Mep(args.build_dir, home, empty, log)
    try:
        done = os.path.join(home, "done")
        cfg = "BENCH_LIVE = {fixture = {name = %s, path = %s}, out = %s, done = %s, loop = %s, seconds = %g}" % (
            lua_str(fixture), lua_str(path), lua_str(os.path.join(home, "r.jsonl")), lua_str(done), lua_str(scenario),
            args.seconds + 2)
        m.cmd("lua " + cfg)
        m.cmd("source " + os.path.join(bc.REPO, "tools", "bench_live.lua"))
        deadline = time.monotonic() + 120
        while not os.path.exists(done + ".ready") and time.monotonic() < deadline:
            time.sleep(0.1)
        secs = str(int(args.seconds))
        if platform.system() == "Darwin":
            subprocess.run(["sample", str(m.proc.pid), secs, "-mayDie", "-file", report], capture_output=True)
            print(summarize_sample(report))
        else:
            data = report.replace(".txt", ".perf.data")
            subprocess.run(["perf", "record", "-g", "-p", str(m.proc.pid), "-o", data, "--", "sleep", secs],
                           capture_output=True)
            with open(report, "w") as f:
                subprocess.run(["perf", "report", "-i", data, "--stdio", "--no-children"], stdout=f)
        hooks = os.path.join(home, "r.jsonl")
        deadline = time.monotonic() + 15
        while not os.path.exists(done) and time.monotonic() < deadline:
            time.sleep(0.2)
        if os.path.exists(hooks) and os.path.getsize(hooks):
            text = open(hooks).read()
            print("\nframe hooks (mep.hook_stats):\n" + text)
            with open(report.replace(".txt", "_hooks.txt"), "w") as f:
                f.write(text)
        print("\nprofile: %s" % report)
    finally:
        m.close()
        log.close()
        shutil.rmtree(work, ignore_errors=True)
    return 0


def run_fixture(args, name, path, work, log, display):
    """One fresh mep through tools/bench_live.lua on `path`: {scenario: (n, ms, extra)}."""
    results = {}
    home = tempfile.mkdtemp(prefix="h", dir=work)
    # Start on an empty buffer so the open itself is timed in Lua.
    empty = os.path.join(home, "start.txt")
    open(empty, "w").close()
    m = Mep(args.build_dir, home, empty, log, display)
    try:
        out = os.path.join(home, "results.jsonl")
        done = os.path.join(home, "done")
        cfg = "BENCH_LIVE = {fixture = {name = %s, path = %s}, out = %s, done = %s, quick = %s}" % (
            lua_str(name), lua_str(path), lua_str(out), lua_str(done), "true" if args.quick else "false")
        m.cmd("lua " + cfg)
        m.cmd("source " + os.path.join(bc.REPO, "tools", "bench_live.lua"))
        deadline = time.monotonic() + 600
        while not os.path.exists(done) and time.monotonic() < deadline and m.proc.poll() is None:
            time.sleep(0.25)
        if not os.path.exists(done):
            print("bench_live: %s did not finish (see %s)" % (name, log.name), file=sys.stderr)
        if os.path.exists(out):
            with open(out) as f:
                for line in f:
                    r = json.loads(line)
                    extra = {k: float(v) for k, v in r.items() if k not in ("fixture", "scenario", "n", "ms")}
                    results[r["scenario"]] = (int(r["n"]), float(r["ms"]), extra)
        m.cmd("lua collectgarbage('collect')")
        time.sleep(0.3)
        mem = m.memory_mb()
        results["memory_mb"] = (int(mem), 0.0, {"memory_mb": mem})
        # Idle: nothing touched for a while, then 10 s of mep's own CPU time
        # (all threads) -- ms of CPU per second of wall time, so lower is
        # better and 10 means 1% of one core.
        if not args.quick:
            time.sleep(12)
            c0 = m.cpu_seconds()
            w0 = time.monotonic()
            time.sleep(10)
            per_s = (m.cpu_seconds() - c0) / (time.monotonic() - w0) * 1000.0
            results["idle_cpu_ms_per_s"] = (1, per_s, {})
    finally:
        m.close()
    return results


def lua_str(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--filter", action="append", default=[])
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--no-record", action="store_true")
    ap.add_argument("--json", default="")
    ap.add_argument("--build-dir", default=os.path.join(bc.REPO, "build/native"))
    ap.add_argument("--display", default=":93")
    ap.add_argument("--profile", default="", help="FIXTURE:SCENARIO, e.g. cpp_60k:typing")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--rounds", type=int, default=1,
                    help="run each fixture this many times (fresh mep each) and record the median of each result")
    args = ap.parse_args()
    if args.profile:
        return profile(args)

    names = list(gen_fixtures("/nonexistent").keys())
    entries = ["live/startup"] + ["live/" + n for n in names]
    if args.list:
        print("\n".join(entries))
        return 0
    chosen = [e for e in entries if bc.entry_wanted(args.filter, e)]
    if not chosen:
        print("bench_live: no benchmark matches the filter", file=sys.stderr)
        return 0

    mep_bin = os.path.join(args.build_dir, "mep")
    if not os.path.exists(mep_bin):
        print("bench_live: %s not built" % mep_bin, file=sys.stderr)
        return 2

    # Short paths: the agent socket lives under XDG_DATA_HOME, and a Unix
    # socket path must fit in ~104 bytes ($TMPDIR on macOS alone is ~50).
    work = tempfile.mkdtemp(prefix="ml", dir="/tmp" if os.path.isdir("/tmp") else None)
    xvfb = None
    display = None
    if platform.system() == "Linux":
        if shutil.which("Xvfb"):
            display = args.display
            xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", "1600x1000x24"], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            time.sleep(1.0)
        elif not os.environ.get("DISPLAY"):
            print("bench_live: no Xvfb and no DISPLAY; skipped", file=sys.stderr)
            return 0

    records = []

    def record(fixture, scenario, n, ms, **extra):
        ident = "live/%s/%s" % (fixture, scenario)
        if not (bc.wanted(args.filter, ident) or bc.wanted(args.filter, "live/" + fixture)):
            return
        extra_str = "  ".join("%s %.3f" % (k, v) for k, v in extra.items() if isinstance(v, float))
        print("%-56s n=%-9d %10.3f ms  %s" % (ident, n, ms, extra_str), flush=True)
        rec = bc.make_record(BINARY, "%s/%s" % (fixture, scenario), ident, n, ms, **extra)
        if "p95" in extra:
            rec["p95_ms"] = extra["p95"]
        records.append(rec)

    log = open(os.path.join(work, "mep.log"), "w")
    fixtures = gen_fixtures(work)
    try:
        if "live/startup" in chosen:
            empty = os.path.join(work, "empty.txt")
            open(empty, "w").close()
            times = []
            for _ in range(2 if args.quick else 5):
                home = tempfile.mkdtemp(prefix="h", dir=work)
                m = Mep(args.build_dir, home, empty, log, display)
                times.append(m.startup_ms)
                m.close()
            times.sort()
            record("startup", "to_socket", 1, times[len(times) // 2], min=times[0], max=times[-1])
            # Until the first frame is on screen: what a person waits for.
            firsts = []
            for _ in range(2 if args.quick else 5):
                home = tempfile.mkdtemp(prefix="h", dir=work)
                t0 = time.monotonic()
                m = Mep(args.build_dir, home, empty, log, display)
                flag = os.path.join(home, "frames")
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    m.cmd("lua local f = io.open('%s', 'w') f:write(mep.frame_stats().frames) f:close()" % flag)
                    try:
                        if int(open(flag).read() or 0) > 0:
                            break
                    except (OSError, ValueError):
                        pass
                    time.sleep(0.01)
                firsts.append((time.monotonic() - t0) * 1000.0)
                m.close()
            firsts.sort()
            record("startup", "to_first_frame", 1, firsts[len(firsts) // 2], min=firsts[0], max=firsts[-1])

        for name in names:
            if "live/" + name not in chosen:
                continue
            path = fixtures[name]()
            rounds = []  # one {scenario: (n, ms, extra)} per round
            for _ in range(max(1, args.rounds)):
                rounds.append(run_fixture(args, name, path, work, log, display))
            for scenario in rounds[0]:
                vals = sorted((r[scenario] for r in rounds if scenario in r), key=lambda v: v[1] + v[2].get("memory_mb", 0))
                n, ms, extra = vals[len(vals) // 2]
                if len(vals) > 1:
                    extra = dict(extra, rounds=float(len(vals)), spread=(vals[-1][1] - vals[0][1]))
                record(name, scenario, n, ms, **extra)
    finally:
        log.close()
        if xvfb:
            xvfb.terminate()

    if not args.no_record:
        bc.append_records(bc.HISTORY, records)
    if args.json:
        bc.append_records(args.json, records)
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
