#!/usr/bin/env python3
"""Generate src/fortran_lsp_vocab.cpp: the names mep's Fortran server knows.

    python3 -I tools/gen_fortran_vocab.py INFO_DIR [--gfortran gfortran]

    e.g. (inside `nix develop`, which puts gfortran on PATH):
    python3 -I tools/gen_fortran_vocab.py \\
        /nix/store/...-gfortran-14.3.0-info/share/info

Two sources, both gfortran's own, read mechanically:

  - The gfortran info manual (INFO_DIR/gfortran.info*, plain or gzipped):
    every node of the "Intrinsic Procedures" chapter gives a procedure's
    Standard, Class and Syntax (the signature lines, with the keyword
    argument names) and its "Specific names" table; the "Intrinsic Modules"
    chapter lists ISO_FORTRAN_ENV's and ISO_C_BINDING's named constants.
    The manual is under the GFDL and mep is MIT, so only names, signatures,
    argument names, class and standard come from it -- never its prose.
  - gfortran itself: a throwaway module that USEs each intrinsic module is
    compiled with --gfortran, and the public names, their flavour
    (constant/type/procedure), procedures' dummy argument names and which
    are OPTIONAL are read back out of the .mod file it writes. That covers
    IEEE_ARITHMETIC / IEEE_EXCEPTIONS / IEEE_FEATURES (which the manual does
    not itemise) and OMP_LIB / OMP_LIB_KINDS, and completes the two ISO
    modules.

Every one-line summary is written here, in our own words (the specific
names' and the C-interop constants' summaries are templated from the type
columns of the manual's tables). The statement keywords are a hand list.

Conventions in the output:
  - signature lines drop the manual's "RESULT = " / "CALL " prefix, except
    that a procedure the manual classes as "Subroutine, function" keeps
    "CALL " on its subroutine form so the two forms stay distinguishable;
  - min/max_args are taken over every form (max -1 when a form ends in
    "..."); -1/-1 for constants, types and keywords;
  - "TS 18508" / "TS 29113" (folded into Fortran 2018) read as Fortran 2018.
"""

import argparse
import gzip
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'src' / 'fortran_lsp_vocab.cpp'

# ----------------------------------------------------------------------------
# Reading the manual


def read_info(info_dir):
    files = sorted(Path(info_dir).glob('gfortran.info*'),
                   key=lambda p: (len(p.name), p.name))
    if not files:
        sys.exit('no gfortran.info* under %s' % info_dir)
    text = ''
    for f in files:
        data = f.read_bytes()
        if f.suffix == '.gz' or data[:2] == b'\x1f\x8b':
            data = gzip.decompress(data)
        text += data.decode('utf-8')
    return text


def nodes(text):
    """[(node name, up name, body)]"""
    out = []
    for chunk in text.split('\x1f'):
        chunk = chunk.lstrip('\n')
        m = re.match(r'File: gfortran\.info,  Node: ([^,\n]+),[^\n]*?Up: ([^\n]+)\n', chunk)
        if m:
            out.append((m.group(1), m.group(2).strip(), chunk[m.end():]))
    return out


def fields(body):
    parts = re.split(r'^(_[A-Za-z ]+_):[ \t]*$', body, flags=re.M)
    d = {}
    for i in range(1, len(parts), 2):
        d[parts[i].strip('_')] = parts[i + 1]
    return d


def squash(s):
    return ' '.join(s.split())


STD_RE = re.compile(r'Fortran (77|90|95|2003|2008|2018|2023)|TS 18508|TS 29113|'
                    r'Technical Specification \(TS\) (?:18508|29113)|GNU extension')


def norm_standard(s):
    m = STD_RE.search(squash(s))
    if not m:
        return ''
    t = m.group(0)
    if 'TS' in t:
        return 'Fortran 2018'
    return t


def norm_class(s):
    s = squash(s).rstrip('.')
    low = s.lower()
    m = re.match(r'(.*) of the module ', low)
    if m:
        return m.group(1)
    if low.startswith('elemental function, except for the transformational'):
        return 'elemental function (transformational for the N1, N2 form)'
    return low


ARG_SPLIT = re.compile(r'\s*,\s*')


def parse_args(arglist):
    """'ARRAY, DIM [, MASK]' -> ([(name, optional)], unbounded)"""
    args = []
    unbounded = False
    depth = 0
    cur = ''

    def flush():
        nonlocal cur
        name = cur.strip()
        cur = ''
        if not name:
            return
        if name == '...':
            nonlocal unbounded
            unbounded = True
            return
        args.append((name, opt_of[0]))

    opt_of = [False]
    for ch in arglist:
        if ch == '[':
            flush()
            depth += 1
            opt_of[0] = True
        elif ch == ']':
            flush()
            depth -= 1
            opt_of[0] = depth > 0
        elif ch == ',':
            flush()
            opt_of[0] = depth > 0
        else:
            if not cur.strip():
                opt_of[0] = depth > 0
            cur += ch
    flush()
    return args, unbounded


def fix_syntax(seg):
    """Normalise one ‘...’ syntax segment; returns (is_call, name, argtext|None)."""
    s = squash(seg)
    s = s.rstrip('.').strip()
    is_call = False
    if s.startswith('CALL '):
        is_call = True
        s = s[5:].strip()
    else:
        s = re.sub(r'^[A-Za-z_][A-Za-z_0-9]*\s*=>?\s*', '', s)
    m = re.match(r'([A-Za-z_][A-Za-z_0-9]*)\s*(\((.*)\))?\s*$', s)
    if not m:
        m = re.match(r'([A-Za-z_][A-Za-z_0-9]*)\s*\((.*)$', s)
        if not m:
            raise ValueError('cannot parse syntax %r' % seg)
        name, inner = m.group(1), m.group(2).rstrip(')')
    else:
        name, inner = m.group(1), m.group(3)
    if inner is not None:
        inner = inner.upper()
        # Typos in the manual: INT(A [, KIND)), FINDLOC(ARRAY, VALUE, [, MASK] ...).
        inner = re.sub(r',\s*\[', ' [', inner)
        inner = re.sub(r'^\s*\[\s*,', '[', inner)
        opens = inner.count('[') - inner.count(']')
        while opens > 0 and inner.endswith(')'):
            inner = inner[:-1]
        inner = inner.rstrip(')')
        inner += ']' * max(0, inner.count('[') - inner.count(']'))
        inner = inner.replace(')', '')
    return is_call, name.upper(), inner


def format_sig(name, args, unbounded):
    out = ''
    for i, (a, opt) in enumerate(args):
        if opt:
            out += (' [, %s]' if i else '[%s]') % a
        else:
            out += (', %s' if i else '%s') % a
    if unbounded:
        out += ' [, ...]'
    return '%s(%s)' % (name, out)


def parse_intrinsics(all_nodes):
    """-> dict name -> entry dict, plus specifics list."""
    procs = {}
    specifics = []
    for node, up, body in all_nodes:
        if up != 'Intrinsic Procedures':
            continue
        f = fields(body)
        if 'Syntax' not in f:
            continue
        std = norm_standard(f.get('Standard', ''))
        klass = norm_class(f.get('Class', ''))
        segs = re.findall(r'‘([^’]*)’', f['Syntax'])
        forms = {}
        for seg in segs:
            is_call, name, inner = fix_syntax(seg)
            forms.setdefault(name, []).append((is_call, inner))
        for name, flist in forms.items():
            sigs = []
            mins, maxs = [], []
            dual = any(c for c, _ in flist) and not all(c for c, _ in flist)
            for is_call, inner in flist:
                if inner is None:
                    args, unb = [], False
                    sig = name + '()'
                else:
                    args, unb = parse_args(inner)
                    sig = format_sig(name, args, unb)
                if dual and is_call:
                    sig = 'CALL ' + sig
                if sig not in sigs:
                    sigs.append(sig)
                mins.append(sum(1 for _, o in args if not o))
                maxs.append(-1 if unb else len(args))
            kind = 'Subroutine' if all(c for c, _ in flist) else 'Function'
            lname = name.lower()
            if lname in procs:
                continue
            # A differently named form in a node (REALPART in REAL's) is
            # gfortran's alias, an extension.
            procs[lname] = dict(name=lname, kind=kind, module='', sig='\n'.join(sigs),
                                klass=klass, std=std if lname == node.split()[0].lower() else 'GNU extension',
                                min=min(mins),
                                max=-1 if -1 in maxs else max(maxs), generic='', node=node)
        # Specific names table.
        sp = f.get('Specific names', '')
        for line in sp.splitlines():
            m = re.match(r'\s*‘([A-Z_0-9]+)\(([^)]*)\)’\s+‘([^’]*)’\s+‘([^’]*)’\s+(.*)$', line)
            if not m:
                # An argument column continued on the next line (DBESJN).
                c = re.match(r'\s+‘([^’]*)’\s*$', line)
                if c and specifics and specifics[-1]['generic'] == node.split()[0].lower():
                    specifics[-1]['argty'] += ', ' + squash(c.group(1))
                continue
            sname, sargs, argty, retty, sstd = m.groups()
            specifics.append(dict(name=sname.lower(), args=sargs.upper(), argty=squash(argty),
                                  retty=re.sub(r'^INT$', 'INTEGER',
                                               re.match(r'[A-Z]+(\(\d+\)|\(LEN=1\))?', retty).group(0)),
                                  std=norm_standard(sstd) or std,
                                  generic=node.split()[0].lower(), klass=klass))
    return procs, specifics


def module_doc_info(all_nodes):
    """Standards, C types and procedure lists for the two ISO modules from the manual."""
    info = {'iso_fortran_env': {}, 'iso_c_binding': {}}
    procs = {'iso_fortran_env': [], 'iso_c_binding': []}
    ctype = {}
    cchar = {}
    for node, up, body in all_nodes:
        if node == 'ISO_FORTRAN_ENV':
            for m in re.finditer(r'^((?:‘[A-Z_0-9]+’(?:, )?)+):\n((?:     .*\n)+)', body, re.M):
                names = re.findall(r'‘([A-Z_0-9]+)’', m.group(1))
                std = 'Fortran 2003'
                n = re.search(r'\((Fortran 2008|TS 18508)', squash(m.group(2)))
                if n:
                    std = norm_standard(n.group(1))
                for nm in names:
                    info['iso_fortran_env'][nm.lower()] = std
            for m in re.finditer(r'\*note\s+([A-Z_]+)::', body):
                procs['iso_fortran_env'].append(m.group(1).lower())
        elif node == 'ISO_C_BINDING':
            for m in re.finditer(r'^‘(C_[A-Z_0-9]+)’\s*$', body, re.M):
                procs['iso_c_binding'].append(m.group(1).lower())
            for m in re.finditer(r'^‘([A-Z]+)’\s+‘(C_[A-Z_0-9]+)’\s+‘([^’]*)’(.*)$', body, re.M):
                fty, nm, cty, ext = m.groups()
                more = re.match(r'\s*\n\s+(\S.*?)’', body[m.end():])
                ext = ext.strip()
                std = 'Fortran 2003'
                if ext.startswith('Ext'):
                    std = 'GNU extension'
                elif ext.startswith('TS'):
                    std = 'Fortran 2018'
                info['iso_c_binding'][nm.lower()] = std
                ctype[nm.lower()] = (fty, cty)
            for m in re.finditer(r'^‘(C_[A-Z_]+)’\s*(.*?)\s+‘\'(\\.)\'’', body, re.M):
                cchar[m.group(1).lower()] = (m.group(2).strip(), m.group(3))
                info['iso_c_binding'][m.group(1).lower()] = 'Fortran 2003'
    return info, procs, ctype, cchar


# ----------------------------------------------------------------------------
# Reading gfortran's module files

TOK_RE = re.compile(r"\(|\)|'(?:[^']|'')*'|[^\s()']+")


def sexp(text):
    """Parse the body of a .mod file (after the header line) into nested lists."""
    stack = [[]]
    for t in TOK_RE.findall(text):
        if t == '(':
            stack.append([])
        elif t == ')':
            x = stack.pop()
            stack[-1].append(x)
        elif t.startswith("'"):
            stack[-1].append(('s', t[1:-1].replace("''", "'")))
        else:
            stack[-1].append(t)
    return stack[0]


def is_str(x):
    return isinstance(x, tuple)


def mod_entities(modtext):
    """-> list of (name, origin module, kind, [(dummy, optional)] per specific, attrs)"""
    body = modtext.split('\n', 1)[1]
    top = sexp(body)
    lists = [x for x in top if isinstance(x, list)]
    generics_l, symbols_l, symtree_l = lists[2], lists[6], lists[7]
    syms = {}
    i = 0
    while i + 5 < len(symbols_l) + 1:
        if (i + 5 < len(symbols_l) and not is_str(symbols_l[i]) and not isinstance(symbols_l[i], list)
                and is_str(symbols_l[i + 1]) and is_str(symbols_l[i + 2]) and is_str(symbols_l[i + 3])
                and isinstance(symbols_l[i + 5], list)):
            sid = symbols_l[i]
            syms[sid] = dict(name=symbols_l[i + 1][1], module=symbols_l[i + 2][1], body=symbols_l[i + 5])
            i += 6
        else:
            i += 1

    def attrs(sid):
        b = syms[sid]['body']
        return [a for a in b[0] if not isinstance(a, list)]

    def formals(sid):
        b = syms[sid]['body']
        out = []
        if len(b) > 5 and isinstance(b[5], list):
            for d in b[5]:
                if d in syms:
                    out.append((syms[d]['name'].upper(), 'OPTIONAL' in attrs(d)))
        return out

    ents = {}

    def add(name, sid_list):
        if name.startswith('__') or not name[:1].islower():
            return
        a = attrs(sid_list[0])
        flavor = a[0]
        origin = syms[sid_list[0]]['module']
        if flavor == 'MODULE':
            return
        if flavor == 'PROCEDURE':
            kind = 'Subroutine' if 'SUBROUTINE' in a else 'Function'
        elif flavor == 'DERIVED':
            kind = 'Type'
        else:
            kind = 'Constant'
        forms = [formals(s) for s in sid_list if attrs(s)[0] == 'PROCEDURE']
        e = ents.get(name)
        if e is None:
            ents[name] = dict(name=name, origin=origin, kind=kind, forms=forms, attrs=a)
        elif kind == 'Type':
            e['kind'] = 'Type'

    for g in generics_l:
        name = g[0][1]
        ids = [x for x in g[2:] if not is_str(x)]
        ids = [x for x in ids if x in syms]
        if ids:
            add(name, ids)
    j = 0
    while j + 2 < len(symtree_l) + 1 and j < len(symtree_l):
        name = symtree_l[j][1]
        sid = symtree_l[j + 2]
        if name not in ents and sid in syms:
            add(name, [sid])
        j += 3
    return list(ents.values())


def compile_modules(gfortran, wanted):
    """USE each intrinsic module in a throwaway module, return {use: modtext}."""
    out = {}
    with tempfile.TemporaryDirectory() as td:
        for use, intrinsic in wanted:
            src = Path(td) / ('w_%s.f90' % use)
            src.write_text('module w_%s\n  use%s %s\nend module\n' %
                           (use, ', intrinsic ::' if intrinsic else '', use))
            r = subprocess.run([gfortran, '-fopenmp', '-c', src.name], cwd=td,
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit('gfortran failed on %s:\n%s' % (use, r.stderr))
            out[use] = gzip.decompress((Path(td) / ('w_%s.mod' % use)).read_bytes()).decode()
    return out


# ----------------------------------------------------------------------------
# Our own words

# One-line summaries of the intrinsic procedures, in our own words.
INTRINSIC_SUMMARY = {
    'abort': 'Stop the program abnormally, possibly with a core dump',
    'abs': 'Absolute value (modulus for complex)',
    'access': 'Check whether a file can be accessed in the given mode; 0 on success',
    'achar': 'Character at a given position in the ASCII collating sequence',
    'acos': 'Arccosine, in radians',
    'acosd': 'Arccosine, in degrees',
    'acosh': 'Inverse hyperbolic cosine',
    'adjustl': 'Move leading blanks of a string to its end',
    'adjustr': 'Move trailing blanks of a string to its start',
    'aimag': 'Imaginary part of a complex number',
    'aint': 'Truncate toward zero to a whole number, keeping type real',
    'alarm': 'Arrange for a handler to run after a number of seconds',
    'all': 'True if every element of a logical mask is true',
    'allocated': 'Whether an allocatable variable is currently allocated',
    'and': 'Bitwise or logical AND (legacy; use IAND or .AND.)',
    'anint': 'Round to the nearest whole number, keeping type real',
    'any': 'True if any element of a logical mask is true',
    'asin': 'Arcsine, in radians',
    'asind': 'Arcsine, in degrees',
    'asinh': 'Inverse hyperbolic sine',
    'associated': 'Whether a pointer is associated (optionally with a given target)',
    'atan': 'Arctangent, in radians; with two arguments the same as ATAN2',
    'atan2': 'Arctangent of Y/X using the signs of both to pick the quadrant, in radians',
    'atan2d': 'Arctangent of Y/X using the signs of both to pick the quadrant, in degrees',
    'atand': 'Arctangent, in degrees',
    'atanh': 'Inverse hyperbolic tangent',
    'atomic_add': 'Atomically add a value to an atomic variable',
    'atomic_and': 'Atomically AND a value into an atomic variable',
    'atomic_cas': 'Atomic compare-and-swap',
    'atomic_define': 'Atomically set an atomic variable',
    'atomic_fetch_add': 'Atomically add to an atomic variable, returning its old value',
    'atomic_fetch_and': 'Atomically AND into an atomic variable, returning its old value',
    'atomic_fetch_or': 'Atomically OR into an atomic variable, returning its old value',
    'atomic_fetch_xor': 'Atomically XOR into an atomic variable, returning its old value',
    'atomic_or': 'Atomically OR a value into an atomic variable',
    'atomic_ref': 'Atomically read an atomic variable',
    'atomic_xor': 'Atomically XOR a value into an atomic variable',
    'backtrace': 'Print a backtrace of the call stack',
    'bessel_j0': 'Bessel function of the first kind, order 0',
    'bessel_j1': 'Bessel function of the first kind, order 1',
    'bessel_jn': 'Bessel function of the first kind, order N (or orders N1..N2)',
    'bessel_y0': 'Bessel function of the second kind, order 0',
    'bessel_y1': 'Bessel function of the second kind, order 1',
    'bessel_yn': 'Bessel function of the second kind, order N (or orders N1..N2)',
    'bge': 'Bitwise greater-than-or-equal comparison (as unsigned)',
    'bgt': 'Bitwise greater-than comparison (as unsigned)',
    'bit_size': 'Number of bits in an integer of the given kind',
    'ble': 'Bitwise less-than-or-equal comparison (as unsigned)',
    'blt': 'Bitwise less-than comparison (as unsigned)',
    'btest': 'Test whether a given bit of an integer is set',
    'c_associated': 'Whether a C pointer is non-null (or equal to a second one)',
    'c_f_pointer': 'Associate a Fortran pointer with the target of a C pointer',
    'c_f_procpointer': 'Associate a Fortran procedure pointer with a C function pointer',
    'c_funloc': 'C function pointer to a Fortran procedure',
    'c_loc': 'C address of a Fortran variable',
    'c_sizeof': 'Size in bytes of an interoperable entity',
    'ceiling': 'Smallest integer not less than the argument',
    'char': 'Character at a given position in the processor collating sequence',
    'chdir': 'Change the current working directory',
    'chmod': 'Change the permissions of a file',
    'cmplx': 'Convert to complex, from one or two (real, imaginary) parts',
    'co_broadcast': 'Copy a value from one image to all images',
    'co_max': 'Maximum of a value across images',
    'co_min': 'Minimum of a value across images',
    'co_reduce': 'Reduce a value across images with a user operation',
    'co_sum': 'Sum of a value across images',
    'command_argument_count': 'Number of command-line arguments',
    'compiler_options': 'String of the options the program was compiled with',
    'compiler_version': 'String naming the compiler and its version',
    'complex': 'Make a complex number from real and imaginary parts (extension)',
    'conjg': 'Complex conjugate',
    'cos': 'Cosine of an angle in radians',
    'cosd': 'Cosine of an angle in degrees',
    'cosh': 'Hyperbolic cosine',
    'cotan': 'Cotangent of an angle in radians',
    'cotand': 'Cotangent of an angle in degrees',
    'count': 'Number of true elements in a logical mask',
    'cpu_time': 'Processor time used, in seconds',
    'cshift': 'Circular shift of an array along a dimension',
    'ctime': 'Convert a time value to a readable date string',
    'date_and_time': 'Current date, time and time zone',
    'dble': 'Convert to double precision real',
    'dcmplx': 'Convert to double precision complex',
    'digits': 'Number of significant digits in the model for a number kind',
    'dim': 'Positive difference: X-Y if positive, else zero',
    'dot_product': 'Dot product of two vectors',
    'dprod': 'Double precision product of two default reals',
    'dreal': 'Real part of a double complex number',
    'dshiftl': 'Combined left shift of two integers',
    'dshiftr': 'Combined right shift of two integers',
    'dtime': 'Elapsed run time since the last call (user and system)',
    'eoshift': 'End-off shift of an array along a dimension',
    'epsilon': 'Smallest number that is not negligible next to 1 in a real kind',
    'erf': 'Error function',
    'erfc': 'Complementary error function',
    'erfc_scaled': 'Exponentially scaled complementary error function',
    'etime': 'Elapsed run time since program start (user and system)',
    'event_query': 'Number of times an event variable has been posted',
    'execute_command_line': 'Run a shell command',
    'exit': 'Stop the program with a status code',
    'exp': 'Exponential function',
    'exponent': 'Exponent part of a real number in its model representation',
    'extends_type_of': 'Whether the dynamic type of A extends that of MOLD',
    'fdate': 'Current date and time as a string',
    'fget': 'Read one character from standard input in stream mode',
    'fgetc': 'Read one character from a unit in stream mode',
    'findloc': 'Location of the first (or last) element equal to a value',
    'floor': 'Largest integer not greater than the argument',
    'flush': 'Flush the buffers of a unit',
    'fnum': 'File descriptor number of a unit',
    'fput': 'Write one character to standard output in stream mode',
    'fputc': 'Write one character to a unit in stream mode',
    'fraction': 'Fractional part of a real number in its model representation',
    'free': 'Free memory obtained from MALLOC',
    'fseek': 'Set the file position of a unit',
    'fstat': 'File status of an open unit',
    'ftell': 'Current file position of a unit',
    'gamma': 'Gamma function',
    'gerror': 'Message for the last system error',
    'get_command': 'The whole command line that started the program',
    'get_command_argument': 'One command-line argument',
    'get_environment_variable': 'Value of an environment variable',
    'getarg': 'One command-line argument (legacy)',
    'getcwd': 'Current working directory',
    'getenv': 'Value of an environment variable (legacy)',
    'getgid': 'Group ID of the process',
    'getlog': 'Login name of the user',
    'getpid': 'Process ID',
    'getuid': 'User ID of the process',
    'gmtime': 'Split a time value into UTC date and time fields',
    'hostnm': 'Host name of the machine',
    'huge': 'Largest number of the argument\'s type and kind',
    'hypot': 'Euclidean distance sqrt(X**2 + Y**2) without undue overflow',
    'iachar': 'Position of a character in the ASCII collating sequence',
    'iall': 'Bitwise AND reduction of an array',
    'iand': 'Bitwise AND',
    'iany': 'Bitwise OR reduction of an array',
    'iargc': 'Number of command-line arguments (legacy)',
    'ibclr': 'Clear one bit of an integer',
    'ibits': 'Extract a sequence of bits from an integer',
    'ibset': 'Set one bit of an integer',
    'ichar': 'Position of a character in the processor collating sequence',
    'idate': 'Current day, month and year',
    'ieor': 'Bitwise exclusive OR',
    'ierrno': 'Last system error number',
    'image_index': 'Image index for a set of cosubscripts',
    'index': 'Position of a substring in a string',
    'int': 'Convert to integer, truncating toward zero',
    'int2': 'Convert to a 2-byte integer (extension)',
    'int8': 'Convert to an 8-byte integer (extension)',
    'ior': 'Bitwise inclusive OR',
    'iparity': 'Bitwise exclusive-OR reduction of an array',
    'irand': 'Pseudo-random integer (legacy)',
    'is_contiguous': 'Whether an array is contiguous in memory',
    'is_iostat_end': 'Whether an IOSTAT value means end of file',
    'is_iostat_eor': 'Whether an IOSTAT value means end of record',
    'isatty': 'Whether a unit is connected to a terminal',
    'ishft': 'Logical shift of the bits of an integer',
    'ishftc': 'Circular shift of the rightmost bits of an integer',
    'isnan': 'Whether a real value is NaN (extension; see IEEE_IS_NAN)',
    'itime': 'Current hour, minute and second',
    'kill': 'Send a signal to a process',
    'kind': 'Kind type parameter of the argument',
    'lbound': 'Lower bound(s) of an array',
    'lcobound': 'Lower cobound(s) of a coarray',
    'leadz': 'Number of leading zero bits',
    'len': 'Length of a character entity',
    'len_trim': 'Length of a string without its trailing blanks',
    'lge': 'String >= comparison in ASCII order',
    'lgt': 'String > comparison in ASCII order',
    'link': 'Create a hard link',
    'lle': 'String <= comparison in ASCII order',
    'llt': 'String < comparison in ASCII order',
    'lnblnk': 'Index of the last non-blank character (legacy LEN_TRIM)',
    'loc': 'Address of a variable as an integer',
    'log': 'Natural logarithm',
    'log10': 'Common (base-10) logarithm',
    'log_gamma': 'Logarithm of the absolute value of the gamma function',
    'logical': 'Convert between logical kinds',
    'lshift': 'Left shift of the bits of an integer (legacy)',
    'lstat': 'File status, not following a symbolic link',
    'ltime': 'Split a time value into local date and time fields',
    'malloc': 'Allocate memory, returning its address (Cray pointers)',
    'maskl': 'Integer with the leftmost I bits set',
    'maskr': 'Integer with the rightmost I bits set',
    'matmul': 'Matrix multiplication',
    'max': 'Largest of the arguments',
    'maxexponent': 'Largest exponent in the model for a real kind',
    'maxloc': 'Location of the largest element of an array',
    'maxval': 'Largest element of an array',
    'mclock': 'Processor clock ticks used (legacy)',
    'mclock8': 'Processor clock ticks used, 8-byte result (legacy)',
    'merge': 'Choose between two values by a logical mask',
    'merge_bits': 'Merge the bits of two integers under a mask',
    'min': 'Smallest of the arguments',
    'minexponent': 'Smallest exponent in the model for a real kind',
    'minloc': 'Location of the smallest element of an array',
    'minval': 'Smallest element of an array',
    'mod': 'Remainder, with the sign of the first argument',
    'modulo': 'Modulo, with the sign of the second argument',
    'move_alloc': 'Move an allocation from one allocatable to another',
    'mvbits': 'Copy a sequence of bits from one integer to another',
    'nearest': 'Nearest representable number in a given direction',
    'new_line': 'Newline character of the argument\'s kind',
    'nint': 'Round to the nearest integer',
    'norm2': 'Euclidean (L2) norm of an array',
    'not': 'Bitwise complement',
    'null': 'Disassociated pointer or unallocated allocatable',
    'num_images': 'Number of images',
    'or': 'Bitwise or logical OR (legacy; use IOR or .OR.)',
    'pack': 'Gather the elements selected by a mask into a vector',
    'parity': 'True if an odd number of mask elements are true',
    'perror': 'Print a message for the last system error',
    'popcnt': 'Number of set bits',
    'poppar': 'Parity of the number of set bits',
    'precision': 'Decimal precision of a real or complex kind',
    'present': 'Whether an optional dummy argument was passed',
    'product': 'Product of the elements of an array',
    'radix': 'Base of the number model for the argument\'s kind',
    'rand': 'Pseudo-random real in [0, 1) (legacy)',
    'random_init': 'Initialise the pseudo-random generator for this image',
    'random_number': 'Fill with pseudo-random numbers in [0, 1)',
    'random_seed': 'Query or set the pseudo-random generator seed',
    'range': 'Decimal exponent range of a number kind',
    'rank': 'Rank (number of dimensions) of a data object',
    'real': 'Convert to real, or take the real part of a complex',
    'realpart': 'Real part of a complex number (extension)',
    'rename': 'Rename a file',
    'repeat': 'Concatenate several copies of a string',
    'reshape': 'Build an array of a given shape from the elements of another',
    'rrspacing': 'Reciprocal of the relative spacing of model numbers near X',
    'rshift': 'Right shift of the bits of an integer (legacy)',
    'same_type_as': 'Whether two objects have the same dynamic type',
    'scale': 'Multiply a real by a power of its radix',
    'scan': 'Position of any character of a set in a string',
    'secnds': 'Seconds since midnight, minus a reference time',
    'second': 'Processor time used, in seconds (legacy)',
    'selected_char_kind': 'Kind number of a named character set',
    'selected_int_kind': 'Integer kind with at least the given decimal range',
    'selected_real_kind': 'Real kind with at least the given precision and range',
    'set_exponent': 'Real with the fraction of X and a given exponent',
    'shape': 'Shape of an array or scalar',
    'shifta': 'Arithmetic right shift',
    'shiftl': 'Logical left shift',
    'shiftr': 'Logical right shift',
    'sign': 'Magnitude of A with the sign of B',
    'signal': 'Install a handler for a signal',
    'sin': 'Sine of an angle in radians',
    'sind': 'Sine of an angle in degrees',
    'sinh': 'Hyperbolic sine',
    'size': 'Number of elements of an array, or the extent along a dimension',
    'sizeof': 'Size of the argument in bytes (extension)',
    'sleep': 'Pause for a number of seconds',
    'spacing': 'Absolute spacing of model numbers near X',
    'spread': 'Replicate an array along a new dimension',
    'sqrt': 'Square root',
    'srand': 'Seed the legacy pseudo-random generator',
    'stat': 'File status of a named file',
    'storage_size': 'Size in bits of an element of the argument',
    'sum': 'Sum of the elements of an array',
    'symlnk': 'Create a symbolic link',
    'system': 'Run a shell command (legacy; see EXECUTE_COMMAND_LINE)',
    'system_clock': 'Count, rate and maximum of a real-time clock',
    'tan': 'Tangent of an angle in radians',
    'tand': 'Tangent of an angle in degrees',
    'tanh': 'Hyperbolic tangent',
    'this_image': 'Index or cosubscripts of the executing image',
    'time': 'Current system time in seconds',
    'time8': 'Current system time in seconds, 8-byte result',
    'tiny': 'Smallest positive normal number of a real kind',
    'trailz': 'Number of trailing zero bits',
    'transfer': 'Reinterpret the bits of a value as another type',
    'transpose': 'Transpose of a rank-2 array',
    'trim': 'String with trailing blanks removed',
    'ttynam': 'Name of the terminal connected to a unit',
    'ubound': 'Upper bound(s) of an array',
    'ucobound': 'Upper cobound(s) of a coarray',
    'umask': 'Set the file creation mask',
    'unlink': 'Delete a file',
    'unpack': 'Scatter a vector into an array under a mask',
    'verify': 'Position of the first character not in a set',
    'xor': 'Bitwise or logical exclusive OR (legacy; use IEOR or .NEQV.)',
}

# Standards for module entities the manual does not date: per (module, name),
# else per module.
MODULE_STD = {
    'iso_fortran_env': 'Fortran 2003', 'iso_c_binding': 'Fortran 2003',
    'ieee_arithmetic': 'Fortran 2003', 'ieee_exceptions': 'Fortran 2003', 'ieee_features': 'Fortran 2003',
    'omp_lib': 'OpenMP', 'omp_lib_kinds': 'OpenMP',
    ('iso_fortran_env', 'event_type'): 'Fortran 2018',
    ('iso_fortran_env', 'team_type'): 'Fortran 2018',
    ('iso_fortran_env', 'logical8'): 'Fortran 2023',
    ('iso_fortran_env', 'logical16'): 'Fortran 2023',
    ('iso_fortran_env', 'logical32'): 'Fortran 2023',
    ('iso_fortran_env', 'logical64'): 'Fortran 2023',
    ('iso_fortran_env', 'real16'): 'Fortran 2023',
    ('iso_c_binding', 'f_c_string'): 'Fortran 2023',
}
for _n in ('ieee_fma', 'ieee_max_num', 'ieee_max_num_mag', 'ieee_min_num', 'ieee_min_num_mag',
           'ieee_quiet_eq', 'ieee_quiet_ge', 'ieee_quiet_gt', 'ieee_quiet_le', 'ieee_quiet_lt', 'ieee_quiet_ne',
           'ieee_signaling_eq', 'ieee_signaling_ge', 'ieee_signaling_gt', 'ieee_signaling_le',
           'ieee_signaling_lt', 'ieee_signaling_ne', 'ieee_signbit', 'ieee_support_subnormal',
           'ieee_negative_subnormal', 'ieee_positive_subnormal', 'ieee_away'):
    MODULE_STD[('ieee_arithmetic', _n)] = 'Fortran 2018'
for _n in ('ieee_get_modes', 'ieee_set_modes', 'ieee_modes_type'):
    MODULE_STD[('ieee_exceptions', _n)] = 'Fortran 2018'
MODULE_STD[('ieee_features', 'ieee_subnormal')] = 'Fortran 2018'

# Classes the .mod attributes cannot tell (the standard's own classification).
MODULE_PROC_CLASS = {n: 'inquiry function' for n in (
    'ieee_support_datatype', 'ieee_support_denormal', 'ieee_support_divide', 'ieee_support_flag',
    'ieee_support_halting', 'ieee_support_inf', 'ieee_support_io', 'ieee_support_nan',
    'ieee_support_rounding', 'ieee_support_sqrt', 'ieee_support_standard', 'ieee_support_subnormal',
    'ieee_support_underflow_control')}
MODULE_PROC_CLASS['ieee_selected_real_kind'] = 'transformational function'
MODULE_PROC_CLASS['ieee_get_rounding_mode'] = 'subroutine'
MODULE_PROC_CLASS['f_c_string'] = 'transformational function'

_SUP = 'Whether the processor supports IEEE %s (for X\'s kind, or all reals)'
MODULE_SUMMARY = {
    # iso_fortran_env
    ('iso_fortran_env', 'atomic_int_kind'): 'Integer kind for atomic variables',
    ('iso_fortran_env', 'atomic_logical_kind'): 'Logical kind for atomic variables',
    ('iso_fortran_env', 'character_kinds'): 'Array of the supported character kinds',
    ('iso_fortran_env', 'character_storage_size'): 'Bits in a character storage unit',
    ('iso_fortran_env', 'error_unit'): 'Unit number of the preconnected error output (stderr)',
    ('iso_fortran_env', 'event_type'): 'Derived type for event variables (EVENT POST/WAIT)',
    ('iso_fortran_env', 'file_storage_size'): 'Bits in a file storage unit',
    ('iso_fortran_env', 'input_unit'): 'Unit number of the preconnected input (stdin, READ *)',
    ('iso_fortran_env', 'int8'): 'Kind of an 8-bit integer',
    ('iso_fortran_env', 'int16'): 'Kind of a 16-bit integer',
    ('iso_fortran_env', 'int32'): 'Kind of a 32-bit integer',
    ('iso_fortran_env', 'int64'): 'Kind of a 64-bit integer',
    ('iso_fortran_env', 'integer_kinds'): 'Array of the supported integer kinds',
    ('iso_fortran_env', 'iostat_end'): 'IOSTAT value for end of file',
    ('iso_fortran_env', 'iostat_eor'): 'IOSTAT value for end of record',
    ('iso_fortran_env', 'iostat_inquire_internal_unit'): 'IOSTAT value from INQUIRE on an internal unit',
    ('iso_fortran_env', 'lock_type'): 'Derived type for lock variables (LOCK/UNLOCK)',
    ('iso_fortran_env', 'logical8'): 'Kind of an 8-bit logical',
    ('iso_fortran_env', 'logical16'): 'Kind of a 16-bit logical',
    ('iso_fortran_env', 'logical32'): 'Kind of a 32-bit logical',
    ('iso_fortran_env', 'logical64'): 'Kind of a 64-bit logical',
    ('iso_fortran_env', 'logical_kinds'): 'Array of the supported logical kinds',
    ('iso_fortran_env', 'numeric_storage_size'): 'Bits in a numeric storage unit',
    ('iso_fortran_env', 'output_unit'): 'Unit number of the preconnected output (stdout, WRITE *)',
    ('iso_fortran_env', 'real16'): 'Kind of a 16-bit real',
    ('iso_fortran_env', 'real32'): 'Kind of a 32-bit real',
    ('iso_fortran_env', 'real64'): 'Kind of a 64-bit real',
    ('iso_fortran_env', 'real128'): 'Kind of a 128-bit real',
    ('iso_fortran_env', 'real_kinds'): 'Array of the supported real kinds',
    ('iso_fortran_env', 'stat_failed_image'): 'STAT value when an image involved has failed',
    ('iso_fortran_env', 'stat_locked'): 'STAT value from LOCK when this image already holds the lock',
    ('iso_fortran_env', 'stat_locked_other_image'): 'STAT value from UNLOCK when another image holds the lock',
    ('iso_fortran_env', 'stat_stopped_image'): 'STAT value when an image involved has stopped',
    ('iso_fortran_env', 'stat_unlocked'): 'STAT value from UNLOCK when the lock is not held',
    ('iso_fortran_env', 'team_type'): 'Derived type for team variables (FORM TEAM, CHANGE TEAM)',
    # iso_c_binding
    ('iso_c_binding', 'c_ptr'): 'Derived type interoperable with a C data pointer',
    ('iso_c_binding', 'c_funptr'): 'Derived type interoperable with a C function pointer',
    ('iso_c_binding', 'c_null_ptr'): 'Null C data pointer',
    ('iso_c_binding', 'c_null_funptr'): 'Null C function pointer',
    ('iso_c_binding', 'f_c_string'): 'Fortran string as a null-terminated C string',
    # ieee_exceptions
    ('ieee_exceptions', 'ieee_flag_type'): 'Type of the IEEE exception flag constants',
    ('ieee_exceptions', 'ieee_status_type'): 'Type holding the whole floating-point status',
    ('ieee_exceptions', 'ieee_modes_type'): 'Type holding the floating-point modes',
    ('ieee_exceptions', 'ieee_overflow'): 'Exception flag: overflow',
    ('ieee_exceptions', 'ieee_divide_by_zero'): 'Exception flag: division by zero',
    ('ieee_exceptions', 'ieee_invalid'): 'Exception flag: invalid operation',
    ('ieee_exceptions', 'ieee_underflow'): 'Exception flag: underflow',
    ('ieee_exceptions', 'ieee_inexact'): 'Exception flag: inexact result',
    ('ieee_exceptions', 'ieee_usual'): 'Array of the overflow, divide-by-zero and invalid flags',
    ('ieee_exceptions', 'ieee_all'): 'Array of all five exception flags',
    ('ieee_exceptions', 'ieee_get_flag'): 'Whether an exception flag is signalling',
    ('ieee_exceptions', 'ieee_set_flag'): 'Set or clear an exception flag',
    ('ieee_exceptions', 'ieee_get_halting_mode'): 'Whether an exception halts the program',
    ('ieee_exceptions', 'ieee_set_halting_mode'): 'Choose whether an exception halts the program',
    ('ieee_exceptions', 'ieee_get_status'): 'Save the floating-point status',
    ('ieee_exceptions', 'ieee_set_status'): 'Restore a saved floating-point status',
    ('ieee_exceptions', 'ieee_get_modes'): 'Save the floating-point modes',
    ('ieee_exceptions', 'ieee_set_modes'): 'Restore saved floating-point modes',
    ('ieee_exceptions', 'ieee_support_flag'): 'Whether an exception flag is supported',
    ('ieee_exceptions', 'ieee_support_halting'): 'Whether halting can be controlled for an exception',
    # ieee_arithmetic
    ('ieee_arithmetic', 'ieee_class_type'): 'Type of the IEEE value-class constants',
    ('ieee_arithmetic', 'ieee_round_type'): 'Type of the IEEE rounding-mode constants',
    ('ieee_arithmetic', 'ieee_signaling_nan'): 'Class: signalling NaN',
    ('ieee_arithmetic', 'ieee_quiet_nan'): 'Class: quiet NaN',
    ('ieee_arithmetic', 'ieee_negative_inf'): 'Class: negative infinity',
    ('ieee_arithmetic', 'ieee_negative_normal'): 'Class: negative normal number',
    ('ieee_arithmetic', 'ieee_negative_denormal'): 'Class: negative subnormal number (older name)',
    ('ieee_arithmetic', 'ieee_negative_subnormal'): 'Class: negative subnormal number',
    ('ieee_arithmetic', 'ieee_negative_zero'): 'Class: negative zero',
    ('ieee_arithmetic', 'ieee_positive_zero'): 'Class: positive zero',
    ('ieee_arithmetic', 'ieee_positive_denormal'): 'Class: positive subnormal number (older name)',
    ('ieee_arithmetic', 'ieee_positive_subnormal'): 'Class: positive subnormal number',
    ('ieee_arithmetic', 'ieee_positive_normal'): 'Class: positive normal number',
    ('ieee_arithmetic', 'ieee_positive_inf'): 'Class: positive infinity',
    ('ieee_arithmetic', 'ieee_other_value'): 'Class: none of the others',
    ('ieee_arithmetic', 'ieee_nearest'): 'Rounding mode: to nearest, ties to even',
    ('ieee_arithmetic', 'ieee_to_zero'): 'Rounding mode: toward zero',
    ('ieee_arithmetic', 'ieee_up'): 'Rounding mode: toward +infinity',
    ('ieee_arithmetic', 'ieee_down'): 'Rounding mode: toward -infinity',
    ('ieee_arithmetic', 'ieee_away'): 'Rounding mode: to nearest, ties away from zero',
    ('ieee_arithmetic', 'ieee_other'): 'Rounding mode: none of the others',
    ('ieee_arithmetic', 'ieee_class'): 'IEEE class of a real value',
    ('ieee_arithmetic', 'ieee_copy_sign'): 'X with the sign of Y',
    ('ieee_arithmetic', 'ieee_fma'): 'Fused multiply-add A*B+C with one rounding',
    ('ieee_arithmetic', 'ieee_get_rounding_mode'): 'Current rounding mode',
    ('ieee_arithmetic', 'ieee_set_rounding_mode'): 'Set the rounding mode',
    ('ieee_arithmetic', 'ieee_get_underflow_mode'): 'Whether underflow is gradual',
    ('ieee_arithmetic', 'ieee_set_underflow_mode'): 'Choose gradual or abrupt underflow',
    ('ieee_arithmetic', 'ieee_is_finite'): 'Whether a value is finite',
    ('ieee_arithmetic', 'ieee_is_nan'): 'Whether a value is NaN',
    ('ieee_arithmetic', 'ieee_is_negative'): 'Whether a value is negative (including -0)',
    ('ieee_arithmetic', 'ieee_is_normal'): 'Whether a value is normal',
    ('ieee_arithmetic', 'ieee_logb'): 'Unbiased exponent of a value',
    ('ieee_arithmetic', 'ieee_max_num'): 'Larger of two values, ignoring a quiet NaN',
    ('ieee_arithmetic', 'ieee_max_num_mag'): 'Value of larger magnitude, ignoring a quiet NaN',
    ('ieee_arithmetic', 'ieee_min_num'): 'Smaller of two values, ignoring a quiet NaN',
    ('ieee_arithmetic', 'ieee_min_num_mag'): 'Value of smaller magnitude, ignoring a quiet NaN',
    ('ieee_arithmetic', 'ieee_next_after'): 'Next representable value after X toward Y',
    ('ieee_arithmetic', 'ieee_quiet_eq'): 'Equality compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_quiet_ge'): 'Greater-or-equal compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_quiet_gt'): 'Greater-than compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_quiet_le'): 'Less-or-equal compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_quiet_lt'): 'Less-than compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_quiet_ne'): 'Inequality compare, quiet on NaN',
    ('ieee_arithmetic', 'ieee_signaling_eq'): 'Equality compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_signaling_ge'): 'Greater-or-equal compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_signaling_gt'): 'Greater-than compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_signaling_le'): 'Less-or-equal compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_signaling_lt'): 'Less-than compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_signaling_ne'): 'Inequality compare, signalling on NaN',
    ('ieee_arithmetic', 'ieee_rem'): 'IEEE remainder X - Y*N, N the nearest integer to X/Y',
    ('ieee_arithmetic', 'ieee_rint'): 'Round to a whole number in the current (or given) rounding mode',
    ('ieee_arithmetic', 'ieee_scalb'): 'X times 2**I',
    ('ieee_arithmetic', 'ieee_selected_real_kind'): 'IEEE real kind with at least the given precision and range',
    ('ieee_arithmetic', 'ieee_signbit'): 'Whether the sign bit is set',
    ('ieee_arithmetic', 'ieee_unordered'): 'Whether X or Y is NaN',
    ('ieee_arithmetic', 'ieee_value'): 'A value of a given IEEE class (NaN, infinity, ...)',
    ('ieee_arithmetic', 'ieee_support_datatype'): _SUP % 'arithmetic',
    ('ieee_arithmetic', 'ieee_support_denormal'): _SUP % 'subnormal numbers',
    ('ieee_arithmetic', 'ieee_support_subnormal'): _SUP % 'subnormal numbers',
    ('ieee_arithmetic', 'ieee_support_divide'): _SUP % 'correctly rounded division',
    ('ieee_arithmetic', 'ieee_support_inf'): _SUP % 'infinities',
    ('ieee_arithmetic', 'ieee_support_io'): _SUP % 'rounding modes in formatted I/O',
    ('ieee_arithmetic', 'ieee_support_nan'): _SUP % 'NaNs',
    ('ieee_arithmetic', 'ieee_support_rounding'): _SUP % 'the given rounding mode',
    ('ieee_arithmetic', 'ieee_support_sqrt'): _SUP % 'correctly rounded square root',
    ('ieee_arithmetic', 'ieee_support_standard'): _SUP % 'all IEEE features',
    ('ieee_arithmetic', 'ieee_support_underflow_control'): _SUP % 'underflow-mode control',
    # ieee_features
    ('ieee_features', 'ieee_features_type'): 'Type of the IEEE feature constants',
    ('ieee_features', 'ieee_datatype'): 'Feature: IEEE arithmetic for some real kind',
    ('ieee_features', 'ieee_denormal'): 'Feature: subnormal numbers (older name)',
    ('ieee_features', 'ieee_subnormal'): 'Feature: subnormal numbers',
    ('ieee_features', 'ieee_divide'): 'Feature: correctly rounded division',
    ('ieee_features', 'ieee_halting'): 'Feature: control of halting on exceptions',
    ('ieee_features', 'ieee_inexact_flag'): 'Feature: the inexact exception',
    ('ieee_features', 'ieee_inf'): 'Feature: infinities',
    ('ieee_features', 'ieee_invalid_flag'): 'Feature: the invalid exception',
    ('ieee_features', 'ieee_nan'): 'Feature: NaNs',
    ('ieee_features', 'ieee_rounding'): 'Feature: control of the rounding mode',
    ('ieee_features', 'ieee_sqrt'): 'Feature: correctly rounded square root',
    ('ieee_features', 'ieee_underflow_flag'): 'Feature: the underflow exception',
}

# Statement keywords (for completion): name -> (standard, summary). A few are
# two-word phrases; a keyword that is also an intrinsic's name (real, exit,
# flush, ...) gets its own entry after the intrinsic's.
_F77, _F90, _F95, _F03, _F08, _F18, _F23 = ('Fortran 77', 'Fortran 90', 'Fortran 95', 'Fortran 2003',
                                            'Fortran 2008', 'Fortran 2018', 'Fortran 2023')
KEYWORDS = {
    'program': (_F77, 'Begin the main program'),
    'module': (_F90, 'Begin a module (or, inside an interface, a separate module procedure)'),
    'submodule': (_F08, 'Begin a submodule that extends a module'),
    'subroutine': (_F77, 'Begin a subroutine'),
    'function': (_F77, 'Begin a function'),
    'block data': (_F77, 'Begin a block data program unit (obsolescent)'),
    'end': (_F77, 'End a program unit, construct or interface'),
    'contains': (_F90, 'Start the internal or module procedures of a scope'),
    'use': (_F90, 'Make a module\'s public entities accessible'),
    'only': (_F90, 'Limit a USE to the listed names'),
    'intrinsic': (_F77, 'Declare names as intrinsic procedures (or USE an intrinsic module)'),
    'non_intrinsic': (_F03, 'USE a non-intrinsic module of that name'),
    'implicit': (_F77, 'Set the implicit typing rules'),
    'none': (_F90, 'With IMPLICIT, turn implicit typing off'),
    'integer': (_F77, 'Integer type'),
    'real': (_F77, 'Real type'),
    'double precision': (_F77, 'Double precision real type'),
    'complex': (_F77, 'Complex type'),
    'character': (_F77, 'Character type'),
    'logical': (_F77, 'Logical type'),
    'type': (_F90, 'Define or declare a derived type'),
    'class': (_F03, 'Declare a polymorphic object'),
    'kind': (_F90, 'Kind type parameter (in a declaration or type definition)'),
    'len': (_F90, 'Length type parameter (in a declaration or type definition)'),
    'if': (_F77, 'Conditional statement or construct'),
    'then': (_F77, 'Start the block of an IF'),
    'else': (_F77, 'Alternative block of an IF'),
    'else if': (_F77, 'Further condition of an IF construct'),
    'elsewhere': (_F90, 'Alternative block of a WHERE construct'),
    'do': (_F77, 'Loop'),
    'do concurrent': (_F08, 'Loop whose iterations may run in any order'),
    'while': (_F90, 'With DO, loop while a condition holds'),
    'select case': (_F90, 'Branch on the value of an expression'),
    'select type': (_F03, 'Branch on the dynamic type of a polymorphic object'),
    'select rank': (_F18, 'Branch on the rank of an assumed-rank object'),
    'case': (_F90, 'One branch of SELECT CASE'),
    'default': (_F90, 'With CASE, CLASS or RANK, the branch taken when no other matches'),
    'type is': (_F03, 'SELECT TYPE branch for an exact type'),
    'class is': (_F03, 'SELECT TYPE branch for a type and its extensions'),
    'rank': (_F18, 'SELECT RANK branch for a given rank'),
    'associate': (_F03, 'Construct naming expressions or variables for its block'),
    'block': (_F08, 'Construct with its own declarations'),
    'critical': (_F08, 'Construct executed by one image at a time'),
    'change team': (_F18, 'Construct executed by the images of a team'),
    'form team': (_F18, 'Divide the current team of images into teams'),
    'sync all': (_F08, 'Synchronise all images'),
    'sync images': (_F08, 'Synchronise with the given images'),
    'sync memory': (_F08, 'Memory barrier between images'),
    'sync team': (_F18, 'Synchronise the images of a team'),
    'event post': (_F18, 'Post an event variable'),
    'event wait': (_F18, 'Wait for an event variable to be posted'),
    'fail image': (_F18, 'Make the executing image fail'),
    'lock': (_F08, 'Acquire a lock variable'),
    'unlock': (_F08, 'Release a lock variable'),
    'forall': (_F95, 'Array assignment over an index set (obsolescent)'),
    'where': (_F90, 'Masked array assignment'),
    'allocate': (_F90, 'Allocate allocatable variables or pointer targets'),
    'deallocate': (_F90, 'Deallocate allocatable variables or pointer targets'),
    'nullify': (_F90, 'Disassociate pointers'),
    'open': (_F77, 'Connect a file to a unit'),
    'close': (_F77, 'Disconnect a unit'),
    'read': (_F77, 'Input data'),
    'write': (_F77, 'Output data to a unit'),
    'print': (_F77, 'Output data to standard output'),
    'inquire': (_F77, 'Ask about a file or unit'),
    'rewind': (_F77, 'Position a file at its start'),
    'backspace': (_F77, 'Move a file back one record'),
    'endfile': (_F77, 'Write an end-of-file record'),
    'flush': (_F03, 'Make data written to a unit available'),
    'wait': (_F03, 'Wait for asynchronous I/O to finish'),
    'format': (_F77, 'Define an edit-descriptor list for formatted I/O'),
    'stop': (_F77, 'End the program normally'),
    'error stop': (_F08, 'End the program with an error'),
    'return': (_F77, 'Return from a procedure'),
    'cycle': (_F90, 'Skip to the next iteration of a loop'),
    'exit': (_F90, 'Leave a loop or named construct'),
    'go to': (_F77, 'Branch to a statement label'),
    'goto': (_F77, 'Branch to a statement label'),
    'continue': (_F77, 'Statement that does nothing (often a label target)'),
    'call': (_F77, 'Invoke a subroutine'),
    'entry': (_F77, 'Extra entry point into a procedure (obsolescent)'),
    'data': (_F77, 'Initialise variables'),
    'common': (_F77, 'Declare a common block (obsolescent)'),
    'equivalence': (_F77, 'Share storage between variables (obsolescent)'),
    'namelist': (_F90, 'Group variables for namelist I/O'),
    'save': (_F77, 'Keep local variables between calls'),
    'parameter': (_F77, 'Declare named constants'),
    'dimension': (_F77, 'Declare array shape'),
    'codimension': (_F08, 'Declare coarray shape'),
    'allocatable': (_F90, 'Attribute: storage is allocated at run time'),
    'pointer': (_F90, 'Attribute: the object is a pointer'),
    'target': (_F90, 'Attribute: the object may be pointed to'),
    'contiguous': (_F08, 'Attribute: the array is contiguous'),
    'intent': (_F90, 'Attribute: IN, OUT or INOUT use of a dummy argument'),
    'in': (_F90, 'With INTENT, the argument is only read'),
    'out': (_F90, 'With INTENT, the argument is only defined'),
    'inout': (_F90, 'With INTENT, the argument is read and defined'),
    'optional': (_F90, 'Attribute: the dummy argument may be omitted'),
    'value': (_F03, 'Attribute: the dummy argument is passed by value'),
    'volatile': (_F03, 'Attribute: the object may change outside the program'),
    'asynchronous': (_F03, 'Attribute: the object takes part in asynchronous I/O or communication'),
    'protected': (_F03, 'Attribute: the module variable is read-only outside the module'),
    'external': (_F77, 'Attribute: the name is an external procedure'),
    'bind': (_F03, 'Attribute: interoperate with C (BIND(C))'),
    'public': (_F90, 'Accessible outside the module'),
    'private': (_F90, 'Accessible only inside the module'),
    'abstract': (_F03, 'Abstract type or abstract interface'),
    'extends': (_F03, 'Type extends a parent type'),
    'procedure': (_F03, 'Procedure declaration, binding or component'),
    'module procedure': (_F90, 'Name module procedures in a generic interface, or define a separate one'),
    'generic': (_F03, 'Generic type-bound procedure'),
    'final': (_F03, 'Finaliser of a derived type'),
    'deferred': (_F03, 'Binding to be overridden by extensions'),
    'non_overridable': (_F03, 'Binding that extensions cannot override'),
    'nopass': (_F03, 'Binding not passed the object'),
    'pass': (_F03, 'Binding passed the object (optionally as a named argument)'),
    'elemental': (_F95, 'Procedure applied element by element'),
    'pure': (_F95, 'Procedure without side effects'),
    'impure': (_F08, 'Procedure that may have side effects'),
    'recursive': (_F90, 'Procedure that may call itself'),
    'non_recursive': (_F18, 'Procedure that may not call itself'),
    'result': (_F90, 'Name the result variable of a function'),
    'interface': (_F90, 'Explicit interface or generic interface block'),
    'operator': (_F90, 'Generic interface for an operator'),
    'assignment': (_F90, 'Generic interface for assignment (=)'),
    'sequence': (_F90, 'Store components in declaration order'),
    'enum': (_F03, 'Begin a C-interoperable enumeration'),
    'enumerator': (_F03, 'Declare enumeration constants'),
    'import': (_F03, 'Make host names visible in an interface body'),
    'include': (_F90, 'Insert the text of another file'),
    'stat': (_F90, 'Specifier receiving a status code'),
    'errmsg': (_F03, 'Specifier receiving an error message'),
    'source': (_F03, 'ALLOCATE specifier: copy shape and value from an object'),
    'mold': (_F08, 'ALLOCATE specifier: copy type and shape from an object'),
    'iostat': (_F77, 'I/O specifier receiving a status code'),
    'iomsg': (_F03, 'I/O specifier receiving an error message'),
    'unit': (_F77, 'I/O specifier: the unit'),
    'file': (_F77, 'OPEN/INQUIRE specifier: the file name'),
    'status': (_F77, 'OPEN/CLOSE specifier: file status'),
    'action': (_F90, 'OPEN specifier: READ, WRITE or READWRITE'),
    'access': (_F77, 'OPEN specifier: SEQUENTIAL, DIRECT or STREAM'),
    'form': (_F77, 'OPEN specifier: FORMATTED or UNFORMATTED'),
    'position': (_F90, 'OPEN specifier: ASIS, REWIND or APPEND'),
    'newunit': (_F08, 'OPEN specifier: choose a free unit number'),
    'advance': (_F90, 'READ/WRITE specifier for non-advancing I/O'),
    'fmt': (_F77, 'I/O specifier: the format'),
    'nml': (_F90, 'I/O specifier: the namelist group'),
    'err': (_F77, 'I/O specifier: label to branch to on error'),
}

# ----------------------------------------------------------------------------
# Output


def cstr(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '?':
            out.append('\\?')  # never a trigraph
        elif o < 0x20 or o > 0x7e:
            if o > 0x7f:
                raise ValueError('non-ASCII in %r' % s)
            out.append('\\x%02x' % o)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('info_dir')
    ap.add_argument('--gfortran', default='gfortran')
    ap.add_argument('-o', '--output', default=str(OUT))
    ap.add_argument('--allow-missing', action='store_true', help='draft run: tolerate missing summaries')
    opts = ap.parse_args()

    all_nodes = nodes(read_info(opts.info_dir))
    procs, specifics = parse_intrinsics(all_nodes)
    doc_std, doc_procs, ctype, cchar = module_doc_info(all_nodes)
    entries = {}
    missing = []

    def put(e):
        entries[(e['name'], e['module'])] = e

    # Intrinsic procedures; the ISO modules' procedures move to their module.
    proc_module = {}
    for mod, names in doc_procs.items():
        for n in names:
            proc_module[n] = mod
    for name, p in procs.items():
        p['module'] = proc_module.get(name, '')
        s = INTRINSIC_SUMMARY.get(name)
        if s is None:
            missing.append(name)
            s = ''
        p['summary'] = s
        put(p)

    # Specific names.
    for sp in specifics:
        if sp['name'] in procs:
            continue  # the generic's own name (ABS in ABS's table)
        gen = sp['generic']
        g = procs.get(gen)
        args, unb = parse_args(sp['args'])
        if g and '\n' not in g['sig']:
            gargs, gunb = parse_args(g['sig'].split('(', 1)[1].rstrip(')'))
            greq = [a for a, o in gargs if not o]
            if gunb or [a for a, _ in args] != greq[:len(args)] or len(args) < len(greq):
                # The table abbreviates: MAX0(A1), BIAND(A), DATAN2(X, Y) take
                # the generic's arguments (MAX's open list, IAND's I and J,
                # ATAN2's Y and X).
                args, unb = gargs, gunb
        tys = sp['argty']
        for a in {a for a, _ in args} | set(re.findall(r'\b[A-Z][A-Z_0-9]*\b(?!\()', sp['argty'])) - {
                'INTEGER', 'REAL', 'COMPLEX', 'LOGICAL', 'CHARACTER'}:
            tys = re.sub(r'\b%s\b(?!\()' % a, '', tys)
        types = {t.strip() for t in tys.split(',') if t.strip()}
        if len(types) == 1:
            what = '%s argument%s' % (types.pop(), 's' if len(args) > 1 else '')
        else:
            what = 'arguments ' + sp['argty']
        kind = g['kind'] if g else 'Function'
        ret = ', returning %s' % sp['retty'] if kind == 'Function' else ''
        put(dict(name=sp['name'], kind=kind, module='', sig=format_sig(sp['name'].upper(), args, unb),
                 klass=g['klass'] if g else sp['klass'], std=sp['std'],
                 min=sum(1 for _, o in args if not o), max=-1 if unb else len(args), generic=gen,
                 summary='Specific %s for %s%s' % (gen.upper(), what, ret)))

    # Module entities from gfortran's .mod files.
    wanted = [('iso_fortran_env', True), ('iso_c_binding', True), ('ieee_arithmetic', True),
              ('ieee_features', True), ('omp_lib', False)]
    mods = compile_modules(opts.gfortran, wanted)
    for use, text in mods.items():
        for e in mod_entities(text):
            origin = e['origin'].lstrip('_') or use
            if origin == 'w_' + use:
                origin = use
            name = e['name']
            if (name, origin) in entries:
                continue
            if proc_module.get(name) == origin and (name, '') in entries:
                continue
            if origin == 'omp_lib' and re.search(r'_8$', name):
                continue  # integer(8) specifics of the generic of the same stem
            std = (doc_std.get(origin, {}).get(name) or MODULE_STD.get((origin, name))
                   or MODULE_STD.get(origin) or '')
            sigs, mins, maxs = [], [], []
            forms = sorted({tuple(f) for f in e['forms']}, key=len)
            if len(forms) > 1 and all(f == forms[-1][:len(f)] for f in forms):
                # IEEE_SUPPORT_DATATYPE() / (X): one form with the extras optional.
                n0 = len(forms[0])
                forms = [[(a, o or i >= n0) for i, (a, o) in enumerate(forms[-1])]]
            for form in forms:
                sig = format_sig(name.upper(), form, False)
                if sig not in sigs:
                    sigs.append(sig)
                mins.append(sum(1 for _, o in form if not o))
                maxs.append(len(form))
            klass = ''
            if e['kind'] in ('Function', 'Subroutine'):
                a = e['attrs']
                word = e['kind'].lower()
                klass = MODULE_PROC_CLASS.get(name) or (
                    ('elemental ' if 'ELEMENTAL' in a else
                     'pure ' if ('PURE' in a or 'IMPLICIT_PURE' in a) and word == 'subroutine' else '') + word)
            summary = MODULE_SUMMARY.get((origin, name))
            if summary is None and origin == 'iso_c_binding' and name in ctype:
                fty, cty = ctype[name]
                summary = 'Kind of %s matching the C type %s' % (fty, cty)
            if summary is None and origin == 'iso_c_binding' and name in cchar:
                summary = 'C_CHAR character constant for %s (%s)' % cchar[name]
            if summary is None and origin.startswith('omp_lib'):
                summary = ('OpenMP runtime library routine' if e['kind'] in ('Function', 'Subroutine')
                           else 'OpenMP derived type' if e['kind'] == 'Type'
                           else 'OpenMP kind parameter' if name.endswith('_kind')
                           else 'OpenMP named constant')
            if summary is None:
                missing.append('%s:%s' % (origin, name))
                summary = ''
            put(dict(name=name, kind=e['kind'], module=origin, sig='\n'.join(sigs), klass=klass,
                     std=std, min=min(mins) if mins else -1, max=max(maxs) if maxs else -1,
                     generic='', summary=summary))

    # The modules themselves.
    for mod, (std, summary) in {
            'iso_fortran_env': ('Fortran 2003', 'Intrinsic module: environment kinds, units and constants'),
            'iso_c_binding': ('Fortran 2003', 'Intrinsic module: interoperability with C'),
            'ieee_arithmetic': ('Fortran 2003', 'Intrinsic module: IEEE arithmetic inquiries and operations'),
            'ieee_exceptions': ('Fortran 2003', 'Intrinsic module: IEEE exception flags and halting'),
            'ieee_features': ('Fortran 2003', 'Intrinsic module: request IEEE features'),
            'omp_lib': ('OpenMP', 'OpenMP runtime library module'),
            'omp_lib_kinds': ('OpenMP', 'OpenMP kind parameters module')}.items():
        put(dict(name=mod, kind='Module', module='', sig='', klass='', std=std,
                 min=-1, max=-1, generic='', summary=summary))

    # Statement keywords.
    for kw, (std, summary) in KEYWORDS.items():
        entries[(kw, '', 'keyword')] = dict(name=kw, kind='Keyword', module='', sig='', klass='', std=std,
                                            min=-1, max=-1, generic='', summary=summary)

    if missing and not opts.allow_missing:
        sys.exit('no summary for: ' + ' '.join(sorted(missing)))

    rows = sorted(entries.values(), key=lambda e: (e['name'], e['module'], e['kind'] == 'Keyword'))
    lines = [
        '// Generated by tools/gen_fortran_vocab.py -- do not edit. See fortran_lsp_vocab.h.',
        '// Names, signatures, keyword argument names, class and standard come from the',
        '// gfortran manual (GFDL) and gfortran\'s intrinsic module files; no prose from the',
        '// manual is copied. Every summary is mep\'s own (MIT).',
        '#include "fortran_lsp_vocab.h"',
        '',
        '#include <cstring>',
        '',
        'namespace {',
        'using K = FortranVocabKind;',
        'const FortranVocabEntry kVocab[] = {',
    ]
    for e in rows:
        lines.append('    {%s, K::%s, %s,\n     %s,\n     %s, %s,\n     %s,\n     %d, %d, %s},' % (
            cstr(e['name']), e['kind'], cstr(e['module']), cstr(e['sig']), cstr(e['klass']),
            cstr(e['std']), cstr(e['summary']), e['min'], e['max'], cstr(e['generic'])))
    lines += [
        '};',
        'constexpr size_t kCount = sizeof(kVocab) / sizeof(kVocab[0]);',
        '}  // namespace',
        '',
        'const FortranVocabEntry *FortranVocab(size_t *count) {',
        '    if (count) *count = kCount;',
        '    return kVocab;',
        '}',
        '',
        'const FortranVocabEntry *FortranVocabFind(const char *lower_name, size_t len) {',
        '    size_t lo = 0, hi = kCount;',
        '    while (lo < hi) {',
        '        size_t mid = lo + (hi - lo) / 2;',
        '        const char *n = kVocab[mid].name;',
        '        int c = std::strncmp(n, lower_name, len);',
        '        if (c == 0 && n[len] != \'\\0\') c = 1;',
        '        if (c < 0) lo = mid + 1;',
        '        else hi = mid;',
        '    }',
        '    if (lo < kCount && std::strncmp(kVocab[lo].name, lower_name, len) == 0 &&',
        '        kVocab[lo].name[len] == \'\\0\')',
        '        return &kVocab[lo];',
        '    return nullptr;',
        '}',
        '',
    ]
    Path(opts.output).write_text('\n'.join(lines))

    # Report.
    from collections import Counter
    c = Counter()
    for e in rows:
        if e['kind'] == 'Keyword':
            c['keywords'] += 1
        elif e['generic']:
            c['specifics'] += 1
        elif e['kind'] == 'Module':
            c['intrinsic modules'] += 1
        elif e['module']:
            c['module ' + e['module']] += 1
        else:
            c['intrinsic ' + e['kind'].lower()] += 1
    print('%d entries -> %s' % (len(rows), opts.output))
    for k, v in sorted(c.items()):
        print('  %-28s %d' % (k, v))


if __name__ == '__main__':
    main()
