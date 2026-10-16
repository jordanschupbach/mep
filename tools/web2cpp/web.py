"""Reading a WEB program: change files, modules, macros, tangling.

The front half of web2cpp (see web2cpp.py): it does what TANGLE does for
the code -- applies a change file, collects the macro definitions and the
named modules, and expands the unnamed program text into one stream of
Pascal tokens -- but stops short of writing Pascal. The token stream goes
to pascal.py instead.

Only the parts of WEB that tex.web (and change files written in its
style) use are supported; anything else is an error rather than a guess.
"""

import re

# --- Tokens -------------------------------------------------------------------

ID, NUM, REAL, POOL, CHAR, PSTR, SYM, MODREF, VERB, METAOPEN, METACLOSE, CHECKSUM = (
    'id', 'num', 'real', 'pool', 'char', 'pstr', 'sym', 'modref', 'verb', 'metaopen', 'metaclose', 'checksum')


class Tok:
    __slots__ = ('kind', 'val', 'mod', 'noexp')

    def __init__(self, kind, val, mod=0, noexp=False):
        self.kind = kind
        self.val = val
        self.mod = mod  # the module the token came from, for messages and comments
        self.noexp = noexp  # made by @& pasting: TANGLE never expands it again

    def __repr__(self):
        return f'{self.kind}:{self.val!r}'

    def is_sym(self, s):
        return self.kind == SYM and self.val == s

    def is_id(self, s=None):
        return self.kind == ID and (s is None or self.val == s)


class WebError(Exception):
    pass


# --- Change files ---------------------------------------------------------------

def apply_changes(web_lines, change_text, change_name='change file'):
    """Applies a WEB change file (@x ... @y ... @z blocks) the way TIE/TANGLE do:
    each @x block must match a run of consecutive lines of the WEB file at or
    after the end of the previous match (compared with trailing blanks
    stripped), and is replaced by its @y lines."""
    blocks = []
    lines = change_text.split('\n')
    i = 0
    while i < len(lines):
        if lines[i].startswith('@x'):
            j = i + 1
            old = []
            while j < len(lines) and not lines[j].startswith('@y'):
                old.append(lines[j])
                j += 1
            k = j + 1
            new = []
            while k < len(lines) and not lines[k].startswith('@z'):
                new.append(lines[k])
                k += 1
            if j >= len(lines) or k >= len(lines):
                raise WebError(f'{change_name}: unterminated change at line {i + 1}')
            blocks.append((i + 1, old, new))
            i = k + 1
        else:
            i += 1
    out = []
    pos = 0
    strip = lambda s: s.rstrip()
    for (lineno, old, new) in blocks:
        if not old:
            raise WebError(f'{change_name}:{lineno}: empty @x block')
        want = [strip(x) for x in old]
        n = len(want)
        found = -1
        p = pos
        while p + n <= len(web_lines):
            if strip(web_lines[p]) == want[0] and [strip(x) for x in web_lines[p:p + n]] == want:
                found = p
                break
            p += 1
        if found < 0:
            raise WebError(f'{change_name}:{lineno}: change does not match: {old[0]!r}')
        out.extend(web_lines[pos:found])
        out.extend(new)
        pos = found + n
    out.extend(web_lines[pos:])
    return out


# --- Splitting into modules ---------------------------------------------------------

class Module:
    def __init__(self, num, text):
        self.num = num
        self.text = text          # the whole module, starting at its `@ ` / `@*`
        self.title = ''           # a starred module's title (the part names)
        self.defs = []            # [(name, params?, kind '=' | '==', body_text)]
        self.code_name = None     # None for `@p`, else the (possibly abbreviated) module name
        self.code = None          # the code part's text, or None


_START = re.compile(r'(?m)^@[ *\t\n]')


def split_modules(text):
    """Returns (limbo, [Module]) -- modules numbered from 1 as WEAVE numbers them."""
    starts = [m.start() for m in _START.finditer(text)]
    if not starts:
        raise WebError('no modules')
    limbo = text[:starts[0]]
    mods = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else len(text)
        m = Module(i + 1, text[s:e])
        if m.text.startswith('@*'):
            m.title = m.text[2:].split('.', 1)[0].strip()
        _parse_module(m)
        mods.append(m)
    return limbo, mods


def _find_control(text, start, codes):
    """Position of the first `@c` (c in codes) at or after `start` that is not
    `@@`, skipping nothing else (that is how WEAVE/TANGLE scan the TeX part)."""
    i = start
    n = len(text)
    while True:
        j = text.find('@', i)
        if j < 0 or j + 1 >= n:
            return -1
        c = text[j + 1]
        if c == '@':
            i = j + 2
            continue
        if c in codes:
            return j
        i = j + 2


def _parse_module(m):
    t = m.text
    # Where the definition part and the code part begin: the first @d/@f,
    # and the first @p or @<...@>= (a module name followed by `=`).
    i = 2
    defs_at = -1
    code_at = -1
    code_body = -1
    pos = 2
    while True:
        j = _find_control(t, pos, 'dDfFpP<')
        if j < 0:
            break
        c = t[j + 1]
        if c in 'dDfF':
            if defs_at < 0:
                defs_at = j
            pos = j + 2
            # keep scanning for the code part
            continue
        if c in 'pP':
            code_at = j
            code_body = j + 2
            m.code_name = None
            break
        if c == '<':
            k = t.find('@>', j + 2)
            if k < 0:
                raise WebError(f'module {m.num}: unterminated module name')
            after = k + 2
            # `@<name@>=` defines; `@<name@>` alone is a reference (in the TeX
            # part, inside |...|, or in a definition) and is skipped.
            rest = t[after:after + 1]
            if rest == '=':
                code_at = j
                code_body = after + 1
                m.code_name = _norm_name(t[j + 2:k])
                break
            pos = after
            continue
    if defs_at >= 0 and (code_at < 0 or defs_at < code_at):
        defs_text = t[defs_at:code_at if code_at >= 0 else len(t)]
        m.defs = _parse_defs(m, defs_text)
    if code_at >= 0:
        m.code = t[code_body:]


def _norm_name(s):
    return ' '.join(s.split())


_DEF = re.compile(r'@[dD]\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\(#\))?\s*(==|=)')


def _parse_defs(m, text):
    out = []
    # Split at each @d / @f (not @@d).
    parts = []
    i = 0
    starts = []
    while True:
        j = _find_control(text, i, 'dDfF')
        if j < 0:
            break
        starts.append(j)
        i = j + 2
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(text)
        parts.append(text[s:e])
    for p in parts:
        if p[1] in 'fF':
            continue  # format definitions only matter to WEAVE
        md = _DEF.match(p)
        if not md:
            raise WebError(f'module {m.num}: bad definition {p[:60]!r}')
        name, params, kind = md.group(1), bool(md.group(2)), md.group(3)
        out.append((name, params, kind, p[md.end():]))
    return out


# --- Lexing code ------------------------------------------------------------------

_SYMS = [':=', '..', '<=', '>=', '<>', '(', ')', '[', ']', ',', ';', ':', '.', '+', '-', '*', '/', '=', '<', '>', '^', '#']


class Lexer:
    """Turns code (or a macro body) into Tok objects. Strings in double quotes
    become CHAR (one character: its code) or POOL (anything else: its text,
    numbered later); Pascal comments are dropped; WEB formatting codes are
    dropped; @{ @} become METAOPEN/METACLOSE (TANGLE emits them as braces, so
    whatever they enclose is commented out of the program)."""

    def __init__(self, text, mod, pool):
        self.t = text
        self.i = 0
        self.mod = mod
        self.pool = pool

    def error(self, msg):
        raise WebError(f'module {self.mod}: {msg} near {self.t[max(0, self.i - 30):self.i + 30]!r}')

    def tokens(self):
        out = []
        t = self.t
        n = len(t)
        while self.i < n:
            c = t[self.i]
            if c in ' \t\n\r\f':
                self.i += 1
                continue
            if c == '{':
                self._skip_comment()
                continue
            if c == '@':
                tok = self._control()
                if tok is not None:
                    if tok == 'PASTE':
                        out.append(Tok(SYM, '@&', self.mod))
                    else:
                        out.append(tok)
                continue
            if c == '"':
                out.append(self._pool_string())
                continue
            if c == "'":
                out.append(self._pascal_string())
                continue
            if c.isdigit():
                out.append(self._number())
                continue
            if c.isalpha() or c == '_':
                j = self.i + 1
                while j < n and (t[j].isalnum() or t[j] == '_'):
                    j += 1
                out.append(Tok(ID, t[self.i:j], self.mod))
                self.i = j
                continue
            for s in _SYMS:
                if t.startswith(s, self.i):
                    # `..` vs a real number's point is handled in _number.
                    out.append(Tok(SYM, s, self.mod))
                    self.i += len(s)
                    break
            else:
                if c == '}':
                    self.error('unbalanced }')
                if c == '|':
                    self.i += 1  # stray |...| marks in code are formatting
                    continue
                # Anything else (`$` in a compiler directive inside @{...@})
                # is passed on; the parser rejects it if it survives.
                out.append(Tok(SYM, c, self.mod))
                self.i += 1
        return _paste(out)

    def _skip_comment(self):
        t = self.t
        depth = 0
        n = len(t)
        while self.i < n:
            c = t[self.i]
            if c == '\\':
                self.i += 2
                continue
            if c == '@' and self.i + 1 < n and t[self.i + 1] == '@':
                self.i += 2
                continue
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    self.i += 1
                    return
            self.i += 1
        self.error('unterminated comment')

    def _upto_close(self):
        """Text up to the next @> (consumed)."""
        k = self.t.find('@>', self.i)
        if k < 0:
            self.error('missing @>')
        s = self.t[self.i:k]
        self.i = k + 2
        return s

    def _control(self):
        t = self.t
        c = t[self.i + 1] if self.i + 1 < len(t) else ''
        self.i += 2
        if c in '!/|#+;,)?\\':
            return None
        if c == '\n' or c == ' ':
            self.error('module start inside code')
        if c in 'tT^.:':
            self._upto_close()
            return None
        if c == '=':
            return Tok(VERB, self._upto_close(), self.mod)
        if c == '{':
            return Tok(METAOPEN, '{', self.mod)
        if c == '}':
            return Tok(METACLOSE, '}', self.mod)
        if c == '&':
            return 'PASTE'
        if c == '$':
            return Tok(CHECKSUM, 0, self.mod)
        if c == "'":
            j = self.i
            while j < len(t) and t[j] in '01234567':
                j += 1
            v = int(t[self.i:j], 8)
            self.i = j
            return Tok(NUM, v, self.mod)
        if c == '"':
            j = self.i
            while j < len(t) and t[j] in '0123456789ABCDEFabcdef':
                j += 1
            v = int(t[self.i:j], 16)
            self.i = j
            return Tok(NUM, v, self.mod)
        if c == '<':
            name = _norm_name(self._upto_close())
            return Tok(MODREF, name, self.mod)
        if c == '@':
            self.error('@@ outside a string')
        self.error(f'unsupported control code @{c}')

    def _pool_string(self):
        t = self.t
        j = self.i + 1
        buf = []
        while True:
            if j >= len(t):
                self.error('unterminated string')
            c = t[j]
            if c == '"':
                if j + 1 < len(t) and t[j + 1] == '"':
                    buf.append('"')
                    j += 2
                    continue
                j += 1
                break
            if c == '@' and j + 1 < len(t) and t[j + 1] == '@':
                buf.append('@')
                j += 2
                continue
            if c == '\n':
                self.error('newline in string')
            buf.append(c)
            j += 1
        self.i = j
        s = ''.join(buf)
        if len(s) == 1:
            return Tok(CHAR, ord(s), self.mod)
        return Tok(POOL, s, self.mod)

    def _pascal_string(self):
        t = self.t
        j = self.i + 1
        buf = []
        while True:
            if j >= len(t):
                self.error('unterminated Pascal string')
            c = t[j]
            if c == "'":
                if j + 1 < len(t) and t[j + 1] == "'":
                    buf.append("'")
                    j += 2
                    continue
                j += 1
                break
            if c == '@' and j + 1 < len(t) and t[j + 1] == '@':
                buf.append('@')
                j += 2
                continue
            buf.append(c)
            j += 1
        self.i = j
        return Tok(PSTR, ''.join(buf), self.mod)

    def _number(self):
        t = self.t
        j = self.i
        while j < len(t) and t[j].isdigit():
            j += 1
        # A real: digits '.' digits (but not `..`), optional exponent.
        if j < len(t) and t[j] == '.' and j + 1 < len(t) and t[j + 1].isdigit():
            k = j + 1
            while k < len(t) and t[k].isdigit():
                k += 1
            if k < len(t) and t[k] in 'eE':
                k += 1
                if t[k] in '+-':
                    k += 1
                while k < len(t) and t[k].isdigit():
                    k += 1
            s = t[self.i:k]
            self.i = k
            return Tok(REAL, s, self.mod)
        v = int(t[self.i:j])
        self.i = j
        return Tok(NUM, v, self.mod)


def _paste(toks):
    """Applies @& (token pasting: `t@&y@&p@&e` is the identifier `type`)."""
    out = []
    i = 0
    while i < len(toks):
        if toks[i].is_sym('@&'):
            if not out or i + 1 >= len(toks):
                raise WebError('@& at an edge')
            a, b = out.pop(), toks[i + 1]
            out.append(Tok(ID if a.kind == ID else a.kind, str(a.val) + str(b.val), a.mod, True))
            i += 2
            continue
        out.append(toks[i])
        i += 1
    return out


# --- The program ------------------------------------------------------------------

class Web:
    """A WEB program read and ready to tangle."""

    def __init__(self, text):
        self.limbo, self.modules = split_modules(text)
        self.pool = {}        # string text -> pool number
        self.pool_list = []   # pool numbers 256.. in order
        self.macros = {}      # name -> (params?, kind, [Tok])
        self.named = {}       # full module name -> [Tok] (all its definitions, concatenated)
        self.unnamed = []     # the @p code, in order
        self._abbrev = {}
        self._collect()

    def pool_number(self, s):
        if s not in self.pool:
            self.pool[s] = 256 + len(self.pool_list)
            self.pool_list.append(s)
        return self.pool[s]

    def _lex(self, text, mod):
        return Lexer(text, mod, self).tokens()

    def _collect(self):
        names = []
        for m in self.modules:
            if m.code is not None and m.code_name is not None:
                names.append((m.code_name, m))
        # Full names are those not ending in `...`, wherever they appear: a
        # module is often defined under an abbreviation and named in full
        # only where it is used.
        full = set(n for n, _ in names if not n.endswith('...'))
        for m in self.modules:
            for nm in re.findall(r'@<(.*?)@>', m.text, re.S):
                nm = _norm_name(nm)
                if not nm.endswith('...'):
                    full.add(nm)
        self._full = sorted(full)
        for m in self.modules:
            for (name, params, kind, body) in m.defs:
                toks = self._lex(body, m.num)
                if name in self.macros:
                    raise WebError(f'module {m.num}: macro {name} defined twice')
                self.macros[name] = (params, kind, toks)
            if m.code is None:
                continue
            toks = self._lex(m.code, m.num)
            if m.code_name is None:
                self.unnamed.extend(toks)
            else:
                try:
                    full_name = self.resolve(m.code_name, m.num)
                except WebError:
                    # A module a change file has made unreachable (its full
                    # name no longer appears anywhere): never expanded, so
                    # it is kept under the abbreviation it was defined by.
                    full_name = m.code_name
                self.named.setdefault(full_name, []).extend(toks)
        # Pool numbers in order of appearance, as TANGLE assigns them.
        for m in self.modules:
            pass

    def resolve(self, name, mod):
        if name in self._abbrev:
            return self._abbrev[name]
        if name.endswith('...'):
            pre = name[:-3]
            hits = [f for f in self._full if f.startswith(pre)]
            if len(hits) != 1:
                raise WebError(f'module {mod}: name {name!r} matches {len(hits)} modules')
            self._abbrev[name] = hits[0]
            return hits[0]
        if name not in self._full:
            raise WebError(f'module {mod}: undefined module name {name!r}')
        return name

    # -- Tangling --------------------------------------------------------------

    def tangle(self):
        """The whole program as one token list: module references expanded,
        macros expanded, @{...@} sections removed, pool strings numbered."""
        toks = self._expand_modules(self.unnamed, set())
        toks = self._expand_macros(toks, 0)
        toks = self._drop_meta(toks)
        toks = _join_reals(toks)
        out = []
        for t in toks:
            if t.kind == POOL:
                # (A module used in two places shares its token objects, so
                # the numbered string is a new token, not an edit.)
                t = Tok(POOL, (self.pool_number(t.val), t.val), t.mod)
            out.append(t)
        return out

    def _expand_modules(self, toks, active):
        out = []
        for t in toks:
            if t.kind == MODREF:
                full = self.resolve(t.val, t.mod)
                if full in active:
                    raise WebError(f'module {full!r} refers to itself')
                body = self.named.get(full)
                if body is None:
                    raise WebError(f'module {full!r} has no code')
                out.extend(self._expand_modules(body, active | {full}))
            else:
                out.append(t)
        return out

    def _expand_macros(self, toks, depth):
        if depth > 60:
            raise WebError('macro expansion too deep')
        out = []
        i = 0
        changed = False
        n = len(toks)
        while i < n:
            t = toks[i]
            if t.kind == ID and not t.noexp and t.val in self.macros:
                params, kind, body = self.macros[t.val]
                if params:
                    if i + 1 >= n or not toks[i + 1].is_sym('('):
                        raise WebError(f'module {t.mod}: macro {t.val} needs an argument')
                    j = i + 2
                    level = 1
                    while j < n:
                        if toks[j].is_sym('('):
                            level += 1
                        elif toks[j].is_sym(')'):
                            level -= 1
                            if level == 0:
                                break
                        j += 1
                    arg = toks[i + 2:j]
                    rep = []
                    for b in body:
                        if b.kind == SYM and b.val == '#':
                            rep.extend(arg)
                        else:
                            rep.append(b)
                    i = j + 1
                else:
                    rep = list(body)
                    i += 1
                if kind == '=':
                    rep = [Tok(SYM, '(', t.mod)] + rep + [Tok(SYM, ')', t.mod)]
                out.extend(Tok(r.kind, r.val, t.mod, r.noexp) for r in rep)
                changed = True
                continue
            if t.kind == MODREF:
                out.extend(self._expand_modules([t], set()))
                changed = True
                i += 1
                continue
            out.append(t)
            i += 1
        if changed:
            return self._expand_macros(out, depth + 1)
        return out

    def _drop_meta(self, toks):
        out = []
        depth = 0
        for t in toks:
            if t.kind == METAOPEN:
                depth += 1
                continue
            if t.kind == METACLOSE:
                depth -= 1
                if depth < 0:
                    raise WebError(f'module {t.mod}: unbalanced @}}')
                continue
            if depth == 0:
                out.append(t)
        return out


def _join_reals(toks):
    """`float_constant(0)` tangles to the tokens `0 . 0`; TANGLE writes them
    side by side, which Pascal reads as the real 0.0."""
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if (t.kind == NUM and i + 2 < len(toks) and toks[i + 1].is_sym('.') and toks[i + 2].kind == NUM):
            out.append(Tok(REAL, f'{t.val}.{toks[i + 2].val}', t.mod))
            i += 3
            continue
        out.append(t)
        i += 1
    return out
