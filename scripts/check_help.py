#!/usr/bin/env python3
"""Consistency checks for the built-in help workspace (help/).

Run by `just help-check` (and by `just test`). Four checks:

  1. Freshness -- every help/*.html is exactly what mep's own exporter
     produces from its .org source right now. Done by re-exporting into a
     temp directory and comparing bytes, NOT by comparing mtimes: git does
     not preserve mtimes, so a fresh clone would fail an mtime check and a
     rebased branch could pass one while shipping stale HTML.
  2. Structure -- every page declares a <title> and a help-section, the two
     things the Help sidebar reads to place it.
  3. Links -- every internal href in a page resolves to a file that exists.
  4. Coverage -- how much of mep's command and <leader> surface the manual
     actually mentions. Reported always; enforced only under --strict,
     because the manual is being written incrementally and a hard gate here
     would just be permanently red until the last page lands.

Usage: check_help.py <path-to-mep-binary> [--strict] [--min-coverage N]
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
HELP = REPO / "help"


def fail(problems, message):
    problems.append(message)


def check_freshness(mep, problems):
    """Re-export every source and compare against the committed HTML."""
    with tempfile.TemporaryDirectory() as tmp:
        for src in sorted(HELP.glob("*.org")):
            if src.name.startswith("_"):
                continue
            committed = src.with_suffix(".html")
            if not committed.exists():
                fail(problems, f"{src.name}: no exported {committed.name} "
                               f"(run `just help`)")
                continue
            out = pathlib.Path(tmp) / committed.name
            result = subprocess.run([mep, "--export-org", str(src), str(out)],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                fail(problems, f"{src.name}: export failed: "
                               f"{result.stderr.strip() or result.returncode}")
                continue
            if out.read_bytes() != committed.read_bytes():
                fail(problems, f"{committed.name} is stale -- it differs from "
                               f"what {src.name} exports now (run `just help`)")


def check_structure(problems):
    """Every page needs a title and a section for the sidebar to place it."""
    for page in sorted(HELP.glob("*.html")):
        text = page.read_text(errors="replace")
        if not re.search(r"<title>\s*\S.*?</title>", text, re.I | re.S):
            fail(problems, f"{page.name}: no <title> (the sidebar label)")
        if not re.search(r'<meta\s+name="help-section"\s+content="[^"]+"', text, re.I):
            fail(problems, f"{page.name}: no help-section meta -- add "
                           f'`#+HTML_HEAD: <meta name="help-section" ...>` '
                           f"to {page.stem}.org (see help/_template.org)")


def check_links(problems):
    """Internal hrefs must resolve. External and in-page ones are skipped."""
    for page in sorted(HELP.glob("*.html")):
        for href in re.findall(r'href="([^"]+)"', page.read_text(errors="replace")):
            if href.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = (HELP / href.split("#", 1)[0]).resolve()
            if not target.exists():
                fail(problems, f"{page.name}: broken link to {href}")


def coverage(problems, strict, minimum):
    """How much of the command/binding surface the manual mentions."""
    main = (REPO / "src" / "main.cpp").read_text(errors="replace")
    commands = set(re.findall(r"mep\.command\('([A-Za-z0-9_]+)'", main))
    leaders = set(re.findall(r"mep\.leader_map\('([^']+)'", main))
    prose = "\n".join(p.read_text(errors="replace") for p in HELP.glob("*.org"))

    missing_cmds = sorted(c for c in commands if c not in prose)
    missing_keys = sorted(k for k in leaders if f"<leader>{k}" not in prose)
    documented = (len(commands) - len(missing_cmds)) + (len(leaders) - len(missing_keys))
    total = len(commands) + len(leaders)
    pct = 100.0 * documented / total if total else 100.0

    print(f"coverage: {documented}/{total} ({pct:.1f}%) of "
          f"{len(commands)} commands and {len(leaders)} <leader> bindings "
          f"are mentioned in help/")
    if missing_cmds:
        print(f"  undocumented commands ({len(missing_cmds)}): "
              f"{', '.join(missing_cmds[:12])}"
              f"{' ...' if len(missing_cmds) > 12 else ''}")
    if missing_keys:
        print(f"  undocumented <leader> keys ({len(missing_keys)}): "
              f"{', '.join(missing_keys[:12])}"
              f"{' ...' if len(missing_keys) > 12 else ''}")
    if strict and pct < minimum:
        fail(problems, f"coverage {pct:.1f}% is below the required {minimum}%")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mep", help="path to the built mep binary")
    parser.add_argument("--strict", action="store_true",
                        help="also fail when coverage is below --min-coverage")
    parser.add_argument("--min-coverage", type=float, default=95.0)
    args = parser.parse_args()

    if not pathlib.Path(args.mep).is_file():
        print(f"check_help: no mep binary at {args.mep}", file=sys.stderr)
        return 2

    problems = []
    check_freshness(args.mep, problems)
    check_structure(problems)
    check_links(problems)
    coverage(problems, args.strict, args.min_coverage)

    if problems:
        print(f"\ncheck_help: {len(problems)} problem(s):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print("check_help: help/ is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
