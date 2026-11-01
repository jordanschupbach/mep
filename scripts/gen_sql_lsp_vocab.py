#!/usr/bin/env python3
"""Generate src/sql_lsp_names.cpp: SQLite's functions and PRAGMAs, for
mep-sql-lsp (completion, hover, signature help, arity checks, --describe).

  gen_sql_lsp_vocab.py SQLITE_DOC_DIR SQLITE3_BINARY > src/sql_lsp_names.cpp

SQLITE_DOC_DIR is an unpacked sqlite-doc-<version>.zip (sqlite.org/download,
public domain like the rest of SQLite). The function and pragma *names*
and every function's accepted argument counts come from the sqlite3 binary
itself (`pragma_function_list`, `pragma_pragma_list`, and an EXPLAIN of a
call with 0..9 arguments: "wrong number of arguments" or not); signatures
and prose come from the documentation pages. Positive knowledge only: a
name the binary does not have is not listed, and a function the pages do
not document gets its argument counts and no prose.

`just sql-lsp-vocab DOCDIR` reruns it.
"""
import html
import os
import re
import subprocess
import sys

DOC_PAGES = [
    ("lang_corefunc.html", "scalar"),
    ("lang_aggfunc.html", "aggregate"),
    ("lang_mathfunc.html", "math"),
    ("windowfunctions.html", "window"),
    ("percentile.html", "aggregate"),
]


def text_of(fragment):
    s = re.sub(r"<br\s*/?>", "\n", fragment)
    s = re.sub(r"</p>|<p>|</li>|<li>", "\n\n", s)
    s = re.sub(r"<[^>]+>", "", s)
    s = html.unescape(s)
    paras = [re.sub(r"[ \t\r\n]+", " ", p).strip() for p in re.split(r"\n\s*\n", s)]
    return "\n\n".join(p for p in paras if p)


def first_paras(text, limit=900):
    out = []
    n = 0
    for p in text.split("\n\n"):
        if out and n + len(p) > limit:
            break
        out.append(p)
        n += len(p)
    return "\n\n".join(out)


def sql(binary, query):
    r = subprocess.run([binary, "-batch", ":memory:", query], capture_output=True, text=True)
    return r.stdout, r.stderr


def probe_arity(binary, name, kind):
    """The argument counts 0..9 an EXPLAIN of a call accepts (9 accepted: no upper bound)."""
    ok = []
    for n in range(10):
        over = " OVER ()" if kind == "w" else ""
        _, err = sql(binary, "EXPLAIN SELECT %s(%s)%s;" % (name, ",".join(["?"] * n), over))
        if "wrong number of arguments" not in err and "no such function" not in err:
            ok.append(n)
    return ok


def doc_functions(docdir):
    funcs = {}

    def add(name, sig, doc, kind):
        name = name.lower()
        e = funcs.setdefault(name, {"sigs": [], "doc": "", "kind": kind})
        if sig and sig not in e["sigs"]:
            e["sigs"].append(sig)
        if doc and not e["doc"]:
            e["doc"] = doc
            e["kind"] = kind

    for page, kind in DOC_PAGES:
        path = os.path.join(docdir, page)
        if not os.path.exists(path):
            continue
        h = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"<dt>(.*?)</dt>\s*<dd>(.*?)</dd>", h, re.S):
            sigs = [re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", s))).strip()
                    for s in re.split(r"<br\s*/?>", m.group(1))]
            sigs = [s for s in sigs if re.match(r"^[A-Za-z_][A-Za-z0-9_]*\s*\(", s)]
            if not sigs:
                continue
            doc = first_paras(text_of(m.group(2)))
            for s in sigs:
                add(s.split("(")[0].strip(), s, doc, kind)
    # Date and time functions: the signature list, then "The <b>date()</b> function ..." paragraphs.
    path = os.path.join(docdir, "lang_datefunc.html")
    if os.path.exists(path):
        h = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"<li>\s*<b>(\w+)\(</b><i>(.*?)</i><b>\)</b>\s*</li>", h, re.S):
            add(m.group(1), "%s(%s)" % (m.group(1), html.unescape(m.group(2)).strip()), "", "date")
        for m in re.finditer(r"<p>\s*The <b>(\w+)\(\)</b> function(.*?)(?=<a name=|<h|</p><p>\s*The <b>)", h, re.S):
            name = m.group(1)
            doc = first_paras(text_of("The %s() function" % name + m.group(2)))
            if name.lower() in funcs:
                funcs[name.lower()]["doc"] = doc
    # JSON: the overview list gives signatures, each section's first paragraphs the prose.
    path = os.path.join(docdir, "json1.html")
    if os.path.exists(path):
        h = open(path, encoding="utf-8", errors="replace").read()
        anchors = {}
        for m in re.finditer(r"<li value='\d+'>(.*?)</li>", h, re.S):
            for part in re.split(r"<br\s*/?>", m.group(1)):
                am = re.search(r"<a href='#(\w+)'>(\w+)</a>", part)
                if not am:
                    continue
                sig = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", part))).strip()
                if not re.match(r"^\w+\(", sig):
                    continue
                anchors.setdefault(am.group(1), []).append(am.group(2))
                add(am.group(2), sig, "", "json")
        for anchor, names in anchors.items():
            sm = re.search(r'<a name="%s"></a>(.*?)(?=<a name=")' % anchor, h, re.S)
            if not sm:
                continue
            body = re.sub(r"<h[1-6].*?</h[1-6]>", "", sm.group(1), flags=re.S)
            body = re.split(r"<p>\s*Examples?:", body)[0]
            doc = first_paras(text_of(body), 700)
            for n in names:
                if not funcs[n.lower()]["doc"]:
                    funcs[n.lower()]["doc"] = doc
    return funcs


def doc_pragmas(docdir):
    out = {}
    path = os.path.join(docdir, "pragma.html")
    h = open(path, encoding="utf-8", errors="replace").read()
    for m in re.finditer(r'<a name="pragma_(\w+)"></a>(.*?)(?=<a name="pragma_|<hr /><a name=|\Z)', h, re.S):
        name = m.group(1)
        body = re.sub(r"<h _id=.*?</h>", "", m.group(2), flags=re.S)
        bm = re.search(r"<p>\s*<b>(.*?)</p>", body, re.S)
        sigs = []
        lead = ""
        rest = body
        if bm:
            raw = bm.group(1)
            for s in re.split(r"<br\s*/?>", raw):
                t = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", s))).strip()
                if t.upper().startswith("PRAGMA"):
                    # "PRAGMA x = N ; If a database ...": the page ran the prose into the bold block
                    m2 = re.match(r"^(PRAGMA [^;]*;)\s+(\S.*)$", t)
                    if m2:
                        t, lead = m2.group(1), m2.group(2)
                    sigs.append(t)
            rest = body[bm.end():]
        doc = first_paras((lead + "\n\n" if lead else "") + text_of(rest), 700)
        out[name] = {"sigs": sigs, "doc": doc}
    return out


def c_str(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    # keep the lines short enough for the source, and never form a trigraph
    s = s.replace("??", "?\\?")
    return '"' + s + '"'


def main():
    docdir, binary = sys.argv[1], sys.argv[2]
    out, _ = sql(binary, "SELECT name, builtin, type, narg FROM pragma_function_list ORDER BY name")
    registered = {}
    for line in out.splitlines():
        name, builtin, kind, narg = line.split("|")
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            continue
        if name.lower() == "unknown":
            continue  # the shell's placeholder for unknown() in .schema output, not a real function
        e = registered.setdefault(name.lower(), {"builtin": False, "kinds": set()})
        e["builtin"] = e["builtin"] or builtin == "1"
        e["kinds"].add(kind)
    docs = doc_functions(docdir)
    rows = []
    for name in sorted(registered):
        e = registered[name]
        kind_code = "w" if "w" in e["kinds"] else ("a" if "a" in e["kinds"] else "s")
        ok = probe_arity(binary, name, kind_code if kind_code == "w" else "s")
        if not ok:
            continue
        lo = ok[0]
        hi = -1 if 9 in ok else ok[-1]
        counts = ",".join(str(n) for n in ok) if hi >= 0 else ""
        d = docs.get(name, {"sigs": [], "doc": "", "kind": ""})
        kind = d["kind"] or ("aggregate" if kind_code in ("a", "w") else "scalar")
        if kind_code == "w" and kind == "aggregate":
            kind = "aggregate"  # usable as a window function too
        if not e["builtin"]:
            kind = "extension"
        sigs = d["sigs"]
        if not sigs:
            letters = "XYZWVUTS"
            n = lo if hi < 0 else hi
            args = [letters[i] if i < len(letters) else "A%d" % i for i in range(n)]
            if hi < 0:
                args.append("...")
            sigs = ["%s(%s)" % (name, ", ".join(args))]
        rows.append((name, kind, "\n".join(sigs), d["doc"], lo, hi, counts))
    pout, _ = sql(binary, "SELECT name FROM pragma_pragma_list ORDER BY name")
    pdocs = doc_pragmas(docdir)
    prows = []
    for name in pout.split():
        d = pdocs.get(name, {"sigs": ["PRAGMA %s;" % name], "doc": ""})
        prows.append((name, "\n".join(d["sigs"] or ["PRAGMA %s;" % name]), d["doc"]))

    w = sys.stdout.write
    w("// GENERATED by scripts/gen_sql_lsp_vocab.py from SQLite's documentation\n")
    w("// (public domain) and the sqlite3 binary's own function and pragma lists.\n")
    w("// Do not edit by hand: rerun `just sql-lsp-vocab DOCDIR`.\n")
    w('#include "sql_lsp.h"\n\n')
    w("namespace {\n\nconst SqlLspFunction kFunctions[] = {\n")
    for name, kind, sigs, doc, lo, hi, counts in rows:
        w("    {%s, %s, %s,\n     %s,\n     %d, %d, %s},\n" % (c_str(name), c_str(kind), c_str(sigs), c_str(doc),
                                                              lo, hi, c_str(counts)))
    w("};\n\nconst SqlLspPragma kPragmas[] = {\n")
    for name, sigs, doc in prows:
        w("    {%s, %s,\n     %s},\n" % (c_str(name), c_str(sigs), c_str(doc)))
    w("};\n\n}  // namespace\n\n")
    w("const SqlLspFunction *SqlLspFunctionTable(size_t *count) {\n")
    w("    *count = sizeof(kFunctions) / sizeof(kFunctions[0]);\n    return kFunctions;\n}\n\n")
    w("const SqlLspPragma *SqlLspPragmaTable(size_t *count) {\n")
    w("    *count = sizeof(kPragmas) / sizeof(kPragmas[0]);\n    return kPragmas;\n}\n")


if __name__ == "__main__":
    main()
