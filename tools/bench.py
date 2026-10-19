#!/usr/bin/env python3
"""mep's benchmark suite runner (plans/PERFORMANCE_PLAN.md).

One front end over every benchmark mep has:

  mep-bench                  editor core, syntax, regex, codecs, search, git, documents (src/bench/)
  mep-buffer-bench           the vector<string> buffer shape         (src/text_buffer_bench.cpp)
  mep-crdt-bench             the collaboration CRDT                  (src/text_crdt_bench.cpp)
  mep-lua-frame-hook-bench   Lua frame-hook dispatch                 (src/lua_frame_hook_bench.cpp)
  mep-vterm-bench            the terminal emulator                   (src/vterm_bench.cpp)
  mep-pdf-bench              PDF render/text (needs `just pdf-fixtures`)
  mep-mepml-bench            the mepml toolchain                     (src/mepml_bench.cpp)
  live                       a real running mep: startup, open, hooks, frame cost, memory (tools/bench_live.py)

Every benchmark has an id like `editor/cpp_60k` (an entry) and
`editor/cpp_60k/substitute_global` (one measurement in it). Patterns
are globs; one without glob characters is a prefix.

  bench.py list [PAT...]                     list entries
  bench.py run [PAT...] [options]            build what is needed, run, print a summary
      --quick                fewer repetitions (a fast smoke run)
      --no-record            do not append to bench_results/history.jsonl
      --no-build             use the binaries as they are
      --no-live              skip the live-editor tier (it opens a window on macOS)
      --save NAME            also save this run as baseline NAME (bench_results/baselines/NAME.jsonl)
      --compare NAME         compare this run against baseline NAME instead of the previous run
      --threshold PCT        changes smaller than this are not flagged (default 5)
      --rounds N             live tier: N fresh mep runs per fixture, median recorded (steadier)
  bench.py compare BASE [NEW] [PAT...]       compare two saved baselines (NEW defaults to the latest run)
  bench.py report [PAT...]                   latest vs previous record per benchmark, from history.jsonl
  bench.py ab (--ref REV | --before-dir DIR) [PAT...] [--rounds N] [--quick]
                                             A/B: the same benchmarks from two builds, interleaved round by
                                             round (so machine load hits both alike), medians compared.
                                             --ref builds REV in a git worktree (build/bench-ab/REV);
                                             nothing is recorded to history.

Run from anywhere; paths resolve against the repo root. `just bench`,
`just bench-list`, `just bench-save`, `just bench-compare` wrap this.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_common as bc  # noqa: E402

BUILD_DIR = os.path.join(bc.REPO, "build", "native")
BASELINES = os.path.join(bc.REPO, "bench_results", "baselines")
LAST_RUN = os.path.join(bc.REPO, "bench_results", "last_run.jsonl")

NATIVE = [
    "mep-bench",
    "mep-buffer-bench",
    "mep-crdt-bench",
    "mep-lua-frame-hook-bench",
    "mep-vterm-bench",
    "mep-pdf-bench",
    "mep-mepml-bench",
]


class Suite:
    def __init__(self, name, target, argv):
        self.name = name
        self.target = target  # cmake target to build, or None
        self.argv = argv
        self._entries = None

    def exists(self):
        return os.path.exists(self.argv[-1]) if self.target else True

    def entries(self):
        if self._entries is None:
            if not self.exists():
                self._entries = []
            else:
                r = subprocess.run(self.argv + ["--list"], cwd=bc.REPO, capture_output=True, text=True)
                self._entries = [l.strip() for l in r.stdout.splitlines() if l.strip()]
        return self._entries


def suites(build_dir, live=True):
    out = [Suite(n, n, [os.path.join(build_dir, n)]) for n in NATIVE]
    if live:
        out.append(Suite("live", None, [sys.executable, os.path.join(bc.REPO, "tools", "bench_live.py"),
                                        "--build-dir", build_dir]))
    return out


def build(targets, build_dir):
    if not targets:
        return True
    print("== building " + " ".join(targets), flush=True)
    r = subprocess.run(["cmake", "--build", build_dir, "-j", "--target"] + targets, cwd=bc.REPO)
    return r.returncode == 0


def latest_by_id(records):
    """key -> latest record, keyed by bc.record_key (displayed by its suite id)."""
    out = {}
    for r in records:
        out[bc.record_key(r)] = r
    return out


def fmt_ms(ms):
    if ms >= 1000:
        return "%.2f s" % (ms / 1000)
    if ms >= 1:
        return "%.2f ms" % ms
    return "%.1f us" % (ms * 1000)


def print_comparison(new, old, threshold, title_old):
    """new/old: id -> record. Prints a table and returns (faster, slower) counts."""
    ids = sorted(new, key=lambda k: bc.record_id(new[k]))
    if not ids:
        print("(no results)")
        return 0, 0
    width = max(len(bc.record_id(new[i])) for i in ids) + 2
    print("\n%-*s %12s %12s %9s" % (width, "benchmark", "now", title_old, "change"))
    print("-" * (width + 36))
    faster = slower = 0
    for i in ids:
        r = new[i]
        name = bc.record_id(r)
        if name.endswith("/memory_mb"):
            now_mb = r.get("memory_mb", r["n"])
            o = old.get(i)
            before = "%.0f MB" % o.get("memory_mb", o["n"]) if o else "-"
            change = ""
            if o and o.get("memory_mb", o["n"]):
                change = "%+.1f%%" % ((now_mb - o.get("memory_mb", o["n"])) / o.get("memory_mb", o["n"]) * 100.0)
            print("%-*s %12s %12s %9s" % (width, name, "%.0f MB" % now_mb, before, change))
            continue
        o = old.get(i)
        change = ""
        if o and o.get("ms", 0) > 0 and r["ms"] > 0:
            pct = (r["ms"] - o["ms"]) / o["ms"] * 100.0
            mark = ""
            if pct <= -threshold:
                mark = "  faster"
                faster += 1
            elif pct >= threshold:
                mark = "  SLOWER"
                slower += 1
            change = "%+.1f%%%s" % (pct, mark)
        print("%-*s %12s %12s %9s" % (width, name, fmt_ms(r["ms"]), fmt_ms(o["ms"]) if o else "-", change))
    return faster, slower


def cmd_list(args):
    for s in suites(args.build_dir, live=True):
        for e in s.entries():
            if not args.patterns or bc.entry_wanted(args.patterns, e):
                print("%-40s %s" % (e, s.name))
    missing = [s.name for s in suites(args.build_dir) if not s.exists()]
    if missing:
        print("\n(not built yet, so their entries are not listed: %s -- `just bench-build`)" % ", ".join(missing),
              file=sys.stderr)
    return 0


def cmd_run(args):
    all_suites = suites(args.build_dir, live=not args.no_live)
    pats = args.patterns
    if not args.no_build:
        # Every native bench binary (an unbuilt one cannot be asked what it
        # contains), plus mep itself when the live tier is in the selection.
        targets = [s.target for s in all_suites if s.target]
        live = [s for s in all_suites if s.name == "live"]
        if live and any(bc.entry_wanted(pats, e) for e in live[0].entries()):
            targets.append("mep")
        if not build(targets, args.build_dir):
            print("build failed", file=sys.stderr)
            return 1
        for s in all_suites:
            s._entries = None

    collect = tempfile.mktemp(prefix="mep-bench-", suffix=".jsonl")
    ran = []
    for s in all_suites:
        entries = s.entries()
        if not entries or not any(bc.entry_wanted(pats, e) for e in entries):
            continue
        argv = list(s.argv)
        for p in pats:
            argv += ["--filter", p]
        if args.quick:
            argv.append("--quick")
        if args.no_record:
            argv.append("--no-record")
        if s.name == "live" and args.rounds > 1:
            argv += ["--rounds", str(args.rounds)]
        argv += ["--json", collect]
        print("\n== %s" % s.name, flush=True)
        r = subprocess.run(argv, cwd=bc.REPO)
        if r.returncode != 0:
            print("   (%s exited with status %d)" % (s.name, r.returncode))
        ran.append(s.name)
    if not ran:
        print("no benchmark matches %s" % " ".join(pats), file=sys.stderr)
        return 1

    records = bc.load_records(collect)
    if os.path.exists(collect):
        os.remove(collect)
    new = latest_by_id(records)
    # The run itself, for `bench.py compare BASE` without a second name.
    with open(LAST_RUN, "w"):
        pass
    bc.append_records(LAST_RUN, records)
    if args.save:
        # Merged into an existing baseline of that name: re-running a subset
        # replaces those results and keeps the rest.
        os.makedirs(BASELINES, exist_ok=True)
        path = os.path.join(BASELINES, args.save + ".jsonl")
        kept = [r for r in bc.load_records(path) if bc.record_key(r) not in new]
        if os.path.exists(path):
            os.remove(path)
        bc.append_records(path, kept + records)
        print("\nsaved baseline %s (%d results, %d kept from before)" % (path, len(records), len(kept)))

    if args.compare:
        old = bc.match_old(new, bc.load_records(os.path.join(BASELINES, args.compare + ".jsonl")))
        title = args.compare
    else:
        # The previous record of each benchmark in history (not this run's own).
        mine = {(r.get("timestamp"), r.get("id"), r.get("ms")) for r in records}
        history = [r for r in bc.load_records(bc.HISTORY) if (r.get("timestamp"), r.get("id"), r.get("ms")) not in mine]
        old = bc.match_old(new, history)
        title = "previous"
    faster, slower = print_comparison(new, old, args.threshold, title)
    print("\n%d results; %d faster, %d slower than %s (threshold %g%%)" % (len(new), faster, slower, title,
                                                                         args.threshold))
    return 0


def cmd_compare(args):
    def load(name):
        if name == "last":
            return latest_by_id(bc.load_records(LAST_RUN))
        path = name if os.path.exists(name) else os.path.join(BASELINES, name + ".jsonl")
        recs = bc.load_records(path)
        if not recs:
            print("no baseline %s" % path, file=sys.stderr)
        return latest_by_id(recs)

    new = load(args.new)
    if args.patterns:
        new = {i: r for i, r in new.items() if bc.wanted(args.patterns, bc.record_id(r))}
    old = bc.match_old(new, list(load(args.base).values()))
    faster, slower = print_comparison(new, old, args.threshold, args.base[:12])
    print("\n%d results; %d faster, %d slower than %s" % (len(new), faster, slower, args.base))
    return 0


def native_cmake_flags(build_dir):
    """The generator and compilers build_dir was configured with, so a second tree builds the same way."""
    flags = []
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt")) as f:
            for line in f:
                for key in ("CMAKE_GENERATOR:INTERNAL", "CMAKE_C_COMPILER:", "CMAKE_CXX_COMPILER:",
                            "CMAKE_OBJCXX_COMPILER:", "CMAKE_BUILD_TYPE:"):
                    if line.startswith(key):
                        name, _, value = line.strip().partition("=")
                        name = name.split(":")[0]
                        if name == "CMAKE_GENERATOR":
                            flags += ["-G", value]
                        elif value:
                            flags.append("-D%s=%s" % (name, value))
    except OSError:
        pass
    return flags


def prepare_ref(rev, after_build_dir):
    """A build of git revision `rev` in a worktree under build/bench-ab/, configured like the current build."""
    sha = subprocess.run(["git", "rev-parse", "--short", rev], cwd=bc.REPO, capture_output=True, text=True).stdout.strip()
    if not sha:
        print("bench ab: unknown revision %s" % rev, file=sys.stderr)
        return None
    tree = os.path.join(bc.REPO, "build", "bench-ab", sha)
    if not os.path.exists(tree):
        os.makedirs(os.path.dirname(tree), exist_ok=True)
        r = subprocess.run(["git", "worktree", "add", "--detach", tree, sha], cwd=bc.REPO)
        if r.returncode != 0:
            return None
    build_dir = os.path.join(tree, "build")
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        subprocess.run(["cmake", "-S", tree, "-B", build_dir] + native_cmake_flags(after_build_dir), check=False)
    return build_dir


def cmd_ab(args):
    if not args.ref and not args.before_dir:
        print("bench ab: give --ref REV or --before-dir DIR", file=sys.stderr)
        return 2
    after_dir = args.build_dir
    before_dir = args.before_dir or prepare_ref(args.ref, after_dir)
    if not before_dir:
        return 1
    pats = args.patterns
    sides = {"before": suites(before_dir, live=not args.no_live), "after": suites(after_dir, live=not args.no_live)}
    if not args.no_build:
        for name, ss in sides.items():
            d = before_dir if name == "before" else after_dir
            targets = [s.target for s in ss if s.target and os.path.exists(os.path.join(d, "build.ninja"))]
            live = [s for s in ss if s.name == "live"]
            if live and any(bc.entry_wanted(pats, e) for e in live[0].entries()):
                targets.append("mep")
            if targets and not build(sorted(set(targets)), d):
                print("bench ab: %s build failed" % name, file=sys.stderr)
                return 1
            for s in ss:
                s._entries = None
    results = {"before": [], "after": []}
    for rnd in range(max(1, args.rounds)):
        # Alternate which side goes first, so neither always runs on the
        # warmer (or busier) machine.
        order = ["before", "after"] if rnd % 2 == 0 else ["after", "before"]
        for side in order:
            collect = tempfile.mktemp(prefix="mep-bench-ab-", suffix=".jsonl")
            for s in sides[side]:
                entries = s.entries()
                if not entries or not any(bc.entry_wanted(pats, e) for e in entries):
                    continue
                argv = list(s.argv)
                for p in pats:
                    argv += ["--filter", p]
                if args.quick:
                    argv.append("--quick")
                argv += ["--no-record", "--json", collect]
                print("== round %d, %s: %s" % (rnd + 1, side, s.name), flush=True)
                subprocess.run(argv, cwd=bc.REPO, stdout=subprocess.DEVNULL)
            results[side].append(latest_by_id(bc.load_records(collect)))
            if os.path.exists(collect):
                os.remove(collect)

    def median_of(runs):
        out = {}
        for k in set().union(*[r.keys() for r in runs]) if runs else []:
            vals = sorted((r[k] for r in runs if k in r), key=lambda x: x.get("ms", 0) + x.get("memory_mb", 0))
            out[k] = vals[len(vals) // 2]
        return out

    new = median_of(results["after"])
    old = median_of(results["before"])
    title = "before" if not args.ref else args.ref[:12]
    faster, slower = print_comparison(new, old, args.threshold, title)
    print("\n%d results over %d round(s); %d faster, %d slower than %s (threshold %g%%)" % (
        len(new), max(1, args.rounds), faster, slower, title, args.threshold))
    if args.save:
        os.makedirs(BASELINES, exist_ok=True)
        for side, recs in (("before", old), ("after", new)):
            path = os.path.join(BASELINES, "%s-%s.jsonl" % (args.save, side))
            if os.path.exists(path):
                os.remove(path)
            bc.append_records(path, list(recs.values()))
        print("saved %s-before / %s-after in %s" % (args.save, args.save, BASELINES))
    return 0


def cmd_report(args):
    history = bc.load_records(bc.HISTORY)
    # The latest record of each benchmark that has an id, against the
    # history before it (records from before ids existed still match).
    latest = {}
    for idx, r in enumerate(history):
        if r.get("id"):
            latest[r["id"]] = idx
    new = {i: history[idx] for i, idx in latest.items() if not args.patterns or bc.wanted(args.patterns, i)}
    old = {}
    for i, r in new.items():
        prior = bc.match_old({i: r}, history[:latest[i]])
        if i in prior:
            old[i] = prior[i]
    faster, slower = print_comparison(new, old, args.threshold, "previous")
    print("\n%d benchmarks; %d faster, %d slower than their previous run" % (len(new), faster, slower))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=BUILD_DIR)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("list")
    p.add_argument("patterns", nargs="*")

    p = sub.add_parser("run")
    p.add_argument("patterns", nargs="*")
    p.add_argument("--quick", action="store_true")
    p.add_argument("--no-record", action="store_true")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--no-live", action="store_true")
    p.add_argument("--save", default="")
    p.add_argument("--compare", default="")
    p.add_argument("--threshold", type=float, default=5.0)
    p.add_argument("--rounds", type=int, default=1, help="live tier: fresh mep runs per fixture (median recorded)")

    p = sub.add_parser("compare")
    p.add_argument("base")
    p.add_argument("new", nargs="?", default="last")
    p.add_argument("patterns", nargs="*")
    p.add_argument("--threshold", type=float, default=5.0)

    p = sub.add_parser("ab")
    p.add_argument("patterns", nargs="*")
    p.add_argument("--ref", default="")
    p.add_argument("--before-dir", default="")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--quick", action="store_true")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--no-live", action="store_true")
    p.add_argument("--threshold", type=float, default=5.0)
    p.add_argument("--save", default="", help="also save both sides as baselines NAME-before / NAME-after")

    p = sub.add_parser("report")
    p.add_argument("patterns", nargs="*")
    p.add_argument("--threshold", type=float, default=5.0)

    args = ap.parse_args()
    if args.cmd is None:
        ap.print_help()
        return 0
    if not shutil.which("cmake") and args.cmd == "run" and not getattr(args, "no_build", True):
        print("cmake not found; pass --no-build", file=sys.stderr)
        return 1
    return {"list": cmd_list, "run": cmd_run, "compare": cmd_compare, "report": cmd_report, "ab": cmd_ab}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
