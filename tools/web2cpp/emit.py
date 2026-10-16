"""Writing C++ from pascal.py's AST.

The program becomes one class: its global variables are members, its
procedures and functions are methods, its main block is `main_body()`.
Hand-written C++ supplies what the generated code calls but does not
define -- file handling, the terminal, the pool's initial strings -- as
members of the base class the generated class derives from (`EngineBase`,
src/tex/tex_runtime.h), and the change file reaches it through @=...@>.

Mapping rules, chosen so that the C++ means what the Pascal meant:
  * every integer scalar (variables, parameters, record fields, results)
    is int32_t, whatever its subrange: Pascal subranges are unchecked in
    TeX, and a narrow C++ type would wrap where Pascal would not
    (`for k:=0 to 255` with k:0..255 must terminate);
  * array elements are as narrow as their declared range allows, since
    arrays are where the memory is;
  * `/` is real division, `div`/`mod` are C++'s truncating / and %;
  * a goto to a label of the main program from inside a routine (TeX's
    jump_out, `goto final_end`) is a thrown TexJump, caught in main_body.
"""

import re

from pascal import const_value

CPP_KEYWORDS = {
    'int', 'char', 'bool', 'double', 'float', 'long', 'short', 'signed', 'unsigned', 'void', 'auto', 'class',
    'struct', 'union', 'enum', 'new', 'delete', 'this', 'template', 'typename', 'operator', 'private', 'public',
    'protected', 'friend', 'virtual', 'inline', 'static', 'extern', 'register', 'volatile', 'const', 'mutable',
    'explicit', 'export', 'namespace', 'using', 'typedef', 'sizeof', 'switch', 'case', 'default', 'break',
    'continue', 'return', 'goto', 'throw', 'try', 'catch', 'true', 'false', 'nullptr', 'and', 'or', 'not', 'xor',
    'bitand', 'bitor', 'compl', 'final', 'override', 'concept', 'requires', 'module', 'import', 'co_await',
    'co_return', 'co_yield', 'alignas', 'alignof', 'decltype', 'noexcept', 'static_assert', 'thread_local',
    'asm', 'char8_t', 'char16_t', 'char32_t', 'wchar_t', 'constexpr', 'consteval', 'constinit', 'do', 'while',
    'for', 'if', 'else', 'line', 'remainder', 'round', 'trunc', 'abs', 'time', 'index', 'free', 'exit', 'main',
    'stdin', 'stdout', 'stderr', 'eof', 'error'}

# Names the generated code must not take for itself because the runtime
# (or C++) has them; mapped to a suffixed spelling.
def cname(name):
    return name + '_' if name in CPP_KEYWORDS else name


HAND_RECORDS = {'two_halves', 'four_quarters', 'memory_word'}


class EmitError(Exception):
    pass


class Emitter:
    def __init__(self, prog, externals=(), int_types=None):
        self.prog = prog
        self.consts = {}         # name -> int value or string
        self.const_exprs = {}
        self.types = {}          # name -> type node
        self.globals = {}        # name -> type node
        self.routines = {}       # name -> routine node (definitions win over forwards)
        self.externals = set(externals)  # hand-written members callable without ()
        self.main_labels = set(prog.labels)
        self.cur = None          # routine being emitted (None = main body)
        self.locals = {}
        self.labels_used = set()
        self.tmp = 0
        for n, e in prog.consts:
            v = const_value(e)
            if v is None and e.kind == 'pstr':
                v = e.text
            if v is None:
                raise EmitError(f'constant {n} is not a number')
            self.consts[n] = v
        for n, t in prog.types:
            self.types[n] = t
        for names, t, mod in prog.vars:
            for n in names:
                self.globals[n] = t
        for r in prog.routines:
            if r.name not in self.routines or not r.forward:
                self.routines[r.name] = r

    # -- types -----------------------------------------------------------------------

    def resolve(self, t):
        seen = 0
        while t.kind == 'tname' and t.name in self.types and t.name not in HAND_RECORDS:
            t = self.types[t.name]
            seen += 1
            if seen > 50:
                raise EmitError('type loop')
        return t

    def bounds(self, t):
        """(lo, hi) of an ordinal type as ints when known, else None."""
        t = self.resolve(t)
        if t.kind == 'sub':
            lo, hi = self.cval(t.lo), self.cval(t.hi)
            if lo is None or hi is None:
                return None
            return lo, hi
        if t.kind == 'tname':
            if t.name == 'char':
                return 0, 255
            if t.name == 'boolean':
                return 0, 1
        return None

    def bound_exprs(self, t):
        """(lo_cpp, hi_cpp) of an index type, as C++ expressions."""
        t = self.resolve(t)
        if t.kind == 'sub':
            return self.expr(t.lo), self.expr(t.hi)
        if t.kind == 'tname' and t.name == 'char':
            return '0', '255'
        if t.kind == 'tname' and t.name == 'boolean':
            return '0', '1'
        raise EmitError(f'unsupported index type {t}')

    def cval(self, e):
        v = const_value(e)
        if v is not None:
            return v
        if e.kind == 'var' and e.name in self.consts and isinstance(self.consts[e.name], int):
            return self.consts[e.name]
        if e.kind == 'paren':
            return self.cval(e.e)
        if e.kind == 'un' and e.op == '-':
            v = self.cval(e.e)
            return None if v is None else -v
        if e.kind == 'bin' and e.op in ('+', '-', '*'):
            a, b = self.cval(e.l), self.cval(e.r)
            if a is None or b is None:
                return None
            return a + b if e.op == '+' else a - b if e.op == '-' else a * b
        return None

    def scalar_type(self, t):
        """The C++ type of a variable, parameter, field or result."""
        if t.kind == 'tname' and t.name in HAND_RECORDS:
            return t.name
        if t.kind == 'tname' and t.name in self.types:
            r = self.types[t.name]
            if r.kind in ('record',):
                return t.name
            if r.kind == 'array':
                return self.array_type(r)
            if r.kind == 'file':
                return self.file_type(r)
            return self.scalar_type(r)
        if t.kind == 'tname':
            return {'integer': 'int32_t', 'boolean': 'bool', 'real': 'double', 'char': 'uint8_t'}.get(t.name) or \
                self._unknown(t.name)
        if t.kind == 'sub':
            return 'int32_t'
        if t.kind == 'array':
            return self.array_type(t)
        if t.kind == 'file':
            return self.file_type(t)
        if t.kind == 'record':
            raise EmitError('anonymous record')
        raise EmitError(f'type {t}')

    def _unknown(self, name):
        raise EmitError(f'unknown type {name}')

    def elem_type(self, t):
        """The C++ type of an array element: as narrow as its range allows."""
        r = self.resolve(t)
        if r.kind == 'tname' and r.name in HAND_RECORDS:
            return r.name
        if t.kind == 'tname' and t.name in self.types and self.types[t.name].kind == 'record':
            return t.name
        if r.kind == 'tname':
            if r.name == 'boolean':
                return 'bool'
            if r.name == 'char':
                return 'uint8_t'
            if r.name == 'real':
                return 'double'
            if r.name == 'integer':
                return 'int32_t'
        b = self.bounds(r)
        if b is not None:
            lo, hi = b
            if lo >= 0 and hi <= 255:
                return 'uint8_t'
            if lo >= 0 and hi <= 65535:
                return 'uint16_t'
            if lo >= -32768 and hi <= 32767:
                return 'int16_t'
            return 'int32_t'
        if r.kind == 'sub':
            return 'int32_t'
        return self.scalar_type(t)

    def array_type(self, t):
        if len(t.index) != 1:
            raise EmitError('multidimensional array')
        return f'PArray<{self.elem_type(t.elem)}>'

    def file_type(self, t):
        e = self.resolve(t.elem)
        if e.kind == 'tname' and e.name == 'char':
            return 'AlphaFile'
        if e.kind == 'tname' and e.name == 'memory_word':
            return 'WordFile'
        return 'ByteFile'

    def type_kind(self, t):
        """'int' | 'real' | 'bool' | 'char' | 'array' | 'record' | 'file' for type inference."""
        if t is None:
            return 'int'
        if t.kind == 'tname':
            if t.name in HAND_RECORDS:
                return 'record'
            if t.name in self.types:
                return self.type_kind(self.types[t.name])
            return {'integer': 'int', 'boolean': 'bool', 'real': 'real', 'char': 'char'}.get(t.name, 'int')
        if t.kind == 'sub':
            return 'int'
        return {'array': 'array', 'record': 'record', 'file': 'file'}[t.kind]

    # -- header ------------------------------------------------------------------------

    def header(self, ns, base, extra_include):
        out = []
        w = out.append
        w('// Generated by tools/web2cpp from third_party/tex/tex.web and')
        w('// src/tex/web/*.ch. Do not edit: change the change files and regenerate')
        w('// (`just tex-gen`).')
        w('#pragma once')
        w('#include "tex/tex_runtime.h"')
        w('#include "tex/gen/tex_consts.h"')
        w('')
        w(f'namespace {ns} {{')
        w('')
        # Records (other than the hand-written memory word family).
        for n, t in self.prog.types:
            if t.kind == 'record' and n not in HAND_RECORDS:
                w(f'struct {n} {{')
                for names, ft in t.fields:
                    for fn in names:
                        w(f'    {self.scalar_type(ft)} {cname(fn)};')
                if t.variants:
                    raise EmitError(f'record {n} has variants')
                w('};')
                w('')
        w(f'struct Engine : {base} {{')
        for n, v in self.consts.items():
            if isinstance(v, int):
                w(f'    static constexpr int32_t {cname(n)} = {v};')
            else:
                w(f'    static constexpr const char *{cname(n)} = {cstr(v)};')
        w('')
        for names, t, mod in self.prog.vars:
            ct = self.scalar_type(t)
            for n in names:
                w(f'    {ct} {cname(n)}{{}};  // §{mod}')
        w('')
        w('    void alloc_arrays();')
        w('    void main_body();')
        for r in self.routines.values():
            if r.forward and r.block is None:
                # a forward with no body: hand-written
                continue
            w(f'    {self.signature(r, decl=True)};  // §{r.mod}')
        w(f'#include "{extra_include}"')
        w('};')
        w('')
        w(f'}}  // namespace {ns}')
        return '\n'.join(out) + '\n'

    def signature(self, r, decl=False, qual=''):
        ps = []
        for names, t, byref in r.params:
            ct = self.scalar_type(t)
            for n in names:
                ps.append(f'{ct} {"&" if byref else ""}{cname(n)}')
        ret = self.scalar_type(r.ret) if r.func else 'void'
        return f'{ret} {qual}{cname(r.name)}({", ".join(ps)})'

    # -- bodies ------------------------------------------------------------------------

    def alloc(self):
        out = ['void Engine::alloc_arrays() {']
        for names, t, mod in self.prog.vars:
            r = self.resolve(t) if t.kind == 'tname' else t
            if r.kind != 'array':
                continue
            lo, hi = self.bound_exprs(r.index[0])
            for n in names:
                out.append(f'    {cname(n)}.alloc({lo}, {hi});')
        out.append('}')
        return '\n'.join(out) + '\n'

    def routine(self, r):
        self.cur = r
        self.locals = {}
        self.tmp = 0
        for names, t, byref in r.params:
            for n in names:
                self.locals[n] = t
        blk = r.block
        for names, t, mod in blk.vars:
            for n in names:
                self.locals[n] = t
        self.labels_used = set()
        self.local_labels = set(blk.labels)
        self.target_labels = set()
        _collect_gotos(blk.body, self.target_labels)
        body = []
        self.stmts(blk.body.stmts, body, 1)
        out = [f'// §{r.mod}', self.signature(r, qual='Engine::') + ' {']
        if blk.consts or blk.types:
            raise EmitError(f'{r.name}: local constants/types')
        for names, t, mod in blk.vars:
            ct = self.scalar_type(t)
            rt = self.resolve(t) if t.kind == 'tname' else t
            for n in names:
                out.append(f'    {ct} {cname(n)}{{}};')
                if rt.kind == 'array':
                    lo, hi = self.bound_exprs(rt.index[0])
                    out.append(f'    {cname(n)}.alloc({lo}, {hi});')
        if r.func:
            out.append(f'    {self.scalar_type(r.ret)} Result{{}};')
        out.extend(body)
        if r.func:
            out.append('    return Result;')
        out.append('}')
        self.cur = None
        return '\n'.join(out) + '\n'

    def main(self):
        self.cur = None
        self.locals = {}
        self.local_labels = set(self.prog.labels)
        self.target_labels = set()
        _collect_gotos(self.prog.body, self.target_labels)
        body = []
        self.stmts(self.prog.body.stmts, body, 3)
        out = ['void Engine::main_body() {',
               '    int32_t jump_to = 0;',
               '    for (;;) {',
               '        try {',
               '            switch (jump_to) {']
        for l in sorted(self.prog.labels):
            out.append(f'            case {l}: goto L{l};')
        out.append('            default: break;')
        out.append('            }')
        out.extend(body)
        out.append('            return;')
        out.append('        } catch (const TexJump &j) {')
        out.append('            jump_to = j.label;')
        out.append('        }')
        out.append('    }')
        out.append('}')
        return '\n'.join(out) + '\n'

    # -- statements ---------------------------------------------------------------------

    def stmts(self, ss, out, ind):
        for s in ss:
            self.stmt(s, out, ind)

    def stmt(self, s, out, ind):
        p = '    ' * ind
        k = s.kind
        if k == 'empty':
            return
        if k == 'compound':
            self.stmts(s.stmts, out, ind)
            return
        if k == 'labeled':
            out.append(f'{"    " * max(0, ind - 1)}L{s.label}:;')
            self.stmt(s.stmt, out, ind)
            return
        if k == 'assign':
            lhs = s.lhs
            if self.cur is not None and self.cur.func and lhs.kind == 'var' and lhs.name == self.cur.name:
                out.append(f'{p}Result = {self.expr(s.rhs)};')
            elif s.rhs.kind == 'pstr' and len(s.rhs.text) != 1:
                # A string constant into a `packed array of char`.
                out.append(f'{p}pset({self.desig(lhs)}, {cstr(s.rhs.text)});')
            else:
                out.append(f'{p}{self.desig(lhs)} = {self.expr(s.rhs)};')
            return
        if k == 'call':
            out.append(f'{p}{self.call(s.name, s.args, stmt=True)};')
            return
        if k == 'if':
            out.append(f'{p}if ({self.cond(s.cond)}) {{')
            self.stmt(s.then, out, ind + 1)
            if s.els is not None and s.els.kind != 'empty':
                out.append(f'{p}}} else {{')
                self.stmt(s.els, out, ind + 1)
            out.append(f'{p}}}')
            return
        if k == 'while':
            out.append(f'{p}while ({self.cond(s.cond)}) {{')
            self.stmt(s.body, out, ind + 1)
            out.append(f'{p}}}')
            return
        if k == 'repeat':
            out.append(f'{p}do {{')
            self.stmts(s.body, out, ind + 1)
            out.append(f'{p}}} while (!({self.cond(s.cond)}));')
            return
        if k == 'for':
            v = self.desig_name(s.var)
            self.tmp += 1
            lim = f'for_end_{self.tmp}'
            op, step = ('>=', '--') if s.down else ('<=', '++')
            out.append(f'{p}{{')
            out.append(f'{p}    const int32_t {lim} = {self.expr(s.stop)};')
            out.append(f'{p}    for ({v} = {self.expr(s.start)}; {v} {op} {lim}; {step}{v}) {{')
            self.stmt(s.body, out, ind + 2)
            out.append(f'{p}    }}')
            out.append(f'{p}}}')
            return
        if k == 'case':
            out.append(f'{p}switch ({self.expr(s.expr)}) {{')
            for labs, st in s.arms:
                cases = []
                for l in labs:
                    v = self.cval(l)
                    cases.append(f'case {v if v is not None else self.expr(l)}:')
                out.append(f'{p}{" ".join(cases)} {{')
                self.stmt(st, out, ind + 1)
                out.append(f'{p}}} break;')
            out.append(f'{p}default: {{')
            if s.others is not None:
                self.stmt(s.others, out, ind + 1)
            out.append(f'{p}}} break;')
            out.append(f'{p}}}')
            return
        if k == 'goto':
            if s.label in self.local_labels:
                out.append(f'{p}goto L{s.label};')
            elif s.label in self.main_labels:
                out.append(f'{p}throw TexJump{{{s.label}}};')
            else:
                raise EmitError(f'goto {s.label}: no such label')
            return
        if k == 'verbatim':
            out.append(f'{p}{s.text}')
            return
        raise EmitError(f'statement {k}')

    # -- expressions ---------------------------------------------------------------------

    def lookup(self, name):
        """('local'|'global'|'const'|'routine'|'external'|'self', type)"""
        if name in self.locals:
            return 'local', self.locals[name]
        if self.cur is not None and name == self.cur.name and self.cur.func:
            return 'self', self.cur.ret
        if name in self.globals:
            return 'global', self.globals[name]
        if name in self.consts:
            return 'const', None
        if name in self.routines:
            return 'routine', self.routines[name].ret
        if name in self.externals:
            return 'external', None
        return None, None

    def desig_name(self, name):
        kind, _ = self.lookup(name)
        if kind in ('local', 'global'):
            return cname(name)
        raise EmitError(f'module {self.cur.mod if self.cur else "main"}: {name} is not a variable')

    def desig(self, e):
        k = e.kind
        if k == 'var':
            kind, t = self.lookup(e.name)
            if kind in ('local', 'global'):
                return cname(e.name)
            if kind == 'const':
                return cname(e.name)
            if kind in ('routine', 'external', 'self'):
                return self.call(e.name, [])
            raise EmitError(f'unknown identifier {e.name!r}' + self._where())
        if k == 'index':
            return f'{self.desig(e.base)}[{", ".join(self.expr(i) for i in e.idx)}]'
        if k == 'field':
            return f'{self.desig(e.base)}.{cname(e.name)}'
        if k == 'deref':
            return f'{self.desig(e.base)}.buf()'
        if k == 'call':
            return self.call(e.name, e.args)
        raise EmitError(f'designator {k}')

    def _where(self):
        return f' in {self.cur.name}' if self.cur else ' in the main body'

    def cond(self, e):
        s = self.expr(e)
        if s.startswith('(') and s.endswith(')') and _balanced(s[1:-1]):
            return s[1:-1]
        return s

    def expr(self, e):
        k = e.kind
        if k == 'num':
            return str(e.val)
        if k == 'real':
            return e.val
        if k == 'bool':
            return 'true' if e.val else 'false'
        if k == 'pool':
            return f'{e.num} /*{_comment(e.text)}*/'
        if k == 'pstr':
            if len(e.text) == 1:
                return f"uint8_t({ord(e.text)})"
            return cstr(e.text)
        if k == 'paren':
            return f'({self.expr(e.e)})'
        if k == 'un':
            if e.op == 'not':
                return f'(!{self.expr(e.e)})'
            return f'(-{self.expr(e.e)})'
        if k == 'bin':
            a, b = self.expr(e.l), self.expr(e.r)
            op = e.op
            if op == '/':
                return f'(double({a}) / double({b}))'
            m = {'div': '/', 'mod': '%', 'and': '&&', 'or': '||', '=': '==', '<>': '!='}.get(op, op)
            return f'({a} {m} {b})'
        if k in ('var', 'index', 'field', 'deref', 'call'):
            return self.desig(e)
        if k == 'width':
            return self.expr(e.expr)
        if k == 'verb':
            return f'({e.text})'
        raise EmitError(f'expression {k}')

    def etype(self, e):
        """Coarse type of an expression: 'int' | 'real' | 'bool' | 'char' | 'str' | 'other'."""
        k = e.kind
        if k == 'num':
            return 'int'
        if k == 'real':
            return 'real'
        if k == 'bool':
            return 'bool'
        if k == 'pstr':
            return 'char' if len(e.text) == 1 else 'str'
        if k == 'pool':
            return 'int'
        if k == 'paren':
            return self.etype(e.e)
        if k == 'un':
            return 'bool' if e.op == 'not' else self.etype(e.e)
        if k == 'bin':
            if e.op in ('=', '<>', '<', '<=', '>', '>=', 'and', 'or'):
                return 'bool'
            if e.op == '/':
                return 'real'
            a, b = self.etype(e.l), self.etype(e.r)
            return 'real' if 'real' in (a, b) else 'int'
        if k == 'width':
            return self.etype(e.expr)
        t = self.dtype(e)
        if t is None:
            return 'other'
        tk = self.type_kind(t)
        return tk if tk in ('int', 'real', 'bool', 'char') else 'other'

    def dtype(self, e):
        """The Pascal type of a designator, when known."""
        k = e.kind
        if k == 'var':
            kind, t = self.lookup(e.name)
            if kind == 'const':
                v = self.consts[e.name]
                return None if isinstance(v, str) else _INT
            return t
        if k == 'index':
            bt = self.dtype(e.base)
            if bt is None:
                return None
            bt = self.resolve(bt)
            if bt.kind != 'array':
                return None
            return bt.elem
        if k == 'field':
            bt = self.dtype(e.base)
            if bt is None:
                return None
            bt = self.resolve(bt) if not (bt.kind == 'tname' and bt.name in HAND_RECORDS) else bt
            if bt.kind == 'tname' and bt.name in HAND_RECORDS:
                return _hand_field_type(bt.name, e.name)
            if bt.kind != 'record':
                return None
            for names, ft in bt.fields:
                if e.name in names:
                    return ft
            return None
        if k == 'deref':
            bt = self.dtype(e.base)
            if bt is None:
                return None
            bt = self.resolve(bt)
            return bt.elem if bt.kind == 'file' else None
        if k == 'call':
            if e.name in self.routines:
                return self.routines[e.name].ret
            if e.name in ('chr',):
                return _CHAR
            if e.name in ('ord', 'round', 'trunc', 'abs'):
                return _INT
            if e.name in ('odd', 'eof', 'eoln'):
                return _BOOL
            return None
        return None

    def call(self, name, args, stmt=False):
        a = [self.expr(x) for x in args]
        # Pascal builtins.
        if name == 'abs':
            return f'pabs({a[0]})'
        if name == 'odd':
            return f'podd({a[0]})'
        if name == 'chr':
            return f'uint8_t({a[0]})'
        if name == 'ord':
            return f'int32_t({a[0]})'
        if name == 'round':
            return f'pround({a[0]})'
        if name == 'trunc':
            return f'int32_t({a[0]})'
        if name in ('eof', 'eoln', 'get', 'put', 'close'):
            return f'{a[0]}.{name}()'
        if name in ('write', 'write_ln', 'writeln'):
            f = a[0]
            rest = [self.write_arg(x) for x in args[1:]]
            fn = 'rt_write' if name == 'write' else 'rt_write_ln'
            return f'{fn}({", ".join([f] + rest)})'
        if name in ('read', 'read_ln'):
            raise EmitError(f'{name}: replace the module with a change' + self._where())
        if name in ('reset', 'rewrite', 'break', 'break_in', 'erstat'):
            raise EmitError(f'{name}: replace the module with a change' + self._where())
        kind, _ = self.lookup(name)
        if kind in ('routine', 'external', 'self') or (kind is None and args):
            return f'{cname(name)}({", ".join(a)})'
        if kind in ('local', 'global'):
            if args:
                raise EmitError(f'{name} is a variable, not a routine' + self._where())
            return cname(name)
        raise EmitError(f'unknown routine {name!r}' + self._where())

    def write_arg(self, x):
        t = self.etype(x)
        s = self.expr(x)
        if t == 'char':
            return f'uint8_t({s})'
        if t == 'str':
            return s
        if t == 'real':
            return f'double({s})'
        return f'int32_t({s})'


class _T:
    def __init__(self, kind, name=None):
        self.kind = kind
        self.name = name


_INT = _T('tname', 'integer')
_CHAR = _T('tname', 'char')
_BOOL = _T('tname', 'boolean')
_REAL = _T('tname', 'real')


def _hand_field_type(rec, field):
    if field in ('int', 'sc', 'rh', 'lh', 'b0', 'b1', 'b2', 'b3'):
        return _INT
    if field == 'gr':
        return _REAL
    if field == 'hh':
        return _T('tname', 'two_halves')
    if field == 'qqqq':
        return _T('tname', 'four_quarters')
    return None


def _collect_gotos(s, acc):
    if s is None:
        return
    k = s.kind
    if k == 'goto':
        acc.add(s.label)
    for attr in ('stmts', 'body'):
        v = getattr(s, attr, None)
        if isinstance(v, list):
            for x in v:
                _collect_gotos(x, acc)
        elif v is not None and hasattr(v, 'kind'):
            _collect_gotos(v, acc)
    for attr in ('then', 'els', 'stmt', 'others'):
        v = getattr(s, attr, None)
        if v is not None and hasattr(v, 'kind'):
            _collect_gotos(v, acc)
    if k == 'case':
        for labs, st in s.arms:
            _collect_gotos(st, acc)


def _balanced(s):
    d = 0
    for c in s:
        if c == '(':
            d += 1
        elif c == ')':
            d -= 1
            if d < 0:
                return False
    return d == 0


def _comment(s):
    return s.replace('*/', '* /').replace('\n', ' ')


def cstr(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '"' or ch == '\\':
            out.append('\\' + ch)
        elif 32 <= o < 127:
            out.append(ch)
        else:
            out.append(f'\\{o:03o}')
    out.append('"')
    return ''.join(out)
