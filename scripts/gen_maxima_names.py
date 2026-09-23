#!/usr/bin/env python3
"""Regenerates src/maxima_lsp_builtin_names.cpp from a real Maxima install.

mep's own Maxima language server (src/maxima_lsp.cpp) checks an
unresolved identifier against this list before it dares report it as
undefined, and offers the name, its kind and the argument list it takes
as completions, hover text and signature help.

What is taken from Maxima is *interface* information only -- which names
exist, whether each is a function or a variable, and the parameters each
one is declared with. No documentation prose is copied: the explanations
the server shows are written by hand in src/maxima_lsp_vocab.cpp.

Run from the repository root with maxima on PATH:

    nix develop --command python3 scripts/gen_maxima_names.py

The generated file is committed, so building mep never needs Maxima. The
output is sorted under a byte comparison, because the tables are
binary-searched.
"""

import os
import re
import shutil
import subprocess
import sys

OUT = "src/maxima_lsp_builtin_names.cpp"


def maxima_share_dir():
    """Locates the `share/info` directory of the maxima on PATH."""
    exe = shutil.which("maxima")
    if exe is None:
        sys.exit("maxima is not on PATH (try: nix develop --command python3 scripts/gen_maxima_names.py)")
    root = os.path.dirname(os.path.dirname(os.path.realpath(exe)))
    for base, _dirs, files in os.walk(root):
        if "maxima-index.lisp" in files:
            return base
    sys.exit("could not find maxima-index.lisp under " + root)


def maxima_version():
    out = subprocess.run(["maxima", "--version"], capture_output=True, text=True, check=False)
    return out.stdout.strip() or "unknown"


# ` -- Function: integrate` names the entry; the argument lists follow it
# on their own deeply-indented lines:
#
#      -- Function: integrate
#           integrate (<expr>, <x>)
#           integrate (<expr>, <x>, <a>, <b>)
ENTRY_RE = re.compile(r"^\s+--\s+([A-Za-z][A-Za-z ]*?):\s*(.*)$")
SIG_RE = re.compile(r"^ {6,}([^\s(]+) \((.*)\)\s*$")
# The documentation writes metavariables as <x>; they read better as
# plain names in a signature popup.
META_RE = re.compile(r"<([^<>]*)>")


def signature_index(share):
    """Every argument list the manual spells out, by name.

    A name's own index entry does not always carry its signature -- the
    index points at the *first* `--` block in a node, and `diff` lands on
    the evflag rather than the function -- so the signature lines are
    collected from the whole manual and looked up by name. A name with
    several argument lists keeps up to three, joined by " | ", which is
    what signature help offers as alternatives.
    """
    sigs = {}

    def record(name, args):
        # An argument list that does arithmetic is a worked example, not a
        # signature: the manual shows `integrate (f(x) * g(t - x), x, 0,
        # t)` under the convolution rule, and offering that as the way to
        # call integrate would be worse than offering nothing.
        if any(ch in args for ch in "*/^"):
            return
        forms = sigs.setdefault(name, [])
        text = "%s(%s)" % (name, args.strip())
        if text not in forms and len(forms) < 3:
            forms.append(text)

    for info in sorted(os.listdir(share)):
        if ".info" not in info:
            continue
        try:
            text = open(os.path.join(share, info), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for line in text.split("\n"):
            # Prose quotes its examples with the manual's own curly
            # quotes, so a bare `name (args)` line really is a signature.
            if "\u2018" in line or "\u2019" in line:
                continue
            m = SIG_RE.match(line)
            if m:
                record(m.group(1), META_RE.sub(r"\1", m.group(2)))
                continue
            # `-- Function: matrix (<row_1>, ..., <row_n>)` puts the one
            # argument list it has on the heading line itself.
            m = ENTRY_RE.match(line)
            if m and "(" in m.group(2):
                head = META_RE.sub(r"\1", m.group(2).strip())
                paren = head.find("(")
                if head.endswith(")"):
                    record(head[:paren].strip(), head[paren + 1 : -1])
    return dict((k, " | ".join(v)) for k, v in sigs.items())


def read_entries(share, packages):
    index = open(os.path.join(share, "maxima-index.lisp"), encoding="utf-8", errors="replace").read()
    pairs = re.findall(r'\("(.*?)" \. \("(.*?)" (\d+) (\d+) "(.*?)"\)\)', index)
    sigs = signature_index(share)
    files = {}
    entries = {}
    for name, info_file, offset, length, node in pairs:
        path = os.path.join(share, info_file)
        if path not in files:
            try:
                files[path] = open(path, "rb").read()
            except OSError:
                files[path] = b""
        text = files[path][int(offset) : int(offset) + int(length)].decode("utf-8", "replace")
        kind = ""
        for line in text.split("\n"):
            m = ENTRY_RE.match(line)
            if m:
                kind = m.group(1).strip()
                break
        if not kind:
            continue
        kind = normalize_kind(kind)
        signature = sigs.get(name, "")
        # A name the manual gives an argument list is a function, whatever
        # heading its index entry happened to land on: `diff` is indexed
        # at the `ev` flag of the same name.
        if signature and kind in ("symbol", "variable"):
            kind = "function"
        entries[name] = (kind, signature, package_of(node, packages))
    return entries


def normalize_kind(kind):
    """Collapses the documentation's kind labels onto the few that matter."""
    k = kind.lower()
    if "function" in k or "macro" in k:
        return "function"
    if "operator" in k:
        return "operator"
    if "constant" in k:
        return "constant"
    if "special symbol" in k or "terminator" in k:
        return "symbol"
    if "property" in k or "declaration" in k:
        return "property"
    if "option" in k or "variable" in k or "object" in k:
        return "variable"
    return "variable"


# "Functions and Variables for to_poly_solve" -> "to_poly_solve": a name
# that only exists once its package is loaded, which the server says out
# loud rather than pretending the name is always there. A chapter title
# that is not a package ("Differentiation", "Plotting") is not one, which
# is why this is checked against the share tree rather than a word list.
NODE_RE = re.compile(r"^(?:Functions and Variables|Options|Graphic Options?|Functions|Variables) for (.+)$")


def share_packages(share):
    """The share packages this Maxima ships, by directory name.

    A loadable package is a directory whose own entry point is inside it
    (`draw/draw.lisp`, `to_poly_solve/to_poly_solve.mac`) -- which is what
    tells a package apart from a manual chapter that happens to share a
    directory name, like `integration`.
    """
    root = os.path.dirname(share)
    names = set()
    for base, dirs, _files in os.walk(root):
        if os.path.basename(base) != "share":
            continue
        for d in dirs:
            entries = set(os.listdir(os.path.join(base, d)))
            if d + ".mac" in entries or d + ".lisp" in entries:
                names.add(d.lower())
    return names


def package_of(node, packages):
    m = NODE_RE.match(node.strip())
    if not m:
        return ""
    topic = m.group(1).strip()
    return topic if topic.lower() in packages else ""


def escape(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def main():
    share = maxima_share_dir()
    entries = read_entries(share, share_packages(share))
    # Names Maxima's reader itself owns, which are documented as syntax
    # rather than as bindings and would otherwise be missing.
    for extra in ("block", "lambda", "local", "return", "throw", "catch", "if", "then", "else",
                  "elseif", "do", "for", "from", "step", "next", "thru", "while", "unless", "in",
                  "and", "or", "not"):
        entries.setdefault(extra, ("function", "", ""))
    names = sorted(entries, key=lambda s: s.encode())

    with open(OUT, "w", encoding="utf-8") as out:
        out.write("// GENERATED by scripts/gen_maxima_names.py -- do not edit by hand.\n")
        out.write("//\n")
        out.write("// Every name %s documents, with the kind of thing it is, the argument\n" % maxima_version())
        out.write("// list it is declared with and -- for a name that only exists once a share\n")
        out.write("// package is loaded -- the package that carries it. mep's own Maxima\n")
        out.write("// language server checks an unresolved identifier against kMaximaBuiltinNames\n")
        out.write("// before it dares report it as undefined, and reads kMaximaBuiltinDecls for\n")
        out.write("// completion, hover and signature help.\n")
        out.write("//\n")
        out.write("// Interface information only: no documentation text is copied here. The\n")
        out.write("// explanations the server shows are written by hand in maxima_lsp_vocab.cpp.\n")
        out.write("//\n")
        out.write("// Both arrays are sorted by byte comparison and binary-searched, so they\n")
        out.write("// must stay in that order.\n")
        out.write("//\n")
        out.write("// %d names.\n\n" % len(names))
        out.write('#include "maxima_lsp_builtin_names.h"\n\n')
        out.write("const char *const kMaximaBuiltinNames[] = {\n")
        for name in names:
            out.write('    "%s",\n' % escape(name))
        out.write("};\n")
        out.write("const size_t kMaximaBuiltinNameCount = sizeof(kMaximaBuiltinNames) / sizeof(kMaximaBuiltinNames[0]);\n\n")
        out.write("const MaximaBuiltinDecl kMaximaBuiltinDecls[] = {\n")
        for name in names:
            kind, signature, package = entries[name]
            out.write('    {"%s", "%s", "%s", "%s"},\n' % (escape(name), kind, escape(signature), escape(package)))
        out.write("};\n")
        out.write("const size_t kMaximaBuiltinDeclCount = sizeof(kMaximaBuiltinDecls) / sizeof(kMaximaBuiltinDecls[0]);\n")
    print("wrote %s (%d names)" % (OUT, len(names)))


if __name__ == "__main__":
    main()
