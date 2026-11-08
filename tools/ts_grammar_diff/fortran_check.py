#!/usr/bin/env python3
"""Is a file valid Fortran, as far as its *syntax* goes? gfortran decides.

    fortran_check.py FILE              exit 0 if gfortran reports no syntax-class
                                       error, else print "LINE: MESSAGE" (the
                                       first one) and exit 1
    fortran_check.py --json FILE...    one JSON object per file: verdict, every
                                       error (line, message, class)
    fortran_check.py --build-mods      compile the corpus once so the modules
                                       its files USE exist (per source tree)

gfortran has no parse-only mode: -fsyntax-only also resolves names, so a
file that USEs a module gfortran cannot find stops at once ("Cannot open
module file"), and one whose derived types are unknown reports semantic
errors -- sometimes worded as syntax errors (`call z%m()` with no type for
`z` is a "Syntax error in CALL statement"). So:

  * the modules come from the corpus itself: `--build-mods` runs every file
    of a source tree with -J into build/grammar-corpus/fortran-mods/<tree>,
    pass after pass until no new module appears;
  * a module (or INCLUDE file, or #include) that still cannot be found is
    stubbed: its USE/INCLUDE line is blanked and gfortran runs again;
  * each error message is classed `syntax` or `semantic` by its wording
    (SEMANTIC below); the verdict counts syntax errors only.

The flags are the lenient ones real builds use: -ffree-line-length-none,
-fcoarray=single (coarray syntax parses), -fallow-argument-mismatch; fixed
form keeps its 72 columns (sequence numbers in 73-80 are comments). A
lower-case .f90 with preprocessor lines gets -cpp, as its build would.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CORPUS = REPO / 'build' / 'grammar-corpus' / 'fortran'
MODS = REPO / 'build' / 'grammar-corpus' / 'fortran-mods'

FIXED_SUFFIXES = {'.f', '.for', '.ftn', '.fpp'}
CPP_SUFFIXES = {'.F', '.F90', '.F95', '.F03', '.F08', '.FOR', '.FPP', '.FTN'}

# Messages that are about meaning, not form: names, types, interfaces,
# constraints checked after a statement was recognised. Everything else
# ("Syntax error in ...", "Unclassifiable statement", "Expecting END ...",
# "Unexpected ... statement", "Invalid character ...", "Missing ...") is
# syntax-class.
SEMANTIC = [
    r'has no IMPLICIT type', r'is being used before it is defined', r'Cannot open module file',
    r'is not a member of', r'has not been declared', r'is not a variable', r'Type mismatch',
    r'Rank mismatch', r'Shape mismatch', r'Interface mismatch', r'There is no specific',
    r'is not a function', r'is not a subroutine', r'is already defined', r'already has basic type',
    r'Symbol .* at \(1\) conflicts', r'conflicts with', r'is not a DUMMY', r'Unexpected \'%\' for nonderived',
    r'is not an intrinsic', r'Missing actual argument', r'Too many arguments', r'Keyword argument',
    r'must be', r'should be', r'shall be', r'cannot be', r'may not', r'is not allowed', r'not permitted',
    r'is used before', r'Cannot IMPORT', r'not found in module', r'is not within a construct',
    r'has not been previously defined', r'only permitted in an INTERFACE body', r'Label .* referenced .* never defined', r'Label .* defined but not used',
    r'Explicit interface required', r'Different (type|kind|shape|ranks)', r'Dummy argument',
    r'Actual argument', r'Allocatable', r'POINTER attribute', r'ALLOCATABLE', r'Assumed[- ]',
    r'Component .* at \(1\)', r'Derived type', r'Procedure .* at \(1\)', r'Function .* at \(1\)',
    r'Subroutine .* at \(1\)', r'Variable .* at \(1\)', r'Name .* at \(1\) is an ambiguous',
    r'ambiguous', r'Generic', r'specific', r'Operator', r'operator', r'Assignment', r'Arithmetic',
    r'Integer too big', r'overflow', r'Array reference', r'Array index', r'out of bounds',
    r'Different CHARACTER lengths', r'Unequal character lengths', r'NULL', r'Entity', r'entity',
    r'Elemental', r'PURE', r'pure', r'Impure', r'RECURSIVE', r'recursive', r'not been (defined|declared)',
    r'is not accessible', r'PRIVATE', r'PUBLIC', r'Unclassifiable statement at \(1\)$__never__',
    r'Coarray', r'coarray', r'Polymorphic', r'polymorphic', r'CLASS variable', r'abstract',
    r'ABSTRACT', r'deferred', r'DEFERRED', r'Non-', r'non-', r'Nonconforming', r'Fortran 20\d\d:',
    r'Fortran 9\d:', r'Legacy Extension', r'GNU Extension', r'Extension:', r'Deleted feature',
    r'Obsolescent', r'BOZ', r'Hollerith', r'Statement function', r'unused', r'Unused',
    r'Cannot change attributes', r'Cannot assign', r'Expression at \(1\) must', r'Bad array',
    r'Incompatible', r'incompatible', r'Interface', r'interface', r'Initialization', r'initialization',
    r'Parameter', r'PARAMETER', r'Equivalence', r'EQUIVALENCE', r'COMMON', r'Common block',
    r'DATA statement', r'Duplicate', r'duplicate', r'redefin', r'Redefin', r'SAVE', r'INTENT',
    r'VALUE attribute', r'BIND\(C\)', r'C interoperable', r'kind', r'KIND', r'Kind', r'Invalid kind',
    r'Unsupported', r'not yet implemented', r'Sorry', r'Fatal Error: Error count', r'Reading module',
    r'Possible change of value', r'Divide by zero', r'Integer division', r'Result of', r'Arguments of',
    r'argument', r'Argument', r'Unexpected end of format', r'Fatal Error: Cannot rename',
    r'Can\'t open included file', r'No such file', r'Unknown attribute', r'type-bound', r'Type-bound',
    r'Type name', r'type name', r'Character length', r'character length', r'length', r'Substring',
    r'substring', r'Unknown intrinsic', r'Intrinsic', r'intrinsic', r'Implicit', r'IMPLICIT',
    r'Selector', r'selector', r'Associate', r'ASSOCIATE', r'associate', r'Unlimited polymorphic',
    r'SELECT TYPE', r'SELECT RANK', r'CASE label', r'Logical range in CASE', r'overlaps with CASE',
    r'Expected (a|an) .* (variable|expression|array)', r'Pointer', r'pointer', r'Target', r'TARGET',
    r'Scalar', r'scalar', r'Rank', r'rank', r'Shape', r'shape', r'Constant', r'constant',
    r'Branch at', r'branch', r'DO loop', r'DO termination', r'DO-iterator', r'GOTO', r'GO TO',
    r'FORALL', r'WHERE', r'Mask', r'mask', r'ENTRY', r'Alternate return', r'alternate return',
    r'RETURN', r'STOP code', r'ERROR STOP', r'Image', r'image', r'Team', r'team', r'EVENT', r'LOCK',
    r'Unit number', r'UNIT', r'FMT', r'Format', r'FORMAT label', r'format', r'NAMELIST', r'Namelist',
    r'IOSTAT', r'IOMSG', r'ADVANCE', r'Positive width', r'Nonnegative width', r'Period required',
    r'Real number', r'Expected P edit', r'Unexpected element', r'Comma required', r'positive',
    r'Assigned label', r'ASSIGN', r'Label .* at \(1\)', r'label', r'Fatal Error: Unexpected EOF$__never__',
]
SEMANTIC_RE = re.compile('|'.join(SEMANTIC))

# These read as "the statement could not be recognised", whatever else
# the message says; they win over SEMANTIC.
SYNTAX_FORCE = re.compile(
    r'^(Syntax error|Unclassifiable statement|Unexpected (END|end|junk|statement|.* statement)|'
    r'Expecting|Expected (a |an )?(\(|\)|,|::|=|right|left|end|END|label|formal|initialization)|'
    r'Invalid character|Missing (\)|\(|right|left|terminating|leading|closing|opening|exponent|comma)|'
    r'Non-numeric character in statement label|Zero is not a valid statement label|'
    r'Unterminated character constant|Unexpected EOF|Junk after|Bad continuation|'
    r'Blank required|Statement label without statement|Ill-formed|Invalid form of|'
    r'Empty IMPLICIT statement|Illegal preprocessor directive|Label .* already used|'
    r'Unexpected CONTAINS|Unexpected .* in|Two main PROGRAMs|Line truncated|'
    r'Invalid Hollerith|Missing format label|END tag label|Statement label in ENDDO|'
    r'Unexpected intrinsic type|Unexpected data declaration|Unexpected (character|junk|end))')

ERROR_RE = re.compile(r'^(?P<file>[^\n:]+):(?P<line>\d+):(?P<col>\d+):\n(?:.*\n)*?(?P<kind>Error|Fatal Error): (?P<msg>.*)$',
                      re.M)
SIMPLE_RE = re.compile(r'^(?P<file>[^\n:]+):(?P<line>\d+):(?:(?P<col>\d+):)? (?:fatal )?(?P<kind>error|Error|Fatal Error):'
                       r' (?P<msg>.*)$', re.M)
MOD_RE = re.compile(r"Cannot open module file [‘'](?P<name>[^’']+)\.mod[’']")
INC_RE = re.compile(r"(?:Can't|Cannot) open included file [‘'](?P<name>[^’']+)[’']")
CPP_INC_RE = re.compile(r'^(?P<file>[^\n:]+):(?P<line>\d+):\d+: fatal error: (?P<name>[^:]+): No such file', re.M)


def classify(msg):
    if SYNTAX_FORCE.search(msg):
        return 'syntax'
    if SEMANTIC_RE.search(msg):
        return 'semantic'
    return 'syntax'


def is_fixed(path):
    suffix = Path(path).suffix
    return suffix.lower() in FIXED_SUFFIXES


_ROOTS = None


def tree_of(path):
    """The corpus source tree a file belongs to (for its module dir), or None:
    an extracted tarball under the corpus dir, or a nixpkgs source read in
    place (grammar_diff.py keeps a GC root per attribute)."""
    global _ROOTS
    p = Path(path).resolve()
    try:
        rel = p.relative_to(CORPUS)
        return rel.parts[0] if rel.parts else None
    except ValueError:
        pass
    if _ROOTS is None:
        _ROOTS = {}
        gc = CORPUS.parent / '.gcroots'
        if gc.is_dir():
            for link in gc.iterdir():
                try:
                    _ROOTS[str(link.resolve())] = link.name
                except OSError:
                    pass
    s = str(p)
    for root, name in _ROOTS.items():
        if s.startswith(root + '/'):
            return name
    return None


def gfortran():
    return shutil.which('gfortran') or 'gfortran'


def base_flags(path, text):
    flags = ['-fsyntax-only', '-fmax-errors=0', '-fcoarray=single', '-fallow-argument-mismatch', '-w',
             '-fdiagnostics-plain-output', '-fno-diagnostics-show-caret']
    if not is_fixed(path):
        flags.append('-ffree-line-length-none')
    if Path(path).suffix not in CPP_SUFFIXES and re.search(rb'^[ \t]*#[ \t]*(if|ifdef|ifndef|define|include)\b', text,
                                                           re.M):
        flags.append('-cpp')
    d = Path(path).resolve().parent
    for inc in (d, d.parent, d.parent / 'include', d / 'include'):
        flags.append(f'-I{inc}')
    return flags


def env():
    e = dict(os.environ)
    for k in ('NIX_CFLAGS_COMPILE', 'NIX_LDFLAGS', 'LD_LIBRARY_PATH'):
        e.pop(k, None)
    e['LC_ALL'] = 'C'
    return e


def blank_lines(text, pred, fixed):
    """Blank every line (and its continuation lines) whose statement matches pred."""
    lines = text.split(b'\n')
    i = 0
    while i < len(lines):
        if pred(lines[i]):
            j = i
            lines[j] = b''
            if fixed:
                while j + 1 < len(lines) and len(lines[j + 1]) > 5 and lines[j + 1][:5].strip() == b'' and \
                        lines[j + 1][5:6] not in (b' ', b'0', b'\t', b''):
                    j += 1
                    lines[j] = b''
            else:
                cur = text.split(b'\n')[i]
                while re.search(rb'&\s*(!.*)?$', cur) and j + 1 < len(lines):
                    j += 1
                    cur = lines[j]
                    lines[j] = b''
            i = j
        i += 1
    return b'\n'.join(lines)


def run_gfortran(path, text, moddir, tmp):
    """gfortran over `text` (a copy of `path` placed beside nothing else): (rc, output)."""
    src = Path(tmp) / ('src' + Path(path).suffix)
    src.write_bytes(text)
    flags = base_flags(path, text)
    argv = [gfortran(), *flags, f'-J{tmp}', f'-I{tmp}']
    if moddir:
        argv.append(f'-I{moddir}')
    argv.append(str(src))
    try:
        p = subprocess.run(argv, capture_output=True, env=env(), timeout=120, cwd=tmp)
    except subprocess.TimeoutExpired:
        return None, ''
    return p.returncode, p.stderr.decode('utf-8', 'replace')


def parse_errors(out):
    errs = []
    for m in SIMPLE_RE.finditer(out):
        msg = m.group('msg').strip()
        errs.append(dict(line=int(m.group('line')), col=int(m.group('col') or 0), msg=msg, cls=classify(msg),
                         fatal=m.group('kind').lower().startswith('fatal')))
    # Errors with no location: `f951: Error: Unexpected end of file in 'x.f90'`
    # (a program unit or construct left open). Placed at line 0 = end of file.
    for m in re.finditer(r'^f951: (?:Fatal )?Error: (?P<msg>Unexpected end of file)\b.*$', out, re.M):
        errs.append(dict(line=0, col=0, msg=m.group('msg').strip(), cls='syntax', fatal=True))
    return errs


def check(path, moddir=None):
    """dict(verdict='ok'|'error'|'timeout', errors=[...], stubbed=[...])."""
    text = Path(path).read_bytes()
    fixed = is_fixed(path)
    if moddir is None:
        tree = tree_of(path)
        if tree and (MODS / tree).is_dir():
            moddir = MODS / tree
    stubbed = []
    with tempfile.TemporaryDirectory(prefix='fchk') as tmp:
        for _ in range(40):
            rc, out = run_gfortran(path, text, moddir, tmp)
            if rc is None:
                return dict(verdict='timeout', errors=[], stubbed=stubbed)
            m = MOD_RE.search(out)
            if m:
                name = m.group('name')
                stubbed.append('use ' + name)
                pat = re.compile(rb'^\s*use\b\s*(,\s*(non_)?intrinsic\s*)?(::)?\s*' + re.escape(name.encode()) +
                                 rb'\b', re.I)
                if fixed:
                    pat = re.compile(rb'^ {6}\s*use\b\s*(,\s*(non_)?intrinsic\s*)?(::)?\s*' +
                                     re.escape(name.encode()) + rb'\b', re.I)
                new = blank_lines(text, pat.match, fixed)
                if new == text:
                    break
                text = new
                continue
            m = INC_RE.search(out)
            if m:
                name = m.group('name')
                stubbed.append('include ' + name)
                pat = re.compile(rb'^\s*include\s*[\'"]' + re.escape(name.encode()) + rb'[\'"]', re.I)
                new = blank_lines(text, pat.match, False)
                if new == text:
                    break
                text = new
                continue
            m = CPP_INC_RE.search(out)
            if m:
                name = m.group('name')
                stubbed.append('#include ' + name)
                pat = re.compile(rb'^\s*#\s*include\s*[<"]' + re.escape(name.encode()) + rb'[>"]')
                new = blank_lines(text, pat.match, True)
                if new == text:
                    break
                text = new
                continue
            break
        errs = parse_errors(out)
    syntax = [e for e in errs if e['cls'] == 'syntax']
    return dict(verdict='error' if syntax else 'ok', errors=errs, stubbed=stubbed)


def build_mods(files):
    """Every corpus tree's modules, built by compiling its files until no new .mod appears."""
    import concurrent.futures
    by_tree = {}
    for f in files:
        t = tree_of(f)
        if t:
            by_tree.setdefault(t, []).append(f)
    for tree, members in sorted(by_tree.items()):
        if tree.startswith('gfortran'):
            continue  # test-suite files are self-contained
        out = MODS / tree
        out.mkdir(parents=True, exist_ok=True)
        pending = list(members)
        for npass in range(12):
            before = len(list(out.glob('*.mod')))

            def one(f):
                text = Path(f).read_bytes()
                if not re.search(rb'^\s*module\s+(?!procedure)\w', text, re.I | re.M):
                    return f, True
                with tempfile.TemporaryDirectory(prefix='fmod') as tmp:
                    src = Path(tmp) / ('src' + Path(f).suffix)
                    src.write_bytes(text)
                    argv = [gfortran(), *base_flags(f, text), f'-J{out}', f'-I{out}', str(src)]
                    try:
                        p = subprocess.run(argv, capture_output=True, env=env(), timeout=120, cwd=tmp)
                    except subprocess.TimeoutExpired:
                        return f, True
                    return f, not MOD_RE.search(p.stderr.decode('utf-8', 'replace'))

            with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 8) as ex:
                results = list(ex.map(one, pending))
            pending = [f for f, done in results if not done]
            after = len(list(out.glob('*.mod')))
            print(f'{tree}: pass {npass + 1}: {after} modules, {len(pending)} files still missing one', flush=True)
            if after == before or not pending:
                break


def main(argv):
    if argv and argv[0] == '--build-mods':
        files = [line for line in (CORPUS / 'files.txt').read_text().splitlines() if line]
        build_mods(files)
        return 0
    if argv and argv[0] == '--json':
        for f in argv[1:]:
            r = check(f)
            r['file'] = f
            print(json.dumps(r), flush=True)
        return 0
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    r = check(argv[0])
    if r['verdict'] == 'timeout':
        return 0  # no verdict
    for e in r['errors']:
        if e['cls'] == 'syntax':
            print(f"{e['line']}: {e['msg']}")
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
