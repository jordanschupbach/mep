#!/usr/bin/env python3
"""Regenerates src/lisp_lsp_cl_names.cpp from a real SBCL.

mep's own Common Lisp language server (src/lisp_lsp.cpp) checks an
operator against this table before it dares report it as undefined,
counts a call's arguments against the lambda list recorded here, and
offers each name's kind, lambda list and summary as completions, hover
text and signature help.

Generated rather than written, for the reason gen_maxima_names.py
gives: a server that remembers a language's vocabulary from memory gets
it wrong, and gets it wrong in the direction that invents diagnostics.
The COMMON-LISP package has exactly 978 external symbols, and an
analyzer that misremembers one of their lambda lists reports a correct
call as a wrong-argument-count error. So the table is read out of a
live image: `do-external-symbols` over the package, `sb-introspect` for
each operator's lambda list, and `documentation` for the first paragraph
of its docstring -- the one summary a hover popup has room for.

What SBCL adds of its own is stripped: lambda-list symbols from its
internal packages (`sb-impl::start`) are printed by bare name,
downcased, because the reader of a signature popup wants `start`, not
where SBCL keeps it. The special operators carry no introspectable
lambda list (they are not functions), so theirs are written down below
from the standard's own syntax sections.

Run from the repository root:

    nix shell nixpkgs#sbcl nixpkgs#python3 -c python3 scripts/gen_cl_names.py

The generated file is committed, so building mep never needs SBCL. The
output is sorted under a byte comparison of the upcased name, because
the table is binary-searched.
"""

import json
import os
import subprocess
import sys
import tempfile

OUT = "src/lisp_lsp_cl_names.cpp"

# The standard's syntax for each special operator (CLHS 3.1.2.1.2.1).
# SBCL implements these in its compiler, not as functions, so there is
# no lambda list to ask the image for.
SPECIAL_LAMBDA_LISTS = {
    "BLOCK": "name &body forms",
    "CATCH": "tag &body forms",
    "EVAL-WHEN": "(&rest situations) &body forms",
    "FLET": "((function-name lambda-list &body local-body)*) &body body",
    "FUNCTION": "name",
    "GO": "tag",
    "IF": "test then &optional else",
    "LABELS": "((function-name lambda-list &body local-body)*) &body body",
    "LET": "((var &optional value)*) &body body",
    "LET*": "((var &optional value)*) &body body",
    "LOAD-TIME-VALUE": "form &optional read-only-p",
    "LOCALLY": "&body body",
    "MACROLET": "((name lambda-list &body local-body)*) &body body",
    "MULTIPLE-VALUE-CALL": "function &rest forms",
    "MULTIPLE-VALUE-PROG1": "first-form &body forms",
    "PROGN": "&body forms",
    "PROGV": "symbols values &body forms",
    "QUOTE": "object",
    "RETURN-FROM": "name &optional result",
    "SETQ": "&rest pairs",
    "SYMBOL-MACROLET": "((symbol expansion)*) &body body",
    "TAGBODY": "&body statements",
    "THE": "value-type form",
    "THROW": "tag result-form",
    "UNWIND-PROTECT": "protected-form &body cleanup-forms",
}

LISP_PROGRAM = r"""
(defun clean-sym (s)
  (cond ((keywordp s) (concatenate 'string ":" (string-downcase (symbol-name s))))
        (t (string-downcase (symbol-name s)))))

(defun ll-string (x)
  (cond ((symbolp x) (clean-sym x))
        ((stringp x) (prin1-to-string x))
        ((consp x)
         (if (and (eq (car x) 'quote) (consp (cdr x)) (null (cddr x)))
             (concatenate 'string "'" (ll-string (cadr x)))
             (with-output-to-string (o)
               (write-char #\( o)
               (loop for tail on x
                     for first = t then nil
                     do (unless first (write-char #\Space o))
                        (write-string (ll-string (car tail)) o)
                        (when (and (cdr tail) (atom (cdr tail)))
                          (write-string " . " o)
                          (write-string (ll-string (cdr tail)) o)))
               (write-char #\) o))))
        (t (string-downcase (prin1-to-string x)))))

(defun ll-body (ll)
  ;; The lambda list without its outer parentheses.
  (let ((s (if ll (ll-string ll) "()")))
    (if (and (> (length s) 1) (char= (char s 0) #\())
        (subseq s 1 (1- (length s)))
        s)))

(defun esc (s)
  (with-output-to-string (o)
    (loop for c across (or s "")
          do (case c
               (#\\ (write-string "\\\\" o))
               (#\Tab (write-string "\\t" o))
               (#\Newline (write-string "\\n" o))
               (t (write-char c o))))))

(defun kinds (s)
  (let ((k '()))
    (cond ((special-operator-p s) (push "special-operator" k))
          ((macro-function s) (push "macro" k))
          ((and (fboundp s) (typep (fdefinition s) 'generic-function)) (push "generic-function" k))
          ((fboundp s) (push "function" k)))
    (cond ((and (boundp s) (constantp s)) (push "constant" k))
          ((boundp s) (push "variable" k)))
    (cond ((find-class s nil) (push "class" k))
          ((member (sb-int:info :type :kind s) '(:primitive :defined :instance :forthcoming-defclass-type))
           (push "type" k)))
    (when (member s '(declare ignore ignorable dynamic-extent ftype inline notinline optimize special
                      declaration type))
      (push "declaration" k))
    (when (member s lambda-list-keywords) (push "lambda-list-keyword" k))
    (when (member s '(speed safety debug space compilation-speed)) (push "optimize-quality" k))
    (when (member s '(:compile-toplevel :load-toplevel :execute compile load eval)) (push "situation" k))
    (if k (reverse k) (list "symbol"))))

(defun doc-of (s)
  (or (and (fboundp s) (documentation s 'function))
      (and (boundp s) (documentation s 'variable))
      (documentation s 'type)
      (documentation s 'structure)
      ""))

(defun lambda-list-of (s)
  (cond ((special-operator-p s) nil)
        ((macro-function s)
         (ll-body (sb-introspect:function-lambda-list (macro-function s))))
        ((and (fboundp s) (typep (fdefinition s) 'generic-function))
         (ll-body (sb-mop:generic-function-lambda-list (fdefinition s))))
        ((fboundp s) (ll-body (sb-introspect:function-lambda-list s)))
        (t nil)))

(defun dump (path)
 (with-open-file (out path :direction :output :if-exists :supersede :external-format :utf-8)
  (let ((syms '()))
   (do-external-symbols (s :common-lisp) (push s syms))
   (setf syms (sort syms #'string< :key #'symbol-name))
   (dolist (s syms)
    (format out "~a~a~a~a~a~a~a~%"
            (esc (symbol-name s)) #\Tab
            (esc (format nil "~{~a~^,~}" (kinds s))) #\Tab
            (esc (or (ignore-errors (lambda-list-of s)) "-")) #\Tab
            (esc (doc-of s)))))))
"""


def unesc(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def first_paragraph(doc):
    """The docstring's first paragraph, whitespace-collapsed."""
    doc = doc.strip()
    if not doc:
        return ""
    para = []
    for line in doc.split("\n"):
        if not line.strip():
            if para:
                break
            continue
        para.append(line.strip())
    text = " ".join(para)
    # "The keywords are as follows:" is a lead-in, not a summary; the
    # paragraph it introduces is the part worth showing.
    if text.endswith(":"):
        rest = doc.split("\n\n", 1)
        if len(rest) == 2 and rest[1].strip():
            text += " " + " ".join(l.strip() for l in rest[1].strip().split("\n\n")[0].split("\n"))
    if len(text) > 600:
        cut = text.rfind(". ", 0, 600)
        text = text[: cut + 1] if cut > 200 else text[:600] + "..."
    return text


def c_string(s):
    out = ['"']
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20:
            out.append("\\x%02x" % ord(ch))
        elif ord(ch) > 0x7E:
            # Docstrings are almost all ASCII; anything else is spelled as
            # its UTF-8 bytes so the source file stays plain ASCII.
            for b in ch.encode("utf-8"):
                out.append("\\%03o" % b)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def main():
    # The program and its output both go through files rather than
    # --eval and a pipe: SBCL's compiler notes share stdout with anything
    # the program prints, and a record split by one is a record lost.
    with tempfile.TemporaryDirectory() as tmp:
        prog = os.path.join(tmp, "dump.lisp")
        data = os.path.join(tmp, "names.tsv")
        with open(prog, "w") as f:
            f.write(LISP_PROGRAM)
        proc = subprocess.run(
            [
                "sbcl", "--noinform", "--non-interactive", "--no-sysinit", "--no-userinit",
                # A separate --eval so the reader has the package before
                # the program mentions `sb-introspect:`.
                "--eval", "(require :sb-introspect)",
                "--load", prog,
                "--eval", "(dump %s)" % json.dumps(data),
            ],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, check=False,
        )
        if proc.returncode != 0 or not os.path.exists(data):
            sys.exit("sbcl failed:\n" + proc.stderr + proc.stdout)
        with open(data, encoding="utf-8") as f:
            records = f.read()
    version = subprocess.run(["sbcl", "--version"], capture_output=True, text=True).stdout.strip()

    rows = []
    for raw in records.split("\n"):
        parts = raw.split("\t")
        if len(parts) != 4:
            continue
        name, kinds, ll, doc = (unesc(p) for p in parts)
        if name in SPECIAL_LAMBDA_LISTS:
            ll = SPECIAL_LAMBDA_LISTS[name]
        if ll == "-":
            ll = ""
        rows.append((name, kinds, ll, first_paragraph(doc)))
    rows.sort(key=lambda r: r[0].encode("utf-8"))
    if len(rows) != 978:
        sys.exit("expected 978 external symbols, got %d" % len(rows))

    with open(OUT, "w") as f:
        f.write("// GENERATED by scripts/gen_cl_names.py -- do not edit by hand.\n")
        f.write("//\n")
        f.write("// Every external symbol of the COMMON-LISP package as %s reports it:\n" % version)
        f.write("// what kind of thing each one is, the lambda list of every operator\n")
        f.write("// (SBCL-internal package prefixes stripped, downcased), and the first\n")
        f.write("// paragraph of its docstring. mep's own Common Lisp language server\n")
        f.write("// reads this for argument-count checks, completion, hover and\n")
        f.write("// signature help; see the generator's header for why it is generated.\n")
        f.write("//\n")
        f.write("// Sorted by byte comparison of the (upper-case) name and binary-\n")
        f.write("// searched, so it must stay in that order.\n")
        f.write("//\n")
        f.write("// %d names.\n\n" % len(rows))
        f.write('#include "lisp_lsp_cl_names.h"\n\n')
        f.write("const LispClDecl kLispClDecls[] = {\n")
        for name, kinds, ll, doc in rows:
            f.write("    {%s, %s, %s,\n     %s},\n" % (c_string(name), c_string(kinds), c_string(ll), c_string(doc)))
        f.write("};\n\n")
        f.write("const size_t kLispClDeclCount = sizeof(kLispClDecls) / sizeof(kLispClDecls[0]);\n")
    print("wrote %s (%d names)" % (OUT, len(rows)))


if __name__ == "__main__":
    main()
