// Fortran language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a
// .f90/.f95/.f03/.f08/.f/.for buffer), kBuiltinLanguageUi's Fortran consumer,
// built on kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Julia
// mode's shape. Included by main.cpp only; BuiltinLanguageUiFortran() is the
// Lua chunk (help/fortran-mode.org is the user's page).
//
// Fortran has no REPL gfortran can drive (LFortran is not packaged), so the
// console is mep's own emulation, the way the C++ mode's crepl is one: a
// Python program (console_source below, written into the session directory
// and started as `python3 mep_fortran_console.py ...`; mep.opt.fortran_ui_cmd
// or a `! REPL:` comment replaces the python3). The session is the `use`
// statements, declarations, derived types, procedures and modules entered so
// far plus every executable input that ran; each input regenerates one
// program from them (procedures after `contains`, modules ahead of it),
// compiles it with the Run button's compiler and Setup flags (plus $FFLAGS
// and mep.opt.fortran_ui_flags) and runs it from the start, showing only what
// was printed after a marker written just before the new statements. An input
// that fails to compile or exits non-zero is dropped; messages point at the
// source file's lines (or `inputN:` for typed ones). A lone expression is
// printed. Modules a `use` names are compiled from source when a .f90 on the
// search path ($MEP_FORTRAN_PATH, -I in the flags, the file's directory, mep's
// working directory) defines them and no -I directory has a prebuilt .mod.
//
// The panels are files the console writes and the poll loop re-reads:
//   - Variables: every declared variable with its type and current value,
//     from print statements the console generates after the last input
//     (an array's shape and first elements; a derived type's components when
//     list-directed output can print them). Clicking a row shows its Help.
//   - Session: the use statements, modules, types, interfaces, procedures.
//   - History: the inputs the session kept; clicking one re-runs it.
//   - Help: `:help NAME` / gh -- a session definition, or one found in the
//     used modules' sources (following their own uses) with the FORD doc
//     comments around it (`!>` above, `!!` below or trailing), a procedure's
//     arguments, a type's components, a module's contents; else gfortran's
//     intrinsics from mep's Fortran vocabulary (`mep-fortran-lsp --intrinsic`).
//
// <leader>re sends the piece at the cursor (the console picks it from a
// snapshot of the buffer: a whole module/procedure/type/interface with its
// docs, or inside a program the statement or construct there, bringing the
// program's use statements along); a Visual selection is sent as is.
// <leader>rr builds and runs a program file -- or runs its `CMD:` line, the
// Run button's own header (main.cpp's mep_run_button_cmd_override) -- and
// loads a file of modules into the session.
#pragma once
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiFortran() {
    static const std::string chunk = std::string(R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.fortran_ui_cmd = mep.opt.fortran_ui_cmd or {'python3'}
mep.opt.fortran_ui_flags = mep.opt.fortran_ui_flags or ''
-- Seconds an input's program may run before the console stops it.
mep.opt.fortran_ui_timeout = mep.opt.fortran_ui_timeout or 60
mep.opt.fortran_ui_info_share = mep.opt.fortran_ui_info_share or 0.34
mep.opt.fortran_ui_bottom_share = mep.opt.fortran_ui_bottom_share or 0.4
mep.opt.fortran_ui_bottom_info_share = mep.opt.fortran_ui_bottom_info_share or 0.45
for _, e in ipairs({'f90', 'f95', 'f03', 'f08', 'f', 'for', 'ftn', 'fpp'}) do
  mep.language_ui_extensions[e] = mep.language_ui_extensions[e] or 'fortran'
end

-- The console (see the header comment), written into each session directory.
local console_source = [===[
)MEPLUA") + R"MEPPY(#!/usr/bin/env python3
# mep's Fortran console: a REPL emulation over gfortran. The session is the
# `use` statements, declarations, types, procedures and modules entered so far
# plus every executable statement that ran; each input regenerates one program
# from them, compiles it and runs it from the start, and only the output after
# a marker (what the new input printed) is shown. The panels are files in the
# session directory that mep's Fortran UI mode re-reads (language_ui_fortran.h).
import argparse
import os
import re
import shlex
import subprocess
import sys

MARK = '\x01mep-new-output\x01'
INTRINSIC_MODULES = {'iso_fortran_env', 'iso_c_binding', 'ieee_arithmetic', 'ieee_exceptions',
                     'ieee_features', 'omp_lib', 'omp_lib_kinds', 'openacc'}
SOURCE_EXTS = ('.f90', '.F90', '.f95', '.F95', '.f03', '.F03', '.f08', '.F08', '.f', '.F', '.for', '.FOR')
TYPE_KW = r'(?:integer|real|double\s*precision|double\s*complex|complex|logical|character|type\s*\(|class\s*\(|procedure\s*\()'
PREFIX = (r'(?:(?:pure|impure|elemental|recursive|non_recursive|module|integer|real|logical|complex|character'
          r'|double\s*precision|type\s*\([^)]*\)|class\s*\([^)]*\))(?:\s*\([^)]*\)|\s*\*\s*\d+)?\s+)*')
PROC_RE = re.compile(r'^' + PREFIX + r'(subroutine|function)\s+(\w+)', re.I)
MODULE_RE = re.compile(r'^module\s+(?!procedure\b|subroutine\b|function\b)(\w+)\s*$', re.I)
TYPE_DEF_RE = re.compile(r'^type\s*(?:,[^:]*)?(?:::)?\s*(\w+)\s*(?:\(.*\))?\s*$', re.I)
KEYWORDS = set('''call print write read if do select allocate deallocate stop error return exit cycle continue
goto go open close inquire rewind backspace flush nullify block associate where forall end contains format
entry sync lock unlock critical case else elseif endif enddo wait data namelist common equivalence save
parameter dimension external intrinsic allocatable pointer target optional intent public private protected
volatile value import include implicit use module program subroutine function interface type class
abstract enum enumerator submodule pure elemental recursive impure'''.split())


def code_part(line):
    """`line` without its comment (a ! inside a string stays)."""
    out, quote = [], None
    for ch in line:
        if quote:
            out.append(ch)
            if ch == quote:
                quote = None
        elif ch in '\'"':
            quote = ch
            out.append(ch)
        elif ch == '!':
            break
        else:
            out.append(ch)
    return ''.join(out).rstrip()


def split_top(text, sep=','):
    """Splits at `sep` outside brackets and strings."""
    parts, depth, quote, cur = [], 0, None, ''
    i = 0
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == quote:
                quote = None
        elif ch in '\'"':
            quote = ch
        elif ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif depth == 0 and text.startswith(sep, i):
            parts.append(cur)
            cur = ''
            i += len(sep)
            continue
        cur += ch
        i += 1
    parts.append(cur)
    return parts


def after_parens(text, start):
    """Index just past the balanced (...) that opens at text[start], or -1."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def strip_label(code):
    m = re.match(r'^\s*(\d+\s+)?(\w+\s*:(?!:)\s*)?', code)
    return code[m.end():] if m else code.lstrip()


def opens_block(code):
    """Whether a statement (comment-free, lower case) opens a construct that `end` closes."""
    c = strip_label(code).lower().strip()
    if not c:
        return False
    if re.match(r'^do\b', c) and not re.match(r'^do\s+\d', c):
        return True
    if re.match(r'^if\s*\(', c):
        return c.endswith('then') and after_parens(c, c.index('(')) > 0 and \
            c[after_parens(c, c.index('(')):].strip() == 'then'
    if re.match(r'^(select\s*(case|type|rank)\s*\(|associate\s*\(|critical\b|block\s*$|enum\b|interface\b|abstract\s+interface\b|change\s+team\b)', c):
        return True
    if re.match(r'^(where|forall)\s*\(', c):
        e = after_parens(c, c.index('('))
        return e > 0 and c[e:].strip() == ''
    if MODULE_RE.match(c) or re.match(r'^(submodule\s*\(|program\s+\w+)', c):
        return True
    if re.match(r'^type\b', c) and not re.match(r'^type\s*\(', c) and not re.match(r'^type\s+is\s*\(', c):
        return TYPE_DEF_RE.match(c) is not None
    if PROC_RE.match(c) and not re.match(r'^module\s+procedure', c):
        return True
    return False


def closes_block(code):
    c = strip_label(code).lower().strip()
    return re.match(r'^end(\s*(do|if|select|associate|critical|block|where|forall|enum|interface|module|submodule'
                    r'|program|subroutine|function|type|team|procedure)\b.*)?\s*$', c) is not None \
        and not re.match(r'^end\s*(file\b|=)', c)


def statements(lines):
    """Joins `&` continuations: yields (first_index, last_index, code) per statement."""
    i = 0
    while i < len(lines):
        first, code = i, code_part(lines[i])
        while code.endswith('&') and i + 1 < len(lines):
            i += 1
            nxt = code_part(lines[i]).lstrip()
            code = code[:-1] + (nxt[1:] if nxt.startswith('&') else nxt)
        yield first, i, code
        i += 1


def needs_more(lines):
    """Whether typed lines leave a construct (or a continuation) open."""
    if lines and code_part(lines[-1]).endswith('&'):
        return True
    depth = 0
    for _, _, code in statements(lines):
        for part in split_top(code, ';'):
            if opens_block(part):
                depth += 1
            elif closes_block(part):
                depth -= 1
    return depth > 0


def parse_decl(code):
    """A type declaration statement as (typespec, attrs, [entity...]), or None."""
    c = code.strip()
    m = re.match(TYPE_KW, c, re.I)
    if not m:
        return None
    end = m.end()
    if c[end - 1] == '(':
        end = after_parens(c, end - 1)
        if end < 0:
            return None
    else:
        rest = c[end:]
        k = re.match(r'\s*\(', rest)
        if k:
            e = after_parens(c, end + k.end() - 1)
            if e < 0:
                return None
            end = e
        else:
            k = re.match(r'\s*\*\s*(\d+|\(\s*[^)]*\))', rest)
            if k:
                end += k.end()
    typespec, rest = c[:end].strip(), c[end:]
    if '::' in rest:
        attr_text, ent_text = rest.split('::', 1)
        if attr_text.strip() and not attr_text.strip().startswith(','):
            return None
        attrs = [a.strip() for a in split_top(attr_text.strip()[1:])] if attr_text.strip() else []
    elif re.match(r'^\s+[a-z_]\w*', rest, re.I) and not re.match(r'^\s*(function|subroutine)\b', rest, re.I):
        attrs, ent_text = [], rest
    else:
        return None
    entities = [e.strip() for e in split_top(ent_text) if e.strip()]
    if not entities or not all(re.match(r'^[a-z_]\w*', e, re.I) for e in entities):
        return None
    return typespec, [a for a in attrs if a], entities


def classify_line(code):
    c = strip_label(code).strip()
    low = c.lower()
    if not c:
        return 'blank'
    if re.match(r'^use\b', low):
        return 'use'
    if re.match(r'^implicit\b', low):
        return 'implicit'
    if re.match(r'^contains\s*$', low):
        return 'contains'
    if parse_decl(c):
        return 'decl'
    return 'exec'


def unit_kind(code):
    c = strip_label(code).lower().strip()
    if MODULE_RE.match(c) or re.match(r'^submodule\s*\(', c):
        return 'module'
    if re.match(r'^program\b', c):
        return 'program'
    if re.match(r'^(abstract\s+)?interface\b', c) or re.match(r'^enum\b', c):
        return 'interface'
    if re.match(r'^type\b', c) and TYPE_DEF_RE.match(c):
        return 'type'
    if PROC_RE.match(c):
        return 'procedure'
    return 'exec'


def unit_name(code):
    c = strip_label(code).strip()
    for rx in (MODULE_RE, TYPE_DEF_RE):
        m = rx.match(c)
        if m:
            return m.group(1).lower()
    m = PROC_RE.match(c)
    if m:
        return m.group(2).lower()
    m = re.match(r'^(?:abstract\s+)?interface\s+(\w+)', c, re.I)
    return m.group(1).lower() if m else None


def is_expression(code):
    """A lone expression typed at the prompt (printed): no keyword, no top-level assignment."""
    c = code.strip()
    if not c or c.endswith('&') or len(split_top(c, ';')) > 1:
        return False
    word = re.match(r'^([a-z_]\w*)', c, re.I)
    if word and word.group(1).lower() in KEYWORDS:
        return False
    return len(split_top(re.sub(r'(==|/=|<=|>=|=>)', '  ', c), '=')) == 1


class Item:
    """One piece of an input: kind, its lines with their origins, and a name for named units."""

    def __init__(self, kind, lines, origins, name=None):
        self.kind, self.lines, self.origins, self.name = kind, lines, origins, name


def split_items(lines, origins):
    """An input's top-level pieces: use, implicit, decl, exec, contains, and whole units."""
    items, pending, i = [], ([], []), 0
    stmts = list(statements(lines))
    k = 0
    while k < len(stmts):
        first, last, code = stmts[k]
        kind = unit_kind(code) if opens_block(code) else None
        if kind is not None:
            depth, j = 0, k
            while j < len(stmts):
                for part in split_top(stmts[j][2], ';'):
                    if opens_block(part):
                        depth += 1
                    elif closes_block(part):
                        depth -= 1
                if depth <= 0:
                    break
                j += 1
            j = min(j, len(stmts) - 1)
            lo, hi = first, stmts[j][1]
            item = Item(kind, pending[0] + lines[lo:hi + 1], pending[1] + origins[lo:hi + 1], unit_name(code))
            pending = ([], [])
            items.append(item)
            k = j + 1
            continue
        text = lines[first:last + 1]
        org = origins[first:last + 1]
        if not code.strip():
            if all(l.strip().startswith('!>') or l.strip().startswith('!!') for l in text if l.strip()) and any(
                    l.strip() for l in text):
                pending = (pending[0] + text, pending[1] + org)
            else:
                pending = ([], [])
            k += 1
            continue
        for part in split_top(code, ';') if ';' in code else [code]:
            ck = classify_line(part)
            if ck == 'blank':
                continue
            if len(split_top(code, ';')) > 1:
                items.append(Item(ck, [part.strip()], [org[0]]))
            else:
                items.append(Item(ck, pending[0] + text, pending[1] + org))
        pending = ([], [])
        k += 1
    return items


def entity_name(entity):
    return re.match(r'^([a-z_]\w*)', entity, re.I).group(1).lower()


def fstr(text):
    return "'" + text.replace("'", "''") + "'"


class Session:
    def __init__(self, args):
        self.args = args
        self.dir = os.path.abspath(args.dir)
        self.build = os.path.join(self.dir, 'build')
        os.makedirs(os.path.join(self.build, 'obj'), exist_ok=True)
        self.reset()
        self.file_cache = {}
        self.object_cache = {}
        self.inputs = 0

    def reset(self):
        self.uses = []          # (text, origin)
        self.implicit = None    # an implicit statement other than `implicit none`
        self.modules = {}       # name -> (lines, origins)
        self.types = {}
        self.interfaces = {}
        self.decls = {}         # name -> (typespec, attrs, entity, doc lines, origin)
        self.procs = {}
)MEPPY" + R"MEPPY(        self.stmts = []         # (lines, origins)
        self.history = []
        self.values = {}

    # -- configuration --------------------------------------------------------------
    def flags(self):
        parts = [self.args.flags or '', os.environ.get('FFLAGS', '')]
        return ' '.join(p for p in parts if p.strip())

    def include_dirs(self):
        dirs = []
        for m in re.finditer(r'(?:^|\s)-I\s*(\S+)', self.flags()):
            d = m.group(1).strip('\'"')
            dirs.append(d if os.path.isabs(d) else os.path.join(os.getcwd(), d))
        return dirs

    def search_path(self):
        dirs = [d for d in os.environ.get('MEP_FORTRAN_PATH', '').split(os.pathsep) if d]
        dirs += self.include_dirs()
        if self.args.src_dir:
            dirs.append(self.args.src_dir)
        dirs.append(os.getcwd())
        seen, out = set(), []
        for d in dirs:
            d = os.path.abspath(d)
            if d not in seen and os.path.isdir(d):
                seen.add(d)
                out.append(d)
        return out

    # -- source index ---------------------------------------------------------------
    def file_info(self, path):
        """{modules: {name: line}, uses: [names]} for a Fortran source file, cached by mtime."""
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            return None
        hit = self.file_cache.get(path)
        if hit and hit[0] == mtime:
            return hit[1]
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                lines = f.read().split('\n')
        except OSError:
            return None
        info = {'modules': {}, 'uses': [], 'lines': lines}
        for idx, line in enumerate(lines):
            c = code_part(line).strip()
            m = MODULE_RE.match(c)
            if m:
                info['modules'][m.group(1).lower()] = idx
            m = re.match(r'^use\b\s*(?:,\s*\w+\s*)?(?:::)?\s*(\w+)', c, re.I)
            if m and m.group(1).lower() not in info['uses']:
                info['uses'].append(m.group(1).lower())
        self.file_cache[path] = (mtime, info)
        return info

    def find_module(self, name):
        """The source file that defines module `name` on the search path, or None."""
        for d in self.search_path():
            try:
                names = sorted(os.listdir(d))
            except OSError:
                continue
            for n in names:
                if n.endswith(SOURCE_EXTS):
                    p = os.path.join(d, n)
                    info = self.file_info(p)
                    if info and name in info['modules']:
                        return p
        return None

    def prebuilt(self, name):
        return any(os.path.exists(os.path.join(d, name + '.mod')) for d in self.include_dirs())

    def module_sources(self, names, defined=()):
        """Source files to compile first for `use names`, dependencies first."""
        order, seen = [], set()

        def visit(name, stack):
            if name in seen or name in INTRINSIC_MODULES or name in defined or self.prebuilt(name):
                return
            seen.add(name)
            path = self.find_module(name)
            if not path:
                return
            info = self.file_info(path)
            for dep in info['uses']:
                if dep not in info['modules'] and dep not in stack:
                    visit(dep, stack | {name})
            if path not in order:
                order.append(path)
        for n in names:
            visit(n, set())
        return order

    def compile_objects(self, sources):
        """Compiles the module sources (cached by mtime) and returns their objects, or None on error."""
        objs, rebuilt = [], False
        for i, path in enumerate(sources):
            obj = os.path.join(self.build, 'obj', '%d_%s.o' % (abs(hash(path)) % 100000, os.path.basename(path)))
            mtime = os.path.getmtime(path)
            if rebuilt or self.object_cache.get(path) != mtime or not os.path.exists(obj):
                cmd = '%s -c %s -J %s -I %s -ffree-line-length-none -fdiagnostics-plain-output %s -o %s' % (
                    self.args.compiler, shlex.quote(path), shlex.quote(self.build), shlex.quote(self.build),
                    self.flags(), shlex.quote(obj))
                p = subprocess.run(['sh', '-c', cmd], capture_output=True, text=True)
                if p.returncode != 0:
                    print(quiet(p.stdout + p.stderr))
                    print('error: building %s failed' % path)
                    return None
                self.object_cache[path] = mtime
                rebuilt = True
            objs.append(obj)
        return objs

    # -- program generation -----------------------------------------------------------
    def used_module_names(self, extra_uses=()):
        names = []
        for text, _ in list(self.uses) + list(extra_uses):
            m = re.match(r'^\s*use\b\s*(?:,\s*\w+\s*)?(?:::)?\s*(\w+)', text, re.I)
            if m and m.group(1).lower() not in names:
                names.append(m.group(1).lower())
        return names

    def generate(self, new=None, with_values=True):
        """The program, as (lines, origins, dump_range). `new` is an executable chunk to run last."""
        out, org = [], []

        def add(lines, origins=None):
            for i, l in enumerate(lines):
                out.append(l)
                org.append(origins[i] if origins else None)
        for lines, origins in self.modules.values():
            add(lines, origins)
        add(['program mep_session'])
        add(['use, intrinsic :: iso_fortran_env, only: mep_ui_out_ => output_unit, mep_ui_err_ => error_unit'])
        for text, origin in self.uses:
            add([text], [origin])
        add([self.implicit or 'implicit none'])
        for lines, origins in list(self.types.values()) + list(self.interfaces.values()):
            add(lines, origins)
        for typespec, attrs, entity, _, origin in self.decls.values():
            add([', '.join([typespec] + attrs) + ' :: ' + entity], [origin])
        for lines, origins in self.stmts:
            add(lines, origins)
        mark = "achar(1) // '%s' // achar(1)" % MARK.strip('\x01')
        add(["write(mep_ui_out_, '(a)') " + mark, "write(mep_ui_err_, '(a)') " + mark,
             'flush(mep_ui_out_)', 'flush(mep_ui_err_)'])
        if new:
            add(new[0], new[1])
        add(['call mep_ui_dump_()', 'contains'])
        for lines, origins in self.procs.values():
            add(lines, origins)
        start = len(out)
        add(self.dump_lines(with_values))
        dump = (start + 1, len(out))
        add(['end program mep_session'])
        return out, org, dump

    def dump_lines(self, with_values):
        path = os.path.join(self.dir, 'values.txt')
        lines = ['subroutine mep_ui_dump_()', 'integer :: mep_u_',
                 'open(newunit=mep_u_, file=%s, status=%s, action=%s)' % (fstr(path), "'replace'", "'write'")]
        for name, (typespec, attrs, entity, _, _) in self.decls.items():
            low = [a.lower().replace(' ', '') for a in attrs]
            ts = typespec.lower().replace(' ', '')
            array = '(' in re.sub(r'^[a-z_]\w*', '', entity.split('=')[0]) or any(a.startswith('dimension') for a in low)
            lines.append("write(mep_u_, '(a)') %s" % fstr('@@' + name))
            if ts.startswith('procedure') or ts.startswith('class'):
                continue
            derived = ts.startswith('type(')
            if derived and not with_values:
                continue
            guard = 'allocated' if 'allocatable' in low else 'associated' if 'pointer' in low else None
            if guard:
                lines.append('if (%s(%s)) then' % (guard, name))
            if array:
                lines.append("write(mep_u_, '(a)') '@shape'")
                lines.append('write(mep_u_, *) shape(%s)' % name)
                lines.append("write(mep_u_, '(a)') '@value'")
                lines.append('write(mep_u_, *) reshape(%s, [min(size(%s), 8)])' % (name, name))
            elif ts.startswith('character'):
                lines.append("write(mep_u_, '(a)') '@value'")
                lines.append("write(mep_u_, '(a)') '\"' // %s // '\"'" % name)
            else:
                lines.append("write(mep_u_, '(a)') '@value'")
                lines.append('write(mep_u_, *) %s' % name)
            if guard:
                lines.append('else')
                lines.append("write(mep_u_, '(a)') '@unset'")
                lines.append('end if')
        lines += ['close(mep_u_)', 'end subroutine mep_ui_dump_']
        return lines

    # -- compile and run --------------------------------------------------------------
    def map_messages(self, text, path, origins, src_label=None):
        """Points compiler/runtime messages at the input's own file and line."""
        base = re.escape(path)

        def where(n):
            o = origins[n - 1] if 0 < n <= len(origins) else None
            return o
        out = []
        for line in text.splitlines():
            m = re.match(r'^(?:%s|%s):(\d+):(\d+):\s*(.*)$' % (base, re.escape(os.path.basename(path))), line)
            if m:
                o = where(int(m.group(1)))
                loc = '%s:%d:%s' % (o[0], o[1], m.group(2)) if o else 'session:%s:%s' % (m.group(1), m.group(2))
                out.append('%s: %s' % (loc, m.group(3)))
                continue
            m = re.match(r'^At line (\d+) of file (\S+)(.*)$', line)
            if m and os.path.basename(m.group(2)) == os.path.basename(path):
                o = where(int(m.group(1)))
                out.append('At line %d of file %s%s' % (o[1], o[0], m.group(3)) if o else line)
                continue
            out.append(line)
        return '\n'.join(out)

    def compile_session(self, new=None):
        """Generates and compiles; returns (exe, origins) or (None, None) after printing errors."""
        defined = set(self.modules)
        objs = self.compile_objects(self.module_sources(self.used_module_names(), defined)
                                    + self.module_sources(self.module_uses(), defined))
        if objs is None:
            return None, None
        src = os.path.join(self.dir, 'session.f90')
        exe = os.path.join(self.build, 'session')
        for with_values in (True, False):
            lines, origins, dump = self.generate(new, with_values)
            with open(src, 'w') as f:
                f.write('\n'.join(lines) + '\n')
            cmd = '%s %s %s -J %s -I %s -ffree-line-length-none -fdiagnostics-plain-output %s -o %s' % (
                self.args.compiler, shlex.quote(src), ' '.join(shlex.quote(o) for o in objs),
                shlex.quote(self.build), shlex.quote(self.build), self.flags(), shlex.quote(exe))
            p = subprocess.run(['sh', '-c', cmd], capture_output=True, text=True)
            if p.returncode == 0:
                return exe, origins
            # A derived-type value list-directed output can't print: drop the values, keep the rest.
            rows = [int(m.group(1)) for m in re.finditer(r'session\.f90:(\d+):\d+: Error', p.stderr)]
            if with_values and rows and all(dump[0] <= r <= dump[1] for r in rows):
                continue
            msg = self.map_messages(quiet(p.stdout + p.stderr), src, origins)
            print('\n'.join(l for l in msg.splitlines() if l.strip()))
            return None, None
        return None, None

    def module_uses(self):
        names = []
        for lines, _ in self.modules.values():
            for l in lines:
                m = re.match(r'^\s*use\b\s*(?:,\s*\w+\s*)?(?:::)?\s*(\w+)', code_part(l), re.I)
                if m:
                    names.append(m.group(1).lower())
        return names

    def run_session(self, new=None):
)MEPPY" + R"MEPPY(        """Compiles and runs; prints the new output. True when the input is kept."""
        exe, origins = self.compile_session(new)
        if not exe:
            return False
        try:
            p = subprocess.run([exe], capture_output=True, text=True, stdin=subprocess.DEVNULL,
                               timeout=self.args.timeout, errors='replace')
        except subprocess.TimeoutExpired:
            print('error: still running after %ds: stopped, input not kept' % self.args.timeout)
            return False
        out = p.stdout.split(MARK + '\n', 1)
        err = p.stderr.split(MARK + '\n', 1)
        sys.stdout.write(out[-1])
        if err[-1]:
            sys.stdout.write(self.map_messages(err[-1], os.path.join(self.dir, 'session.f90'), origins) + '\n')
        sys.stdout.flush()
        if p.returncode != 0:
            print('error: exit status %d: input not kept' % p.returncode)
            return False
        self.read_values()
        return True

    def read_values(self):
        values, cur, mode = {}, None, None
        try:
            with open(os.path.join(self.dir, 'values.txt'), errors='replace') as f:
                text = f.read()
        except OSError:
            return
        for line in text.split('\n'):
            if line.startswith('@@'):
                cur = line[2:]
                values[cur] = {'shape': '', 'value': '', 'unset': False}
            elif cur is None:
                continue
            elif line == '@shape' or line == '@value':
                mode = line[1:]
            elif line == '@unset':
                values[cur]['unset'] = True
            elif mode:
                values[cur][mode] = (values[cur][mode] + ' ' + line.strip()).strip()
        self.values = values

    # -- input --------------------------------------------------------------------------
    def evaluate(self, lines, origins, quiet=False):
        """Adds one input to the session; runs it when it has executable statements."""
        items = split_items(lines, origins)
        if any(i.kind == 'program' for i in items):
            print('error: a whole program: <leader>rr runs it (:run FILE)')
            return False
        saved = self.snapshot()
        exec_lines, exec_origins, in_contains = [], [], False
        for item in items:
            if item.kind == 'contains':
                in_contains = True
            elif item.kind == 'use':
                text = code_part(' '.join(l.strip() for l in item.lines if not l.strip().startswith('!')))
                text = re.sub(r'&\s*', '', text)
                if text.lower() not in [u[0].lower() for u in self.uses]:
                    self.uses.append((text, item.origins[-1]))
            elif item.kind == 'implicit':
                text = code_part(item.lines[-1]).strip()
                self.implicit = None if re.match(r'^implicit\s+none', text, re.I) else text
            elif item.kind == 'decl':
                code = ' '.join(re.sub(r'&\s*$', '', code_part(l)).strip().lstrip('&') for l in item.lines
                                if not l.strip().startswith('!'))
                typespec, attrs, entities = parse_decl(code)
                docs = [l.strip() for l in item.lines if l.strip().startswith('!>')]
                for e in entities:
                    self.decls.pop(entity_name(e), None)
                    self.decls[entity_name(e)] = (typespec, attrs, e, docs, item.origins[-1])
            elif item.kind == 'module':
                self.modules.pop(item.name, None)
                self.modules[item.name] = (item.lines, item.origins)
            elif item.kind == 'type':
                self.types[item.name] = (item.lines, item.origins)
            elif item.kind == 'interface':
                self.interfaces[item.name or 'interface %d' % len(self.interfaces)] = (item.lines, item.origins)
            elif item.kind == 'procedure' or in_contains:
                self.procs[item.name or 'proc %d' % len(self.procs)] = (item.lines, item.origins)
            else:
                code_lines = [l for l in item.lines]
                joined = ' '.join(code_part(l) for l in code_lines)
                if len(code_lines) == 1 and is_expression(joined) and item.kind == 'exec':
                    code_lines = ['print *, ' + code_part(code_lines[0]).strip()]
                exec_lines += code_lines
                exec_origins += item.origins
        new = (exec_lines, exec_origins) if exec_lines else None
        ok = self.run_session(new)
        if not ok:
            self.restore(saved)
            return False
        if new:
            self.stmts.append(new)
        self.history.append('\n'.join(lines).strip('\n'))
        self.write_panels()
        return True

    def snapshot(self):
        return (list(self.uses), self.implicit, dict(self.modules), dict(self.types), dict(self.interfaces),
                dict(self.decls), dict(self.procs))

    def restore(self, s):
        (self.uses, self.implicit, self.modules, self.types, self.interfaces, self.decls, self.procs) = s

    # -- panels ---------------------------------------------------------------------------
    def write_file(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path + '.tmp', 'w') as f:
            f.write(text)
        os.replace(path + '.tmp', path)

    def write_panels(self):
        rows = []
        for name, (typespec, attrs, entity, docs, _) in self.decls.items():
            v = self.values.get(name, {})
            shown_type = ', '.join([typespec] + [a for a in attrs if not a.lower().startswith('intent')])
            if v.get('unset'):
                value = '(not allocated)' if any(a.lower() == 'allocatable' for a in attrs) else '(not associated)'
            elif v.get('shape'):
                value = '[' + ' x '.join(v['shape'].split()) + '] ' + v.get('value', '')
            else:
                value = v.get('value', '')
            if len(value) > 120:
                value = value[:117] + '...'
            display = entity.split('=')[0].strip()
            rows.append('\t'.join(esc(x) for x in (display, shown_type, value)))
        self.write_file('variables.tsv', '\n'.join(rows) + ('\n' if rows else ''))
        sess = []
        if self.uses:
            sess.append('Uses:')
            sess += ['  ' + u[0] for u in self.uses]
        for title, table in (('Modules', self.modules), ('Types', self.types), ('Interfaces', self.interfaces),
                             ('Procedures', self.procs)):
            if table:
                sess.append(title + ':')
                for name, (lines, _) in table.items():
                    head = next((code_part(l).strip() for l in lines if code_part(l).strip()), name)
                    sess.append('  ' + head)
        if self.implicit:
            sess.append(self.implicit)
        self.write_file('session.txt', '\n'.join(sess) + ('\n' if sess else ''))
        self.write_file('history.txt', ''.join(esc(h) + '\n' for h in self.history))

    # -- help -----------------------------------------------------------------------------
    def help(self, name, context_file=None):
        name = name.strip().lower()
        if not re.match(r'^[a-z_]\w*$', name):
            text = '%s: not a Fortran name\n' % name
        else:
            text = self.help_text(name, context_file)
        self.write_file('help.txt', text)
        first = text.split('\n', 1)[0]
        print('# %s: in the Help tab -- %s' % (name, first[:70]))

    def help_text(self, name, context_file=None):
        # The session's own procedures, types, modules and variables first.
        session_lines, session_origins = [], []
        for table in (self.modules, self.types, self.interfaces, self.procs):
            for lines, origins in table.values():
                session_lines += lines + ['']
                session_origins += list(origins) + [None]
        found = find_entity(session_lines, name)
        if found is not None:
            o = session_origins[found] if found < len(session_origins) else None
            return describe(session_lines, found, name, '(this session)' + (' %s:%d' % o if o else ''))
        if name in self.decls:
            typespec, attrs, entity, docs, origin = self.decls[name]
            doc = '\n'.join(d[2:].strip() for d in docs)
            v = self.values.get(name, {})
            return '%s\n\n%s%s\nvariable of this session%s\n' % (
                ', '.join([typespec] + attrs) + ' :: ' + entity, doc + '\n\n' if doc else '',
                ('value: ' + v.get('value', '')) if v.get('value') else '',
                (' (%s:%d)' % origin) if origin else '')
        # Then what the session's `use`d modules (and the file's) define, following their own uses.
        names = self.used_module_names() + self.module_uses()
        context_lines = None
        if context_file:
            info = self.file_info(context_file)
            if info:
                context_lines = info['lines']
                names += [n for n in info['uses'] if n not in names]
                found = find_entity(context_lines, name)
                if found is not None:
                    return describe(context_lines, found, name, '%s:%d' % (context_file, found + 1))
        seen = set()
        queue = [n for n in names]
        while queue:
            mod = queue.pop(0)
            if mod in seen or mod in INTRINSIC_MODULES:
                continue
            seen.add(mod)
            path = self.find_module(mod)
            if not path:
                continue
            info = self.file_info(path)
            lines = info['lines']
            start = info['modules'][mod]
            if mod == name:
                return describe(lines, start, name, '%s:%d' % (path, start + 1))
            end = module_end(lines, start)
            found = find_entity(lines[start:end], name)
            if found is not None:
                return describe(lines, start + found, name, 'module %s, %s:%d' % (mod, path, start + found + 1))
            queue += [u for u in module_uses_in(lines[start:end]) if u not in seen]
        # A module named directly (not used yet).
        path = self.find_module(name)
        if path:
            info = self.file_info(path)
            return describe(info['lines'], info['modules'][name], name, '%s:%d' % (path, info['modules'][name] + 1))
        # gfortran's intrinsics, from mep's Fortran vocabulary.
        if self.args.lsp:
            try:
                p = subprocess.run([self.args.lsp, '--intrinsic', name], capture_output=True, text=True, timeout=10)
                if p.returncode == 0 and p.stdout.strip():
                    return p.stdout + '\n(gfortran intrinsic)\n'
            except (OSError, subprocess.TimeoutExpired):
                pass
        where = ', '.join(self.search_path())
        return ('%s: no documentation found -- not defined in this session, in the modules it uses, or as an '
                'intrinsic.\nModules are looked up in: %s\n' % (name, where or '(nothing)'))


def module_end(lines, start):
    for i in range(start + 1, len(lines)):
        c = code_part(lines[i]).strip().lower()
        if re.match(r'^end\s*module\b', c) or c == 'end':
            if c == 'end':
                continue
            return i + 1
    return len(lines)


def module_uses_in(lines):
    out = []
    for l in lines:
        m = re.match(r'^\s*use\b\s*(?:,\s*\w+\s*)?(?:::)?\s*(\w+)', code_part(l), re.I)
        if m:
            out.append(m.group(1).lower())
    return out


def find_entity(lines, name):
    """The index of the line that defines `name` (a procedure, type, interface, module or
    declared entity), or None. Definitions win over declarations."""
    decl = None
    for _, (first, last, code) in enumerate(statements(lines)):
        c = strip_label(code).strip()
        low = c.lower()
        if not c:
            continue
)MEPPY" + R"MEPPY(        m = PROC_RE.match(c)
        if m and m.group(2).lower() == name and not low.startswith('module procedure'):
            return first
        m = MODULE_RE.match(c)
        if m and m.group(1).lower() == name:
            return first
        if re.match(r'^type\b', low) and not re.match(r'^type\s*\(', low):
            m = TYPE_DEF_RE.match(c)
            if m and m.group(1).lower() == name:
                return first
        m = re.match(r'^(?:abstract\s+)?interface\s+(\w+)', c, re.I)
        if m and m.group(1).lower() == name:
            return first
        if decl is None:
            d = parse_decl(c)
            if d and any(entity_name(e) == name for e in d[2]):
                decl = first
    return decl


def doc_text(comment):
    """A FORD doc comment's text: `!> text` / `!! text` -> `text`."""
    text = comment.strip()[2:]
    return (text[1:] if text.startswith(' ') else text).rstrip()


def trailing_doc(lines, last):
    """The doc of the declaration ending on line `last`: a trailing `!! x` / `!< x`, or `!!` lines below."""
    m = re.search(r'!(?:!|<)\s?(.*)$', lines[last])
    if m and code_part(lines[last]).strip():
        return m.group(1).rstrip()
    rows = []
    k = last + 1
    while k < len(lines) and lines[k].strip().startswith('!!'):
        rows.append(doc_text(lines[k]))
        k += 1
    return ' '.join(rows)


def describe(lines, idx, name, where):
    """What Help shows for the entity defined on line idx: the definition, its FORD docs (`!>`
    lines above it, `!!` below or trailing), then a procedure's dummy arguments, a type's
    components or a module's contents, each with its own doc."""
    docs = []
    j = idx - 1
    while j >= 0 and lines[j].strip().startswith('!>'):
        docs.insert(0, doc_text(lines[j]))
        j -= 1
    head, k = [], idx
    while k < len(lines):
        head.append(lines[k])
        if not code_part(lines[k]).endswith('&'):
            break
        k += 1
    after = trailing_doc(lines, k)
    if after:
        docs.append(after)
    k += 1
    while k < len(lines) and lines[k].strip().startswith('!!'):
        k += 1
    code = ' '.join(re.sub(r'&\s*$', '', code_part(h)).strip().lstrip('&').strip() for h in head)
    code = re.sub(r'\s+', ' ', code)
    out = [code]
    if docs:
        out += [''] + ['\n'.join(docs).strip('\n')]
    c = strip_label(code).strip()
    m = PROC_RE.match(c)
    title, rows = None, []
    if m:
        args = re.search(r'\(([^)]*)\)', c[m.end():])
        dummies = [a.strip().lower() for a in args.group(1).split(',') if a.strip()] if args else []
        res = re.search(r'result\s*\(\s*(\w+)\s*\)', c, re.I)
        result = res.group(1).lower() if res else (m.group(2).lower() if m.group(1).lower() == 'function' else None)
        title = 'Arguments:'
        for first, last, stmt in statements(lines[k:]):
            st = strip_label(stmt).strip()
            if closes_block(st) or opens_block(st) or re.match(r'^contains\b', st, re.I):
                break
            d = parse_decl(st)
            if d and any(entity_name(e) in dummies or entity_name(e) == result for e in d[2]):
                doc = trailing_doc(lines, k + last)
                rows.append('  ' + re.sub(r'\s+', ' ', st) + ('   ! ' + doc if doc else ''))
    elif MODULE_RE.match(c):
        title = 'Contents:'
        end = module_end(lines, idx)
        depth = 0
        for first, last, stmt in statements(lines[k:end - 1]):
            st = strip_label(stmt).strip()
            if depth == 0 and st:
                kind = unit_kind(st) if opens_block(st) else None
                if kind in ('procedure', 'type', 'interface') and unit_name(st):
                    rows.append('  ' + re.sub(r'\s+', ' ', st))
                elif not kind:
                    d = parse_decl(st)
                    if d:
                        rows += ['  ' + re.sub(r'\s+', ' ', ', '.join([d[0]] + d[1]) + ' :: ' + e) for e in d[2]]
            if opens_block(st):
                depth += 1
            elif closes_block(st):
                depth -= 1
    elif re.match(r'^type\b', c, re.I) and TYPE_DEF_RE.match(c):
        title = 'Components:'
        for first, last, stmt in statements(lines[k:]):
            st = strip_label(stmt).strip()
            if closes_block(st) or re.match(r'^contains\b', st, re.I):
                break
            if parse_decl(st):
                doc = trailing_doc(lines, k + last)
                rows.append('  ' + re.sub(r'\s+', ' ', st) + ('   ! ' + doc if doc else ''))
    if rows:
        out += ['', title] + rows
    return '\n'.join(out) + '\n\n' + where + '\n'


def quiet(text):
    """Compiler output without the warnings a C toolchain's environment causes (NIX_CFLAGS_COMPILE's
    C-only options reach gfortran too) and without repeats of the line before."""
    out = []
    for line in text.splitlines():
        if re.search(r'Warning: command-line option .* is valid for .* but not for Fortran', line):
            continue
        if out and out[-1] == line:
            continue
        out.append(line)
    return '\n'.join(out)


def esc(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')


def find_cmd_header(lines):
    """The Run button's `CMD:` comment (main.cpp's mep_run_button_cmd_override), or None."""
    for line in lines:
        m = re.match(r'^\s*[^\w\s]+\s*[Cc][Mm][Dd]:\s*(.*?)\s*$', line)
        if m:
            cmd = re.sub(r'\s*\*/$', '', m.group(1))
            cmd = re.sub(r'\s*-->$', '', cmd)
            if cmd:
                return cmd
    return None


def block_at(lines, row):
    """The piece of `lines` (1-based `row`) that <leader>re sends: a whole top-level unit
    (module, procedure, type, interface) with its `!>` docs, or inside a program the
    statement or construct at the cursor. Returns (first, last) 0-based, inclusive, and
    the program's `use` lines (to bring along), or None."""
    stmts = list(statements(lines))
    # Depth of each statement, and the top-level units.
    depth, units, stack = 0, [], []
    info = []
    for first, last, code in stmts:
        d0 = depth
        for part in split_top(code, ';'):
            if opens_block(part):
                if depth == 0:
                    stack = [(first, unit_kind(part))]
                depth += 1
            elif closes_block(part):
                depth -= 1
                if depth == 0 and stack:
                    units.append((stack[0][0], last, stack[0][1]))
                    stack = []
        info.append((first, last, code, d0))
    r = row - 1
    # On a unit's `!>` docs: the unit they document.
    while r + 1 < len(lines) and lines[r].strip().startswith('!>'):
        r += 1
    unit = next((u for u in units if u[0] <= r <= u[1]), None)

    def with_docs(first):
        while first > 0 and lines[first - 1].strip().startswith('!>'):
            first -= 1
        return first
    if unit is None:
        # Outside any unit: the statement at the cursor (a loose snippet file).
        for first, last, code, d in info:
            if first <= r <= last and code.strip():
                if opens_block(code):
                    return block_from(info, first, lines), []
                return (first, last), []
        return None
    if unit[2] != 'program':
        return (with_docs(unit[0]), unit[1]), []
    uses = [code_part(lines[f]).strip() for f, l, c, d in info if unit[0] <= f <= unit[1] and d == 1
            and classify_line(c) == 'use']
    if r == unit[0] or r == unit[1]:
        # On `program` / `end program`: the body.
        return (unit[0] + 1, unit[1] - 1), []
    contains_at = next((f for f, l, c, d in info if unit[0] < f < unit[1] and d == 1
                        and re.match(r'^\s*contains\s*$', c, re.I)), None)
    for idx, (first, last, code, d) in enumerate(info):
        if d == 1 and first <= r and code.strip() and unit[0] < first < unit[1]:
            if not opens_block(code):
                if last >= r:
                    return (first, last), uses
                continue
            span = block_from(info, first, lines)
            if span[0] <= r <= span[1]:
                if contains_at is not None and first > contains_at:
                    return (with_docs(span[0]), span[1]), uses
                return span, uses
    return None


def block_from(info, first, lines):
    depth = 0
    for f, l, code, d in info:
        if f < first:
            continue
        for part in split_top(code, ';'):
            if opens_block(part):
                depth += 1
            elif closes_block(part):
                depth -= 1
        if depth <= 0:
            return first, l
    return first, info[-1][1]


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    if text.endswith('\n'):
        text = text[:-1]
    return text.split('\n')


def run_file(session, path, name):
    """<leader>rr: a program is built and run (or the file's CMD: line is); a file of
    modules and procedures is loaded into the session; anything else is evaluated."""
    lines = read_lines(path)
    cmd = find_cmd_header(lines)
    if cmd:
        print('$ ' + cmd)
        sys.stdout.flush()
        subprocess.run(['sh', '-c', cmd])
        return
    items = split_items(lines, [(name, i + 1) for i in range(len(lines))])
    if any(i.kind == 'program' for i in items):
        defined = {i.name for i in items if i.kind == 'module'}
        uses = []
        for l in lines:
            m = re.match(r'^\s*use\b\s*(?:,\s*\w+\s*)?(?:::)?\s*(\w+)', code_part(l), re.I)
            if m:
                uses.append(m.group(1).lower())
        objs = session.compile_objects(session.module_sources(uses, defined))
        if objs is None:
            return
        exe = os.path.join(session.build, 'program')
        ext = os.path.splitext(name)[1] or '.f90'
        src = os.path.join(session.build, 'program' + ext)
        with open(src, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        cmd = '%s %s %s -J %s -I %s -fdiagnostics-plain-output %s -o %s' % (
            session.args.compiler, shlex.quote(src), ' '.join(shlex.quote(o) for o in objs),
            shlex.quote(os.path.join(session.build, 'program_mod')), shlex.quote(session.build),
            session.flags(), shlex.quote(exe))
        os.makedirs(os.path.join(session.build, 'program_mod'), exist_ok=True)
        print('$ %s %s %s -o %s' % (session.args.compiler, os.path.basename(name), session.flags(), 'program'))
        sys.stdout.flush()
        p = subprocess.run(['sh', '-c', cmd], capture_output=True, text=True)
        if p.returncode != 0:
            print(quiet(p.stdout + p.stderr).replace(src, name).rstrip())
            print('error: build failed')
            return
        if quiet(p.stderr).strip():
            print(quiet(p.stderr).replace(src, name).rstrip())
        sys.stdout.flush()
        r = subprocess.run([exe], stderr=subprocess.PIPE, text=True, errors='replace')
        if r.stderr:
            sys.stdout.write(r.stderr.replace(src, name))
        if r.returncode != 0:
            print('[exit status %d]' % r.returncode)
        return
    before = set(session.modules)
    if session.evaluate(lines, [(name, i + 1) for i in range(len(lines))]):
        added = [n for n in session.modules if n not in before]
        if added:
            print('# loaded module%s %s: `use %s` to use it' % ('s' if len(added) > 1 else '', ', '.join(added),
                                                                 added[0]))


USAGE = '''Fortran console (gfortran) -- each input recompiles the session and shows its new output.
  Type statements, declarations (integer :: n = 3), use statements, procedures, types or
  modules; a lone expression is printed. Constructs continue until their `end`.
)MEPPY" + R"MEPPY(  :help NAME      documentation of NAME (session, used modules' !> docs, intrinsics)
  :vars           the declared variables and their values
  :show           the generated program        :undo   drop the last executable input
  :reset          start an empty session       :flags  the compile command's flags
  :begin ... :end a multi-line input           :quit   leave
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--compiler', default='gfortran')
    ap.add_argument('--flags', default='')
    ap.add_argument('--src-dir', default='')
    ap.add_argument('--lsp', default='')
    ap.add_argument('--timeout', type=int, default=60)
    args = ap.parse_args()
    session = Session(args)
    session.write_panels()
    for n in ('help.txt',):
        session.write_file(n, '')
    try:
        import readline  # noqa: F401  line editing and history at the prompt
    except ImportError:
        pass
    ver = subprocess.run(['sh', '-c', args.compiler + ' --version'], capture_output=True, text=True)
    first = (ver.stdout.split('\n', 1)[0] if ver.returncode == 0 else 'compiler not found: ' + args.compiler)
    print('%s -- mep Fortran console: <leader>re sends the piece at the cursor, <leader>rr runs the file, '
          ':help NAME / gh shows docs, :help for commands.' % first)
    interactive = sys.stdin.isatty()
    while True:
        try:
            line = input('f> ' if interactive else '')
        except EOFError:
            break
        except KeyboardInterrupt:
            print()
            continue
        stripped = line.strip()
        try:
            if not stripped:
                continue
            if stripped.startswith(':'):
                cmd, _, rest = stripped[1:].partition(' ')
                rest = rest.strip()
                if cmd in ('quit', 'q', 'exit'):
                    break
                elif cmd == 'help':
                    if rest:
                        parts = rest.split(None, 1)
                        session.help(parts[0], parts[1] if len(parts) > 1 else None)
                    else:
                        sys.stdout.write(USAGE)
                elif cmd == 'eval':
                    # :eval PATH NAME -- PATH padded with blank lines to NAME's line numbers.
                    path, _, name = rest.partition(' ')
                    lines = read_lines(path)
                    origins = [(name or path, i + 1) for i in range(len(lines))]
                    session.evaluate(lines, origins)
                elif cmd == 'evalat':
                    # :evalat ROW PATH NAME -- the piece around ROW of a snapshot of NAME.
                    row, _, rest2 = rest.partition(' ')
                    path, _, name = rest2.partition(' ')
                    lines = read_lines(path)
                    found = block_at(lines, int(row))
                    if not found:
                        print('# nothing to evaluate at line %s' % row)
                        continue
                    (first, last), uses = found
                    origins = [(name or path, i + 1) for i in range(len(lines))]
                    sel = lines[first:last + 1]
                    org = origins[first:last + 1]
                    have = [u[0].lower() for u in session.uses]
                    extra = [u for u in uses if u.lower() not in have]
                    if extra and not all(classify_line(code_part(l)) == 'use' for l in sel if code_part(l).strip()):
                        sel, org = extra + sel, [None] * len(extra) + org
                    print('# %s:%d-%d' % (os.path.basename(name or path), first + 1, last + 1))
                    session.evaluate(sel, org)
                elif cmd == 'run':
                    path, _, name = rest.partition(' ')
                    run_file(session, path, name or path)
                elif cmd == 'vars':
                    session.write_panels()
                    with open(os.path.join(session.dir, 'variables.tsv')) as f:
                        for row in f:
                            print('  '.join(row.rstrip('\n').split('\t')))
                elif cmd == 'show':
                    lines, _, _ = session.generate()
                    print('\n'.join(lines))
                elif cmd == 'undo':
                    if session.stmts:
                        session.stmts.pop()
                        session.run_session(None)
                        session.write_panels()
                        print('# dropped the last executable input')
                elif cmd == 'reset':
                    session.reset()
                    session.write_panels()
                    print('# empty session')
                elif cmd == 'flags':
                    print(args.compiler + ' ' + session.flags())
                    print('module search path: ' + os.pathsep.join(session.search_path()))
                elif cmd == 'begin':
                    lines = []
                    while True:
                        more = input('.. ' if interactive else '')
                        if more.strip() == ':end':
                            break
                        lines.append(more)
                    session.inputs += 1
                    session.evaluate(lines, [('input%d' % session.inputs, i + 1) for i in range(len(lines))])
                else:
                    print('unknown command :%s (:help lists them)' % cmd)
                continue
            lines = [line]
            while needs_more(lines):
                lines.append(input('.. ' if interactive else ''))
            session.inputs += 1
            session.evaluate(lines, [('input%d' % session.inputs, i + 1) for i in range(len(lines))])
        except EOFError:
            break
        except KeyboardInterrupt:
            print('\n# interrupted')
        except Exception as e:  # keep the console alive
            print('mep console error: %s' % e)
        sys.stdout.flush()


if __name__ == '__main__':
    main()
)MEPPY" + R"MEPLUA(]===]

-- tab id -> session state; the sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state, sidebars = {}, nil
local shq = mep.language_ui_shq

local function decode(text)
  return (text:gsub('\\([nrt\\])', function(c) return ({n = '\n', r = '\r', t = '\t', ['\\'] = '\\'})[c] end))
end

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 50),
    session = mep.sidebar_create('Session', 'right', 50),
    history = mep.sidebar_create('History', 'right', 50),
    help = mep.sidebar_create('Help', 'right', 50),
  }
end

local function send(st, text)
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local info = mep.terminal_info and mep.terminal_info(st.console_buf)
  if info and info.exited then
    mep.notify('Fortran console exited; :MepFortranRestart starts a fresh session', 'warn')
    return false
  end
  mep.terminal_write(st.console_buf, text .. '\n')
  return true
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.variables_text) or ''):gmatch('[^\n]+') do
    local name, ty, value = line:match('^([^\t]*)\t([^\t]*)\t(.*)$')
    if name then
      name, ty, value = decode(name), decode(ty), decode(value)
      local bare = name:match('^[%w_]+') or name
      widgets[#widgets + 1] = {id = bare, text = name .. ' : ' .. ty .. (value ~= '' and (' = ' .. value) or ''),
        wrap = true, wrap_indent = 2, tooltip = 'help ' .. bare, on_click = function()
          send(st, ':help ' .. bare)
          if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
        end}
    end
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no variables yet -- declare one: integer :: n = 3)'} end
  mep.sidebar_set_sections(sidebars.variables, {{id = 'variables', title = '', collapsed = false, widgets = widgets}})
  mep.language_ui_render_textbox(sidebars.session, st and st.session_text,
    '(the use statements, types, procedures and modules this session has)')
  widgets = {}
  local history = {}
  for line in ((st and st.history_text) or ''):gmatch('[^\n]+') do history[#history + 1] = decode(line) end
  for i = #history, math.max(1, #history - 199), -1 do
    local code = history[i]
    widgets[#widgets + 1] = {id = 'h' .. i, text = code, wrap = true,
      on_click = function() send(st, ':begin\n' .. code .. '\n:end') end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(inputs the session kept appear here; clicking re-runs one)'} end
  mep.sidebar_set_sections(sidebars.history, {{id = 'history', title = '', collapsed = false, widgets = widgets}})
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(gh over a name, or :help NAME in the console: a procedure, type or module from the used modules\' !> docs, or an intrinsic)')
end

-- A snapshot of the live source buffer (unsaved edits included), named with
-- the source's extension so the compiler reads the same source form.
local function snapshot(st)
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  st.submission = (st.submission or 0) + 1
  local path = st.dir .. '/source_' .. st.submission .. '.' .. st.ext
  mep.language_ui_write_file(path, table.concat(lines, '\n') .. '\n')
  return path
end

-- <leader>re: the Visual selection if one is live (padded to its source
-- lines), else the piece at the cursor, which the console picks from a
-- snapshot of the buffer: a whole module, procedure, type or interface
-- (with its !> docs), or inside a program the statement or construct there.
function mep.fortran_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Fortran console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  if mep.current_buffer() ~= st.src_buf then mep.notify('Send code from the source pane', 'warn') return end
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local r = mep.visual_range()
    mep.cmd('normal!')
    local text
    if r and r.kind == 'char' then
      text = string.rep('\n', r.start_row - 1) .. sel .. '\n'
    elseif r then
      local chunk = {}
      for i = r.start_row, r.end_row do chunk[#chunk + 1] = mep.get_line(i) or '' end
      text = string.rep('\n', r.start_row - 1) .. table.concat(chunk, '\n') .. '\n'
    end
    if text then
      mep.language_ui_write_file(st.eval_path, text)
      send(st, ':eval ' .. st.eval_path .. ' ' .. st.src_abs)
    end
    return
  end
  local path = snapshot(st)
  send(st, ':evalat ' .. (mep.cursor()) .. ' ' .. path .. ' ' .. st.src_abs)
end
mep.language_ui_eval_handlers.fortran = mep.fortran_ui_eval_block

-- The Run button's `CMD:` comment (the same rule as mep_run_button_cmd_override).
local function has_cmd_header(lines)
  for _, line in ipairs(lines) do
    local cmd = line:match('^%s*[^%w%s]+%s*[Cc][Mm][Dd]:%s*(.-)%s*$')
    if cmd and cmd:gsub('%s*%*/$', ''):gsub('%s*%-%->$', '') ~= '' then return true end
  end
  return false
end

function mep.fortran_ui_open()
  local fname = mep.filename()
  local src_abs = mep_lsp_abspath(fname)
  local ext = src_abs:match('%.([%w]+)$') or 'f90'
  local dir = mep.language_ui_session_dir('fortranui')
  local paths = {variables = dir .. '/variables.tsv', session = dir .. '/session.txt',
    history = dir .. '/history.txt', help = dir .. '/help.txt'}
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local script = dir .. '/mep_fortran_console.py'
  mep.language_ui_write_file(script, console_source)
  local cfg = mep.run_button_config_for and (mep.run_button_config_for(ext) or mep.run_button_config_for('f90'))
  local compiler = (cfg and cfg.compiler) or 'gfortran'
  local flags = (cfg and cfg.flags) or ''
  if mep.opt.fortran_ui_flags ~= '' then flags = flags .. ' ' .. mep.opt.fortran_ui_flags end
  local src_dir = src_abs:match('^(.*)/[^/]*$') or '.'
  local argv = mep.language_ui_cmd(mep.opt.fortran_ui_cmd)
  for _, a in ipairs({script, '--dir', dir, '--compiler', compiler, '--flags=' .. flags, '--src-dir', src_dir,
      '--lsp', mep.bundled_tool and mep.bundled_tool('mep-fortran-lsp') or 'mep-fortran-lsp',
      '--timeout', tostring(mep.opt.fortran_ui_timeout)}) do
    argv[#argv + 1] = a
  end
  local src_buf = mep.current_buffer()
  local layout = mep.language_ui_layout({
    info_share = mep.opt.fortran_ui_info_share, bottom_share = mep.opt.fortran_ui_bottom_share,
    bottom_info_share = mep.opt.fortran_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'fortran') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = dir, ext = ext, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, eval_path = dir .. '/eval.' .. ext, argv = argv,
    variables_text = '', session_text = '', history_text = '', help_text = '',
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.session, sidebars.history})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Fortran language UI: gfortran console below; Variables/Session/History top-right, Help bottom-right; <leader>re sends the piece at the cursor, <leader>rr runs the file')
  -- <leader>rr / the Run button: the buffer as it is now. A program is built
  -- and run (its CMD: line instead, when it has one: that needs the saved
  -- file); a file of modules or procedures is loaded into the session.
  local function run_source(f)
    local lines = mep.buffer_get_lines(st.src_buf) or {}
    if has_cmd_header(lines) then mep.cmd('write') end
    send(st, ':run ' .. snapshot(st) .. ' ' .. mep_lsp_abspath(f or fname))
  end
  return {
    run_source = run_source,
    prefer_session_run = true,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Fortran language UI mode closed')
    end,
  }
end
mep.language_ui_modes.fortran = {open = mep.fortran_ui_open}

-- gh: the console's :help for the name under the cursor -- the session's own
-- definitions, then the !> docs of the modules it and the file use, then
-- gfortran's intrinsics -- in the Help tab.
function mep.fortran_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_]')
  if not sym or sym == '' or not sym:match('^[%a_]') then mep.notify('gh: no Fortran name under the cursor', 'warn') return true end
  local from_source = mep.current_buffer() == st.src_buf
  send(st, ':help ' .. sym .. (from_source and (' ' .. snapshot(st)) or ''))
  if from_source then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('fortran', mep.fortran_ui_help_at_cursor)

mep.command('MepFortranRestart', function()
  if not state[mep.current_tab_id()] then return end
  mep.language_ui_toggle(); mep.language_ui_toggle()
end)

-- This tab's session as the panels see it (nil when no Fortran mode is open).
function mep.fortran_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, variables = st.variables_text,
    session = st.session_text, history = st.history_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Fortran mode
-- is open on the active tab.
function mep.fortran_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed, help_changed = false, false
  for _, key in ipairs({'variables', 'session', 'history', 'help'}) do
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[key .. '_text'] then
      st[key .. '_text'] = text
      changed = true
      if key == 'help' then help_changed = true end
    end
  end
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.fortran_ui_poll(st)
  end)
end
)MEPLUA";
    return chunk.c_str();
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
