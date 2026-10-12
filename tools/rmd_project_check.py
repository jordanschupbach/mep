#!/usr/bin/env python3
"""End-to-end check that an R Markdown project made by mep's own project
wizard works out of the box -- with R *only* inside the project's flake.

This is the path a real user takes: mep launched from a desktop session
(no R on PATH), the wizard's "R Markdown notebook" preset creating
flake.nix + .envrc + analysis.Rmd, and Shift+Enter pressed in the starter
notebook straight away, long before the flake has been built.

What it asserts:
  - mep's own PATH has no Rscript, so the R kernel is not offered at first
  - the wizard leaves the environment loading (not "blocked", not "ready")
  - a chunk run meanwhile waits (Queued) instead of failing
  - once direnv has loaded the flake, the R kernel exists and the waiting
    chunk runs on the flake's R (ggplot2 loads, a plot comes back)

Needs nix, direnv and network access (nix-direnv's direnvrc, the flake
inputs); skips (exit 0) without nix or direnv. Pass --lock to reuse a
flake.lock (e.g. ~/projects/p2_analysis/flake.lock) so the build comes
from the store instead of a fresh nixpkgs.

    python3 tools/rmd_project_check.py [--build-dir build/native] [--lock F] [--keep]
"""
import argparse
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fold_live_check import RESULTS, Mep, Xvfb, check  # noqa: E402
from rmd_live_check import cell_state, shift_enter  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The wizard's rows for the R preset, as its options page leaves them once
# the preset is ticked (it ticks ggplot2; the extras ride along).
ROWS = """{
  {kind = 'flake', enabled = true},
  {kind = 'justfile', enabled = true},
  {kind = 'pkg', lang = 'r', attr = 'ggplot2', enabled = true},
  {kind = 'preset', enabled = true, preset = mep.new_project_presets[2]},
}"""


def path_without(program):
    """$PATH minus every directory holding `program` -- a desktop session."""
    keep = []
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if d and not os.path.exists(os.path.join(d, program)):
            keep.append(d)
    return os.pathsep.join(keep)


def find_cell(m, path, needle):
    """Index of the cell holding the first line of `path` containing
    `needle` (the cursor goes there)."""
    with open(path) as fh:
        for row, line in enumerate(fh.read().splitlines(), 1):
            if needle in line:
                m.cmd(str(row))
                return int(m.lua_value("mep.notebook_cell_index()") or -1)
    return -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(REPO, "build", "native"))
    ap.add_argument("--lock", default=None, help="flake.lock to seed the new project with")
    ap.add_argument("--timeout", type=float, default=900, help="seconds to allow the flake to build")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not shutil.which("nix") or not shutil.which("direnv"):
        print("nix or direnv not on PATH -- skipping")
        return 0
    home = tempfile.mkdtemp(prefix="/tmp/mepw-")
    proj = os.path.join(home, "rproj")
    os.makedirs(proj)
    if args.lock:
        shutil.copy(args.lock, os.path.join(proj, "flake.lock"))
    os.environ["PATH"] = path_without("Rscript")
    os.environ.pop("LD_LIBRARY_PATH", None)
    os.environ.pop("DIRENV_DIR", None)
    os.environ.pop("DIRENV_FILE", None)
    os.environ.pop("DIRENV_DIFF", None)
    os.environ.pop("DIRENV_WATCHES", None)
    xv = Xvfb()
    m = None
    try:
        m = Mep(args.build_dir, home, xv.display, [], cwd=home)
        names = m.lua_value("(function() local t = {} for _, k in ipairs(mep.notebook_kernels()) do "
                            "t[#t + 1] = k.name end return table.concat(t, ',') end)()") or ""
        check("mep's own PATH has no R, so no R kernel yet", "ir" not in names.split(","), names)

        m.cmd("lua mep.new_project_generate(%r, %s)" % (proj, ROWS.replace("\n", " ")))
        m.pump(20)
        check("the wizard wrote flake.nix, .envrc and analysis.Rmd",
              all(os.path.exists(os.path.join(proj, f)) for f in ("flake.nix", ".envrc", "analysis.Rmd")),
              str(os.listdir(proj)))
        check("the starter opened as an rmd notebook", m.lua_value("mep.notebook_format()") == "rmd",
              str(m.lua_value("mep.notebook_format()")))
        check("the environment is loading", m.lua_value("tostring(mep.direnv_pending())") == "true",
              str(m.lua_value("tostring(mep.direnv_pending())")))

        rmd = os.path.join(proj, "analysis.Rmd")
        plot = find_cell(m, rmd, "geom_point")
        setup = find_cell(m, rmd, "library(ggplot2)")
        check("found the starter's chunks", setup >= 0 and plot >= 0, "%d %d" % (setup, plot))
        # Shift+Enter in `setup` (the cursor is there) right away, then queue the rest.
        shift_enter(m)
        m.cmd("NotebookRunAll")
        m.pump(10)
        st = cell_state(m, setup)
        check("a chunk run before the environment is in waits (queued), not fails",
              st.startswith("queued") and "Error" not in st, st)
        m.screenshot(os.path.join(home, "proj-1-waiting.png"))

        end = time.time() + args.timeout
        while time.time() < end and m.lua_value("tostring(mep.direnv_pending())") == "true":
            m.pump(10)
            time.sleep(2)
        check("direnv finished loading", m.lua_value("tostring(mep.direnv_pending())") == "false",
              str(m.lua_value("tostring(mep.direnv_pending())")))
        names = m.lua_value("(function() local t = {} for _, k in ipairs(mep.notebook_kernels()) do "
                            "t[#t + 1] = k.name end return table.concat(t, ',') end)()") or ""
        check("the flake's R is now a kernel", "ir" in names.split(","), names)

        end = time.time() + 120
        st = ""
        while time.time() < end:
            st = cell_state(m, plot)
            if st.startswith("idle") and "#-1#" not in st:
                break
            m.pump(10)
            time.sleep(0.5)
        check("the queued chunks ran on the flake's R (ggplot2 loads)",
              "Error" not in cell_state(m, setup) and cell_state(m, setup).startswith("idle"),
              cell_state(m, setup))
        check("the plot chunk drew a plot", "[img]" in st, st)
        m.screenshot(os.path.join(home, "proj-2-ran.png"))

    finally:
        if m:
            m.quit()
        xv.close()
    bad = [r for r in RESULTS if not r[1]]
    print("\n---- %d checks, %d failures ----" % (len(RESULTS), len(bad)))
    for name, _, detail in bad:
        print("  FAIL %s\n       %s" % (name, detail))
    print("home: %s" % home)
    if not args.keep and not bad:
        shutil.rmtree(home, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
