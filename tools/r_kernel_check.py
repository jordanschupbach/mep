#!/usr/bin/env python3
"""Drives the built-in R notebook kernel (NotebookRKernelScript in
src/notebook_doc.cpp) over its JSON-line protocol, the same way
Editor::NotebookPumpQueue does, and checks what comes back: state kept
between requests, stdout/stderr streams, inline plots, errors that leave
the session alive, and SIGINT interrupting a cell without killing R.

The script is read straight out of notebook_doc.cpp, so this checks the
code mep actually ships. Skips (exit 0) when Rscript is not on PATH.

    python3 tools/r_kernel_check.py
"""
import json
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print("   %-62s %s" % (name, "PASS" if ok else "FAIL"))


def kernel_script():
    src = open(os.path.join(REPO, "src", "notebook_doc.cpp")).read()
    m = re.search(r'R"RKERNEL\((.*?)\)RKERNEL"', src, re.S)
    if not m:
        sys.exit("NotebookRKernelScript not found in src/notebook_doc.cpp")
    return m.group(1)


class Kernel:
    def __init__(self):
        self.proc = subprocess.Popen(["Rscript", "--no-save", "--no-restore", "-e", kernel_script()],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1)

    def messages(self, until, timeout=30):
        """Reads protocol lines until `until(msg)` is true; returns them all."""
        got = []
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([self.proc.stdout], [], [], 0.5)
            if not ready:
                if self.proc.poll() is not None:
                    break
                continue
            line = self.proc.stdout.readline()
            if not line:
                break
            try:
                msg = json.loads(line)
            except ValueError:
                got.append({"type": "RAW", "text": line})
                continue
            got.append(msg)
            if until(msg):
                break
        return got

    def execute(self, rid, code, timeout=30):
        self.proc.stdin.write(json.dumps({"op": "execute", "id": rid, "code": code}) + "\n")
        self.proc.stdin.flush()
        return self.messages(lambda m: m.get("type") == "done" and m.get("id") == rid, timeout)

    def close(self):
        try:
            self.proc.stdin.write('{"op":"shutdown"}\n')
            self.proc.stdin.flush()
            self.proc.wait(10)
        except Exception:
            self.proc.kill()


def text_of(msgs, name="stdout"):
    return "".join(m.get("text") or "" for m in msgs if m.get("type") == "stream" and m.get("name") == name)


def of_type(msgs, kind):
    return [m for m in msgs if m.get("type") == kind]


def main():
    if not shutil.which("Rscript"):
        print("Rscript not on PATH -- skipping")
        return 0
    k = Kernel()
    try:
        ready = k.messages(lambda m: m.get("type") == "ready")
        check("kernel says ready first", bool(ready) and ready[0].get("type") == "ready",
              ready or k.proc.stderr.read())
        if not ready or ready[-1].get("type") != "ready":
            return 1

        m = k.execute(1, "x <- 2")
        check("an assignment prints nothing", not of_type(m, "stream") and of_type(m, "done"), m)

        m = k.execute(2, "x * 21")
        check("state carries over: x * 21 is [1] 42", text_of(m) == "[1] 42\n", m)
        check("done carries an execution count", of_type(m, "done")[0].get("execution_count") == 2, m)

        m = k.execute(3, 'cat("hi\\n")')
        check("cat() is a stdout stream", text_of(m) == "hi\n", m)
        check("no raw (non-JSON) line leaked onto the protocol", not of_type(m, "RAW"), m)

        m = k.execute(4, 'message("note"); warning("careful"); 5')
        err = text_of(m, "stderr")
        check("message() and warning() are stderr", "note" in err and "Warning: careful" in err, m)
        check("the value after them still prints", text_of(m) == "[1] 5\n", m)

        m = k.execute(5, "plot(1:3)")
        pngs = of_type(m, "display_data")
        check("plot() sends one PNG", len(pngs) == 1 and pngs[0].get("png", "").startswith("iVBOR"),
              [dict(p, png=len(p.get("png") or "")) for p in pngs])

        m = k.execute(6, "plot(1); plot(2)")
        check("two pages are two PNGs", len(of_type(m, "display_data")) == 2, len(of_type(m, "display_data")))

        m = k.execute(7, "y <- 3")
        check("a cell that draws nothing sends no PNG", not of_type(m, "display_data"), m)

        m = k.execute(8, 'stop("boom")\nprint("not reached")')
        errs = of_type(m, "error")
        check("stop() is an error output", len(errs) == 1 and errs[0].get("evalue") == "boom", m)
        check("top-level error reads 'Error: boom'", errs and errs[0].get("traceback") == ["Error: boom"], errs)
        check("the error stops the cell", "not reached" not in text_of(m), m)

        m = k.execute(9, "f(")
        check("a syntax error is an error output", len(of_type(m, "error")) == 1, m)

        m = k.execute(10, "x")
        check("the session survives errors", text_of(m) == "[1] 2\n", m)

        m = k.execute(11, "ls()")
        check("the driver leaves the global env clean", text_of(m) == '[1] "x" "y"\n', m)

        has_ggplot = "TRUE" in text_of(k.execute(12, 'print(requireNamespace("ggplot2", quietly = TRUE))'))
        if has_ggplot:
            m = k.execute(13, "library(ggplot2)\nggplot(data.frame(a = 1:3, b = 1:3), aes(a, b)) + geom_point()")
            check("an auto-printed ggplot is a PNG", len(of_type(m, "display_data")) == 1, m)
        else:
            print("   (ggplot2 not installed -- skipping its check)")

        k.proc.stdin.write(json.dumps({"op": "execute", "id": 14, "code": "Sys.sleep(20); 1"}) + "\n")
        k.proc.stdin.flush()
        time.sleep(1.5)
        k.proc.send_signal(signal.SIGINT)
        m = k.messages(lambda msg: msg.get("type") == "done" and msg.get("id") == 14, 15)
        check("SIGINT interrupts the running cell", any(e.get("ename") == "Interrupted" for e in of_type(m, "error")), m)
        check("...and R is still running", k.proc.poll() is None)
        if k.proc.poll() is None:
            m = k.execute(15, "x")
            check("...with its state intact", text_of(m) == "[1] 2\n", m)
    finally:
        k.close()
    check("shutdown exits cleanly", k.proc.returncode == 0, k.proc.returncode)
    bad = [r for r in RESULTS if not r[1]]
    print("\n---- %d checks, %d failures ----" % (len(RESULTS), len(bad)))
    for name, _, detail in bad:
        print("  FAIL %s\n       %s" % (name, detail))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
