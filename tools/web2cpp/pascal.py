"""Parsing the Pascal that TANGLE would have written (web.py's token stream).

The subset is TeX's: one program block with labels, constants, types,
variables and procedures/functions (no nesting), and the statements and
expressions TeX uses. The result is a small AST that emit.py turns into C++.
Identifiers are case-folded to lower case, as Pascal treats them.
"""

from web import ID, NUM, REAL, POOL, CHAR, PSTR, SYM, VERB, CHECKSUM, WebError


class PascalError(Exception):
    pass


# --- AST ----------------------------------------------------------------------

class Node:
    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)

    def __repr__(self):
        items = ', '.join(f'{k}={v!r}' for k, v in self.__dict__.items() if k != 'kind')
        return f'{self.kind}({items})'


def N(kind, **kw):
    return Node(kind, **kw)


KEYWORDS = {'program', 'label', 'const', 'type', 'var', 'procedure', 'function', 'forward', 'begin', 'end',
            'if', 'then', 'else', 'case', 'of', 'others', 'while', 'do', 'repeat', 'until', 'for', 'to',
            'downto', 'goto', 'array', 'packed', 'record', 'file', 'div', 'mod', 'and', 'or', 'not', 'in', 'with',
            'set', 'nil'}


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    # -- token helpers -----------------------------------------------------------

    def peek(self, k=0):
        j = self.i + k
        return self.toks[j] if j < len(self.toks) else None

    def error(self, msg):
        t = self.peek()
        ctx = ' '.join(str(x.val) for x in self.toks[max(0, self.i - 12):self.i + 12])
        mod = t.mod if t else '?'
        raise PascalError(f'module {mod}: {msg}\n  ... {ctx} ...')

    def kw(self, word):
        t = self.peek()
        return t is not None and t.kind == ID and t.val.lower() == word

    def sym(self, s):
        t = self.peek()
        return t is not None and t.kind == SYM and t.val == s

    def take_kw(self, word):
        if not self.kw(word):
            self.error(f'expected {word}')
        self.i += 1

    def take_sym(self, s):
        if not self.sym(s):
            self.error(f'expected {s!r}')
        self.i += 1

    def opt_sym(self, s):
        if self.sym(s):
            self.i += 1
            return True
        return False

    def ident(self):
        t = self.peek()
        if t is None or t.kind != ID or t.val.lower() in KEYWORDS:
            self.error('expected an identifier')
        self.i += 1
        return t.val.lower()

    def mod_here(self):
        t = self.peek()
        return t.mod if t else 0

    # -- program -------------------------------------------------------------------

    def program(self):
        self.take_kw('program')
        name = self.ident()
        self.take_sym(';')
        blk = self.block(top=True)
        self.take_sym('.')
        if self.i != len(self.toks):
            self.error('text after the end of the program')
        blk.name = name
        return blk

    def block(self, top=False):
        labels, consts, types, vars_, routines = [], [], [], [], []
        while True:
            if self.kw('label'):
                self.i += 1
                while True:
                    labels.append(self.label_value())
                    if not self.opt_sym(','):
                        break
                self.take_sym(';')
            elif self.kw('const'):
                self.i += 1
                while self.peek().kind == ID and not self.peek().val.lower() in KEYWORDS:
                    n = self.ident()
                    self.take_sym('=')
                    consts.append((n, self.expr()))
                    self.take_sym(';')
            elif self.kw('type'):
                self.i += 1
                while self.peek().kind == ID and not self.peek().val.lower() in KEYWORDS:
                    n = self.ident()
                    self.take_sym('=')
                    types.append((n, self.type_()))
                    self.take_sym(';')
            elif self.kw('var'):
                self.i += 1
                while self.peek().kind == ID and not self.peek().val.lower() in KEYWORDS:
                    names = [self.ident()]
                    while self.opt_sym(','):
                        names.append(self.ident())
                    self.take_sym(':')
                    vars_.append((names, self.type_(), self.mod_here()))
                    self.take_sym(';')
            elif top and (self.kw('procedure') or self.kw('function')):
                routines.append(self.routine())
            else:
                break
        mod = self.mod_here()
        body = self.compound()
        return N('block', labels=labels, consts=consts, types=types, vars=vars_, routines=routines, body=body,
                 mod=mod)

    def label_value(self):
        e = self.expr()
        v = const_value(e)
        if v is None:
            self.error('label is not a number')
        return v

    def routine(self):
        mod = self.mod_here()
        is_func = self.kw('function')
        self.i += 1
        name = self.ident()
        params = []
        if self.opt_sym('('):
            while True:
                byref = False
                if self.kw('var'):
                    self.i += 1
                    byref = True
                names = [self.ident()]
                while self.opt_sym(','):
                    names.append(self.ident())
                self.take_sym(':')
                params.append((names, self.type_(), byref))
                if not self.opt_sym(';'):
                    break
            self.take_sym(')')
        ret = None
        if is_func:
            self.take_sym(':')
            ret = self.type_()
        self.take_sym(';')
        if self.kw('forward'):
            self.i += 1
            self.take_sym(';')
            return N('routine', name=name, func=is_func, params=params, ret=ret, forward=True, block=None, mod=mod)
        blk = self.block()
        self.take_sym(';')
        return N('routine', name=name, func=is_func, params=params, ret=ret, forward=False, block=blk, mod=mod)

    # -- types -----------------------------------------------------------------------

    def type_(self):
        packed = False
        if self.kw('packed'):
            self.i += 1
            packed = True
        if self.kw('array'):
            self.i += 1
            self.take_sym('[')
            idx = [self.type_()]
            while self.opt_sym(','):
                idx.append(self.type_())
            self.take_sym(']')
            self.take_kw('of')
            return N('array', packed=packed, index=idx, elem=self.type_())
        if self.kw('file'):
            self.i += 1
            self.take_kw('of')
            return N('file', elem=self.type_())
        if self.kw('record'):
            self.i += 1
            fields, variants = self.field_list()
            self.take_kw('end')
            return N('record', fields=fields, variants=variants)
        # A subrange `lo..hi`, or a type name.
        t = self.peek()
        if t.kind == ID and not self.peek(1).is_sym('..') and t.val.lower() not in KEYWORDS:
            self.i += 1
            return N('tname', name=t.val.lower())
        lo = self.expr()
        self.take_sym('..')
        hi = self.expr()
        return N('sub', lo=lo, hi=hi)

    def field_list(self):
        fields = []
        variants = None
        while self.peek().kind == ID and self.peek().val.lower() not in KEYWORDS:
            names = [self.ident()]
            while self.opt_sym(','):
                names.append(self.ident())
            self.take_sym(':')
            fields.append((names, self.type_()))
            if not self.opt_sym(';'):
                break
        if self.kw('case'):
            self.i += 1
            self.type_()  # the tag type (TeX's variants have no tag field)
            self.take_kw('of')
            variants = []
            while not self.kw('end'):
                labs = [self.expr()]
                while self.opt_sym(','):
                    labs.append(self.expr())
                self.take_sym(':')
                self.take_sym('(')
                vf, vv = self.field_list()
                self.take_sym(')')
                variants.append(vf)
                if not self.opt_sym(';'):
                    break
        return fields, variants

    # -- statements ------------------------------------------------------------------

    def compound(self):
        mod = self.mod_here()
        self.take_kw('begin')
        stmts = self.stmt_seq(('end',))
        self.take_kw('end')
        return N('compound', stmts=stmts, mod=mod)

    def stmt_seq(self, enders):
        stmts = [self.statement()]
        while True:
            if self.opt_sym(';'):
                stmts.append(self.statement())
            elif stmts[-1].kind == 'verbatim' and not any(self.kw(e) for e in enders):
                # @=...@> text is C++ that ends itself; no Pascal `;' needed
                stmts.append(self.statement())
            else:
                break
        if not any(self.kw(e) for e in enders):
            self.error(f'expected ; or {"/".join(enders)}')
        return stmts

    def statement(self):
        mod = self.mod_here()
        t = self.peek()
        # A label: a number (or parenthesized number, from a macro) and ':'.
        lab = self._try_label()
        if lab is not None:
            return N('labeled', label=lab, stmt=self.statement(), mod=mod)
        if t.kind == ID:
            w = t.val.lower()
            if w == 'begin':
                return self.compound()
            if w == 'if':
                self.i += 1
                c = self.expr()
                self.take_kw('then')
                th = self.statement()
                el = None
                if self.kw('else'):
                    self.i += 1
                    el = self.statement()
                return N('if', cond=c, then=th, els=el, mod=mod)
            if w == 'while':
                self.i += 1
                c = self.expr()
                self.take_kw('do')
                return N('while', cond=c, body=self.statement(), mod=mod)
            if w == 'repeat':
                self.i += 1
                body = self.stmt_seq(('until',))
                self.take_kw('until')
                return N('repeat', body=body, cond=self.expr(), mod=mod)
            if w == 'for':
                self.i += 1
                v = self.ident()
                self.take_sym(':=')
                a = self.expr()
                down = False
                if self.kw('downto'):
                    down = True
                    self.i += 1
                else:
                    self.take_kw('to')
                b = self.expr()
                self.take_kw('do')
                return N('for', var=v, start=a, stop=b, down=down, body=self.statement(), mod=mod)
            if w == 'case':
                return self.case_stmt()
            if w == 'goto':
                self.i += 1
                return N('goto', label=self.label_value(), mod=mod)
            if w in ('end', 'until', 'else'):
                return N('empty', mod=mod)
            if w == 'others':
                return N('empty', mod=mod)
        if t.kind == SYM and t.val == ';':
            return N('empty', mod=mod)
        if t.kind == VERB:
            self.i += 1
            return N('verbatim', text=t.val, mod=mod)
        # Assignment or procedure call.
        lhs = self.designator()
        if self.opt_sym(':='):
            return N('assign', lhs=lhs, rhs=self.expr(), mod=mod)
        if lhs.kind == 'var':
            return N('call', name=lhs.name, args=[], mod=mod)
        if lhs.kind == 'call':
            return N('call', name=lhs.name, args=lhs.args, mod=mod)
        self.error('expected a statement')

    def _try_label(self):
        """A statement label: a constant built from numbers, parentheses and
        + / - (macros like `main_loop_move+1` expand to that), then `:` that
        does not begin `:=`."""
        j = self.i
        n = len(self.toks)
        while j < n:
            x = self.toks[j]
            if x.kind == NUM or x.is_sym('(') or x.is_sym(')') or x.is_sym('+') or x.is_sym('-'):
                j += 1
                continue
            break
        if j == self.i or j >= n or not self.toks[j].is_sym(':'):
            return None
        if j + 1 < n and self.toks[j + 1].is_sym('='):
            return None
        save = self.i
        e = self.expr()
        v = const_value(e)
        if v is None or not self.sym(':'):
            self.i = save
            return None
        self.i += 1
        return v

    def case_stmt(self):
        mod = self.mod_here()
        self.take_kw('case')
        e = self.expr()
        self.take_kw('of')
        arms = []
        others = None
        while True:
            if self.kw('end'):
                break
            if self.kw('others'):
                self.i += 1
                self.take_sym(':')
                others = self.statement()
                self.opt_sym(';')
                continue
            if self.sym(';'):
                self.i += 1
                continue
            labs = [self.expr()]
            while self.opt_sym(','):
                labs.append(self.expr())
            self.take_sym(':')
            st = self.statement()
            arms.append((labs, st))
            if not self.opt_sym(';'):
                if not (self.kw('end') or self.kw('others')):
                    self.error('expected ; in case')
        self.take_kw('end')
        return N('case', expr=e, arms=arms, others=others, mod=mod)

    # -- expressions -------------------------------------------------------------------

    def designator(self):
        name = self.ident()
        e = N('var', name=name)
        while True:
            if self.sym('['):
                self.i += 1
                idx = [self.expr()]
                while self.opt_sym(','):
                    idx.append(self.expr())
                self.take_sym(']')
                e = N('index', base=e, idx=idx)
            elif self.sym('.') and self.peek(1) is not None and self.peek(1).kind == ID:
                self.i += 1
                e = N('field', base=e, name=self.ident())
            elif self.sym('^'):
                self.i += 1
                e = N('deref', base=e)
            elif self.sym('(') and e.kind == 'var':
                self.i += 1
                args = []
                if not self.sym(')'):
                    args.append(self.arg())
                    while self.opt_sym(','):
                        args.append(self.arg())
                self.take_sym(')')
                e = N('call', name=name, args=args)
            else:
                break
        return e

    def arg(self):
        e = self.expr()
        # write(f, x:w) field widths
        if self.sym(':'):
            self.i += 1
            w = self.expr()
            return N('width', expr=e, width=w)
        return e

    def expr(self):
        a = self.simple()
        t = self.peek()
        if t is not None and t.kind == SYM and t.val in ('=', '<>', '<', '<=', '>', '>='):
            self.i += 1
            return N('bin', op=t.val, l=a, r=self.simple())
        return a

    def simple(self):
        sign = None
        if self.sym('-') or self.sym('+'):
            sign = self.peek().val
            self.i += 1
        a = self.term()
        if sign == '-':
            a = N('un', op='-', e=a)
        while True:
            t = self.peek()
            if t is None:
                break
            if t.kind == SYM and t.val in ('+', '-'):
                self.i += 1
                a = N('bin', op=t.val, l=a, r=self.term())
            elif t.kind == ID and t.val.lower() == 'or':
                self.i += 1
                a = N('bin', op='or', l=a, r=self.term())
            else:
                break
        return a

    def term(self):
        a = self.factor()
        while True:
            t = self.peek()
            if t is None:
                break
            if t.kind == SYM and t.val in ('*', '/'):
                self.i += 1
                a = N('bin', op=t.val, l=a, r=self.factor())
            elif t.kind == ID and t.val.lower() in ('div', 'mod', 'and'):
                self.i += 1
                a = N('bin', op=t.val.lower(), l=a, r=self.factor())
            else:
                break
        return a

    def factor(self):
        t = self.peek()
        if t is None:
            self.error('unexpected end')
        if t.kind == NUM:
            self.i += 1
            return N('num', val=t.val)
        if t.kind == REAL:
            self.i += 1
            return N('real', val=t.val)
        if t.kind == CHAR:
            self.i += 1
            return N('num', val=t.val, char=True)
        if t.kind == POOL:
            self.i += 1
            return N('pool', num=t.val[0], text=t.val[1])
        if t.kind == PSTR:
            self.i += 1
            return N('pstr', text=t.val)
        if t.kind == CHECKSUM:
            self.i += 1
            return N('num', val=0)
        if t.kind == VERB:
            self.i += 1
            return N('verb', text=t.val)
        if t.kind == SYM and t.val == '(':
            self.i += 1
            e = self.expr()
            self.take_sym(')')
            return N('paren', e=e)
        if t.kind == SYM and t.val in ('-', '+'):
            self.i += 1
            f = self.factor()
            return N('un', op='-', e=f) if t.val == '-' else f
        if t.kind == ID:
            w = t.val.lower()
            if w == 'not':
                self.i += 1
                return N('un', op='not', e=self.factor())
            if w in ('true', 'false'):
                self.i += 1
                return N('bool', val=(w == 'true'))
            return self.designator()
        self.error(f'unexpected {t.val!r} in an expression')


def const_value(e):
    """The integer value of a constant expression, or None."""
    k = e.kind
    if k == 'num':
        return e.val
    if k == 'paren':
        return const_value(e.e)
    if k == 'un' and e.op == '-':
        v = const_value(e.e)
        return None if v is None else -v
    if k == 'bin' and e.op in ('+', '-', '*', 'div'):
        a, b = const_value(e.l), const_value(e.r)
        if a is None or b is None:
            return None
        if e.op == '+':
            return a + b
        if e.op == '-':
            return a - b
        if e.op == '*':
            return a * b
        if b == 0:
            return None
        q = abs(a) // abs(b)
        return q if (a >= 0) == (b >= 0) else -q
    return None
