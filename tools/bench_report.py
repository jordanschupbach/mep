#!/usr/bin/env python3
"""Summarizes bench_results/history.jsonl (CRDT_PERFORMANCE_PLAN.md
Phase 1): for each (binary, scenario) pair, prints the latest run's
ops/sec next to the previous run's, with a percent delta, so a
regression or a win is visible at a glance without hand-diffing JSON.
history.jsonl itself is append-only (mep-crdt-bench/mep-buffer-bench
each add one line per scenario per run, via src/bench_util.h's
RecordResult) -- this script only ever reads it.

Usage: bench_report.py [path/to/history.jsonl]
"""
import json
import sys
from collections import defaultdict

DEFAULT_PATH = "bench_results/history.jsonl"


def load_records(path):
    records = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                records.append(json.loads(line))
    except FileNotFoundError:
        return records
    return records


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PATH
    records = load_records(path)
    if not records:
        print(f"no records in {path} yet -- run `just bench` first")
        return 0

    # Group by (binary, scenario), keeping every run in arrival order --
    # history.jsonl is append-only so this is also chronological order.
    by_key = defaultdict(list)
    for r in records:
        by_key[(r["binary"], r["scenario"])].append(r)

    print(f"{len(records)} records, {len(by_key)} scenarios, from {path}\n")
    header = f"{'binary':<20}{'scenario':<45}{'n':>10}{'latest ops/sec':>18}{'prev ops/sec':>16}{'delta':>10}"
    print(header)
    print("-" * len(header))
    for (binary, scenario), runs in sorted(by_key.items()):
        latest = runs[-1]
        prev = runs[-2] if len(runs) > 1 else None
        delta = ""
        if prev and prev["ops_per_sec"] > 0:
            pct = (latest["ops_per_sec"] - prev["ops_per_sec"]) / prev["ops_per_sec"] * 100.0
            delta = f"{pct:+.1f}%"
        prev_str = f"{prev['ops_per_sec']:,.1f}" if prev else "-"
        print(f"{binary:<20}{scenario:<45}{latest['n']:>10}{latest['ops_per_sec']:>18,.1f}{prev_str:>16}{delta:>10}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
