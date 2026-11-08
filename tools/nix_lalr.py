#!/usr/bin/env python3
"""Generate src/nix_parse_tables.h: the LALR(1) tables mep-nix-lsp's parser
(src/nix_parse.cpp) runs on.

    python3 tools/nix_lalr.py            # rewrite src/nix_parse_tables.h
    python3 tools/nix_lalr.py --report   # also print the states

Why tables and not a hand-written parser: Nix's own parser is a bison
LALR(1) parser with `parse.error verbose`, so its syntax errors read
"syntax error, unexpected X, expecting A or B". Which tokens are listed,
and where the error is noticed at all, follow from bison's tables -- its
default reductions in particular. Building the same automaton the same way
gives mep the same messages at the same places, which a recursive-descent
parser could only approximate.

The grammar below is the Nix language's (as Nix 2.31 accepts it), written
out here as data. What has to agree with bison for the messages to agree:

  - token numbers: $end, error, $undefined, then every token in order of
    first declaration -- named tokens, then those introduced by the
    precedence lines, then character tokens in order of first use in the
    rules ("expecting" lists tokens in that order);
  - rule order (reduce/reduce ties and default-reduction ties go to the
    earliest rule);
  - precedence: a rule's is its %prec or its last token's; shift/reduce
    conflicts are settled by bison's rules, %nonassoc making an explicit
    error entry;
  - LALR(1) lookaheads; a state's default reduction is its most common
    reduction (bison's lr.default-reduction=most), which replaces the error
    entries too, so errors are only noticed in states without one;
  - a state with no explicit entries needs no lookahead (bison skips
    reading a token there -- which decides whether a lexer error or a
    reduction's error comes first);
  - the "expecting" list: a state's explicit, non-error entries, in token
    order, and none at all when there are more than four.
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'src' / 'nix_parse_tables.h'

# (symbol, display name in messages). Order = bison's token numbers.
TOKENS = [
    ('$end', 'end of file'), ('error', 'error'), ('$undefined', 'invalid token'),
    ('ID', 'ID'), ('STR', 'STR'), ('IND_STR', 'IND_STR'), ('INT_LIT', 'INT_LIT'), ('FLOAT_LIT', 'FLOAT_LIT'),
    ('PATH', 'PATH'), ('HPATH', 'HPATH'), ('SPATH', 'SPATH'), ('PATH_END', 'PATH_END'), ('URI', 'URI'),
    ('IF', 'IF'), ('THEN', 'THEN'), ('ELSE', 'ELSE'), ('ASSERT', 'ASSERT'), ('WITH', 'WITH'), ('LET', 'LET'),
    ('IN_KW', 'IN_KW'), ('REC', 'REC'), ('INHERIT', 'INHERIT'), ('EQ', 'EQ'), ('NEQ', 'NEQ'), ('AND', 'AND'),
    ('OR', 'OR'), ('IMPL', 'IMPL'), ('OR_KW', 'OR_KW'), ('PIPE_FROM', 'PIPE_FROM'), ('PIPE_INTO', 'PIPE_INTO'),
    ('DOLLAR_CURLY', 'DOLLAR_CURLY'), ('IND_STRING_OPEN', 'IND_STRING_OPEN'),
    ('IND_STRING_CLOSE', 'IND_STRING_CLOSE'), ('ELLIPSIS', 'ELLIPSIS'),
    # introduced by the precedence declarations
    ("'<'", "'<'"), ("'>'", "'>'"), ('LEQ', 'LEQ'), ('GEQ', 'GEQ'), ('UPDATE', 'UPDATE'), ('NOT', 'NOT'),
    ("'+'", "'+'"), ("'-'", "'-'"), ("'*'", "'*'"), ("'/'", "'/'"), ('CONCAT', 'CONCAT'), ("'?'", "'?'"),
    ('NEGATE', 'NEGATE'),
    # character tokens, in order of first use in the rules
    ("':'", "':'"), ("'@'", "'@'"), ("';'", "';'"), ("'!'", "'!'"), ("'.'", "'.'"), ("'\"'", "'\"'"),
    ("'('", "'('"), ("')'", "')'"), ("'{'", "'{'"), ("'}'", "'}'"), ("'['", "'['"), ("']'", "']'"),
    ("'='", "'='"), ("','", "','"),
]

# Lowest first: (associativity, tokens).
PRECEDENCE = [
    ('right', ['IMPL']),
    ('left', ['OR']),
    ('left', ['AND']),
    ('nonassoc', ['EQ', 'NEQ']),
    ('nonassoc', ["'<'", "'>'", 'LEQ', 'GEQ']),
    ('right', ['UPDATE']),
    ('left', ['NOT']),
    ('left', ["'+'", "'-'"]),
    ('left', ["'*'", "'/'"]),
    ('right', ['CONCAT']),
    ('nonassoc', ["'?'"]),
    ('nonassoc', ['NEGATE']),
]

# (C++ name, lhs, rhs, %prec). Order = bison's rule numbers (after rule 0,
# $accept: start $end). The C++ name is what the parser's reduce switch uses.
RULES = [
    ('Start', 'start', ['expr'], None),
    ('Expr', 'expr', ['expr_function'], None),
    ('LambdaId', 'expr_function', ['ID', "':'", 'expr_function'], None),
    ('LambdaFormals', 'expr_function', ['formal_set', "':'", 'expr_function'], None),
    ('LambdaFormalsAt', 'expr_function', ['formal_set', "'@'", 'ID', "':'", 'expr_function'], None),
    ('LambdaAtFormals', 'expr_function', ['ID', "'@'", 'formal_set', "':'", 'expr_function'], None),
    ('Assert', 'expr_function', ['ASSERT', 'expr', "';'", 'expr_function'], None),
    ('With', 'expr_function', ['WITH', 'expr', "';'", 'expr_function'], None),
    ('Let', 'expr_function', ['LET', 'binds', 'IN_KW', 'expr_function'], None),
    ('FunctionIf', 'expr_function', ['expr_if'], None),
    ('If', 'expr_if', ['IF', 'expr', 'THEN', 'expr', 'ELSE', 'expr'], None),
    ('IfPipeFrom', 'expr_if', ['expr_pipe_from'], None),
    ('IfPipeInto', 'expr_if', ['expr_pipe_into'], None),
    ('IfOp', 'expr_if', ['expr_op'], None),
    ('PipeFromChain', 'expr_pipe_from', ['expr_op', 'PIPE_FROM', 'expr_pipe_from'], None),
    ('PipeFrom', 'expr_pipe_from', ['expr_op', 'PIPE_FROM', 'expr_op'], None),
    ('PipeIntoChain', 'expr_pipe_into', ['expr_pipe_into', 'PIPE_INTO', 'expr_op'], None),
    ('PipeInto', 'expr_pipe_into', ['expr_op', 'PIPE_INTO', 'expr_op'], None),
    ('Not', 'expr_op', ["'!'", 'expr_op'], 'NOT'),
    ('Negate', 'expr_op', ["'-'", 'expr_op'], 'NEGATE'),
    ('OpEq', 'expr_op', ['expr_op', 'EQ', 'expr_op'], None),
    ('OpNeq', 'expr_op', ['expr_op', 'NEQ', 'expr_op'], None),
    ('OpLt', 'expr_op', ['expr_op', "'<'", 'expr_op'], None),
    ('OpLeq', 'expr_op', ['expr_op', 'LEQ', 'expr_op'], None),
    ('OpGt', 'expr_op', ['expr_op', "'>'", 'expr_op'], None),
    ('OpGeq', 'expr_op', ['expr_op', 'GEQ', 'expr_op'], None),
    ('OpAnd', 'expr_op', ['expr_op', 'AND', 'expr_op'], None),
    ('OpOr', 'expr_op', ['expr_op', 'OR', 'expr_op'], None),
    ('OpImpl', 'expr_op', ['expr_op', 'IMPL', 'expr_op'], None),
    ('OpUpdate', 'expr_op', ['expr_op', 'UPDATE', 'expr_op'], None),
    ('OpHasAttr', 'expr_op', ['expr_op', "'?'", 'attrpath'], None),
    ('OpAdd', 'expr_op', ['expr_op', "'+'", 'expr_op'], None),
    ('OpSub', 'expr_op', ['expr_op', "'-'", 'expr_op'], None),
    ('OpMul', 'expr_op', ['expr_op', "'*'", 'expr_op'], None),
    ('OpDiv', 'expr_op', ['expr_op', "'/'", 'expr_op'], None),
    ('OpConcat', 'expr_op', ['expr_op', 'CONCAT', 'expr_op'], None),
    ('OpApp', 'expr_op', ['expr_app'], None),
    ('App', 'expr_app', ['expr_app', 'expr_select'], None),
    ('AppSelect', 'expr_app', ['expr_select'], None),
    ('Select', 'expr_select', ['expr_simple', "'.'", 'attrpath'], None),
    ('SelectOr', 'expr_select', ['expr_simple', "'.'", 'attrpath', 'OR_KW', 'expr_select'], None),
    ('CursedOr', 'expr_select', ['expr_simple', 'OR_KW'], None),
    ('SelectSimple', 'expr_select', ['expr_simple'], None),
    ('Var', 'expr_simple', ['ID'], None),
    ('Int', 'expr_simple', ['INT_LIT'], None),
    ('Float', 'expr_simple', ['FLOAT_LIT'], None),
    ('String', 'expr_simple', ["'\"'", 'string_parts', "'\"'"], None),
    ('IndString', 'expr_simple', ['IND_STRING_OPEN', 'ind_string_parts', 'IND_STRING_CLOSE'], None),
    ('Path', 'expr_simple', ['path_start', 'PATH_END'], None),
    ('PathInterp', 'expr_simple', ['path_start', 'string_parts_interpolated', 'PATH_END'], None),
    ('SPath', 'expr_simple', ['SPATH'], None),
    ('Uri', 'expr_simple', ['URI'], None),
    ('Paren', 'expr_simple', ["'('", 'expr', "')'"], None),
    ('OldLet', 'expr_simple', ['LET', "'{'", 'binds', "'}'"], None),
    ('RecSet', 'expr_simple', ['REC', "'{'", 'binds', "'}'"], None),
    ('Set', 'expr_simple', ["'{'", 'binds1', "'}'"], None),
    ('EmptySet', 'expr_simple', ["'{'", "'}'"], None),
    ('List', 'expr_simple', ["'['", 'expr_list', "']'"], None),
    ('StrPartsStr', 'string_parts', ['STR'], None),
    ('StrPartsInterp', 'string_parts', ['string_parts_interpolated'], None),
    ('StrPartsEmpty', 'string_parts', [], None),
    ('InterpStr', 'string_parts_interpolated', ['string_parts_interpolated', 'STR'], None),
    ('InterpExpr', 'string_parts_interpolated', ['string_parts_interpolated', 'DOLLAR_CURLY', 'expr', "'}'"],
     None),
    ('InterpFirstExpr', 'string_parts_interpolated', ['DOLLAR_CURLY', 'expr', "'}'"], None),
    ('InterpStrExpr', 'string_parts_interpolated', ['STR', 'DOLLAR_CURLY', 'expr', "'}'"], None),
    ('PathStart', 'path_start', ['PATH'], None),
    ('HPathStart', 'path_start', ['HPATH'], None),
    ('IndPartsStr', 'ind_string_parts', ['ind_string_parts', 'IND_STR'], None),
    ('IndPartsExpr', 'ind_string_parts', ['ind_string_parts', 'DOLLAR_CURLY', 'expr', "'}'"], None),
    ('IndPartsEmpty', 'ind_string_parts', [], None),
    ('Binds', 'binds', ['binds1'], None),
    ('BindsEmpty', 'binds', [], None),
    ('BindAssign', 'binds1', ['binds1', 'attrpath', "'='", 'expr', "';'"], None),
    ('BindInherit', 'binds1', ['binds', 'INHERIT', 'attrs', "';'"], None),
    ('BindInheritFrom', 'binds1', ['binds', 'INHERIT', "'('", 'expr', "')'", 'attrs', "';'"], None),
    ('BindFirst', 'binds1', ['attrpath', "'='", 'expr', "';'"], None),
    ('AttrsAttr', 'attrs', ['attrs', 'attr'], None),
    ('AttrsString', 'attrs', ['attrs', 'string_attr'], None),
    ('AttrsEmpty', 'attrs', [], None),
    ('PathAttr', 'attrpath', ['attrpath', "'.'", 'attr'], None),
    ('PathString', 'attrpath', ['attrpath', "'.'", 'string_attr'], None),
    ('PathFirstAttr', 'attrpath', ['attr'], None),
    ('PathFirstString', 'attrpath', ['string_attr'], None),
    ('AttrId', 'attr', ['ID'], None),
    ('AttrOr', 'attr', ['OR_KW'], None),
    ('StringAttr', 'string_attr', ["'\"'", 'string_parts', "'\"'"], None),
    ('DynamicAttr', 'string_attr', ['DOLLAR_CURLY', 'expr', "'}'"], None),
    ('ListElem', 'expr_list', ['expr_list', 'expr_select'], None),
    ('ListEmpty', 'expr_list', [], None),
    ('FormalsEllipsis', 'formal_set', ["'{'", 'formals', "','", 'ELLIPSIS', "'}'"], None),
    ('OnlyEllipsis', 'formal_set', ["'{'", 'ELLIPSIS', "'}'"], None),
    ('FormalsComma', 'formal_set', ["'{'", 'formals', "','", "'}'"], None),
    ('FormalsPlain', 'formal_set', ["'{'", 'formals', "'}'"], None),
    ('FormalsEmpty', 'formal_set', ["'{'", "'}'"], None),
    ('FormalsMore', 'formals', ['formals', "','", 'formal'], None),
    ('FormalsOne', 'formals', ['formal'], None),
    ('Formal', 'formal', ['ID'], None),
    ('FormalDefault', 'formal', ['ID', "'?'", 'expr'], None),
]

START = 'start'


class Grammar:
    def __init__(self):
        self.tokens = [t for t, _ in TOKENS]
        self.tok_index = {t: i for i, t in enumerate(self.tokens)}
        self.nonterms = ['$accept']
        for _, lhs, _, _ in RULES:
            if lhs not in self.nonterms:
                self.nonterms.append(lhs)
        self.nt_index = {n: i for i, n in enumerate(self.nonterms)}
        for _, _, rhs, _ in RULES:
            for s in rhs:
                if s not in self.tok_index and s not in self.nt_index:
                    sys.exit(f'nix_lalr: unknown symbol {s}')
        # rule 0: $accept -> start $end
        self.rules = [('Accept', '$accept', [START, '$end'], None)] + RULES
        self.prec = {}
        for level, (assoc, toks) in enumerate(PRECEDENCE, 1):
            for t in toks:
                self.prec[t] = (level, assoc)
        self.rule_prec = []
        for _, _, rhs, pr in self.rules:
            if pr:
                self.rule_prec.append(self.prec[pr])
            else:
                last = [s for s in rhs if s in self.tok_index]
                self.rule_prec.append(self.prec.get(last[-1]) if last else None)
        self.compute_first()

    def is_token(self, s):
        return s in self.tok_index

    def compute_first(self):
        self.nullable = set()
        self.first = defaultdict(set)
        changed = True
        while changed:
            changed = False
            for _, lhs, rhs, _ in self.rules:
                if lhs not in self.nullable and all(s in self.nullable for s in rhs):
                    self.nullable.add(lhs)
                    changed = True
                for s in rhs:
                    add = {s} if self.is_token(s) else self.first[s]
                    if not add <= self.first[lhs]:
                        self.first[lhs] |= add
                        changed = True
                    if s not in self.nullable:
                        break

    def first_of(self, seq, la):
        out = set()
        for s in seq:
            if self.is_token(s):
                out.add(s)
                return out
            out |= self.first[s]
            if s not in self.nullable:
                return out
        out.add(la)
        return out


def lr0(g):
    """The LR(0) automaton: kernels (tuples of (rule, dot)) and transitions."""
    by_lhs = defaultdict(list)
    for r, (_, lhs, _, _) in enumerate(g.rules):
        by_lhs[lhs].append(r)

    def closure(items):
        out = list(items)
        seen = set(items)
        for rule, dot in out:
            rhs = g.rules[rule][2]
            if dot < len(rhs) and not g.is_token(rhs[dot]):
                for r in by_lhs[rhs[dot]]:
                    if (r, 0) not in seen:
                        seen.add((r, 0))
                        out.append((r, 0))
        return out

    start = ((0, 0),)
    states = [start]
    index = {start: 0}
    trans = []
    k = 0
    while k < len(states):
        items = closure(states[k])
        moves = {}
        order = []
        for rule, dot in items:
            rhs = g.rules[rule][2]
            if dot < len(rhs):
                s = rhs[dot]
                if s not in moves:
                    moves[s] = []
                    order.append(s)
                moves[s].append((rule, dot + 1))
        t = {}
        for s in order:
            kern = tuple(sorted(moves[s]))
            if kern not in index:
                index[kern] = len(states)
                states.append(kern)
            t[s] = index[kern]
        trans.append(t)
        k += 1
    return states, trans, closure, by_lhs


def lalr_lookaheads(g, states, trans, by_lhs):
    """LALR(1) lookaheads of every kernel item: spontaneous generation + propagation."""
    la = [{item: set() for item in kern} for kern in states]
    la[0][(0, 0)].add('#END')  # never used: rule 0 shifts $end explicitly
    prop = defaultdict(list)
    DUMMY = '#'

    def lr1_closure(item, look):
        out = {(item, look)}
        work = [(item, look)]
        while work:
            (rule, dot), a = work.pop()
            rhs = g.rules[rule][2]
            if dot < len(rhs) and not g.is_token(rhs[dot]):
                for b in g.first_of(rhs[dot + 1:], a):
                    for r in by_lhs[rhs[dot]]:
                        x = ((r, 0), b)
                        if x not in out:
                            out.add(x)
                            work.append(x)
        return out

    for si, kern in enumerate(states):
        for item in kern:
            for (rule, dot), a in lr1_closure(item, DUMMY):
                rhs = g.rules[rule][2]
                if dot >= len(rhs):
                    continue
                target = trans[si][rhs[dot]]
                titem = (rule, dot + 1)
                if a == DUMMY:
                    prop[(si, item)].append((target, titem))
                else:
                    la[target][titem].add(a)
    changed = True
    while changed:
        changed = False
        for (si, item), targets in prop.items():
            src = la[si][item]
            for ti, titem in targets:
                if not src <= la[ti][titem]:
                    la[ti][titem] |= src
                    changed = True
    return la, lr1_closure


def build(g):
    states, trans, closure, by_lhs = lr0(g)
    la, lr1_closure = lalr_lookaheads(g, states, trans, by_lhs)
    ntok = len(g.tokens)
    ERR = 'err'
    actions, defaults, conflicts = [], [], []
    for si, kern in enumerate(states):
        # Reductions: items with the dot at the end, closure items included
        # (empty rules), with their LALR lookaheads.
        reds = defaultdict(set)
        for item in kern:
            for (rule, dot), a in lr1_closure(item, '#'):
                if dot == len(g.rules[rule][2]):
                    if a == '#':
                        reds[rule] |= la[si][item]
                    else:
                        reds[rule].add(a)
        row = [None] * ntok
        for s, t in trans[si].items():
            if g.is_token(s):
                row[g.tok_index[s]] = ('s', t)
        for rule in sorted(reds):
            for a in reds[rule]:
                if a == '#END':
                    continue
                ti = g.tok_index[a]
                cur = row[ti]
                if cur is None:
                    row[ti] = ('r', rule)
                elif cur[0] == 's':
                    rp, tp = g.rule_prec[rule], g.prec.get(a)
                    if rp is None or tp is None:
                        conflicts.append(f'state {si}: shift/reduce on {a} (rule {rule})')
                        continue  # bison: shift
                    if rp[0] > tp[0]:
                        row[ti] = ('r', rule)
                    elif rp[0] < tp[0]:
                        pass
                    elif tp[1] == 'left':
                        row[ti] = ('r', rule)
                    elif tp[1] == 'right':
                        pass
                    else:
                        row[ti] = (ERR,)
                elif cur[0] == 'r':
                    conflicts.append(f'state {si}: reduce/reduce on {a} (rules {cur[1]}, {rule})')
                    if rule < cur[1]:
                        row[ti] = ('r', rule)
        # Default reduction: the most common one (ties: the earliest rule).
        default = -1
        best = 0
        for rule in sorted(reds):
            n = sum(1 for x in row if x == ('r', rule))
            if n > best:
                best, default = n, rule
        if default >= 0:
            row = [None if x == ('r', default) else x for x in row]
        actions.append(row)
        defaults.append(default)
    return states, trans, actions, defaults, conflicts


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit(g, states, trans, actions, defaults):
    ntok, nnt = len(g.tokens), len(g.nonterms)
    final = trans[trans[0][START]]['$end']
    out = []
    w = out.append
    w('// Generated by tools/nix_lalr.py -- do not edit. The LALR(1) tables of the')
    w("// Nix grammar, built the way bison builds Nix's own parser (see that script).")
    w('#pragma once')
    w('')
    w('#include <cstdint>')
    w('')
    w('namespace nixparse {')
    w('')
    w(f'constexpr int kNumTokens = {ntok};')
    w(f'constexpr int kNumNonterms = {nnt};')
    w(f'constexpr int kNumStates = {len(states)};')
    w(f'constexpr int kNumRules = {len(g.rules)};')
    w(f'constexpr int kFinalState = {final};')
    w('')
    w('enum Tok : int {')
    for i, t in enumerate(g.tokens):
        name = {'$end': 'T_END', 'error': 'T_ERROR', '$undefined': 'T_UNDEF'}.get(t)
        if not name:
            if t.startswith("'"):
                c = t[1:-1]
                name = 'T_' + {':': 'COLON', '@': 'AT', ';': 'SEMI', '!': 'BANG', '.': 'DOT', '"': 'QUOTE',
                               '(': 'LPAREN', ')': 'RPAREN', '{': 'LBRACE', '}': 'RBRACE', '[': 'LBRACK',
                               ']': 'RBRACK', '=': 'ASSIGN', ',': 'COMMA', '<': 'LT', '>': 'GT', '+': 'PLUS',
                               '-': 'MINUS', '*': 'STAR', '/': 'SLASH', '?': 'QUESTION'}[c]
            else:
                name = 'T_' + t
        w(f'    {name} = {i},')
    w('};')
    w('')
    w('// How bison names each token in "unexpected X, expecting Y".')
    w('inline const char *const kTokenNames[kNumTokens] = {')
    w('    ' + ', '.join(cstr(n) for _, n in TOKENS) + ',')
    w('};')
    w('')
    w('enum Rule : int {')
    for i, (name, _, _, _) in enumerate(g.rules):
        w(f'    R_{name} = {i},')
    w('};')
    w('')
    w('inline const int16_t kRuleLhs[kNumRules] = {')
    w('    ' + ', '.join(str(g.nt_index[lhs]) for _, lhs, _, _ in g.rules) + ',')
    w('};')
    w('inline const int8_t kRuleLen[kNumRules] = {')
    w('    ' + ', '.join(str(len(rhs)) for _, _, rhs, _ in g.rules) + ',')
    w('};')
    w('')
    w('// kAction[state][token]: 0 = the default action (kDefaultReduce, or an')
    w('// error when the state has none), > 0 = shift to state n - 1,')
    w('// < 0 = reduce by rule -n - 1, kExplicitError = a %nonassoc error.')
    w('constexpr int16_t kExplicitError = -32768;')
    w('inline const int16_t kAction[kNumStates][kNumTokens] = {')
    for si, row in enumerate(actions):
        cells = []
        for x in row:
            if x is None:
                cells.append('0')
            elif x[0] == 's':
                cells.append(str(x[1] + 1))
            elif x[0] == 'r':
                cells.append(str(-x[1] - 1))
            else:
                cells.append('kExplicitError')
        w('    {' + ','.join(cells) + '},')
    w('};')
    w('inline const int16_t kDefaultReduce[kNumStates] = {')
    for k in range(0, len(defaults), 24):
        w('    ' + ', '.join(str(d) for d in defaults[k:k + 24]) + ',')
    w('};')
    w('// kGoto[state][nonterminal]: the state after reducing to it, -1 = none.')
    w('inline const int16_t kGoto[kNumStates][kNumNonterms] = {')
    for si in range(len(states)):
        cells = [str(trans[si].get(n, -1)) for n in g.nonterms]
        w('    {' + ','.join(cells) + '},')
    w('};')
    w('')
    w('}  // namespace nixparse')
    w('')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--check', action='store_true', help='exit 1 if the checked-in tables are stale')
    a = ap.parse_args()
    g = Grammar()
    states, trans, actions, defaults, conflicts = build(g)
    if conflicts:
        print('\n'.join(conflicts), file=sys.stderr)
        sys.exit('nix_lalr: unresolved conflicts (Nix declares %expect 0)')
    text = emit(g, states, trans, actions, defaults)
    if a.report:
        for si, kern in enumerate(states):
            print(f'state {si}:')
            for rule, dot in kern:
                _, lhs, rhs, _ = g.rules[rule]
                print(f'    {lhs} -> {" ".join(rhs[:dot] + ["."] + rhs[dot:])}')
            row = actions[si]
            for t, x in zip(g.tokens, row):
                if x:
                    print(f'      {t}: {x}')
            if defaults[si] >= 0:
                print(f'      default: reduce {defaults[si]}')
    if a.check:
        sys.exit(0 if OUT.exists() and OUT.read_text() == text else 1)
    OUT.write_text(text)
    print(f'{len(states)} states, {len(g.rules)} rules -> {OUT.relative_to(REPO)}', file=sys.stderr)


if __name__ == '__main__':
    main()
