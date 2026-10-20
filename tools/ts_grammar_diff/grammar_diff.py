#!/usr/bin/env python3
"""Compare a tree-sitter grammar of mep's own against the one it replaces.

The method in plans/TREESITTER_GRAMMAR_PLAN.md, as commands:

  corpus  <lang>   fetch and unpack a body of real code in the language
                   (from nixpkgs sources) and write its file list
  symbols <name>   the old and new grammars' node types and fields, and
                   which ones the new grammar lacks
  diff    <name>   parse the corpus with both grammars and compare every
                   named node: type, field and byte range; --oracle asks
                   the language's own implementation about every file
                   whose trees differ
  fuzz    <name>   parse damaged copies of the corpus with the new grammar
                   under AddressSanitizer + UBSan
  recovery <name>  the same damaged copies with both grammars: how many
                   bytes each leaves inside ERROR nodes

<name> is a grammar: lua, c, cpp, python, javascript, markdown,
markdown_inline, org, r, nix, just, mepml. The "new" grammar is
grammars/tree-sitter-<name>/src; the "old" is third_party/grammars/<name>/src
while it is still vendored, and that directory as of the last commit that
had it once it is gone (so the oracle outlives the vendored copy). Either
can be overridden with --old/--new <src dir>.

Builds and corpora go under build/grammar-diff/ and build/grammar-corpus/.
"""

import argparse
import concurrent.futures
import collections
import fnmatch
import hashlib
import io
import os
import random
import re
import shutil
import subprocess
import sys
import tarfile
import textwrap
import threading
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL_DIR = Path(__file__).resolve().parent
BUILD = REPO / 'build' / 'grammar-diff'
CORPUS = REPO / 'build' / 'grammar-corpus'
RUNTIME = REPO / 'third_party' / 'tree-sitter'

# Characters every language's scanner should survive being cut at or fed.
GENERIC_TOKENS = ['"', "'", '`', '(', ')', '[', ']', '{', '}', '\n', '\\', '#', '/*', '*/', '${', '"""', "''"]

# Per language: the files to collect, where to get a corpus (nixpkgs
# attributes, each with the member paths to keep), the language's own
# "does this parse?" check ({} is the file), and extra fuzz tokens.
# Grammars that share a language (markdown, markdown_inline) share an entry.
LANGUAGES = {
    'r': dict(
        globs=['*.R', '*.r'],
        sources=[('R.src', ['*.R', '*.r'])] + [
            (f'rPackages.{p}.src', ['*.R', '*.r'])
            for p in ['ggplot2', 'dplyr', 'data_table', 'rlang', 'shiny', 'testthat', 'vctrs', 'purrr', 'knitr',
                      'Matrix', 'R6']
        ],
        oracle=['Rscript', '-e', 'invisible(parse(commandArgs(TRUE)[1]))', '{}'],
        tokens=['r"(', 'r"--[', ')"', '%', 'else ', 'function(', '[[', ']]', '<-', '\\('],
    ),
    'nix': dict(
        globs=['*.nix'],
        sources=[('@nixpkgs', ['*.nix'])],
        oracle=['nix-instantiate', '--parse', '{}'],
        tokens=["''", "''$", "'''", '${', './', '~/', '<', '//', ':'],
    ),
    'just': dict(
        globs=['justfile', 'Justfile', '.justfile', '*.just'],
        sources=[('just.src', ['*/justfile', '*.just']), ('@just-tests', [])],
        oracle=['just', '--justfile', '{}', '--dump'],
        tokens=['{{', '}}', '{{{{', ':=', '\n    ', '\n', '@', '-', '\\\n'],
    ),
    'mepml': dict(globs=['*.mepml'], sources=[], oracle=None, tokens=['*', '_', '```', '$$', '|']),
    'lua': dict(
        globs=['*.lua'],
        sources=[(a, ['*.lua']) for a in ['luarocks.src', 'awesome.src', 'lua-language-server.src',
                                          'vimPlugins.telescope-nvim.src', 'vimPlugins.nvim-treesitter.src',
                                          'vimPlugins.lazy-nvim.src']],
        oracle=['luac', '-p', '{}'],
        tokens=['[[', ']]', '[==[', ']==]', '--[[', 'end', 'function', '::', '...'],
    ),
    'c': dict(
        # mep's own headers (src/*.h) are C++; only third_party's are C.
        globs=['*.c', 'third_party/*.h'],
        sources=[('sqlite.src', ['*.c', '*.h']), ('lua5_4.src', ['*.c', '*.h']),
                 ('python3.src', ['*/Objects/*.c', '*/Python/*.c', '*/Include/*.h', '*/Modules/*.c'])],
        # A C file's validity depends on its include path and macros, so a
        # compiler run on it alone says little: no oracle.
        oracle=None,
        tokens=['#if', '#endif', '#define', '\\\n', ';', '*/', 'struct ', '->'],
    ),
    'cpp': dict(
        globs=['*.cpp', '*.cc', '*.cxx', '*.hpp', '*.hh', '*.hxx', '*.h'],
        sources=[('fmt.src', ['*.cc', '*.h']), ('abseil-cpp.src', ['*.cc', '*.h']),
                 ('nlohmann_json.src', ['*/include/*.hpp'])],
        oracle=None,
        tokens=['R"x(', ')x"', '<', '>', '>>', '::', 'template<', '#if', '#endif', ';'],
    ),
    'python': dict(
        globs=['*.py', '*.pyi'],
        sources=[('python3.src', ['*/Lib/*.py'])] + [
            (f'python3Packages.{p}.src', ['*.py', '*.pyi']) for p in ('numpy', 'django', 'requests', 'sympy')],
        oracle=['python3', '-c', 'import ast,sys; ast.parse(open(sys.argv[1], "rb").read())', '{}'],
        tokens=['f"', 'f"{', '"""', ':\n', '\n    ', '\n\t', 'lambda', 'match ', '\\\n'],
    ),
    'javascript': dict(
        globs=['*.js', '*.mjs', '*.cjs', '*.jsx'],
        # Three React front ends for JSX (Node's lib/ has none).
        sources=[('nodejs.src', ['*/lib/*.js', '*/lib/*.mjs']),
                 ('mastodon.src', ['app/javascript/*.js', 'app/javascript/*.jsx']),
                 ('homepage-dashboard.src', ['src/*.js', 'src/*.jsx']),
                 ('navidrome.src', ['ui/src/*.js', 'ui/src/*.jsx'])],
        # node rejects JSX: esbuild (strict, and parses JSX) checks .jsx.
        oracle=['sh', '-c', 'case "$1" in *.jsx) exec nix shell nixpkgs#esbuild --command '
                'esbuild --loader=jsx --log-level=error "$1" >/dev/null;; '
                '*) exec node --check "$1";; esac', 'sh', '{}'],
        tokens=['`', '${', '/', '/g', '=>', '<div>', '</div>', ';', '\n'],
    ),
    'commonlisp': dict(
        globs=['*.lisp', '*.lsp', '*.cl', '*.asd'],
        # SBCL's own sources (compiler, runtime library, contribs) and a few
        # widely used libraries with heavy reader-macro and backquote use.
        sources=[('sbcl.src', ['*.lisp', '*.asd'])] + [
            (f'sbclPackages.{p}.src', ['*.lisp', '*.asd'])
            for p in ('alexandria', 'cl-ppcre', 'iterate', 'trivia', 'cffi', 'fiveam')],
        # Reading a file needs its packages to exist (`pkg:sym`) and its
        # reader macros installed, so the reader alone says little: no oracle.
        oracle=None,
        tokens=['#|', '|#', '#\\', '#+', '#-', ',@', '`', '|', '\\', '#(', "#'", ';', ':'],
    ),
    'org': dict(
        globs=['*.org'],
        # Emacs's Org manual and friends, and four Org packages' docs and
        # test files (ox-hugo's test site is 55 files of every feature).
        sources=[('emacs.src', ['*.org']), ('emacsPackages.ox-hugo.src', ['*.org']),
                 ('emacsPackages.org-roam.src', ['*.org']), ('emacsPackages.org-ql.src', ['*.org']),
                 ('emacsPackages.org-super-agenda.src', ['*.org'])],
        oracle=None,
        tokens=['* ', '** ', '#+begin_src', '#+end_src', ':PROPERTIES:', ':END:', '[[', ']]', '|', '- [ ] '],
    ),
    'markdown': dict(
        globs=['*.md', '*.markdown'],
        # Plus every example of the CommonMark and GFM specs (and both
        # suites' regression and extension tests), one file each: the edge
        # cases the specs were written to pin down.
        sources=[('cmark.src', ['*.md']), ('nodejs.src', ['*/doc/*.md']), ('@spec-examples', [])],
        # Markdown accepts any text, so its oracle compares structure: the
        # blocks cmark-gfm (the reference implementation, with GFM's tables
        # and task lists) reads, against the tree's (markdown_skeleton).
        oracle=None,
        structure_oracle='markdown',
        tokens=['```', '~~~', '# ', '> ', '- ', '1. ', '*', '_', '[', '](', '<', '|', '\n\n'],
    ),
}
LANGUAGES['markdown_inline'] = LANGUAGES['markdown']


def die(message):
    print(f'grammar_diff: {message}', file=sys.stderr)
    sys.exit(2)


def run(argv, **kw):
    return subprocess.run(argv, check=True, **kw)


def language(name):
    if name not in LANGUAGES:
        die(f'unknown grammar {name!r}; one of: {", ".join(sorted(LANGUAGES))}')
    return LANGUAGES[name]


# ---- corpus -----------------------------------------------------------------

def nix_out_path(attr):
    if attr == '@nixpkgs':
        return subprocess.run(['nix', 'eval', '--raw', '--impure', '--expr', '(builtins.getFlake "nixpkgs").outPath'],
                              check=True, capture_output=True, text=True).stdout.strip()
    # A GC root beside the corpus: a source directory is read in place, and
    # an unrooted store path can be garbage-collected between `corpus` and
    # `diff` (it was: half of Python's corpus, once).
    root = CORPUS / '.gcroots' / attr
    root.parent.mkdir(parents=True, exist_ok=True)
    return subprocess.run(['nix', 'build', '--out-link', str(root), '--print-out-paths', f'nixpkgs#{attr}'],
                          check=True, capture_output=True, text=True).stdout.strip().splitlines()[0]


SPEC_FILES = [('cmark.src', ['test/spec.txt', 'test/regression.txt']),
              ('cmark-gfm.src', ['test/spec.txt', 'test/regression.txt', 'test/extensions.txt'])]


def spec_examples(dest):
    """Each example of the CommonMark/GFM spec files as its own .md file
    (and its expected HTML beside it as .html), the spec's `→` back to the
    tab it stands for."""
    fence = '`' * 32
    got = []
    for attr, names in SPEC_FILES:
        root = Path(nix_out_path(attr))
        for name in names:
            text = (root / name).read_text(encoding='utf-8')
            stem = f'{attr.split(".")[0]}-{Path(name).stem}'
            n = 0
            for block in text.split(fence + ' example')[1:]:
                body = block.split('\n' + fence, 1)[0]
                body = body.split('\n', 1)[1] if '\n' in body else ''
                if '\n.\n' in '\n' + body:
                    md, html = ('\n' + body).split('\n.\n', 1)
                    md = md[1:] + '\n' if md.startswith('\n') else md
                else:
                    md, html = body, ''
                n += 1
                out = dest / f'{stem}-{n:04d}.md'
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text(md.replace('\u2192', '\t'), encoding='utf-8')
                out.with_suffix('.html').write_text(html.replace('\u2192', '\t'), encoding='utf-8')
                got.append(str(out))
    return got


def matches(path, patterns):
    return any(fnmatch.fnmatch(path, p) or fnmatch.fnmatch(os.path.basename(path), p) for p in patterns)


def unpack(store_path, patterns, dest):
    """The source's files matching `patterns`: in place for a directory,
    extracted (data filter: no links, devices or absolute paths) for a
    tarball. Downloaded archives are untrusted, so nothing is run from them."""
    src = Path(store_path)
    if src.is_dir():
        return [str(p) for p in sorted(src.rglob('*')) if p.is_file() and matches(str(p.relative_to(src)), patterns)]
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with tarfile.open(src) as tar:
        members = [m for m in tar.getmembers() if m.isfile() and matches(m.name, patterns)]
        tar.extractall(dest, members=members, filter='data')
    return [str(p) for p in sorted(dest.rglob('*')) if p.is_file()]


def just_test_justfiles(dest):
    """The justfiles embedded in just's own tests (tests/*.rs), un-indented
    the way its test harness does it."""
    src = Path(nix_out_path('just.src'))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    out = []
    for rs in sorted(src.glob('tests/*.rs')):
        text = rs.read_text(errors='replace')
        for m in re.finditer(r'\.justfile\(\s*(?:indoc!\()?\s*(r#*)?"', text):
            start, raw = m.end(), m.group(1)
            if raw is not None:
                body = text[start:text.find('"' + raw[1:], start)]
            else:
                body, i = [], start
                escapes = {'n': '\n', 't': '\t', '\\': '\\', '"': '"'}
                while i < len(text) and text[i] != '"':
                    if text[i] == '\\' and i + 1 < len(text):
                        c = text[i + 1]
                        if c == '\n':
                            i += 2
                            while i < len(text) and text[i] in ' \t\n':
                                i += 1
                            continue
                        body.append(escapes.get(c, c))
                        i += 2
                    else:
                        body.append(text[i])
                        i += 1
                body = ''.join(body)
            if '{{' in body and 'format!' in text[max(0, m.start() - 200):m.start()]:
                continue  # a Rust format string, not a justfile
            path = dest / f'{len(out):04d}.just'
            path.write_text(textwrap.dedent(body).lstrip('\n'))
            out.append(str(path))
    return out


def corpus_list(lang):
    # A grammar parsed over another's ranges reads that one's corpus.
    return CORPUS / RANGES_FROM.get(lang, lang) / 'files.txt'


def cmd_corpus(args):
    spec = language(args.lang)
    files = [str(REPO / p) for p in subprocess.run(['git', 'ls-files', *spec['globs'], *[f'*/{g}' for g in spec['globs']]],
                                                   cwd=REPO, check=True, capture_output=True, text=True).stdout.split()]
    print(f'repo: {len(files)} files')
    for attr, patterns in spec['sources']:
        dest = CORPUS / args.lang / attr.replace('@', '_')
        if attr == '@just-tests':
            got = just_test_justfiles(dest)
        elif attr == '@spec-examples':
            got = spec_examples(dest)
        else:
            got = unpack(nix_out_path(attr), patterns, dest)
        print(f'{attr}: {len(got)} files')
        files += got
    files = sorted(set(files))
    out = corpus_list(args.lang)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(''.join(f + '\n' for f in files))
    lines = sum(Path(f).read_bytes().count(b'\n') for f in files)
    print(f'{len(files)} files, {lines} lines -> {out.relative_to(REPO)}')


def read_corpus(args, lang):
    path = Path(args.corpus) if args.corpus else corpus_list(lang)
    if not path.exists():
        die(f'no corpus at {path}; run: just grammar-corpus {lang}')
    listed = [line for line in path.read_text().splitlines() if line]
    files = [f for f in listed if os.path.exists(f)]
    if len(files) < len(listed):
        # A repository file can go away after the list was made (a vendored
        # grammar removed, say); rerun `corpus` to refresh it.
        print(f'corpus: skipping {len(listed) - len(files)} listed files that no longer exist', file=sys.stderr)
    return files


# ---- building the dump tool -------------------------------------------------

def git(*argv):
    return subprocess.run(['git', *argv], cwd=REPO, capture_output=True, text=True)


def old_source(name, override):
    """The replaced grammar's src/ dir, and where it came from."""
    if override:
        return Path(override), override
    vendored = REPO / 'third_party' / 'grammars' / name / 'src'
    if (vendored / 'parser.c').exists():
        return vendored, 'third_party (vendored)'
    rel = f'third_party/grammars/{name}'
    if git('cat-file', '-e', f'HEAD:{rel}/src/parser.c').returncode == 0:
        rev = git('rev-parse', 'HEAD').stdout.strip()
    else:
        last = git('rev-list', '-1', 'HEAD', '--', rel).stdout.strip()
        if not last:
            die(f'{name} was never vendored; pass --old <src dir> (an upstream checkout, say)')
        rev = git('rev-parse', f'{last}^').stdout.strip()
    dest = BUILD / name / f'old-{rev[:12]}'
    if not (dest / rel / 'src' / 'parser.c').exists():
        dest.mkdir(parents=True, exist_ok=True)
        archive = subprocess.run(['git', 'archive', rev, f'{rel}/src'], cwd=REPO, check=True, capture_output=True)
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(dest, filter='data')
    return dest / rel / 'src', f'git {rev[:12]}'


def new_source(name, override):
    src = Path(override) if override else REPO / 'grammars' / f'tree-sitter-{name}' / 'src'
    if not (src / 'parser.c').exists():
        die(f'no grammar at {src} yet')
    return src, str(src.relative_to(REPO)) if not override else override


# A grammar parsed only over another's ranges: the inline grammar runs
# over the block grammar's `inline` nodes (dump.c's TS_RANGES_LANGUAGE).
# Both variants take their ranges from the same (the replaced) block
# grammar, so a difference is the inline grammar's own.
RANGES_FROM = {'markdown_inline': 'markdown'}


def build_dump(name, src, variant, sanitize=False):
    """build/grammar-diff/<name>/<variant>/dump, rebuilt when its sources change."""
    sources = [src / 'parser.c'] + [p for p in (src / 'scanner.c', src / 'scanner.cc') if p.exists()]
    ranges = []
    if name in RANGES_FROM:
        ranges_src, _ = old_source(RANGES_FROM[name], None)
        ranges = [ranges_src / 'parser.c'] + [p for p in (ranges_src / 'scanner.c',) if p.exists()]
    flags = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else ['-O2']
    key = hashlib.sha256()
    for p in sources + ranges + [TOOL_DIR / 'dump.c', RUNTIME / 'src' / 'lib.c']:
        key.update(p.read_bytes())
    key.update(' '.join(flags).encode())
    out_dir = BUILD / name / variant
    exe, stamp = out_dir / 'dump', out_dir / 'dump.key'
    if exe.exists() and stamp.exists() and stamp.read_text() == key.hexdigest():
        return exe
    out_dir.mkdir(parents=True, exist_ok=True)
    cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
    include = ['-I', str(src), '-I', str(RUNTIME / 'include'), '-I', str(RUNTIME / 'src')]
    objects = []
    for c in sources + [TOOL_DIR / 'dump.c', RUNTIME / 'src' / 'lib.c']:
        obj = out_dir / (c.name + '.o')
        compiler = cxx if c.suffix == '.cc' else cc
        extra = [f'-DTS_LANGUAGE=tree_sitter_{name}'] if c.name == 'dump.c' else []
        if c.name == 'dump.c' and ranges:
            extra.append(f'-DTS_RANGES_LANGUAGE=tree_sitter_{RANGES_FROM[name]}')
        print(f'  building {variant}: {c.name}', file=sys.stderr)
        run([compiler, *flags, '-w', *include, *extra, '-c', str(c), '-o', str(obj)])
        objects.append(str(obj))
    for c in ranges:
        obj = out_dir / ('ranges-' + c.name + '.o')
        print(f'  building {variant}: {RANGES_FROM[name]} {c.name}', file=sys.stderr)
        run([cc, *flags, '-w', '-I', str(c.parent), '-I', str(RUNTIME / 'include'), '-c', str(c), '-o', str(obj)])
        objects.append(str(obj))
    run([cxx, *flags, *objects, '-o', str(exe)])
    stamp.write_text(key.hexdigest())
    return exe


# ---- symbols ----------------------------------------------------------------

def cmd_symbols(args):
    old_src, old_from = old_source(args.name, args.old)
    new_src, new_from = new_source(args.name, args.new)
    old = set(subprocess.run([build_dump(args.name, old_src, 'old'), '--symbols'], check=True, capture_output=True,
                             text=True).stdout.splitlines())
    new = set(subprocess.run([build_dump(args.name, new_src, 'new'), '--symbols'], check=True, capture_output=True,
                             text=True).stdout.splitlines())
    print(f'old: {old_from}: {sum(l.startswith("node") for l in old)} node types, '
          f'{sum(l.startswith("field") for l in old)} fields')
    print(f'new: {new_from}: {sum(l.startswith("node") for l in new)} node types, '
          f'{sum(l.startswith("field") for l in new)} fields')
    for title, items in (('only in old (queries using these would break)', old - new), ('only in new', new - old)):
        print(f'{title}: {len(items)}')
        for item in sorted(items):
            print(f'  {item}')
    return 1 if old - new else 0


# ---- diff -------------------------------------------------------------------

def parse_dump(path):
    trees, errors, current = {}, {}, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('@@FILE '):
                current = line[7:].rstrip('\n')
                trees[current] = []
            elif line.startswith('@@ERRORS '):
                errors[current] = int(line.split()[1])
            else:
                trees[current].append(line.rstrip('\n'))
    return trees, errors


def top_level_items(lines):
    """A dump's top-level items (the root's children) as
    {(start, end): (lines, has_error)}, each with its whole subtree."""
    items, current = {}, None
    for line in lines:
        depth, _, kind, start, end = line.split()
        if depth == '0':
            continue
        if depth == '1':
            current = (int(start), int(end))
            items[current] = ([], False)
        if current is None:
            continue
        sub, err = items[current]
        sub.append(line)
        items[current] = (sub, err or kind in ('ERROR', 'MISSING'))
    return items


def compare_items(old, new, files):
    """Top-level items the old grammar parsed without errors: how many the
    new grammar parses to the identical subtree, and the files of those it
    does not."""
    total = same = 0
    mismatched = collections.Counter()
    clean_but_different = []
    for f in files:
        new_items = top_level_items(new.get(f, []))
        for span, (sub, err) in top_level_items(old[f]).items():
            if err:
                continue
            total += 1
            got = new_items.get(span)
            if got and got[0] == sub:
                same += 1
            else:
                mismatched[f] += 1
                if got and not got[1]:
                    clean_but_different.append((f, span))
    return total, same, mismatched, clean_but_different


def error_bytes(lines):
    """Bytes inside ERROR nodes (outermost only, not counting a root that
    is itself ERROR -- that is the whole file, the same for both grammars)."""
    total, covered_to = 0, -1
    for line in lines:
        depth, _, kind, start, end = line.split()
        start, end = int(start), int(end)
        if kind == 'ERROR' and depth != '0' and start >= covered_to:
            total += end - start
            covered_to = end
    return total


def snippet(path, byte, width=70):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return ''
    start = data.rfind(b'\n', 0, byte) + 1
    end = data.find(b'\n', byte)
    line = data[start:end if end >= 0 else len(data)].decode(errors='replace')
    row = data.count(b'\n', 0, byte) + 1
    return f'line {row}: {line.strip()[:width]}'


def oracle_accepts(oracle, path):
    argv = [path if a == '{}' else a for a in oracle]
    try:
        return subprocess.run(argv, capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def known_differences(name):
    """grammars/tree-sitter-<name>/test/known-differences.txt: reviewed
    differences where the new grammar is right and the old one wrong, as
    `<path glob>  # why`. Blank lines and lines starting with # are skipped."""
    path = REPO / 'grammars' / f'tree-sitter-{name}' / 'test' / 'known-differences.txt'
    if not path.exists():
        return []
    known = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        pattern, _, why = line.partition('#')
        known.append((pattern.strip(), why.strip()))
    return known


def is_known(path, known):
    return next((why for pattern, why in known if fnmatch.fnmatch(path, pattern)), None)


# ---- Markdown's structural oracle -------------------------------------------

# Tree node types that are blocks to cmark, by cmark's names. Sections, link
# reference definitions (cmark drops them) and the rest are left out.
MARKDOWN_BLOCKS = {
    'paragraph': 'paragraph', 'thematic_break': 'thematic_break', 'block_quote': 'block_quote',
    'list': 'list', 'list_item': 'item', 'indented_code_block': 'code_block',
    'fenced_code_block': 'code_block', 'html_block': 'html_block', 'pipe_table': 'table',
}
CMARK_BLOCKS = {'paragraph', 'thematic_break', 'block_quote', 'list', 'item', 'code_block', 'html_block',
                'table', 'heading'}


def markdown_skeleton(lines):
    """A tree's blocks as (depth, kind) in document order, depth counting
    only block quotes, lists and items -- the shape cmark's XML has."""
    out, stack = [], []  # stack: (tree depth, is container)
    heading_depth = None
    for line in lines:
        d, _, t = line.split()[:3]
        d = int(d)
        while stack and stack[-1][0] >= d:
            stack.pop()
        if heading_depth is not None and d <= heading_depth:
            heading_depth = None
        nest = sum(1 for _, c in stack if c)
        if t in ('atx_heading', 'setext_heading'):
            out.append([nest, 'heading'])
            heading_depth = d
            stack.append((d, False))
            continue
        if heading_depth is not None:
            # The level, from the marker or underline; nothing else inside.
            for level in '123456':
                if t in (f'atx_h{level}_marker', f'setext_h{level}_underline'):
                    out[-1][1] = f'heading{level}'
            continue
        kind = MARKDOWN_BLOCKS.get(t)
        if kind:
            out.append([nest, kind])
        stack.append((d, kind in ('block_quote', 'list', 'item')))
    return [tuple(x) for x in out]


def cmark_skeleton(xml_text):
    import xml.etree.ElementTree as ET
    root = ET.fromstring(xml_text)
    out = []
    def walk(el, nest):
        tag = el.tag.split('}')[-1]
        if tag in CMARK_BLOCKS:
            out.append((nest, f'heading{el.get("level")}' if tag == 'heading' else tag))
            if tag in ('block_quote', 'list', 'item'):
                nest += 1
            elif tag in ('paragraph', 'heading', 'code_block', 'html_block', 'table', 'thematic_break'):
                return
        for child in el:
            walk(child, nest)
    walk(root, 0)
    return out


def cmark_binaries():
    """cmark-gfm (GFM's tables and task lists, on CommonMark 0.29) and cmark
    (CommonMark 0.31): a tree agreeing with either reads the file right."""
    return [[str(Path(nix_out_path('cmark-gfm')) / 'bin' / 'cmark-gfm'), '-e', 'table', '-e', 'tasklist'],
            [str(Path(nix_out_path('cmark')) / 'bin' / 'cmark')]]


def markdown_oracle(differ, old, new):
    """For each differing file: does cmark-gfm's block structure agree with
    the new tree, the old one, or neither? Fails when only the old agrees."""
    binaries = cmark_binaries()
    def judge(f):
        if any(' minus_metadata ' in l or ' plus_metadata ' in l for l in new.get(f, [])):
            return 'front matter (cmark has none)'
        wants = [cmark_skeleton(subprocess.run([*b, '--to', 'xml', f], capture_output=True, text=True,
                                               errors='replace').stdout) for b in binaries]
        n, o = markdown_skeleton(new.get(f, [])), markdown_skeleton(old.get(f, []))
        if n in wants:
            return 'new agrees with cmark'
        if o in wants:
            return 'only old agrees with cmark'
        return 'neither agrees with cmark'
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        verdicts = dict(zip(differ, pool.map(judge, differ)))
    counts = collections.Counter(verdicts.values())
    print(f'\noracle (cmark-gfm/cmark block structure), {len(differ)} differing files:')
    for verdict, count in counts.most_common():
        print(f'  {count:5}  {verdict}')
    bad = [f for f, v in verdicts.items() if v == 'only old agrees with cmark']
    for f in bad:
        print(f'  fails: {f}')
    for f, v in verdicts.items():
        if v == 'neither agrees with cmark':
            print(f'  neither: {f}')
    return 1 if bad else 0


def cmd_diff(args):
    spec = language(args.name)
    files = read_corpus(args, args.name)
    old_src, old_from = old_source(args.name, args.old)
    new_src, new_from = new_source(args.name, args.new)
    dumps = {}
    for variant, src in (('old', old_src), ('new', new_src)):
        exe = build_dump(args.name, src, variant)
        out = BUILD / args.name / variant / 'trees.txt'
        with open(out, 'w') as f:
            run([str(exe)], input=''.join(p + '\n' for p in files), stdout=f, text=True)
        dumps[variant] = parse_dump(out)
    (old, old_err), (new, new_err) = dumps['old'], dumps['new']

    all_differ = [f for f in old if old[f] != new.get(f)]
    known = known_differences(args.name)
    expected = {f: is_known(f, known) for f in all_differ if is_known(f, known)}
    differ = [f for f in all_differ if f not in expected]
    print(f'old: {old_from}\nnew: {new_from}')
    print(f'{len(old)} files: {len(old) - len(all_differ)} identical, {len(all_differ)} differ'
          + (f' ({len(expected)} of them known differences, {len(differ)} not)' if known else ''))
    errs_old = {f for f, n in old_err.items() if n}
    errs_new = {f for f, n in new_err.items() if n}
    print(f'files with parse errors: old {len(errs_old)}, new {len(errs_new)} '
          f'({len(errs_new - errs_old)} only in new, {len(errs_old - errs_new)} only in old)')

    buckets, examples = collections.Counter(), {}
    for f in differ:
        a, b = old[f], new.get(f, [])
        i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        left = a[i].split() if i < len(a) else ['', 'END', '', '0', '0']
        right = b[i].split() if i < len(b) else ['', 'END', '', '0', '0']
        key = f'{left[1]} {left[2]}  ->  {right[1]} {right[2]}'
        buckets[key] += 1
        examples.setdefault(key, (f, left, right))
    if buckets:
        print('\nfirst difference in each file, by kind (old -> new):')
    for key, count in buckets.most_common(args.kinds):
        f, left, right = examples[key]
        print(f'{count:6}  {key}')
        print(f'        e.g. {os.path.relpath(f, CORPUS) if f.startswith(str(CORPUS)) else f}')
        print(f'             old {left[3:]} new {right[3:]}  {snippet(f, int(left[3] or 0))}')

    if expected and args.show_known:
        reasons = collections.Counter(expected.values())
        print('\nknown differences (test/known-differences.txt):')
        for why, count in reasons.most_common():
            print(f'{count:6}  {why}')
    unused = [p for p, _ in known if not any(fnmatch.fnmatch(f, p) for f in all_differ)]
    if unused:
        print(f'\nknown-differences entries that no longer match a differing file: {len(unused)}')
        for p in unused:
            print(f'  {p}')

    status = 0
    if args.oracle and spec.get('structure_oracle') == 'markdown' and args.name == 'markdown':
        status = markdown_oracle(differ, old, new)
    elif args.oracle and spec.get('structure_oracle'):
        print('\noracle: none for the inline grammar (its differences are reviewed by hand)')
        status = 1 if differ else 0
    elif args.oracle:
        if not spec['oracle']:
            die(f'{args.name} has no oracle (see LANGUAGES in this script)')
        suspects = sorted(set(differ) | {f for f in errs_new - errs_old if f not in expected})
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            accepted = [f for f, ok in zip(suspects, pool.map(lambda f: oracle_accepts(spec['oracle'], f), suspects))
                        if ok]
        print(f'\noracle ({spec["oracle"][0]}): of {len(suspects)} files that differ or fail only in new, '
              f'{len(suspects) - len(accepted)} are rejected by the language itself, {len(accepted)} accepted')
        for f in accepted:
            print(f'  accepted: {f}' + ('  [parse error in new]' if f in errs_new else ''))
        status = 1 if accepted else 0
    elif args.old_clean:
        clean = [f for f in differ if f not in errs_old]
        total, same, mismatched, clean_but_different = compare_items(old, new, differ)
        total_all = total + sum(1 for f in old if f not in differ for _, (_, e) in top_level_items(old[f]).items()
                                if not e)
        print(f'\nold-clean: {len(clean)} differing files the old grammar parsed without errors')
        for f in clean[:args.kinds]:
            print(f'  {f}')
        print(f'top-level items the old grammar parsed without errors: {total_all}; '
              f'{total_all - (total - same)} identical in the new, {total - same} not, in {len(mismatched)} files')
        clean_but_different = [(f, span) for f, span in clean_but_different if f not in expected]
        print(f'  parsed without errors by both but differently: {len(clean_but_different)} (fails)')
        for f, (start, end) in clean_but_different[:args.kinds]:
            print(f'    {f}:{start}-{end}  {snippet(f, start)}')
        recovery = (total - same) - len(clean_but_different)
        print(f'  inside or across an error in the new tree: {recovery} (error recovery; see below)')
        for f, n in mismatched.most_common(args.kinds):
            print(f'    {n:4}  {f}')
        old_err = sum(error_bytes(old[f]) for f in old)
        new_err = sum(error_bytes(new[f]) for f in new)
        worse = sum(1 for f in old if error_bytes(new[f]) > error_bytes(old[f]))
        better = sum(1 for f in old if error_bytes(new[f]) < error_bytes(old[f]))
        print(f'bytes inside ERROR nodes: old {old_err}, new {new_err} '
              f'({100 * (new_err - old_err) / max(old_err, 1):+.1f}%); new worse in {worse} files, better in {better}')
        status = 1 if [f for f in clean if f not in expected] or clean_but_different else 0
    elif differ:
        status = 1
    return status


# ---- fuzz -------------------------------------------------------------------

def mutate(data, tokens, rng):
    b = bytearray(data)
    for _ in range(rng.randint(1, 8)):
        pos, op = rng.randrange(len(b) + 1), rng.random()
        if op < 0.4 and b:
            del b[pos:pos + rng.randint(1, 20)]
        elif op < 0.8:
            b[pos:pos] = rng.choice(tokens).encode()
        elif b:
            b[min(pos, len(b) - 1)] = rng.randrange(256)
    return bytes(b)


def make_mutants(args, spec, files, dirname):
    """args.count damaged copies of corpus files (seeded), in build/grammar-diff/<name>/<dirname>
    (one per command, so a fuzz and a recovery run can share a machine)."""
    mutants_dir = BUILD / args.name / dirname
    if mutants_dir.exists():
        shutil.rmtree(mutants_dir)
    mutants_dir.mkdir(parents=True)
    rng = random.Random(args.seed)
    tokens = GENERIC_TOKENS + spec['tokens']
    mutants = []
    for i in range(args.count):
        data = Path(rng.choice(files)).read_bytes()
        path = mutants_dir / f'{i:05d}{Path(files[0]).suffix or ".txt"}'
        path.write_bytes(mutate(data, tokens, rng))
        mutants.append(str(path))
    return mutants


def cmd_fuzz(args):
    spec = language(args.name)
    files = read_corpus(args, args.name)
    src, src_from = (old_source(args.name, args.old) if args.fuzz_old else new_source(args.name, args.new))
    exe = build_dump(args.name, src, 'old-asan' if args.fuzz_old else 'asan', sanitize=True)

    mutants = make_mutants(args, spec, files, 'mutants')
    queue = ([] if args.mutants_only else files) + mutants
    print(f'{src_from}: {len(queue)} files ({len(mutants)} damaged, seed {args.seed}) under ASan + UBSan')

    env = dict(os.environ, DUMP_TRACE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
               UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1')
    env.pop('LD_LIBRARY_PATH', None)
    if shutil.which('llvm-symbolizer'):
        env['ASAN_SYMBOLIZER_PATH'] = shutil.which('llvm-symbolizer')
    failures_dir = BUILD / args.name / 'failures'
    if failures_dir.exists():
        shutil.rmtree(failures_dir)
    failures = []
    batch = 500
    i = 0
    while i < len(queue):
        chunk = queue[i:i + batch]
        log = BUILD / args.name / 'fuzz.log'
        with open(log, 'w') as err:
            try:
                proc = subprocess.run([str(exe)], input=''.join(p + '\n' for p in chunk), stdout=subprocess.DEVNULL,
                                      stderr=err, text=True, env=env, timeout=args.timeout)
                failed = proc.returncode != 0
                kind = 'crash'
            except subprocess.TimeoutExpired:
                failed, kind = True, 'hang'
        if not failed:
            i += len(chunk)
            continue
        text = log.read_text(errors='replace')
        parsing = re.findall(r'^@@PARSING (.*)$', text, re.M)
        culprit = parsing[-1] if parsing else chunk[0]
        report = text[text.rfind('@@PARSING'):].split('\n', 1)[-1].strip()
        failures_dir.mkdir(parents=True, exist_ok=True)
        saved = failures_dir / f'{len(failures):03d}-{kind}{Path(culprit).suffix}'
        shutil.copyfile(culprit, saved)
        failures.append((kind, culprit, saved, report))
        print(f'{kind}: {culprit} (saved as {saved.relative_to(REPO)})')
        for line in report.splitlines()[:8]:
            print(f'    {line}')
        i += chunk.index(culprit) + 1 if culprit in chunk else len(chunk)
    print(f'{len(failures)} crash(es)/hang(s)' if failures else 'clean')
    return 1 if failures else 0


# ---- recovery ---------------------------------------------------------------

def stream_error_bytes(exe, files):
    """error_bytes of each file's tree, read from the dump as it runs: the
    trees of a few thousand damaged files run to gigabytes."""
    out, current, lines = {}, None, []
    with subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                          errors='replace') as proc:
        # Fed from a thread: the dump answers each name as it reads it, so
        # writing the whole list first deadlocks once both pipes fill.
        def feed():
            proc.stdin.write(''.join(f + '\n' for f in files))
            proc.stdin.close()
        feeder = threading.Thread(target=feed)
        feeder.start()
        for line in proc.stdout:
            if line.startswith('@@FILE '):
                current, lines = line[7:].rstrip('\n'), []
            elif line.startswith('@@ERRORS '):
                out[current] = error_bytes(lines)
            else:
                lines.append(line)
    feeder.join()
    if proc.returncode != 0:
        die(f'{exe} failed (exit {proc.returncode}) after {len(out)} of {len(files)} files')
    return out


def cmd_recovery(args):
    """How much of each damaged file ends up inside ERROR nodes, old grammar
    vs new: code being typed is broken most of the time, and an editor
    colours what recovery leaves outside the errors. Informational -- the
    two recover differently file by file; the totals are the measure."""
    spec = language(args.name)
    mutants = make_mutants(args, spec, read_corpus(args, args.name), 'recovery-mutants')
    totals, per_file = {}, {}
    for variant, (src, _) in (('old', old_source(args.name, args.old)), ('new', new_source(args.name, args.new))):
        per_file[variant] = stream_error_bytes(build_dump(args.name, src, variant), mutants)
        totals[variant] = sum(per_file[variant].values())
    old, new = per_file['old'], per_file['new']
    better = sum(new[m] < old[m] for m in mutants)
    worse = sorted(((new[m] - old[m], m) for m in mutants if new[m] > old[m]), reverse=True)
    change = (totals['new'] - totals['old']) / max(totals['old'], 1)
    print(f'{len(mutants)} damaged files (seed {args.seed}): bytes in ERROR nodes old {totals["old"]}, '
          f'new {totals["new"]} ({change:+.1%}); new better in {better}, worse in {len(worse)}')
    for delta, m in worse[:args.worst]:
        print(f'  +{delta}  {m}')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('corpus', help='fetch and unpack a corpus')
    p.add_argument('lang')
    p.set_defaults(func=cmd_corpus)

    for name, func, helptext in (('symbols', cmd_symbols, 'compare node types and fields'),
                                 ('diff', cmd_diff, 'compare trees over the corpus'),
                                 ('fuzz', cmd_fuzz, 'damaged files under ASan + UBSan'),
                                 ('recovery', cmd_recovery, 'error recovery on damaged files, old vs new')):
        p = sub.add_parser(name, help=helptext)
        p.add_argument('name', help='grammar name')
        p.add_argument('--old', help='src dir of the grammar being replaced')
        p.add_argument('--new', help='src dir of the new grammar')
        p.set_defaults(func=func)
        if name != 'symbols':
            p.add_argument('--corpus', help='file list (default: build/grammar-corpus/<name>/files.txt)')
    sub.choices['diff'].add_argument('--oracle', action='store_true',
                                     help="ask the language's own implementation about each differing file")
    sub.choices['diff'].add_argument('--kinds', type=int, default=15, help='difference kinds to show')
    sub.choices['diff'].add_argument('--old-clean', action='store_true',
                                     help='for a language with no oracle: fail only on differing files '
                                          'the old grammar parsed without errors')
    sub.choices['diff'].add_argument('--show-known', action='store_true',
                                     help='list the known differences by reason')
    for name in ('fuzz', 'recovery'):
        sub.choices[name].add_argument('--count', type=int, default=3000, help='damaged files to make')
        sub.choices[name].add_argument('--seed', type=int, default=7)
    sub.choices['recovery'].add_argument('--worst', type=int, default=10, help='worst files to list')
    fuzz = sub.choices['fuzz']
    fuzz.add_argument('--timeout', type=int, default=300, help='seconds per batch of 500 before calling it a hang')
    fuzz.add_argument('--mutants-only', action='store_true', help='skip the undamaged corpus')
    fuzz.add_argument('--fuzz-old', action='store_true', help='fuzz the old grammar instead')

    args = parser.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == '__main__':
    main()
