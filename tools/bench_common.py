"""Shared pieces of mep's benchmark suite (plans/PERFORMANCE_PLAN.md):
benchmark-id filtering with the same rules as src/bench_util.h, and
appending records to bench_results/history.jsonl in the same shape the
C++ bench binaries write."""

import datetime
import fnmatch
import json
import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HISTORY = os.path.join(REPO, "bench_results", "history.jsonl")


def glob_match(pattern, ident):
    """A pattern with no glob characters is a prefix (`editor` matches `editor/cpp_60k`)."""
    if not any(c in pattern for c in "*?"):
        pattern += "*"
    return fnmatch.fnmatchcase(ident, pattern)


def wanted(filters, ident):
    return not filters or any(glob_match(f, ident) for f in filters)


def entry_wanted(filters, entry):
    """Whether entry `entry` could hold a wanted measurement (see EntryWanted in bench_util.h)."""
    if not filters:
        return True
    depth = entry.count("/") + 1
    for f in filters:
        if glob_match(f, entry):
            return True
        parts = f.split("/")
        if len(parts) > depth and fnmatch.fnmatchcase(entry, "/".join(parts[:depth])):
            return True
    return False


def git_commit():
    try:
        h = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=REPO, capture_output=True,
                               text=True).stdout.strip()
        return (h or "unknown") + ("+dirty" if h and dirty else "")
    except OSError:
        return "unknown"


def make_record(binary, scenario, ident, n, ms, **extra):
    rec = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": git_commit(),
        "binary": binary,
        "scenario": scenario,
        "id": ident,
        "n": n,
        "ms": ms,
        "ops_per_sec": (n / ms * 1000.0) if ms > 0 else 0.0,
    }
    rec.update(extra)
    return rec


def append_records(path, records):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a") as f:
        for r in records:
            f.write(json.dumps(r, separators=(",", ":")) + "\n")


def load_records(path):
    records = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    except FileNotFoundError:
        pass
    return records


def record_id(r):
    """A record's suite id; older records (before ids existed) get `binary/scenario`."""
    return r.get("id") or "%s/%s" % (r.get("binary", "?"), r.get("scenario", "?"))


def record_key(r):
    """What identifies "the same benchmark" across runs: the suite id when the record has one; older records
    (before ids existed) fall back to (binary, scenario) -- see match_old for how the two are bridged."""
    return r["id"] if r.get("id") else "%s\x00%s" % (r.get("binary", "?"), r.get("scenario", "?"))


def legacy_key(r):
    return "%s\x00%s" % (r.get("binary", "?"), r.get("scenario", "?"))


def match_old(new, old_records):
    """For each key in `new` (key -> record), the latest matching record in `old_records`: by id, or, for
    records written before ids existed, by (binary, scenario) when that pair names one benchmark in `new`."""
    pairs = {}
    for k, r in new.items():
        pairs.setdefault(legacy_key(r), []).append(k)
    out = {}
    for r in old_records:
        if r.get("id"):
            if r["id"] in new:
                out[r["id"]] = r
        else:
            ks = pairs.get(legacy_key(r), [])
            if len(ks) == 1:
                out[ks[0]] = r
    return out
