#!/usr/bin/env python3
"""Generate src/rust_lsp_vocab_data.cpp: the Rust standard library's
vocabulary for mep-rust-lsp (hover, completion, signature help).

    just rust-lsp-vocab
    python3 -I tools/gen_rust_vocab.py [--tarball T] [--work DIR] [--out FILE]

Method: rustdoc's JSON output (``-Zunstable-options --output-format json``,
enabled on the stable compiler with RUSTC_BOOTSTRAP=1) over the library
sources of the same rustc that is in the dev shell:

  1. ``library/`` (plus LICENSE-MIT) is extracted from the rustc source
     tarball into --work.
  2. rustdoc documents ``core`` and ``alloc`` (no dependencies beyond the
     sysroot), then ``std`` with its dependencies (libc, hashbrown, ...)
     taken from the sysroot's own ``.rmeta`` files. alloc's copy of lib.rs
     gets two ``#[rustc_doc_primitive]`` modules appended first: without a
     primitive page of its own, rustdoc drops alloc's inherent ``impl [T]``
     and ``impl str`` (``to_vec``, ``to_uppercase``, ...), and std's JSON
     does not inline the slice ones either.
  3. The three JSON files are walked from each crate root, following
     ``pub use`` re-exports across crates, so every public path
     (``std::...``, ``core::...``, ``alloc::...``) reaches its item. Each
     item gets one canonical path (the std one when std re-exports it) and
     every other path becomes an alias row.

Only stable items are kept (own ``#[stable]`` or inherited from the
enclosing item); ``#[unstable]`` items and their children are dropped,
rustdoc already drops ``#[doc(hidden)]`` and private items. The contents of
``core::arch``/``std::arch`` (thousands of SIMD intrinsics) are left out.
Signatures are re-rendered from the JSON types (rustdoc's own rendering of
the declaration, body dropped); docs are the summary paragraph plus the
first example when it is short. The library is MIT OR Apache-2.0; the MIT
notice is embedded in the output.

Keywords come from library/std/src/keyword_docs.rs (same licence); the
built-in attribute table and the reserved-keyword one-liners are mep's own
text, written for this generator.
"""

import argparse
import collections
import html
import json
import os
import re
import subprocess
import sys
import tarfile

RUST_VERSION = "1.97.1"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_TARBALL = os.path.join(ROOT, "build/grammar-corpus/.gcroots/pinned-rustc.unwrapped.src")
DEFAULT_WORK = os.path.join(ROOT, "build/rust/scratch/vocab")
SRC_DIR = "rustc-%s-src" % RUST_VERSION

MAX_SUMMARY = 900      # bytes of summary paragraph kept
MAX_EXAMPLE_LINES = 12  # first example kept only when this short
MAX_EXAMPLE_BYTES = 600


# --------------------------------------------------------------------------
# Step 1-2: extract and run rustdoc

def extract(tarball, work):
    lib = os.path.join(work, SRC_DIR, "library")
    if os.path.isdir(os.path.join(lib, "core", "src")) and os.path.exists(
            os.path.join(work, SRC_DIR, "LICENSE-MIT")):
        return lib
    os.makedirs(work, exist_ok=True)
    print("extracting library/ from %s" % tarball, file=sys.stderr)
    with tarfile.open(os.path.realpath(tarball)) as tf:
        members = [m for m in tf.getmembers()
                   if m.name.startswith(SRC_DIR + "/library/") or m.name == SRC_DIR + "/LICENSE-MIT"]
        tf.extractall(work, members=members, filter="data")
    return lib


PRIM_PATCH = ('\n// added by tools/gen_rust_vocab.py so rustdoc keeps alloc\'s primitive impls\n'
              '#[rustc_doc_primitive = "slice"]\n/// slice\nmod prim_slice {}\n'
              '#[rustc_doc_primitive = "str"]\n/// str\nmod prim_str {}\n')


def run_rustdoc(lib, out):
    os.makedirs(out, exist_ok=True)
    alloc_lib = os.path.join(lib, "alloc", "src", "lib.rs")
    with open(alloc_lib, encoding="utf-8") as f:
        text = f.read()
    if "gen_rust_vocab.py" not in text:
        with open(alloc_lib, "a", encoding="utf-8") as f:
            f.write(PRIM_PATCH)
    env = dict(os.environ, RUSTC_BOOTSTRAP="1", STD_ENV_ARCH="x86_64")
    base = ["rustdoc", "-Zunstable-options", "--output-format", "json", "--edition", "2024",
            "--crate-type", "lib", "-o", out]
    for crate in ("core", "alloc"):
        print("rustdoc %s" % crate, file=sys.stderr)
        subprocess.run(base + ["--crate-name", crate, os.path.join(lib, crate, "src", "lib.rs")],
                       check=True, env=env)
    sysroot = subprocess.run(["rustc", "--print", "sysroot"], check=True, capture_output=True,
                             text=True).stdout.strip()
    sr = os.path.join(sysroot, "lib", "rustlib", "x86_64-unknown-linux-gnu", "lib")
    files = os.listdir(sr)
    ext = []
    for name, stem in [("alloc", "alloc"), ("core", "core"), ("libc", "libc"), ("hashbrown", "hashbrown"),
                       ("std_detect", "std_detect"), ("rustc_demangle", "rustc_demangle"),
                       ("unwind", "unwind"), ("panic_unwind", "panic_unwind"),
                       ("panic_abort", "panic_abort"), ("cfg_if", "cfg_if"), ("addr2line", "addr2line"),
                       ("miniz_oxide", "miniz_oxide"), ("object", "object"),
                       ("compiler_builtins", "compiler_builtins")]:
        rlib = sorted(f for f in files if f.startswith("lib%s-" % stem) and f.endswith(".rlib"))
        if not rlib:
            sys.exit("no %s rlib in %s" % (stem, sr))
        ext += ["--extern", "%s=%s" % (name, os.path.join(sr, rlib[0][:-5] + ".rmeta"))]
    print("rustdoc std", file=sys.stderr)
    subprocess.run(base + ["--crate-name", "std", '-Zcrate-attr=feature(rustc_private)',
                           "--cfg", "backtrace_in_libstd", "--cfg", 'feature="backtrace"',
                           "--cfg", 'feature="panic_unwind"', "-L", sr] + ext +
                   [os.path.join(lib, "std", "src", "lib.rs")], check=True, env=env)


# --------------------------------------------------------------------------
# Step 3: model

class Crate:
    def __init__(self, name, path):
        self.name = name
        with open(path, encoding="utf-8") as f:
            j = json.load(f)
        self.idx = j["index"]
        self.paths = j["paths"]
        self.root = str(j["root"])
        self.ext = {int(k): v["name"] for k, v in j["external_crates"].items()}
        self.ext[0] = name
        self.by_path = {}
        for k, p in self.paths.items():
            if p["crate_id"] == 0 and k in self.idx:
                self.by_path.setdefault((tuple(p["path"]), p["kind"]), k)
        self.parent = {}
        for k, it in self.idx.items():
            inner = it["inner"]
            kind = next(iter(inner))
            kids = []
            v = inner[kind]
            if kind == "module":
                kids = v["items"]
            elif kind in ("impl", "trait"):
                kids = v["items"]
            elif kind == "enum":
                kids = v["variants"]
            elif kind == "struct":
                sk = v["kind"]
                if isinstance(sk, dict) and "plain" in sk:
                    kids = sk["plain"]["fields"]
                elif isinstance(sk, dict) and "tuple" in sk:
                    kids = [x for x in sk["tuple"] if x is not None]
            elif kind == "union":
                kids = v["fields"]
            elif kind == "variant":
                vk = v["kind"]
                if isinstance(vk, dict) and "struct" in vk:
                    kids = vk["struct"]["fields"]
                elif isinstance(vk, dict) and "tuple" in vk:
                    kids = [x for x in vk["tuple"] if x is not None]
            for c in kids:
                self.parent.setdefault(str(c), k)


CRATES = {}


def kind_of(it):
    return next(iter(it["inner"]))


def attr_strings(it):
    out = []
    for a in it.get("attrs") or []:
        if isinstance(a, str):
            out.append(a)
        elif isinstance(a, dict):
            if "other" in a:
                out.append(a["other"])
            else:
                out.append(json.dumps(a))
    return out


VER_RE = re.compile(r"major: (\d+), minor: (\d+), patch: (\d+)")


def own_stability(it):
    """('stable', since) | ('unstable', None) | None"""
    for s in attr_strings(it):
        if s.startswith("#[attr = Stability "):
            if "level: Stable" in s:
                m = VER_RE.search(s)
                since = "%s.%s.%s" % m.groups() if m else RUST_VERSION
                return ("stable", since)
            return ("unstable", None)
    return None


def const_stable(it):
    # A stable `const fn` in the standard library always carries
    # #[rustc_const_stable]; a const fn without it (or inside a `const impl`
    # block, whose attribute is on the block) is const only on nightly.
    for s in attr_strings(it):
        if s.startswith("#[attr = RustcConstStability "):
            return "level: Stable" in s
    return False


def is_hidden(it):
    return any("doc(hidden)" in s for s in attr_strings(it))


_stab_cache = {}


def stability(crate, k):
    key = (crate.name, k)
    if key in _stab_cache:
        return _stab_cache[key]
    it = crate.idx.get(k)
    res = None
    if it is not None:
        if is_hidden(it):
            res = ("unstable", None)
        else:
            res = own_stability(it)
            if res is None:
                p = crate.parent.get(k)
                res = stability(crate, p) if p is not None else ("stable", None)
    else:
        res = ("unstable", None)
    _stab_cache[key] = res
    return res


def is_stable(crate, k):
    return stability(crate, k)[0] == "stable"


def resolve(crate, ident):
    """A local item key (crate_name, id) for an id seen in `crate`, following
    external references into the other documented crates."""
    if ident is None:
        return None
    s = str(ident)
    it = crate.idx.get(s)
    if it is not None and it["crate_id"] == 0:
        return (crate.name, s)
    p = crate.paths.get(s)
    if p is None:
        return None
    cname = crate.ext.get(p["crate_id"])
    if cname not in CRATES or cname == crate.name:
        return None
    d = CRATES[cname]
    k = d.by_path.get((tuple(p["path"]), p["kind"]))
    if k is None:
        return None
    return (cname, k)


def item(key):
    return CRATES[key[0]].idx[key[1]]


# --------------------------------------------------------------------------
# Walking the public module trees

reach = collections.defaultdict(list)   # key -> [path string]
SKIP_MODULES = {"core::arch", "std::arch", "alloc::arch"}


def walk(cname, mod_id, prefix, depth, stack):
    if depth > 14:
        return
    crate = CRATES[cname]
    mod = crate.idx[mod_id]
    for cid in mod["inner"]["module"]["items"]:
        cid = str(cid)
        it = crate.idx.get(cid)
        if it is None or it.get("visibility") != "public":
            continue
        kind = kind_of(it)
        if kind == "impl":
            continue
        if kind == "use":
            u = it["inner"]["use"]
            if not is_stable(crate, cid):
                continue
            target = resolve(crate, u["id"])
            if target is None:
                continue
            tc = CRATES[target[0]]
            if not is_stable(tc, target[1]):
                continue
            tit = item(target)
            tkind = kind_of(tit)
            if u["is_glob"]:
                if tkind == "module" and target not in stack:
                    walk(target[0], target[1], prefix, depth + 1, stack | {target})
                elif tkind == "enum":
                    for v in tit["inner"]["enum"]["variants"]:
                        vk = (target[0], str(v))
                        reach[vk].append(prefix + "::" + item(vk)["name"])
                continue
            path = prefix + "::" + u["name"]
            reach[target].append(path)
            if tkind == "module" and target not in stack and path not in SKIP_MODULES:
                walk(target[0], target[1], path, depth + 1, stack | {target})
            continue
        if not is_stable(crate, cid) or it.get("name") is None:
            continue
        key = (cname, cid)
        path = prefix + "::" + it["name"]
        reach[key].append(path)
        if kind == "module" and key not in stack and path not in SKIP_MODULES:
            walk(cname, cid, path, depth + 1, stack | {key})


# --------------------------------------------------------------------------
# Type and signature rendering

def last_seg(p):
    return p.split("::")[-1]


def r_args(a):
    if a is None:
        return ""
    if "angle_bracketed" in a:
        ab = a["angle_bracketed"]
        parts = []
        for g in ab["args"]:
            if "lifetime" in g:
                parts.append(g["lifetime"])
            elif "type" in g:
                parts.append(r_type(g["type"]))
            elif "const" in g:
                c = g["const"]
                parts.append(c.get("expr") or c.get("value") or "_")
            else:
                parts.append("_")
        for c in ab.get("constraints", []):
            s = c["name"] + r_args(c.get("args"))
            b = c["binding"]
            if "equality" in b:
                t = b["equality"]
                s += " = " + (r_type(t["type"]) if "type" in t else (t["constant"].get("expr") or "_"))
            else:
                s += ": " + r_bounds(b["constraint"])
            parts.append(s)
        return "<" + ", ".join(parts) + ">" if parts else ""
    if "parenthesized" in a:
        p = a["parenthesized"]
        s = "(" + ", ".join(r_type(t) for t in p["inputs"]) + ")"
        if p.get("output") is not None:
            s += " -> " + r_type(p["output"])
        return s
    return "(..)"


def r_path(p):
    return last_seg(p["path"]) + r_args(p.get("args"))


def r_hrtb(gps):
    lts = [g["name"] for g in gps or [] if "lifetime" in g["kind"]]
    return "for<%s> " % ", ".join(lts) if lts else ""


def r_bound(b):
    if "trait_bound" in b:
        tb = b["trait_bound"]
        mod = tb.get("modifier", "none")
        pre = "?" if mod == "maybe" else ""
        return r_hrtb(tb.get("generic_params")) + pre + r_path(tb["trait"])
    if "outlives" in b:
        return b["outlives"]
    if "use" in b:
        xs = []
        for u in b["use"]:
            if isinstance(u, dict):
                xs.append(u.get("lifetime") or u.get("param") or "_")
            else:
                xs.append(str(u))
        return "use<%s>" % ", ".join(xs)
    return "?"


def r_bounds(bs):
    return " + ".join(r_bound(b) for b in bs)


def r_fnptr(fp):
    h = fp["header"]
    s = r_hrtb(fp.get("generic_params"))
    if h.get("is_unsafe"):
        s += "unsafe "
    abi = r_abi(h.get("abi"))
    if abi:
        s += abi + " "
    sig = fp["sig"]
    ins = [r_type(t) for _, t in sig["inputs"]]
    if sig.get("is_c_variadic"):
        ins.append("...")
    s += "fn(" + ", ".join(ins) + ")"
    if sig.get("output") is not None:
        s += " -> " + r_type(sig["output"])
    return s


def r_type(t):
    if t is None:
        return "()"
    k = next(iter(t))
    v = t[k]
    if k == "resolved_path":
        return r_path(v)
    if k == "dyn_trait":
        parts = [r_hrtb(pt.get("generic_params")) + r_path(pt["trait"]) for pt in v["traits"]]
        if v.get("lifetime"):
            parts.append(v["lifetime"])
        return "dyn " + " + ".join(parts)
    if k in ("generic", "primitive"):
        return v
    if k == "function_pointer":
        return r_fnptr(v)
    if k == "tuple":
        if len(v) == 1:
            return "(" + r_type(v[0]) + ",)"
        return "(" + ", ".join(r_type(x) for x in v) + ")"
    if k == "slice":
        return "[" + r_type(v) + "]"
    if k == "array":
        return "[%s; %s]" % (r_type(v["type"]), v["len"])
    if k == "pat":
        return r_type(v["type"])
    if k == "impl_trait":
        return "impl " + r_bounds(v)
    if k == "infer":
        return "_"
    if k == "raw_pointer":
        return ("*mut " if v["is_mutable"] else "*const ") + r_type(v["type"])
    if k == "borrowed_ref":
        s = "&"
        if v.get("lifetime"):
            s += v["lifetime"] + " "
        if v["is_mutable"]:
            s += "mut "
        return s + r_type(v["type"])
    if k == "qualified_path":
        st = v["self_type"]
        name = v["name"] + r_args(v.get("args"))
        if v.get("trait") is None or "generic" in st:
            return r_type(st) + "::" + name
        return "<%s as %s>::%s" % (r_type(st), r_path(v["trait"]), name)
    return "_"


def r_generics_params(g):
    parts = []
    for p in g["params"]:
        k = p["kind"]
        if "lifetime" in k:
            s = p["name"]
            if k["lifetime"].get("outlives"):
                s += ": " + " + ".join(k["lifetime"]["outlives"])
            parts.append(s)
        elif "type" in k:
            tk = k["type"]
            if tk.get("is_synthetic"):
                continue
            s = p["name"]
            if tk.get("bounds"):
                s += ": " + r_bounds(tk["bounds"])
            if tk.get("default") is not None:
                s += " = " + r_type(tk["default"])
            parts.append(s)
        elif "const" in k:
            ck = k["const"]
            s = "const %s: %s" % (p["name"], r_type(ck["type"]))
            if ck.get("default"):
                s += " = " + ck["default"]
            parts.append(s)
    return "<" + ", ".join(parts) + ">" if parts else ""


def r_where(g):
    preds = []
    for w in g.get("where_predicates", []):
        if "bound_predicate" in w:
            bp = w["bound_predicate"]
            if not bp["bounds"]:
                continue
            preds.append(r_hrtb(bp.get("generic_params")) + r_type(bp["type"]) + ": " + r_bounds(bp["bounds"]))
        elif "lifetime_predicate" in w:
            lp = w["lifetime_predicate"]
            preds.append(lp["lifetime"] + ": " + " + ".join(lp["outlives"]))
        elif "eq_predicate" in w:
            ep = w["eq_predicate"]
            rhs = ep["rhs"]
            preds.append(r_type(ep["lhs"]) + " == " + (r_type(rhs["type"]) if "type" in rhs else "_"))
    return " where " + ", ".join(preds) if preds else ""


def r_abi(abi):
    if abi is None or abi == "Rust":
        return ""
    if isinstance(abi, str):
        return 'extern "%s"' % abi
    name = next(iter(abi))
    if name == "Other":
        return 'extern "%s"' % abi[name]
    return 'extern "%s%s"' % (name.replace("_", "-"), "-unwind" if (abi[name] or {}).get("unwind") else "")


def r_self_param(name, t):
    if name != "self":
        return None
    if t == {"generic": "Self"}:
        return "self"
    if "borrowed_ref" in t and t["borrowed_ref"]["type"] == {"generic": "Self"}:
        b = t["borrowed_ref"]
        s = "&"
        if b.get("lifetime"):
            s += b["lifetime"] + " "
        if b["is_mutable"]:
            s += "mut "
        return s + "self"
    return "self: " + r_type(t)


def r_function(it, vis, has_self_out=None):
    f = it["inner"]["function"]
    h = f["header"]
    s = vis
    if h.get("is_const") and const_stable(it):
        s += "const "
    if h.get("is_async"):
        s += "async "
    if h.get("is_unsafe"):
        s += "unsafe "
    abi = r_abi(h.get("abi"))
    if abi:
        s += abi + " "
    s += "fn " + it["name"] + r_generics_params(f["generics"])
    ins = []
    has_self = False
    for i, (name, t) in enumerate(f["sig"]["inputs"]):
        sp = r_self_param(name, t) if i == 0 else None
        if sp is not None:
            has_self = True
            ins.append(sp)
        else:
            ins.append("%s: %s" % (name, r_type(t)))
    if f["sig"].get("is_c_variadic"):
        ins.append("...")
    s += "(" + ", ".join(ins) + ")"
    if f["sig"].get("output") is not None:
        s += " -> " + r_type(f["sig"]["output"])
    s += r_where(f["generics"])
    if has_self_out is not None:
        has_self_out.append(has_self)
    return s


def field_sig(crate, fid):
    fit = crate.idx.get(str(fid))
    if fit is None:
        return None
    t = r_type(fit["inner"]["struct_field"])
    nm = fit["name"]
    if nm is not None and nm.isdigit():
        return "pub " + t
    return "pub %s: %s" % (nm, t)


def public_fields(crate, ids):
    out = []
    for fid in ids:
        if fid is None:
            continue
        fit = crate.idx.get(str(fid))
        if fit is None or fit.get("visibility") != "public" or not is_stable(crate, str(fid)):
            continue
        out.append(fid)
    return out


def r_struct(crate, it):
    st = it["inner"]["struct"]
    s = "pub struct " + it["name"] + r_generics_params(st["generics"])
    sk = st["kind"]
    where = r_where(st["generics"])
    if sk == "unit":
        return s + where + ";"
    if "tuple" in sk:
        fs = []
        for fid in sk["tuple"]:
            if fid is None or not is_stable(crate, str(fid)):
                fs.append("/* private field */")
            else:
                fs.append(field_sig(crate, fid))
        return s + "(" + ", ".join(fs) + ")" + where + ";"
    plain = sk["plain"]
    fields = public_fields(crate, plain["fields"])
    body = [field_sig(crate, f) for f in fields[:8]]
    if len(fields) > 8:
        body.append("/* %d more fields */" % (len(fields) - 8))
    if plain.get("has_stripped_fields") or len(fields) < len(plain["fields"]):
        body.append("/* private fields */")
    return s + where + " { " + ", ".join(body) + " }" if body else s + where + " {}"


def r_variant(crate, it):
    v = it["inner"]["variant"]
    s = it["name"]
    vk = v["kind"]
    if isinstance(vk, dict) and "tuple" in vk:
        fs = []
        for fid in vk["tuple"]:
            if fid is None:
                fs.append("_")
            else:
                fs.append(r_type(crate.idx[str(fid)]["inner"]["struct_field"]))
        s += "(" + ", ".join(fs) + ")"
    elif isinstance(vk, dict) and "struct" in vk:
        fs = []
        for fid in vk["struct"]["fields"]:
            fit = crate.idx.get(str(fid))
            if fit is not None:
                fs.append("%s: %s" % (fit["name"], r_type(fit["inner"]["struct_field"])))
        if vk["struct"].get("has_stripped_fields"):
            fs.append("..")
        s += " { " + ", ".join(fs) + " }"
    if v.get("discriminant"):
        s += " = " + (v["discriminant"].get("expr") or v["discriminant"].get("value"))
    return s


def r_enum(crate, it):
    e = it["inner"]["enum"]
    s = "pub enum " + it["name"] + r_generics_params(e["generics"]) + r_where(e["generics"])
    vs = [crate.idx[str(v)] for v in e["variants"] if str(v) in crate.idx and is_stable(crate, str(v))]
    if len(vs) <= 8:
        body = [r_variant(crate, v) for v in vs]
        if e.get("has_stripped_variants"):
            body.append("/* some variants omitted */")
        return s + " { " + ", ".join(body) + " }" if body else s + " {}"
    return s + " { /* %d variants */ }" % len(vs)


def r_union(crate, it):
    u = it["inner"]["union"]
    s = "pub union " + it["name"] + r_generics_params(u["generics"]) + r_where(u["generics"])
    fields = public_fields(crate, u["fields"])
    body = [field_sig(crate, f) for f in fields]
    if u.get("has_stripped_fields"):
        body.append("/* private fields */")
    return s + " { " + ", ".join(body) + " }"


def r_trait(crate, it):
    t = it["inner"]["trait"]
    s = "pub "
    if t.get("is_unsafe"):
        s += "unsafe "
    if t.get("is_auto"):
        s += "auto "
    s += "trait " + it["name"] + r_generics_params(t["generics"])
    if t.get("bounds"):
        s += ": " + r_bounds(t["bounds"])
    s += r_where(t["generics"])
    # Header plus the associated types, which a reader needs most.
    assoc = []
    for i in t["items"]:
        sub = crate.idx.get(str(i))
        if sub is not None and kind_of(sub) == "assoc_type" and is_stable(crate, str(i)):
            assoc.append(r_assoc_type(sub, ""))
    return s + " { " + " ".join(assoc + ["..."]) + " }"


def r_assoc_type(it, vis):
    a = it["inner"]["assoc_type"]
    s = vis + "type " + it["name"] + r_generics_params(a["generics"])
    if a.get("bounds"):
        s += ": " + r_bounds(a["bounds"])
    s += r_where(a["generics"])
    if a.get("type") is not None:
        s += " = " + r_type(a["type"])
    return s + ";"


def r_assoc_const(it, vis):
    a = it["inner"]["assoc_const"]
    s = vis + "const %s: %s" % (it["name"], r_type(a["type"]))
    val = a.get("value")
    if val and val != "_" and len(val) <= 60:
        s += " = " + val
    return s + ";"


def r_const(it):
    c = it["inner"]["constant"]
    s = "pub const %s: %s" % (it["name"], r_type(c["type"]))
    k = c["const"]
    val = k.get("value") if k.get("is_literal") or not k.get("expr") else k.get("expr")
    if not val:
        val = k.get("value") or k.get("expr")
    if val and len(val) <= 60 and val != "_":
        s += " = " + val
    return s + ";"


def r_static(it):
    st = it["inner"]["static"]
    s = "pub "
    if st.get("is_unsafe"):
        s += "unsafe "
    s += "static "
    if st.get("is_mutable"):
        s += "mut "
    return s + "%s: %s;" % (it["name"], r_type(st["type"]))


def r_type_alias(it):
    ta = it["inner"]["type_alias"]
    return "pub type %s%s%s = %s;" % (it["name"], r_generics_params(ta["generics"]),
                                     r_where(ta["generics"]), r_type(ta["type"]))


def r_impl_header(imp):
    s = "impl" + r_generics_params(imp["generics"]) + " "
    if imp.get("is_unsafe"):
        s = "unsafe " + s
    if imp.get("trait") is not None:
        if imp.get("is_negative"):
            s += "!"
        s += r_path(imp["trait"]) + " for "
    s += r_type(imp["blanket_impl"] if imp.get("blanket_impl") else imp["for"])
    return s + r_where(imp["generics"])


# --------------------------------------------------------------------------
# Docs

LINKDEF_RE = re.compile(r"^\s*\[[^\]]+\]:\s*\S+.*$")
INLINE_LINK_RE = re.compile(r"\[([^\]\n]*)\]\([^)\n]*\)")
REF_LINK_RE = re.compile(r"\[([^\]\n]*)\]\[[^\]\n]*\]")
BARE_LINK_RE = re.compile(r"\[(`[^`\n]+`|[^\]\[\n]+)\](?![\(\[:])")
HTML_RE = re.compile(r"</?(code|em|strong|sub|span|div|a|b|i|kbd|br)\b[^>]*>")
SUP_RE = re.compile(r"<sup>(.*?)</sup>")


def delink(text):
    text = INLINE_LINK_RE.sub(r"\1", text)
    text = REF_LINK_RE.sub(r"\1", text)
    text = BARE_LINK_RE.sub(r"\1", text)
    text = SUP_RE.sub(r"^\1", text)
    text = HTML_RE.sub("", text)
    return html.unescape(text)


def split_blocks(md):
    """Paragraph/code-block list: [('p', text) | ('code', lang, [lines]) | ('h', text)]"""
    lines = md.split("\n")
    out = []
    para = []
    i = 0

    def flush():
        if para:
            out.append(("p", "\n".join(para)))
            para.clear()
    while i < len(lines):
        ln = lines[i]
        st = ln.strip()
        if st.startswith("```"):
            flush()
            lang = st[3:].strip()
            body = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                body.append(lines[i])
                i += 1
            out.append(("code", lang, body))
            i += 1
            continue
        if st.startswith("#") and not st.startswith("#["):
            flush()
            out.append(("h", st.lstrip("#").strip()))
        elif st == "":
            flush()
        elif LINKDEF_RE.match(ln):
            pass
        else:
            para.append(ln)
        i += 1
    flush()
    return out


def visible_example(lang, body):
    langs = [x.strip() for x in lang.split(",") if x.strip()]
    if any(x in ("compile_fail", "text", "ignore", "console", "sh", "bash", "toml", "json", "c") or
           x.startswith("ignore-") for x in langs):
        return None
    if langs and not any(x in ("rust", "no_run", "should_panic", "edition2015", "edition2018",
                               "edition2021", "edition2024") or x.startswith("edition")
                         for x in langs):
        return None
    vis = []
    for ln in body:
        s = ln.lstrip()
        if s == "#" or s.startswith("# "):
            continue
        if s.startswith("##"):
            ln = ln.replace("##", "#", 1)
        vis.append(ln)
    while vis and not vis[0].strip():
        vis.pop(0)
    while vis and not vis[-1].strip():
        vis.pop()
    if not vis or len(vis) > MAX_EXAMPLE_LINES:
        return None
    text = "\n".join(vis)
    if len(text) > MAX_EXAMPLE_BYTES:
        return None
    return text


def process_doc(md, deprecation=None):
    md = (md or "").replace("\r", "")
    blocks = split_blocks(md)
    summary = ""
    for b in blocks:
        if b[0] == "p":
            summary = delink(b[1]).strip()
            break
        if b[0] == "h":
            break
    if len(summary) > MAX_SUMMARY:
        cut = summary.rfind(". ", 0, MAX_SUMMARY)
        summary = summary[:cut + 1] if cut > 200 else summary[:MAX_SUMMARY].rstrip() + "..."
    example = None
    for b in blocks:
        if b[0] == "code":
            example = visible_example(b[1], b[2])
            break
    parts = []
    if deprecation:
        parts.append(deprecation)
    if summary:
        parts.append(summary)
    if example:
        parts.append("```rust\n" + example + "\n```")
    return "\n\n".join(parts)


def deprecation_of(it):
    d = it.get("deprecation")
    if not d:
        return False, None
    since = d.get("since") or ""
    if since == "TBD":
        return False, None
    m = re.match(r"(\d+)\.(\d+)\.(\d+)", since)
    if m:
        cur = tuple(int(x) for x in RUST_VERSION.split("."))
        if tuple(int(x) for x in m.groups()) > cur:
            return False, None
    note = "Deprecated"
    if since:
        note += " since " + since
    if d.get("note"):
        note += ": " + delink(d["note"])
    return True, note


# --------------------------------------------------------------------------
# Hand-written tables (mep's own text)

RESERVED = {
    "abstract": "Reserved for future use.", "become": "Reserved for future use (explicit tail calls).",
    "box": "Reserved for future use (formerly box syntax).", "do": "Reserved for future use.",
    "final": "Reserved for future use.", "macro": "Reserved for future use (declarative macros 2.0).",
    "override": "Reserved for future use.", "priv": "Reserved for future use.",
    "typeof": "Reserved for future use.", "unsized": "Reserved for future use.",
    "virtual": "Reserved for future use.", "yield": "Reserved for future use (generators/coroutines).",
    "try": "Reserved keyword since the 2018 edition (try blocks).",
    "gen": "Reserved keyword since the 2024 edition (gen blocks).",
}
WEAK = {
    "macro_rules": "Defines a declarative macro: `macro_rules! name { (pattern) => { expansion }; ... }`.",
    "union": "Declares a union (a weak keyword: only a keyword in `union Name { ... }`).",
    "'static": "The `'static` lifetime: data that lives for the whole program.",
    "safe": "Marks an item in an `unsafe extern` block as safe to use (a weak keyword).",
    "raw": "Raw borrows: `&raw const place` / `&raw mut place` (a weak keyword).",
}
KEYWORD_EDITION = {"async": "2018", "await": "2018", "dyn": "2018", "try": "2018", "gen": "2024"}

# (name, usage line, one-line doc). Written for mep from the Rust
# Reference's list of built-in attributes; no text copied.
ATTRIBUTES = [
    ("cfg", "#[cfg(predicate)]", "Conditional compilation: keep the item only when the predicate (`unix`, `test`, `feature = \"x\"`, `all(..)`, `any(..)`, `not(..)`) holds."),
    ("cfg_attr", "#[cfg_attr(predicate, attr, ...)]", "Apply the listed attributes only when the predicate holds."),
    ("test", "#[test]", "Mark a function as a unit test, run by `cargo test`."),
    ("ignore", "#[ignore] / #[ignore = \"reason\"]", "Skip this test unless tests are run with `--ignored`."),
    ("should_panic", "#[should_panic] / #[should_panic(expected = \"msg\")]", "The test passes only if it panics (optionally with a message containing `expected`)."),
    ("derive", "#[derive(Trait, ...)]", "Generate trait implementations (Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash, or a derive macro)."),
    ("automatically_derived", "#[automatically_derived]", "Marks an impl generated by a derive macro."),
    ("macro_export", "#[macro_export]", "Export a `macro_rules!` macro from the crate root."),
    ("macro_use", "#[macro_use]", "Make macros of a module (or an `extern crate`) visible after it."),
    ("proc_macro", "#[proc_macro]", "Define a function-like procedural macro."),
    ("proc_macro_derive", "#[proc_macro_derive(Name, attributes(helper, ...))]", "Define a derive macro."),
    ("proc_macro_attribute", "#[proc_macro_attribute]", "Define an attribute macro."),
    ("allow", "#[allow(lint, ...)]", "Silence the listed lints."),
    ("expect", "#[expect(lint, ...)]", "Silence the listed lints, and warn if they do not fire."),
    ("warn", "#[warn(lint, ...)]", "Report the listed lints as warnings."),
    ("deny", "#[deny(lint, ...)]", "Report the listed lints as errors."),
    ("forbid", "#[forbid(lint, ...)]", "Report the listed lints as errors and disallow raising them again further in."),
    ("deprecated", "#[deprecated(since = \"ver\", note = \"text\")]", "Mark an item as deprecated; uses warn."),
    ("must_use", "#[must_use] / #[must_use = \"reason\"]", "Warn when the value (or the function's return value) is unused."),
    ("diagnostic::on_unimplemented", "#[diagnostic::on_unimplemented(message = \"..\", label = \"..\", note = \"..\")]", "Customise the error shown when a trait is not implemented."),
    ("diagnostic::do_not_recommend", "#[diagnostic::do_not_recommend]", "Keep this impl out of the compiler's suggestions."),
    ("link", "#[link(name = \"lib\", kind = \"static\")]", "Link a native library for an `extern` block."),
    ("link_name", "#[link_name = \"symbol\"]", "The symbol name to import for an item in an `extern` block."),
    ("link_ordinal", "#[link_ordinal(n)]", "Import a function by ordinal (Windows raw-dylib)."),
    ("no_link", "#[no_link]", "Do not link the `extern crate`; only use its macros."),
    ("repr", "#[repr(C)] / #[repr(transparent)] / #[repr(u8)] / #[repr(align(n))] / #[repr(packed)]", "Choose the memory layout of a struct, enum or union."),
    ("crate_type", "#![crate_type = \"lib\"]", "The kind of crate to build (bin, lib, rlib, dylib, cdylib, staticlib, proc-macro)."),
    ("crate_name", "#![crate_name = \"name\"]", "Set the crate's name."),
    ("no_main", "#![no_main]", "Do not emit the `main` symbol."),
    ("no_std", "#![no_std]", "Do not link `std`; only `core` (and the `core` prelude) is in scope."),
    ("no_implicit_prelude", "#[no_implicit_prelude]", "Do not bring the prelude into scope in this module."),
    ("export_name", "#[export_name = \"symbol\"]", "Export the function or static under this symbol name."),
    ("link_section", "#[link_section = \".name\"]", "Put the function or static in this object-file section."),
    ("no_mangle", "#[no_mangle]", "Export the item under its plain name (no symbol mangling); `#[unsafe(no_mangle)]` in edition 2024."),
    ("used", "#[used]", "Keep the static in the object file even if it looks unused."),
    ("inline", "#[inline] / #[inline(always)] / #[inline(never)]", "Hint whether the function should be inlined."),
    ("cold", "#[cold]", "Hint that the function is rarely called."),
    ("naked", "#[unsafe(naked)]", "The function's body is a single `naked_asm!`, with no prologue or epilogue."),
    ("no_builtins", "#![no_builtins]", "Do not let the compiler replace code with calls to builtin functions like memcpy."),
    ("target_feature", "#[target_feature(enable = \"avx2\")]", "Compile the function with the given CPU features enabled."),
    ("track_caller", "#[track_caller]", "Report panics at the caller's location (`Location::caller()`)."),
    ("instruction_set", "#[instruction_set(arm::t32)]", "Choose the instruction set the function is compiled for."),
    ("doc", "#[doc = \"text\"] / #[doc(hidden)] / #[doc(alias = \"x\")]", "Documentation attributes (a `///` comment is `#[doc = \"...\"]`)."),
    ("path", "#[path = \"file.rs\"]", "Load a module from this file path."),
    ("recursion_limit", "#![recursion_limit = \"256\"]", "Raise the maximum depth for macro expansion and auto-deref."),
    ("type_length_limit", "#![type_length_limit = \"n\"]", "Raise the maximum size of a monomorphised type."),
    ("panic_handler", "#[panic_handler]", "The function called on panic in a `#![no_std]` binary."),
    ("global_allocator", "#[global_allocator]", "The static that implements `GlobalAlloc` and becomes the program's allocator."),
    ("windows_subsystem", "#![windows_subsystem = \"windows\"]", "The subsystem to link a Windows binary for (console or windows)."),
    ("non_exhaustive", "#[non_exhaustive]", "The type or variant may gain fields or variants; other crates cannot build it or match it exhaustively."),
    ("debugger_visualizer", "#![debugger_visualizer(natvis_file = \"x.natvis\")]", "Embed a debugger visualizer (Natvis or GDB script)."),
    ("collapse_debuginfo", "#[collapse_debuginfo(yes)]", "How debuginfo locations inside a macro's expansion are reported."),
    ("unsafe", "#[unsafe(attr)]", "Wrap an attribute whose use carries safety obligations (`no_mangle`, `export_name`, `link_section`)."),
    ("bench", "#[bench]", "Mark a benchmark function (unstable test framework)."),
]


def parse_keywords(lib):
    path = os.path.join(lib, "std", "src", "keyword_docs.rs")
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = {}
    cur = None
    doc = []
    for ln in lines:
        m = re.match(r'\s*#\[doc\(keyword\s*=\s*"([^"]+)"\)\]', ln)
        if m:
            cur = m.group(1)
            doc = []
            continue
        if cur is None:
            continue
        s = ln.strip()
        if s.startswith("///"):
            doc.append(s[4:] if s.startswith("/// ") else s[3:])
        elif s.startswith("mod ") or s.startswith("pub mod "):
            out[cur] = "\n".join(doc)
            cur = None
    return out


# --------------------------------------------------------------------------
# Building rows

KIND_CHAR = {
    "Module": "M", "Struct": "s", "Enum": "e", "Variant": "v", "Union": "u", "Trait": "t", "Fn": "f",
    "Method": "m", "AssocFn": "a", "AssocConst": "C", "AssocType": "T", "Const": "c", "Static": "S",
    "TypeAlias": "y", "Macro": "!", "Derive": "d", "Primitive": "p", "Keyword": "k", "Attribute": "#",
    "Field": "F",
}

PRIM_FOR = {"slice": "slice", "array": "array", "raw_pointer": "pointer", "borrowed_ref": "reference",
            "function_pointer": "fn"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarball", default=DEFAULT_TARBALL)
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--out", default=os.path.join(ROOT, "src/rust_lsp_vocab_data.cpp"))
    ap.add_argument("--skip-rustdoc", action="store_true", help="reuse the JSON already in WORK/json")
    args = ap.parse_args()

    lib = extract(args.tarball, args.work)
    jdir = os.path.join(args.work, "json")
    if not args.skip_rustdoc:
        run_rustdoc(lib, jdir)
    for c in ("core", "alloc", "std"):
        CRATES[c] = Crate(c, os.path.join(jdir, c + ".json"))

    for c in ("std", "core", "alloc"):
        cr = CRATES[c]
        walk(c, cr.root, c, 0, frozenset({(c, cr.root)}))

    # ---- canonical paths
    canon = {}
    for key, paths in reach.items():
        it = item(key)
        kind = kind_of(it)
        if kind == "primitive":
            canon[key] = it["name"]
            continue
        cr = CRATES[key[0]]
        own = cr.paths.get(key[1])
        pref = None
        if own:
            p = list(own["path"])
            cand = "::".join(["std"] + p[1:])
            if cand in paths:
                pref = cand
            elif "::".join(p) in paths and not any(x.startswith("std::") for x in paths):
                pref = "::".join(p)

        def rank(p):
            return (0 if p.startswith("std::") else 1, 1 if "::prelude::" in p else 0,
                    1 if "::primitive::" in p else 0, p.count("::"), len(p), p)
        canon[key] = pref or min(paths, key=rank)
    # Primitives: one entry per name (core's and std's pages are the same).

    entries = {}   # (path, name, kindchar) -> row dict
    aliases = {}

    def add(path, name, kind, sig, doc, since, deprecated, src_key=None):
        k = (path, name, KIND_CHAR[kind])
        if k in entries:
            return
        entries[k] = dict(path=path, name=name, kind=KIND_CHAR[kind], sig=sig, doc=doc,
                          since=since or "", deprecated=deprecated, prelude=False, key=src_key)

    def since_of(crate, k):
        st = stability(crate, k)
        return st[1] if st[0] == "stable" else None

    def doc_of(it):
        dep, note = deprecation_of(it)
        return dep, process_doc(it.get("docs"), note)

    owner_kind = {}   # canonical path -> key
    for key, cpath in canon.items():
        it = item(key)
        cr = CRATES[key[0]]
        kind = kind_of(it)
        dep, doc = doc_of(it)
        since = since_of(cr, key[1])
        if kind == "module":
            add(cpath, "", "Module", "pub mod %s" % it["name"], doc, since, dep, key)
        elif kind == "struct":
            add(cpath, "", "Struct", r_struct(cr, it), doc, since, dep, key)
        elif kind == "enum":
            add(cpath, "", "Enum", r_enum(cr, it), doc, since, dep, key)
        elif kind == "union":
            add(cpath, "", "Union", r_union(cr, it), doc, since, dep, key)
        elif kind == "trait":
            add(cpath, "", "Trait", r_trait(cr, it), doc, since, dep, key)
        elif kind == "function":
            add(cpath, "", "Fn", r_function(it, "pub "), doc, since, dep, key)
        elif kind == "constant":
            add(cpath, "", "Const", r_const(it), doc, since, dep, key)
        elif kind == "static":
            add(cpath, "", "Static", r_static(it), doc, since, dep, key)
        elif kind == "type_alias":
            add(cpath, "", "TypeAlias", r_type_alias(it), doc, since, dep, key)
        elif kind == "macro":
            add(cpath, "", "Macro", it["inner"]["macro"], doc, since, dep, key)
        elif kind == "proc_macro":
            pk = it["inner"]["proc_macro"]["kind"]
            if pk == "derive":
                add(cpath, "", "Derive", "#[derive(%s)]" % it["name"], doc, since, dep, key)
            elif pk == "attr":
                add(cpath, "", "Attribute", "#[%s]" % it["name"], doc, since, dep, key)
            else:
                add(cpath, "", "Macro", "%s!(...)" % it["name"], doc, since, dep, key)
        elif kind == "primitive":
            add(cpath, "", "Primitive", "primitive %s" % it["name"], doc, since, dep, key)
        elif kind == "variant":
            # A re-exported variant (Some, None, Ok, Err): owned by its enum.
            continue
        else:
            continue
        if kind in ("struct", "enum", "union", "trait", "primitive"):
            owner_kind.setdefault(cpath, key)

    # aliases (every other reached path)
    for key, paths in reach.items():
        cpath = canon[key]
        kind = kind_of(item(key))
        for p in paths:
            if p == cpath:
                continue
            if kind == "primitive" and "::primitive::" not in p:
                continue
            if kind == "variant":
                # std::prelude::v1::Some -> std::option::Option::Some, as owner + member
                continue
            aliases.setdefault(p, cpath)
    variant_aliases = {}
    for key, paths in reach.items():
        it = item(key)
        if kind_of(it) != "variant":
            continue
        cr = CRATES[key[0]]
        parent = cr.parent.get(key[1])
        if parent is None or (key[0], parent) not in canon:
            continue
        for p in paths:
            variant_aliases[p] = (canon[(key[0], parent)], it["name"])

    # ---- members of types, traits, enums
    impl_rows = {}
    supertraits = []

    def owner_of_type(crate, t):
        if t is None:
            return None
        k = next(iter(t))
        if k == "resolved_path":
            key = resolve(crate, t[k]["id"])
            return canon.get(key) if key else None
        if k == "primitive":
            return t[k]
        if k == "tuple":
            return "unit" if not t[k] else "tuple"
        if k == "borrowed_ref" and "resolved_path" in t[k]["type"]:
            # impl IntoIterator for &'a Vec<T>: listed with Vec (the header
            # keeps the &), which is where a reader looks for it.
            return owner_of_type(crate, t[k]["type"])
        if k in PRIM_FOR:
            return PRIM_FOR[k]
        return None

    def member_rows(crate, owner, ids, trait_owner, impl_crate_stable=True):
        for i in ids:
            si = str(i)
            sub = crate.idx.get(si)
            if sub is None or not is_stable(crate, si):
                continue
            if trait_owner is None and sub.get("visibility") != "public":
                continue
            vis = "" if trait_owner else "pub "
            k = kind_of(sub)
            dep, doc = doc_of(sub)
            since = since_of(crate, si)
            if k == "function":
                hs = []
                sig = r_function(sub, vis, hs)
                add(owner, sub["name"], "Method" if hs[0] else "AssocFn", sig, doc, since, dep)
            elif k == "assoc_const":
                add(owner, sub["name"], "AssocConst", r_assoc_const(sub, vis), doc, since, dep)
            elif k == "assoc_type":
                add(owner, sub["name"], "AssocType", r_assoc_type(sub, vis), doc, since, dep)

    for cpath, key in list(owner_kind.items()):
        it = item(key)
        cr = CRATES[key[0]]
        kind = kind_of(it)
        if kind == "struct":
            sk = it["inner"]["struct"]["kind"]
            ids = []
            if isinstance(sk, dict) and "plain" in sk:
                ids = sk["plain"]["fields"]
            elif isinstance(sk, dict) and "tuple" in sk:
                ids = [x for x in sk["tuple"] if x is not None]
            for fid in public_fields(cr, ids):
                fit = cr.idx[str(fid)]
                dep, doc = doc_of(fit)
                add(cpath, fit["name"], "Field", field_sig(cr, fid), doc, since_of(cr, str(fid)), dep)
        elif kind == "union":
            for fid in public_fields(cr, it["inner"]["union"]["fields"]):
                fit = cr.idx[str(fid)]
                dep, doc = doc_of(fit)
                add(cpath, fit["name"], "Field", field_sig(cr, fid), doc, since_of(cr, str(fid)), dep)
        elif kind == "enum":
            for vid in it["inner"]["enum"]["variants"]:
                vit = cr.idx.get(str(vid))
                if vit is None or not is_stable(cr, str(vid)):
                    continue
                dep, doc = doc_of(vit)
                add(cpath, vit["name"], "Variant", r_variant(cr, vit), doc, since_of(cr, str(vid)), dep)
        elif kind == "trait":
            member_rows(cr, cpath, it["inner"]["trait"]["items"], cpath)
            for b in it["inner"]["trait"].get("bounds") or []:
                if "trait_bound" in b:
                    tk = resolve(cr, b["trait_bound"]["trait"]["id"])
                    if tk and tk in canon:
                        supertraits.append((cpath, canon[tk]))

    for cname in ("core", "alloc", "std"):
        cr = CRATES[cname]
        for k, it in cr.idx.items():
            if it["crate_id"] != 0 or kind_of(it) != "impl":
                continue
            imp = it["inner"]["impl"]
            if imp.get("is_synthetic"):
                continue
            owner = owner_of_type(cr, imp["for"])
            if owner is None or not any(e[0] == owner and e[1] == "" for e in [(owner, "")]):
                pass
            if owner is None:
                continue
            if owner not in owner_kind and owner not in PRIM_NAMES:
                continue
            if imp.get("trait") is None:
                member_rows(cr, owner, imp["items"], None)
                continue
            st = own_stability(it)
            if st is not None and st[0] != "stable":
                continue
            tkey = resolve(cr, imp["trait"]["id"])
            if tkey is None or tkey not in canon or kind_of(item(tkey)) != "trait":
                continue
            tpath = canon[tkey]
            header = r_impl_header(imp)
            assoc = []
            target = ""
            for i in imp["items"]:
                sub = cr.idx.get(str(i))
                if sub is not None and kind_of(sub) == "assoc_type" and sub["inner"]["assoc_type"].get("type"):
                    t = sub["inner"]["assoc_type"]["type"]
                    assoc.append("%s = %s" % (sub["name"], r_type(t)))
                    if tpath == "std::ops::Deref" and sub["name"] == "Target":
                        target = owner_of_type(cr, t) or ""
            rk = (owner, tpath, header)
            if rk not in impl_rows:
                impl_rows[rk] = dict(owner=owner, trait=tpath, sig=header, assoc="; ".join(assoc),
                                     target=target, blanket=bool(imp.get("blanket_impl")),
                                     negative=bool(imp.get("is_negative")))

    # ---- preludes
    prelude = {}
    for key, paths in reach.items():
        for p in paths:
            m = re.match(r"std::prelude::rust_(2015|2018|2021|2024)::([^:]+)$", p)
            if not m:
                continue
            ed = {"2015": 1, "2018": 2, "2021": 4, "2024": 8}[m.group(1)]
            name = m.group(2)
            it = item(key)
            kind = kind_of(it)
            if kind == "variant":
                cr = CRATES[key[0]]
                parent = (key[0], cr.parent[key[1]])
                target = canon[parent] + "::" + it["name"]
                kc = "v"
            else:
                target = canon[key]
                kc = {"struct": "s", "enum": "e", "trait": "t", "function": "f", "macro": "!",
                      "type_alias": "y", "module": "M", "constant": "c"}.get(kind)
                if kind == "proc_macro":
                    kc = "d" if it["inner"]["proc_macro"]["kind"] == "derive" else "#"
                if kc is None:
                    continue
            pk = (name, target, kc)
            prelude[pk] = prelude.get(pk, 0) | ed
    prelude_paths = {t for (_, t, _) in prelude}
    for e in entries.values():
        if e["name"] == "" and e["path"] in prelude_paths:
            e["prelude"] = True
        if e["kind"] == "!" and e["path"].count("::") == 1 and e["path"].startswith("std::"):
            e["prelude"] = True

    # ---- keywords and attributes
    kwdocs = parse_keywords(lib)
    keywords = []
    for kw, d in kwdocs.items():
        kw = {"SelfTy": "Self"}.get(kw, kw)   # rustdoc's page name for `Self`
        keywords.append((kw, process_doc(d), KEYWORD_EDITION.get(kw, "")))
    for kw, d in list(RESERVED.items()) + list(WEAK.items()):
        if kw not in kwdocs:
            keywords.append((kw, d, KEYWORD_EDITION.get(kw, "")))
    keywords.sort()
    attrs = sorted(ATTRIBUTES)

    write_output(args, lib, entries, aliases, variant_aliases, impl_rows, prelude, supertraits, keywords, attrs)


PRIM_NAMES = {"i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128", "usize",
              "f16", "f32", "f64", "f128", "bool", "char", "str", "slice", "array", "pointer", "reference",
              "tuple", "unit", "never", "fn"}


# --------------------------------------------------------------------------
# Output

def cstr(s):
    out = ['"']
    prev = ""
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "?" and prev == "?":
            out.append("\\?")
        elif ord(ch) < 0x20:
            pass
        else:
            out.append(ch)
        prev = ch
    out.append('"')
    return "".join(out)


def write_output(args, lib, entries, aliases, variant_aliases, impl_rows, prelude, supertraits,
                 keywords, attrs):
    with open(os.path.join(args.work, SRC_DIR, "LICENSE-MIT"), encoding="utf-8") as f:
        lic = f.read().strip()
    o = []
    o.append("// Generated by tools/gen_rust_vocab.py (just rust-lsp-vocab) -- do not edit.")
    o.append("// The Rust %s standard library (core, alloc, std): stable public items, their" % RUST_VERSION)
    o.append("// signatures and documentation, read with rustdoc's JSON output from")
    o.append("// rust-lang/rust %s's library/ sources (the rustc source tarball), and the" % RUST_VERSION)
    o.append("// keyword docs from library/std/src/keyword_docs.rs. That text is licensed")
    o.append("// MIT OR Apache-2.0; it is used here under the MIT licence:")
    o.append("//")
    for ln in lic.split("\n"):
        o.append(("// " + ln).rstrip())
    o.append("//")
    o.append("// The attribute table and the reserved-keyword lines are mep's own text.")
    o.append("")
    o.append('#include "rust_lsp_vocab.h"')
    o.append("")
    rows = sorted(entries.values(), key=lambda e: (e["path"].encode(), e["name"].encode(), e["kind"]))
    o.append("extern const RustVocabEntry kRustVocab[];")
    o.append("extern const size_t kRustVocabCount;")
    o.append("const RustVocabEntry kRustVocab[] = {")
    for e in rows:
        o.append("    {%s, %s, RustVocabKind('%s'), %s,\n     %s, %s, %s, %s}," % (
            cstr(e["path"]), cstr(e["name"]), e["kind"], cstr(e["sig"]), cstr(e["doc"]), cstr(e["since"]),
            "true" if e["deprecated"] else "false", "true" if e["prelude"] else "false"))
    o.append("};")
    o.append("const size_t kRustVocabCount = sizeof(kRustVocab) / sizeof(kRustVocab[0]);")
    o.append("")
    # aliases: item aliases (member "") and variant aliases (owner::Variant)
    al = [(a, c, "") for a, c in aliases.items()] + [(a, o_, n) for a, (o_, n) in variant_aliases.items()]
    al = sorted(set(al), key=lambda x: x[0].encode())
    dedup = []
    seen = set()
    for a in al:
        if a[0] in seen:
            continue
        seen.add(a[0])
        dedup.append(a)
    o.append("extern const RustVocabAlias kRustVocabAliases[];")
    o.append("extern const size_t kRustVocabAliasCount;")
    o.append("const RustVocabAlias kRustVocabAliases[] = {")
    for a, c, n in dedup:
        o.append("    {%s, %s, %s}," % (cstr(a), cstr(c), cstr(n)))
    o.append("};")
    o.append("const size_t kRustVocabAliasCount = sizeof(kRustVocabAliases) / sizeof(kRustVocabAliases[0]);")
    o.append("")
    # impls, with repeated header strings pooled
    irows = sorted(impl_rows.values(), key=lambda r: (r["owner"].encode(), r["trait"].encode(), r["sig"].encode()))
    counts = collections.Counter(r["sig"] for r in irows)
    pool = {}
    o.append("namespace {")
    for s, n in sorted(counts.items()):
        if n >= 3:
            pool[s] = "kImpl%d" % len(pool)
            o.append("const char %s[] = %s;" % (pool[s], cstr(s)))
    o.append("}  // namespace")
    o.append("extern const RustVocabImpl kRustVocabImpls[];")
    o.append("extern const size_t kRustVocabImplCount;")
    o.append("const RustVocabImpl kRustVocabImpls[] = {")
    for r in irows:
        o.append("    {%s, %s, %s, %s, %s, %s, %s}," % (
            cstr(r["owner"]), cstr(r["trait"]), pool.get(r["sig"]) or cstr(r["sig"]), cstr(r["assoc"]),
            cstr(r["target"]), "true" if r["blanket"] else "false", "true" if r["negative"] else "false"))
    o.append("};")
    o.append("const size_t kRustVocabImplCount = sizeof(kRustVocabImpls) / sizeof(kRustVocabImpls[0]);")
    o.append("")
    o.append("extern const RustVocabPreludeName kRustVocabPrelude[];")
    o.append("extern const size_t kRustVocabPreludeCount;")
    o.append("const RustVocabPreludeName kRustVocabPrelude[] = {")
    for (name, target, kc), mask in sorted(prelude.items(), key=lambda x: (x[0][0].encode(), x[0][2])):
        o.append("    {%s, %s, RustVocabKind('%s'), %d}," % (cstr(name), cstr(target), kc, mask))
    o.append("};")
    o.append("const size_t kRustVocabPreludeCount = sizeof(kRustVocabPrelude) / sizeof(kRustVocabPrelude[0]);")
    o.append("")
    o.append("extern const RustVocabSupertrait kRustVocabSupertraits[];")
    o.append("extern const size_t kRustVocabSupertraitCount;")
    o.append("const RustVocabSupertrait kRustVocabSupertraits[] = {")
    for t, s in sorted(set(supertraits)):
        o.append("    {%s, %s}," % (cstr(t), cstr(s)))
    o.append("};")
    o.append("const size_t kRustVocabSupertraitCount = sizeof(kRustVocabSupertraits) / sizeof(kRustVocabSupertraits[0]);")
    o.append("")
    o.append("extern const RustVocabEntry kRustVocabKeywords[];")
    o.append("extern const size_t kRustVocabKeywordCount;")
    o.append("const RustVocabEntry kRustVocabKeywords[] = {")
    for kw, d, ed in keywords:
        o.append("    {\"\", %s, RustVocabKind('k'), %s,\n     %s, %s, false, true}," % (
            cstr(kw), cstr(kw), cstr(d), cstr(ed)))
    o.append("};")
    o.append("const size_t kRustVocabKeywordCount = sizeof(kRustVocabKeywords) / sizeof(kRustVocabKeywords[0]);")
    o.append("")
    o.append("extern const RustVocabEntry kRustVocabAttributes[];")
    o.append("extern const size_t kRustVocabAttributeCount;")
    o.append("const RustVocabEntry kRustVocabAttributes[] = {")
    for name, usage, d in attrs:
        o.append("    {\"\", %s, RustVocabKind('#'), %s,\n     %s, \"\", false, true}," % (
            cstr(name), cstr(usage), cstr(d)))
    o.append("};")
    o.append("const size_t kRustVocabAttributeCount = sizeof(kRustVocabAttributes) / sizeof(kRustVocabAttributes[0]);")
    text = "\n".join(o) + "\n"
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, args.out)
    kc = collections.Counter(e["kind"] for e in rows)
    names = {v: k for k, v in KIND_CHAR.items()}
    print("wrote %s: %d bytes, %d entries, %d aliases, %d impls, %d prelude names, %d keywords, %d attributes"
          % (args.out, len(text), len(rows), len(dedup), len(irows), len(prelude), len(keywords), len(attrs)),
          file=sys.stderr)
    for k, n in sorted(kc.items(), key=lambda x: -x[1]):
        print("  %-11s %6d" % (names[k], n), file=sys.stderr)


if __name__ == "__main__":
    main()
