#!/usr/bin/env python3
"""Regenerates src/c_lsp_std_names.cpp: what the C standard library and
the common POSIX headers actually declare, harvested from the real
headers rather than remembered.

mep's own C language server (src/c_lsp.cpp) checks an unresolved
identifier against this list before it dares report it as undeclared,
and offers the same list -- with the signatures reconstructed here -- as
completions and hover text. Writing that from memory is how a server
ends up insisting that `strlcpy` does not exist and that `memccpy`
takes three arguments, so it is not written from memory.

Run from the repository root, in an environment with a C compiler:

    nix develop --command python3 scripts/gen_c_names.py

The generated file is committed, so building mep never needs a compiler
with headers to hand. Re-run this only to refresh the list; the output
is sorted (byte order, LC_COLLATE=C -- the array is binary-searched by
byte comparison), so the diff stays readable.
"""

import os
import re
import subprocess
import sys

# The headers whose contents this server claims to know. The C11/C17
# standard set first, then the POSIX and Linux headers that any real C
# file includes. A header absent from here is one this server treats as
# opaque: including it switches the undeclared-name check off for the
# whole file, which is the honest answer when the declarations are out
# of reach (see c_lsp.h).
# Most primitive first, because a name is credited to the first header
# here that declares it and `size_t`, `NULL` and `offsetof` belong to
# <stddef.h> however many other headers drag them in.
STANDARD = [
    "stddef.h", "stdarg.h", "stdint.h", "stdbool.h", "stdalign.h", "stdnoreturn.h", "iso646.h",
    "limits.h", "float.h", "errno.h", "assert.h", "ctype.h", "string.h", "stdio.h", "stdlib.h",
    "math.h", "time.h", "signal.h", "setjmp.h", "locale.h", "inttypes.h", "wchar.h", "wctype.h",
    "uchar.h", "complex.h", "fenv.h", "stdatomic.h", "tgmath.h", "threads.h",
]
POSIX = [
    "aio.h", "alloca.h", "arpa/inet.h", "byteswap.h", "cpio.h", "dirent.h", "dlfcn.h", "endian.h",
    "err.h", "error.h", "fcntl.h", "fnmatch.h", "ftw.h", "getopt.h", "glob.h", "grp.h", "iconv.h",
    "langinfo.h", "libgen.h", "malloc.h", "memory.h", "mqueue.h", "net/if.h", "netdb.h",
    "netinet/in.h", "netinet/tcp.h", "nl_types.h", "poll.h", "pthread.h", "pwd.h", "regex.h",
    "sched.h", "search.h", "semaphore.h", "spawn.h", "strings.h", "sys/epoll.h", "sys/eventfd.h",
    "sys/file.h", "sys/ioctl.h", "sys/mman.h", "sys/param.h", "sys/resource.h", "sys/select.h",
    "sys/sendfile.h", "sys/socket.h", "sys/stat.h", "sys/statvfs.h", "sys/syscall.h", "sys/time.h",
    "sys/times.h", "sys/types.h", "sys/uio.h", "sys/un.h", "sys/utsname.h", "sys/wait.h",
    "syslog.h", "termios.h", "ucontext.h", "unistd.h", "utime.h", "utmp.h", "wordexp.h",
]
HEADERS = STANDARD + POSIX

CC = os.environ.get("CC", "gcc")

TOKEN = re.compile(r"[A-Za-z_][A-Za-z_0-9]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[0-9][A-Za-z_0-9.]*|"
                   r"\.\.\.|->|\S")
LINE_MARKER = re.compile(r'^#\s+\d+\s+"([^"]*)"')
DEFINE = re.compile(r"^#define\s+([A-Za-z_][A-Za-z_0-9]*)(\(([^)]*)\))?(.*)$")

# Words that are never the declared name, so the "last identifier" rule
# below cannot mistake one for it.
TYPE_WORDS = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "const",
    "volatile", "restrict", "static", "extern", "inline", "register", "struct", "union", "enum",
    "typedef", "_Bool", "_Complex", "_Atomic", "_Noreturn", "_Thread_local", "__restrict",
    "__extension__", "__inline", "__inline__", "__const", "__signed", "__signed__", "__volatile",
    "__attribute__", "__asm__", "__thread", "__int128", "__complex__", "typeof", "__typeof",
    "__typeof__", "_Float32", "_Float64", "_Float128", "_Float32x", "_Float64x",
}
# Noise in glibc's declarations that says nothing about the interface.
DROP_WORDS = {
    "extern", "__extension__", "__restrict", "__restrict__", "restrict", "__inline", "__inline__",
    "__const", "__signed", "__signed__", "__volatile", "__volatile__",
}


def preprocess(header):
    """Preprocess `#include <header>`, keeping macro definitions and line markers."""
    src = "#include <%s>\n" % header
    try:
        # _GNU_SOURCE on purpose: without a feature-test macro, glibc
        # hides most of POSIX behind `__USE_XOPEN_EXTENDED` and friends,
        # and a name this server has never heard of is a name it might
        # report as undeclared. Over-collecting here only ever costs a
        # missed report; under-collecting invents one.
        r = subprocess.run([CC, "-E", "-dD", "-std=gnu11", "-D_GNU_SOURCE", "-x", "c", "-"], input=src,
                           capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    return r.stdout


def strip_groups(tokens, opener, closer):
    """Drop every balanced `opener ... closer` group, keeping what is outside."""
    out, depth = [], 0
    for t in tokens:
        if t == opener:
            depth += 1
        elif t == closer:
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(t)
    return out


def clean_declaration(tokens):
    """Turn a harvested declaration into something a reader wants to see."""
    out = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("__attribute__", "__attribute", "__asm__", "__asm", "__declspec"):
            i += 1
            depth = 0
            while i < len(tokens):
                if tokens[i] == "(":
                    depth += 1
                elif tokens[i] == ")":
                    depth -= 1
                    if depth == 0:
                        i += 1
                        break
                i += 1
            continue
        if t in DROP_WORDS:
            i += 1
            continue
        # glibc spells its parameter names `__format`, `__s`, `__n`. The
        # leading underscores exist to stay out of the user's namespace
        # and mean nothing to a reader.
        if t.startswith("__") and len(t) > 2 and not t.endswith("_t") and t[2].islower() and \
                t not in TYPE_WORDS:
            t = t[2:]
        out.append(t)
        i += 1
    text = ""
    for k, t in enumerate(out):
        prev = out[k - 1] if k else ""
        if not text:
            text = t
            continue
        if t in (",", ")", "]", ";", "(") and t != "(":
            text += t
        elif t == "(" and prev not in (",", "(", "*"):
            text += "("
        elif t == "(":
            text += "("
        elif prev in ("(", "[", "*"):
            text += t
        elif t in ("*", "["):
            text += " " + t if prev not in ("*", "(") else t
        else:
            text += " " + t
    return text.replace("( ", "(").replace(" )", ")").replace(" ,", ",")


def declared_name(tokens):
    """Find the identifier a declaration declares, or None."""
    # Cut the initializer and any array suffix off first.
    depth = 0
    cut = len(tokens)
    for i, t in enumerate(tokens):
        if t in "([{":
            depth += 1
        elif t in ")]}":
            depth -= 1
        elif t == "=" and depth == 0:
            cut = i
            break
    tokens = tokens[:cut]
    if not tokens:
        return None
    # A function declarator: the name sits just before the first `(` at
    # depth 0, unless that `(` groups a pointer declarator, in which case
    # the name is the last identifier inside the group.
    depth = 0
    for i, t in enumerate(tokens):
        if t == "(" and depth == 0:
            if i > 0 and re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", tokens[i - 1]) and \
                    tokens[i - 1] not in TYPE_WORDS:
                return tokens[i - 1]
            # `int (*fp)(void)` -- read the grouped declarator instead.
            inner, d = [], 0
            for u in tokens[i:]:
                if u == "(":
                    d += 1
                elif u == ")":
                    d -= 1
                    if d == 0:
                        break
                elif d == 1:
                    inner.append(u)
            for u in reversed(inner):
                if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", u) and u not in TYPE_WORDS:
                    return u
            return None
        if t in "([{":
            depth += 1
        elif t in ")]}":
            depth -= 1
    # An object declarator: the last identifier before any `[`.
    depth = 0
    end = len(tokens)
    for i, t in enumerate(tokens):
        if t == "[" and depth == 0:
            end = i
            break
        if t in "([{":
            depth += 1
        elif t in ")]}":
            depth -= 1
    for t in reversed(tokens[:end]):
        if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", t) and t not in TYPE_WORDS:
            return t
    return None


def harvest(header, text, other_header_paths):
    """Collect every name `#include <header>` brings into scope."""
    names = {}          # name -> detail
    macros = {}
    current = ""
    own = True
    buf = []
    brace = 0
    paren = 0
    for raw in text.splitlines():
        m = LINE_MARKER.match(raw)
        if m:
            current = m.group(1)
            base = current.lstrip("./")
            own = not any(base.endswith("/" + h) or base == h for h in other_header_paths)
            continue
        if raw.startswith("#define"):
            if not own:
                continue
            d = DEFINE.match(raw)
            if d:
                name, params, body = d.group(1), d.group(3), d.group(4).strip()
                if params is not None:
                    macros[name] = "#define %s(%s)" % (name, ", ".join(
                        p.strip() for p in params.split(",")))
                else:
                    macros[name] = "#define %s%s" % (name, (" " + body) if body else "")
            continue
        if raw.startswith("#"):
            continue
        for t in TOKEN.findall(raw):
            if t == "{":
                brace += 1
            elif t == "}":
                brace -= 1
            elif t == "(":
                paren += 1
            elif t == ")":
                paren -= 1
            if t == ";" and brace == 0 and paren == 0:
                if own and buf:
                    record(buf, names)
                buf = []
                continue
            if t == "}" and brace == 0 and paren == 0 and buf and buf[0] == "typedef":
                buf.append(t)
                continue
            buf.append(t)
    return names, macros


def record(tokens, names):
    """Record what one top-level declaration declares."""
    if tokens[0] in ("__extension__",):
        tokens = tokens[1:]
    if not tokens:
        return
    # Tags and enumeration constants, which are declared by the body
    # rather than by a declarator.
    for i, t in enumerate(tokens):
        if t in ("struct", "union", "enum") and i + 1 < len(tokens) and \
                re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", tokens[i + 1]):
            names.setdefault(tokens[i + 1] + "\ttag", "%s %s" % (t, tokens[i + 1]))
    if "enum" in tokens and "{" in tokens:
        start = tokens.index("{")
        depth, expect = 0, True
        for t in tokens[start:]:
            if t == "{":
                depth += 1
                expect = True
                continue
            if t == "}":
                depth -= 1
                continue
            if depth != 1:
                continue
            if t == ",":
                expect = True
                continue
            if t == "=":
                expect = False
                continue
            if expect and re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", t):
                names.setdefault(t, "enumeration constant")
                expect = False
    # Declarators, split on the top-level commas.
    body = tokens
    if body[0] == "typedef":
        body = body[1:]
        prefix = "typedef "
    else:
        prefix = ""
    parts, depth, cur = [], 0, []
    for t in body:
        if t in "([{":
            depth += 1
        elif t in ")]}":
            depth -= 1
        if t == "," and depth == 0:
            parts.append(cur)
            cur = []
            continue
        cur.append(t)
    parts.append(cur)
    base = []
    for k, part in enumerate(parts):
        if k == 0:
            base = part
            tokens_for_name = part
        else:
            # `int a, *b;` -- later declarators reuse the leading type.
            lead = []
            for t in base:
                if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", t) and t not in TYPE_WORDS:
                    break
                lead.append(t)
            tokens_for_name = lead + part
        name = declared_name(tokens_for_name)
        if not name:
            continue
        names.setdefault(name, prefix + clean_declaration(tokens_for_name))


def cescape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n" or ch == "\r" or ch == "\t":
            out.append(" ")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("?")
    return "".join(out)


def main():
    all_names = set()
    per_header = {}
    missing = []
    for h in HEADERS:
        text = preprocess(h)
        if text is None:
            missing.append(h)
            continue
        others = [o for o in HEADERS if o != h]
        names, macros = harvest(h, text, others)
        entries = {}
        for key, detail in names.items():
            name = key.split("\t")[0]
            is_tag = key.endswith("\ttag")
            all_names.add(name)
            if is_tag:
                continue
            entries.setdefault(name, detail)
        for name, detail in macros.items():
            all_names.add(name)
            entries.setdefault(name, detail)
        per_header[h] = entries
    if missing:
        sys.stderr.write("headers this toolchain does not have: %s\n" % " ".join(missing))

    # A name is offered from the first header that declares it, so
    # `size_t` is stddef.h's rather than every header's.
    owner = {}
    detail_of = {}
    for h in HEADERS:
        for name, detail in sorted(per_header.get(h, {}).items()):
            if name in owner:
                continue
            owner[name] = h
            detail_of[name] = detail

    public = sorted(n for n in owner if not n.startswith("__"))
    every = sorted(all_names)

    out = []
    out.append("// GENERATED by scripts/gen_c_names.py -- do not edit by hand.")
    out.append("//")
    out.append("// What the C standard library and the common POSIX headers actually")
    out.append("// declare, read out of the real headers on the machine that ran the")
    out.append("// script. mep's own C language server (src/c_lsp.cpp) checks an")
    out.append("// unresolved identifier against kCStdNames before it dares report it as")
    out.append("// undeclared, and offers kCStdDecls -- name, header and reconstructed")
    out.append("// declaration -- as completions, hover text and signature help.")
    out.append("//")
    out.append("// Both arrays are sorted by byte comparison and binary-searched, so they")
    out.append("// must stay in that order; the generator sorts under LC_COLLATE=C for")
    out.append("// exactly that reason.")
    out.append("//")
    out.append("// %d names over %d headers, %d of them public." % (len(every), len(per_header), len(public)))
    out.append("")
    out.append('#include "c_lsp_std_names.h"')
    out.append("")
    out.append("// Every identifier any of those headers introduces, `__`-prefixed")
    out.append("// implementation names included: this array exists to answer \"could")
    out.append("// this name have come from a header\", and an implementation name can.")
    out.append("const char *const kCStdNames[] = {")
    for n in every:
        out.append('    "%s",' % cescape(n))
    out.append("};")
    out.append("const size_t kCStdNameCount = sizeof(kCStdNames) / sizeof(kCStdNames[0]);")
    out.append("")
    out.append("// The public ones, with the header they come from and the declaration")
    out.append("// as written (glibc's `__`-prefixed parameter names unwrapped, its")
    out.append("// attributes dropped).")
    out.append("const CStdDecl kCStdDecls[] = {")
    for n in public:
        out.append('    {"%s", "%s", "%s"},' % (cescape(n), cescape(owner[n]), cescape(detail_of[n])))
    out.append("};")
    out.append("const size_t kCStdDeclCount = sizeof(kCStdDecls) / sizeof(kCStdDecls[0]);")
    out.append("")
    out.append("// The headers above, in the order the generator asked for them, which")
    out.append("// is the C standard set first and then POSIX.")
    out.append("const char *const kCStdHeaders[] = {")
    for h in HEADERS:
        if h in per_header:
            out.append('    "%s",' % cescape(h))
    out.append("};")
    out.append("const size_t kCStdHeaderCount = sizeof(kCStdHeaders) / sizeof(kCStdHeaders[0]);")
    out.append("")

    with open("src/c_lsp_std_names.cpp", "w") as f:
        f.write("\n".join(out))
    sys.stderr.write("wrote src/c_lsp_std_names.cpp: %d names, %d public, %d headers\n"
                     % (len(every), len(public), len(per_header)))


if __name__ == "__main__":
    main()
