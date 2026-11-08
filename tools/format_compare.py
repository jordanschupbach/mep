#!/usr/bin/env python3
"""Compare one of mep's formatters with the language's standard formatter
over a corpus -- the measurement every formatter in
plans/LANGUAGE_PARITY_PLAN.md reports.

    just format-compare nix                  # whole corpus, summary
    just format-compare nix --sample 500     # a random 500 files
    just format-compare bash --show 3        # plus the 3 worst diffs
    just format-compare nix --oracle         # also: does the language accept our output?

For every corpus file (`just grammar-corpus <lang>` writes the list):

  - ref: the reference formatter's output (a file it fails on is skipped:
    there is nothing to compare against);
  - mine: `build/native/mep-<lang>-format-test --format FILE` (exit 0 and
    the text on stdout, or non-zero with the reason on stderr = refused);
  - identical bytes, and the share of lines that agree;
  - idempotence: mine formatted again must not change;
  - with --oracle: the language's own parser (grammar_diff.py's LANGUAGES
    entry) must accept our output whenever it accepts the input.

A crash (killed by a signal) or a timeout is reported separately from a
refusal: a refusal is the formatter doing its job, a crash never is.

Adding a language: a PRESETS entry -- the mep binary's extra arguments and
the reference command ('{}' is the file; without '{}' the file goes to
stdin). A reference tool that is not on PATH is fetched once with
`nix build nixpkgs#<pkg>` (`pkg`).
"""

import argparse
import concurrent.futures
import difflib
import json
import os
import random
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import grammar_diff  # noqa: E402  (LANGUAGES: globs, oracle; corpus_list)

BUILD = REPO / 'build' / 'native'

# Per language: mep's formatter binary and arguments, the reference command
# and the nixpkgs package it comes from, and the file suffix the oracle
# needs for a temporary copy. Settings on both sides are the ones the
# formatter's help/formatting.org entry says it matches.
PRESETS = {
    'nix': dict(binary='mep-nix-format-test', args=[], ref=['nixfmt'], pkg='nixfmt', suffix='.nix'),
    'bash': dict(binary='mep-bash-format-test', args=['--tabs'], ref=['shfmt', '{}'], pkg='shfmt', suffix='.sh'),
    'c': dict(binary='mep-c-format-test', args=[],
              ref=['clang-format', '--style={BasedOnStyle: LLVM, SortIncludes: Never}', '{}'],
              pkg='clang-tools', suffix='.c'),
    'crystal': dict(binary='mep-crystal-format-test', args=[], ref=['crystal', 'tool', 'format', '-'],
                    pkg='crystal', suffix='.cr'),
    # StyLua's defaults (no stylua.toml): width 120, tabs, AutoPreferDouble quotes.
    'lua': dict(binary='mep-lua-format-test', args=[], ref=['stylua', '-'], pkg='stylua', suffix='.lua'),
    'sql': dict(binary='mep-sql-format-test', args=[], ref=None, pkg=None, suffix='.sql'),
    # Prettier 3.6.2's defaults over its acorn parser (the one mep's js_parse
    # ports); `--stdin-filepath x.js` (babel) agreed on a 600-file sample too.
    'javascript': dict(binary='mep-js-format-test', args=[], ref=['prettier', '--parser', 'acorn'],
                       pkg='prettier', suffix='.js'),
    # The same printer over typescript-estree trees (`--parser typescript`);
    # the dialect comes from the file name (.tsx is TSX). --no-config and
    # --no-editorconfig: corpus repos carry their own .prettierrc/.editorconfig.
    'typescript': dict(binary='mep-js-format-test', args=[],
                       ref=['prettier', '--no-config', '--no-editorconfig', '--parser', 'typescript', '{}'],
                       pkg='prettier', suffix='.ts'),
    'tsx': dict(binary='mep-js-format-test', args=[],
                ref=['prettier', '--no-config', '--no-editorconfig', '--parser', 'typescript', '{}'],
                pkg='prettier', suffix='.tsx'),
    # fish_indent (fish 4.8; the corpus is fish 4.2's own scripts + plugins).
    # gofmt (no flags; it has none that change layout) on the file, so a file
    # without a package clause is an error for both sides' comparison.
    'go': dict(binary='mep-go-format-test', args=[], ref=['gofmt', '{}'], pkg='go', suffix='.go'),
    # `zig fmt` (Zig 0.16; no options) reading stdin, as `zig fmt --stdin` prints it.
    'zig': dict(binary='mep-zig-format-test', args=[], ref=['zig', 'fmt', '--stdin'], pkg='zig', suffix='.zig'),
    'fish': dict(binary='mep-fish-format-test', args=[], ref=['fish_indent', '{}'], pkg='fish', suffix='.fish'),
    # tclfmt (tclint 0.6.2) defaults, no tclint.toml: indent 4, max 2 blank lines, no spaces in braces.
    'tcl': dict(binary='mep-tcl-format-test', args=[], ref=['tclfmt', '{}'], pkg='tclint', suffix='.tcl'),
    # fprettify 0.3.7 defaults (indent 3, whitespace 2, line length 132, no case
    # changes) over the free-form files only (`suffixes`): it mangles fixed
    # form, and mep's formatter refuses fixed form.
    'fortran': dict(binary='mep-fortran-format-test', args=[], ref=['fprettify', '--stdout', '{}'], pkg='fprettify',
                    suffix='.f90', suffixes=('.f90', '.f95', '.f03', '.f08')),
    # Runic.jl 1.5.0 (no options; what `runic FILE` prints) via the cache that
    # tools/julia_runic_cache.jl fills in one julia process -- Runic's start-up
    # per file would dominate; julia_runic_ref.py says how to fill it.
    'julia': dict(binary='mep-julia-format-test', args=[],
                  ref=['python3', str(REPO / 'tools' / 'julia_runic_ref.py'), '{}'], pkg=None, suffix='.jl'),
    # Emacs 30's scheme-mode indent-region (indent-region-line-by-line, no column
    # cache) plus mep's whitespace rules: tools/scheme_indent_oracle.el.
    # MISS_HIT's `mh_style --fix` (miss_hit_core 0.9.44; default rules, no
    # miss_hit.cfg, --octave latest) via tools/mh_style_ref.py, over the matlab
    # corpus; --mh-style is one mh_style run exactly (mep's default leaves out
    # the ';' MISS_HIT adds and runs to a fixed point).
    'octave': dict(binary='mep-octave-format-test', args=['--mh-style'],
                   ref=['python3', str(REPO / 'tools' / 'mh_style_ref.py'), '{}'], pkg=None, suffix='.m',
                   corpus='matlab'),
    # rustfmt 1.97.1 defaults (no rustfmt.toml: the corpus repos' own are
    # ignored) via tools/rustfmt_ref.py, which caches its outputs; the edition
    # (and so the style edition) per file comes from rust_check.py's edition(),
    # and mep's side finds the same one itself (--edition auto, the default).
    'rust': dict(binary='mep-rust-format-test', args=[],
                 ref=['python3', str(REPO / 'tools' / 'rustfmt_ref.py'), '{}'], pkg=None, suffix='.rs'),
    'scheme': dict(binary='mep-scheme-format-test', args=[],
                   ref=['emacs', '-Q', '--batch', '-l', str(REPO / 'tools' / 'scheme_indent_oracle.el'), '{}'],
                   pkg='emacs-nox', suffix='.scm'),
}


def _rust_edition_args(path):
    import rust_check  # tools/ts_grammar_diff, on sys.path
    return ['--edition', rust_check.edition(path)]


# Per formatter binary: extra arguments for the second (idempotence) pass,
# from the original file's path.
SECOND_PASS_ARGS = {'mep-rust-format-test': _rust_edition_args}


def die(msg):
    print(f'format_compare: {msg}', file=sys.stderr)
    sys.exit(2)


def resolve_tool(cmd, pkg):
    """The reference command with its program made absolute: PATH first, else nixpkgs#pkg."""
    if shutil.which(cmd[0]):
        return cmd
    if not pkg:
        die(f'{cmd[0]} is not on PATH and the preset names no package')
    print(f'fetching {cmd[0]} from nixpkgs#{pkg} ...', file=sys.stderr)
    out = subprocess.run(['nix', 'build', '--no-link', '--print-out-paths', f'nixpkgs#{pkg}'],
                         check=True, capture_output=True, text=True).stdout.split()
    for path in out:
        exe = Path(path) / 'bin' / cmd[0]
        if exe.exists():
            return [str(exe)] + cmd[1:]
    die(f'nixpkgs#{pkg} has no bin/{cmd[0]}')


def run(cmd, path, data, timeout):
    """(status, stdout bytes, stderr text); status 'ok', 'fail', 'crash' or 'timeout'."""
    argv = [path if a == '{}' else a for a in cmd]
    stdin = None if '{}' in cmd else data
    try:
        p = subprocess.run(argv, input=stdin, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'timeout', b'', ''
    if p.returncode < 0:
        return 'crash', p.stdout, f'signal {-p.returncode}'
    return ('ok' if p.returncode == 0 else 'fail'), p.stdout, p.stderr.decode('utf-8', 'replace')


def oracle_accepts(oracle, data, suffix):
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as f:
        f.write(data)
        name = f.name
    try:
        argv = [name if a == '{}' else a for a in oracle]
        return subprocess.run(argv, capture_output=True, timeout=60).returncode == 0
    except subprocess.TimeoutExpired:
        return True  # no verdict: don't count it against us
    finally:
        os.unlink(name)


def line_agreement(a, b):
    """Lines of `a` matched in `b` over the longer one's line count."""
    la, lb = a.splitlines(), b.splitlines()
    if not la and not lb:
        return 1.0
    sm = difflib.SequenceMatcher(None, la, lb, autojunk=False)
    same = sum(block.size for block in sm.get_matching_blocks())
    return same / max(len(la), len(lb))


def compare_one(path, mine_cmd, ref_cmd, oracle, suffix, timeout):
    r = dict(path=path)
    data = Path(path).read_bytes()
    if ref_cmd:
        st, ref, err = run(ref_cmd, path, data, timeout)
        r['ref'] = st
        if st != 'ok':
            return r
    st, mine, err = run(mine_cmd, path, data, timeout)
    r['mine'] = st
    if st != 'ok':
        r['reason'] = err.strip().splitlines()[-1] if err.strip() else ''
        return r
    # Second pass, over a temporary copy of the first pass's output.
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as f:
        f.write(mine)
        name = f.name
    try:
        # The copy lives elsewhere: settings the formatter derives from the
        # file's location (Rust's edition) are passed explicitly.
        cmd2 = mine_cmd[:-1] + SECOND_PASS_ARGS.get(Path(mine_cmd[0]).name, lambda _: [])(path) + mine_cmd[-1:]
        st2, again, _ = run(cmd2, name, mine, timeout)
    finally:
        os.unlink(name)
    r['idempotent'] = st2 == 'ok' and again == mine
    if st2 != 'ok':
        r['second_pass'] = st2
    r['changed'] = mine != data
    if ref_cmd:
        r['identical'] = mine == ref
        if not r['identical']:
            r['lines'] = line_agreement(ref.decode('utf-8', 'replace'), mine.decode('utf-8', 'replace'))
            r['diff'] = ''.join(difflib.unified_diff(ref.decode('utf-8', 'replace').splitlines(True),
                                                     mine.decode('utf-8', 'replace').splitlines(True),
                                                     'reference', 'mep', n=1))
        else:
            r['lines'] = 1.0
    if oracle and r['changed'] and oracle_accepts(oracle, data, suffix):
        r['oracle_rejects_output'] = not oracle_accepts(oracle, mine, suffix)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('lang')
    ap.add_argument('--corpus', help='a files.txt (default: build/grammar-corpus/<lang>/files.txt)')
    ap.add_argument('--sample', type=int, help='compare a random N files')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--binary', help='mep formatter binary (default from the preset)')
    ap.add_argument('--build-dir', default=os.environ.get('MEP_BUILD_DIR'),
                    help='where the preset binary is (default build/native, or $MEP_BUILD_DIR)')
    ap.add_argument('--args', default='', help='extra arguments for mep\'s formatter (one string)')
    ap.add_argument('--ref', help='reference command (one string; {} = the file, none = stdin)')
    ap.add_argument('--no-ref', action='store_true', help='skip the reference: refusals/idempotence/oracle only')
    ap.add_argument('--oracle', action='store_true', help="check the language's parser accepts the output")
    ap.add_argument('--show', type=int, default=0, help='print the N least-agreeing diffs')
    ap.add_argument('--list', help='write the paths of every file that differs (or failed) here')
    ap.add_argument('--json', help='write every per-file result here')
    a = ap.parse_args()

    preset = PRESETS.get(a.lang, {})
    build = Path(a.build_dir) if a.build_dir else BUILD
    binary = Path(a.binary) if a.binary else build / preset.get('binary', f'mep-{a.lang}-format-test')
    if not binary.exists():
        die(f'{binary} not built: cmake --build build/native --target {binary.name}')
    mine_cmd = [str(binary), '--format', *preset.get('args', []), *shlex.split(a.args), '{}']

    ref_cmd = None
    if not a.no_ref:
        if a.ref:
            ref_cmd = resolve_tool(shlex.split(a.ref), None)
        elif preset.get('ref'):
            ref_cmd = resolve_tool(preset['ref'], preset.get('pkg'))
        else:
            print(f'no reference formatter for {a.lang}: refusals/idempotence only', file=sys.stderr)

    oracle = None
    if a.oracle:
        spec = grammar_diff.LANGUAGES.get(a.lang)
        oracle = spec and spec.get('oracle')
        if not oracle:
            die(f'grammar_diff.py has no oracle for {a.lang}')

    corpus = Path(a.corpus) if a.corpus else grammar_diff.corpus_list(preset.get('corpus', a.lang))
    if not corpus.exists():
        die(f'no corpus at {corpus}; run: just grammar-corpus {a.lang}')
    files = [f for f in corpus.read_text().split('\n') if f]
    if preset.get('suffixes'):  # a subset of the corpus (case-insensitive file suffixes)
        files = [f for f in files if f.lower().endswith(preset['suffixes'])]
    if a.sample and a.sample < len(files):
        files = sorted(random.Random(a.seed).sample(files, a.sample))

    suffix = preset.get('suffix', '')
    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(compare_one, f, mine_cmd, ref_cmd, oracle, suffix, a.timeout) for f in files]
        for k, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            results.append(fut.result())
            if k % 500 == 0:
                print(f'  {k}/{len(files)}', file=sys.stderr)
    results.sort(key=lambda r: r['path'])

    def count(pred):
        return sum(1 for r in results if pred(r))

    compared = [r for r in results if r.get('ref', 'ok') == 'ok']
    formatted = [r for r in compared if r.get('mine') == 'ok']
    print(f'{a.lang}: {len(results)} files')
    if ref_cmd:
        print(f'  reference fails on      {len(results) - len(compared)} (skipped)')
    print(f'  refused by mep          {count(lambda r: r.get("mine") == "fail")}')
    print(f'  crashed / timed out     {count(lambda r: r.get("mine") == "crash")} / '
          f'{count(lambda r: r.get("mine") == "timeout")}')
    if formatted:
        if ref_cmd:
            same = count(lambda r: r.get('identical'))
            lines = sum(r['lines'] for r in formatted) / len(formatted)
            print(f'  byte-identical          {same} of {len(formatted)} ({100 * same / len(formatted):.1f}%)')
            print(f'  lines agreeing          {100 * lines:.2f}% (mean per file)')
        print(f'  not idempotent          {count(lambda r: r.get("mine") == "ok" and not r.get("idempotent"))}')
        print(f'  left unchanged          {count(lambda r: r.get("mine") == "ok" and not r.get("changed"))}')
        if oracle:
            print(f'  output rejected by lang {count(lambda r: r.get("oracle_rejects_output"))}')

    reasons = {}
    for r in results:
        if r.get('mine') == 'fail':
            reasons.setdefault(r['reason'].split(': ', 1)[-1][:80], []).append(r['path'])
    if reasons:
        print('  refusals by reason:')
        for reason, paths in sorted(reasons.items(), key=lambda kv: -len(kv[1]))[:15]:
            print(f'    {len(paths):6}  {reason}   e.g. {paths[0]}')
    for r in results:
        if r.get('mine') in ('crash', 'timeout'):
            print(f'  {r["mine"].upper()}: {r["path"]}')

    if a.show:
        worst = sorted((r for r in formatted if not r.get('identical', True)), key=lambda r: r['lines'])[:a.show]
        for r in worst:
            print(f'\n=== {r["path"]} ({100 * r["lines"]:.1f}% of lines agree)')
            print(r['diff'][:6000])
    if a.list:
        Path(a.list).write_text(''.join(r['path'] + '\n' for r in results
                                        if r.get('mine') != 'ok' or not r.get('identical', True)
                                        or not r.get('idempotent', True)))
    if a.json:
        Path(a.json).write_text(json.dumps([{k: v for k, v in r.items() if k != 'diff'} for r in results], indent=1))
    bad = count(lambda r: r.get('mine') in ('crash', 'timeout') or r.get('oracle_rejects_output'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
