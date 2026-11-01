#!/usr/bin/env python3
"""Measure mep's SQL parser (src/sql_parse.cpp, `mep-sql-lsp --chunks`)
against SQLite itself.

The oracle is libsqlite3 through ctypes: the script is cut into statements
with sqlite3_complete() (as the sqlite3 shell cuts it), each statement goes
through sqlite3_prepare_v2(), and sqlite3_errmsg() / sqlite3_error_offset()
say whether it parsed and where it failed. A statement that prepares is
executed when it is DDL (CREATE/DROP/ALTER), so later statements see the
schema; an error that is not one of SQLite's three syntax messages
("near X: syntax error", "incomplete input", "unrecognized token") is a
semantic error after a successful parse.

  sql_parse_check.py extract SQLITE_SRC_DIR OUT_DIR
      write the SQL inside the test suite's execsql/catchsql/do_execsql_test
      blocks (sqlite-src/test/*.test) as one file per block
  sql_parse_check.py check --lsp BIN FILE_OR_DIR...
      per-statement agreement: verdict, message, and error offset
  sql_parse_check.py mutate --lsp BIN [--limit N] FILE_OR_DIR...
      delete one token from statements SQLite accepts; of the mutants SQLite
      rejects as a syntax error, how many mep also reports
  sql_parse_check.py lint --lsp BIN FILE_OR_DIR...
      false-positive calibration of `BIN --lint`: every diagnostic other
      than a syntax error, counted by code against SQLite's verdict on the
      statement it falls in (prepared fine: a false positive unless the
      code is advisory; failed with a semantic error: a true positive)
  sql_parse_check.py format --fmt BIN FILE_OR_DIR...
      run mep's formatter (`mep-sql-format-test --format`) over every file:
      refusals, idempotence, and whether SQLite gives every statement the
      same verdict (ok / which error) before and after

LIBSQLITE (env) names the libsqlite3.so to load (default: ctypes.util).
"""
import ctypes
import ctypes.util
import json
import os
import random
import re
import subprocess
import sys

SYNTAX_RE = re.compile(r'^(near ".*": syntax error|incomplete input|unrecognized token: .*)$', re.S)


def load_sqlite():
    path = os.environ.get("LIBSQLITE") or ctypes.util.find_library("sqlite3")
    lib = ctypes.CDLL(path)
    lib.sqlite3_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.sqlite3_close_v2.argtypes = [ctypes.c_void_p]
    lib.sqlite3_prepare_v2.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]
    lib.sqlite3_errmsg.argtypes = [ctypes.c_void_p]
    lib.sqlite3_errmsg.restype = ctypes.c_char_p
    lib.sqlite3_error_offset.argtypes = [ctypes.c_void_p]
    lib.sqlite3_step.argtypes = [ctypes.c_void_p]
    lib.sqlite3_finalize.argtypes = [ctypes.c_void_p]
    lib.sqlite3_complete.argtypes = [ctypes.c_char_p]
    lib.sqlite3_stmt_readonly.argtypes = [ctypes.c_void_p]
    PROGRESS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
    lib.sqlite3_progress_handler.argtypes = [ctypes.c_void_p, ctypes.c_int, PROGRESS, ctypes.c_void_p]
    lib.PROGRESS = PROGRESS
    return lib


LIB = None


class Oracle:
    def __init__(self):
        self.db = ctypes.c_void_p()
        LIB.sqlite3_open(b":memory:", ctypes.byref(self.db))
        self.steps = [0]

        def progress(_):
            self.steps[0] += 1
            return 1 if self.steps[0] > 200 else 0

        self._cb = LIB.PROGRESS(progress)
        LIB.sqlite3_progress_handler(self.db, 1000, self._cb, None)

    def close(self):
        LIB.sqlite3_close_v2(self.db)

    def prepare(self, sql: bytes):
        """('ok'|'syntax'|'semantic', message, offset) for the first statement of sql."""
        stmt = ctypes.c_void_p()
        tail = ctypes.c_char_p()
        buf = ctypes.create_string_buffer(sql, len(sql) + 1)
        rc = LIB.sqlite3_prepare_v2(self.db, buf, len(sql), ctypes.byref(stmt), ctypes.byref(tail))
        if rc != 0:
            msg = LIB.sqlite3_errmsg(self.db).decode("utf-8", "replace")
            off = LIB.sqlite3_error_offset(self.db)
            return ("syntax" if SYNTAX_RE.match(msg) else "semantic", msg, off)
        if stmt.value:
            head = sql.lstrip().split(None, 1)[0].upper() if sql.strip() else b""
            if head in (b"CREATE", b"DROP", b"ALTER") and not LIB.sqlite3_stmt_readonly(stmt):
                self.steps[0] = 0
                LIB.sqlite3_step(stmt)
            LIB.sqlite3_finalize(stmt)
        return ("ok", "", -1)


def split_complete(data: bytes):
    """Statement byte ranges as sqlite3_complete() cuts them."""
    out = []
    start = 0
    i = 0
    while True:
        j = data.find(b";", i)
        if j < 0:
            break
        if LIB.sqlite3_complete(data[start:j + 1]):
            out.append((start, j + 1))
            start = j + 1
        i = j + 1
    if data[start:].strip() or not out:
        out.append((start, len(data)))
    return out


def mine_chunks(lsp, path):
    r = subprocess.run([lsp, "--chunks", path], capture_output=True)
    rows = []
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split("\t", 4)
        if len(parts) < 5:
            continue
        rows.append((int(parts[0]), int(parts[1]), parts[2], int(parts[3]), parts[4]))
    return rows


def files_of(args):
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                for n in sorted(names):
                    yield os.path.join(root, n)
        else:
            yield a


def cmd_extract(src, out):
    os.makedirs(out, exist_ok=True)
    pat = re.compile(rb"(?:\b(?:execsql|catchsql)\s+|\b(?:do_execsql_test|do_catchsql_test|do_eqp_test)\s+"
                     rb"(?:-\w+\s+\S+\s+)*\S+\s+|\bdb\d?\s+eval\s+)\{")
    n = 0
    tdir = os.path.join(src, "test")
    for name in sorted(os.listdir(tdir)):
        if not name.endswith(".test"):
            continue
        data = open(os.path.join(tdir, name), "rb").read()
        k = 0
        for m in pat.finditer(data):
            i = m.end()
            depth = 1
            j = i
            while j < len(data) and depth:
                c = data[j]
                if c == 0x5C:  # backslash
                    j += 2
                    continue
                if c == 0x7B:
                    depth += 1
                elif c == 0x7D:
                    depth -= 1
                j += 1
            body = data[i:j - 1]
            if not body.strip():
                continue
            k += 1
            with open(os.path.join(out, "%s.%04d.sql" % (name[:-5], k)), "wb") as f:
                f.write(body)
            n += 1
    print("wrote %d blocks" % n)


def cmd_check(lsp, paths, verbose):
    stats = dict(statements=0, agree=0, both_ok=0, both_syntax=0, same_message=0, same_offset=0,
                 false_syntax=0, missed=0, unverifiable=0, boundary_mismatch=0, files=0, files_agree=0)
    bad = []
    for path in files_of(paths):
        data = open(path, "rb").read()
        if b"\0" in data:
            continue
        stats["files"] += 1
        oracle = Oracle()
        ranges = split_complete(data)
        mine = mine_chunks(lsp, path)
        if [(a, b) for a, b, *_ in mine] != ranges:
            stats["boundary_mismatch"] += 1
            if verbose:
                bad.append((path, "boundaries", ranges[:5], [(a, b) for a, b, *_ in mine][:5]))
            oracle.close()
            continue
        file_ok = True
        for (a, b), (_, _, status, off, msg) in zip(ranges, mine):
            chunk = data[a:b]
            o_status, o_msg, o_off = oracle.prepare(chunk)
            o_msg = o_msg.replace("\n", "\\n")
            stats["statements"] += 1
            m_syntax = status == "syntax"
            if o_status == "syntax":
                if m_syntax:
                    stats["both_syntax"] += 1
                    stats["agree"] += 1
                    if msg == o_msg:
                        stats["same_message"] += 1
                    if off == o_off:
                        stats["same_offset"] += 1
                    if (msg != o_msg or off != o_off):
                        file_ok = False
                        bad.append((path, "message", a, o_msg, o_off, msg, off))
                else:
                    stats["missed"] += 1
                    file_ok = False
                    bad.append((path, "missed", a, o_msg, o_off))
            elif o_status == "ok":
                if m_syntax:
                    stats["false_syntax"] += 1
                    file_ok = False
                    bad.append((path, "false", a, msg, off))
                else:
                    stats["both_ok"] += 1
                    stats["agree"] += 1
            else:
                if m_syntax:
                    stats["unverifiable"] += 1
                    bad.append((path, "unverifiable", a, o_msg, o_off, msg, off))
                else:
                    stats["agree"] += 1
                    stats["both_ok"] += 1
        if file_ok:
            stats["files_agree"] += 1
        oracle.close()
    print(json.dumps(stats, indent=1))
    if verbose:
        for b in bad[:verbose]:
            print(b)


def tokens_of(lsp, path):
    r = subprocess.run([lsp, "--tokens", path], capture_output=True)
    toks = []
    for line in r.stdout.decode().splitlines():
        a, b, t = line.split("\t")
        toks.append((int(a), int(b), t))
    return toks


def cmd_mutate(lsp, paths, limit, seed, verbose):
    rnd = random.Random(seed)
    statements = []
    for path in files_of(paths):
        data = open(path, "rb").read()
        if b"\0" in data:
            continue
        oracle = Oracle()
        for a, b in split_complete(data):
            chunk = data[a:b]
            st, _, _ = oracle.prepare(chunk)
            if st == "ok" and chunk.strip(b" \t\r\n;"):
                statements.append(chunk)
        oracle.close()
    rnd.shuffle(statements)
    tmp = "/tmp/sql_mutant_%d.sql" % os.getpid()
    stats = dict(mutants=0, rejected=0, killed=0, valid_mutants=0, false_syntax=0)
    misses = []
    for chunk in statements:
        if stats["mutants"] >= limit:
            break
        with open(tmp, "wb") as f:
            f.write(chunk)
        toks = [t for t in tokens_of(lsp, tmp) if t[2] not in ("comment", "eof")]
        if not toks:
            continue
        a, b, _ = toks[rnd.randrange(len(toks))]
        mutant = chunk[:a] + chunk[b:]
        stats["mutants"] += 1
        oracle = Oracle()
        st, msg, off = oracle.prepare(mutant)
        oracle.close()
        with open(tmp, "wb") as f:
            f.write(mutant)
        mine = mine_chunks(lsp, tmp)
        m_syntax = any(r[2] == "syntax" for r in mine)
        if st == "syntax":
            stats["rejected"] += 1
            if m_syntax:
                stats["killed"] += 1
            else:
                misses.append((mutant[:300], msg))
        elif st == "ok":
            stats["valid_mutants"] += 1
            if m_syntax:
                stats["false_syntax"] += 1
                misses.append(("FALSE", mutant[:300], [r for r in mine if r[2] == "syntax"]))
    os.unlink(tmp) if os.path.exists(tmp) else None
    print(json.dumps(stats, indent=1))
    for m in misses[:verbose]:
        print(m)


def verdicts(data):
    oracle = Oracle()
    out = []
    for a, b in split_complete(data):
        chunk = data[a:b]
        if not chunk.strip():
            continue
        st, msg, _ = oracle.prepare(chunk)
        out.append((st, msg if st != "ok" else ""))
    oracle.close()
    # whitespace/comment-only statements are not statements
    return [v for v in out]


ADVISORY = {"compare-null", "unused-cte", "double-quoted-string", "unknown-pragma", "duplicate-alias",
            "unknown-function", "database-unavailable"}


def cmd_lint(lsp, paths, verbose):
    import collections
    fp = collections.Counter()
    tp = collections.Counter()
    advisory = collections.Counter()
    other = collections.Counter()
    deferred = collections.Counter()
    sem_total = 0
    sem_caught = 0
    examples = collections.defaultdict(list)
    files = 0
    for path in files_of(paths):
        data = open(path, "rb").read()
        if b"\0" in data:
            continue
        files += 1
        starts = [0]
        for i, ch in enumerate(data):
            if ch == 0x0A:
                starts.append(i + 1)
        r = subprocess.run([lsp, "--lint", path], capture_output=True)
        diags = []
        for line in r.stdout.decode("utf-8", "replace").splitlines():
            m = re.match(r"^(.*):(\d+):(\d+): (\w+): (.*) \[([\w-]+)\]$", line)
            if not m or m.group(6) in ("syntax-error", "invalid-clause"):
                continue
            ln, col = int(m.group(2)) - 1, int(m.group(3)) - 1
            off = (starts[ln] if ln < len(starts) else len(data)) + col
            diags.append((off, m.group(4), m.group(6), m.group(5)))
        oracle = Oracle()
        verdict = []
        for a, b in split_complete(data):
            st, msg, _ = oracle.prepare(data[a:b])
            verdict.append((a, b, st, msg))
            if st == "semantic":
                sem_total += 1
        oracle.close()
        caught = set()
        for off, sev, code, msg in diags:
            v = next((x for x in verdict if x[0] <= off < x[1]), None)
            if v is None:
                continue
            head = re.sub(rb"(--[^\n]*\n|/\*.*?\*/|\s)+", b" ", data[v[0]:v[1]], flags=re.S).strip().upper()
            if v[2] == "ok" and re.match(rb"^CREATE\s+(TEMP\s+|TEMPORARY\s+)?(VIEW|TRIGGER)\b", head):
                # SQLite resolves a view's or trigger's names when it is used, not when it is created: no verdict
                deferred[code] += 1
            elif v[2] == "ok":
                if code in ADVISORY:
                    advisory[code] += 1
                else:
                    fp[code] += 1
                    if len(examples[code]) < 8:
                        examples[code].append((path, msg, data[v[0]:v[1]][:200]))
            elif v[2] == "semantic":
                tp[code] += 1
                caught.add(v[0])
            else:
                other[code] += 1
        sem_caught += len(caught)
    print(json.dumps(dict(files=files, false_positive=fp, true_positive=tp, advisory_on_valid=advisory,
                          on_syntax_error_statements=other, in_view_or_trigger_bodies=deferred,
                          semantic_errors=sem_total,
                          semantic_errors_flagged=sem_caught), indent=1))
    for code, ex in examples.items():
        for e in ex[:verbose]:
            print(code, e)


def run_fmt(fmt, path):
    r = subprocess.run([fmt, "--format", path], capture_output=True)
    return r.returncode, r.stdout


def cmd_format(fmt, paths, verbose):
    stats = dict(files=0, formatted=0, refused=0, refused_syntax=0, changed=0, not_idempotent=0,
                 verdict_changed=0, crashed=0)
    bad = []
    tmp = "/tmp/sql_fmt_%d.sql" % os.getpid()
    for path in files_of(paths):
        data = open(path, "rb").read()
        if b"\0" in data:
            continue
        stats["files"] += 1
        rc, out = run_fmt(fmt, path)
        if rc != 0:
            if rc < 0 or rc > 1:
                stats["crashed"] += 1
                bad.append((path, "crash", rc))
                continue
            stats["refused"] += 1
            # a refusal must be a syntax error SQLite also sees
            if any(v[0] == "syntax" for v in verdicts(data)):
                stats["refused_syntax"] += 1
            else:
                bad.append((path, "refused-valid"))
            continue
        stats["formatted"] += 1
        if out != data:
            stats["changed"] += 1
        with open(tmp, "wb") as f:
            f.write(out)
        rc2, out2 = run_fmt(fmt, tmp)
        if rc2 != 0 or out2 != out:
            stats["not_idempotent"] += 1
            bad.append((path, "idempotence"))
        before = [v for v in verdicts(data)]
        after = [v for v in verdicts(out)]
        if before != after:
            stats["verdict_changed"] += 1
            bad.append((path, "verdict", before[:3], after[:3]))
    if os.path.exists(tmp):
        os.unlink(tmp)
    print(json.dumps(stats, indent=1))
    for b in bad[:verbose]:
        print(b)


def main():
    global LIB
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    LIB = load_sqlite()
    cmd = args.pop(0)
    if cmd == "extract":
        cmd_extract(args[0], args[1])
        return 0
    lsp = None
    limit = 2000
    verbose = 0
    seed = 1
    rest = []
    while args:
        a = args.pop(0)
        if a in ("--lsp", "--fmt"):
            lsp = args.pop(0)
        elif a == "--limit":
            limit = int(args.pop(0))
        elif a == "--verbose":
            verbose = int(args.pop(0))
        elif a == "--seed":
            seed = int(args.pop(0))
        else:
            rest.append(a)
    if cmd == "check":
        cmd_check(lsp, rest, verbose)
    elif cmd == "mutate":
        cmd_mutate(lsp, rest, limit, seed, verbose)
    elif cmd == "lint":
        cmd_lint(lsp, rest, verbose)
    elif cmd == "format":
        cmd_format(lsp, rest, verbose)
    return 0


if __name__ == "__main__":
    sys.exit(main())
