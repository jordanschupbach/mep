// Coverage for clojure_format.h/.cpp: mep's own Clojure formatter
// (cljfmt's layout). Each case is an input/expected pair, checked once for
// the layout and once fed back through Format() to prove it is a fixed
// point.
//
// `mep-clojure-format-test --format FILE...` prints each file formatted
// instead (the reason a file was left alone on stderr), which is how the
// formatter is compared with cljfmt on a corpus.

#include "clojure_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

int checks = 0;

void Same(const std::string &src, const std::string &want) {
    checks++;
    const cljfmt::Result r = cljfmt::Format(src);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
    if (r.text != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(), want.c_str(),
                     r.text.c_str());
        std::abort();
    }
    const cljfmt::Result again = cljfmt::Format(r.text);
    if (!again.ok || again.text != r.text) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", r.text.c_str(), again.text.c_str());
        std::abort();
    }
}

void Unchanged(const std::string &src) { Same(src, src); }

int FormatFiles(int argc, char **argv) {
    int status = 0;
    for (int k = 2; k < argc; k++) {
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const cljfmt::Result r = cljfmt::Format(ss.str());
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", argv[k], r.error_line, r.error.c_str());
            status = 1;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    // --- indentation ------------------------------------------------------------
    Same("(defn foo [x]\n(let [a 1\nb 2]\n(+ a b x)\n)\n)\n",
         "(defn foo [x]\n  (let [a 1\n        b 2]\n    (+ a b x)))\n");
    // A call's arguments: under the first, or one column in.
    Same("(foo a\nb\n   c)\n", "(foo a\n     b\n     c)\n");
    Same("(foo\na\nb)\n", "(foo\n a\n b)\n");
    // :block rules: a body two columns in once the fixed arguments are on the head's line.
    Same("(when x\ny)\n", "(when x\n  y)\n");
    Same("(if x\ny\nz)\n", "(if x\n  y\n  z)\n");
    Same("(let\n[a 1]\na)\n", "(let\n [a 1]\n  a)\n");
    // :inner rules, and the def.../with-... patterns.
    Same("(defn f\n[x]\nx)\n", "(defn f\n  [x]\n  x)\n");
    Same("(defsomething x\ny)\n", "(defsomething x\n  y)\n");
    Same("(default-thing x\ny)\n", "(default-thing x\n               y)\n");
    Same("(with-whatever [x]\ny)\n", "(with-whatever [x]\n  y)\n");
    Same("(s/defn f [x]\nx)\n", "(s/defn f [x]\n  x)\n");
    // Nested :inner: a protocol's methods, letfn's functions.
    Same("(defprotocol P\n(m [this]\n\"doc\"))\n", "(defprotocol P\n  (m [this]\n    \"doc\"))\n");
    Same("(letfn [(f [x]\nx)]\n(f 1))\n", "(letfn [(f [x]\n          x)]\n  (f 1))\n");
    // Collections: under the first element.
    Same("[a\nb]\n{:a 1\n:b 2}\n#{a\nb}\n", "[a\n b]\n{:a 1\n :b 2}\n#{a\n  b}\n");
    // #( is one column wider.
    Same("#(foo %\n%)\n", "#(foo %\n      %)\n");
    // Threading forms are plain calls.
    Same("(-> x\n(assoc :a 1)\n  (dissoc :b))\n", "(-> x\n    (assoc :a 1)\n    (dissoc :b))\n");
    // Top-level forms go to column 0.
    Same("  (foo)\n  (bar)\n", "  (foo)\n(bar)\n");

    // --- whitespace ---------------------------------------------------------------
    Same("( foo  1 )\n", "(foo  1)\n");  // inner runs of spaces stay (cljfmt's default)
    Same("(foo(bar)\"x\"[y])\n", "(foo (bar) \"x\" [y])\n");
    Same("(foo)   \n", "(foo)\n");
    Same("(a)\n\n\n\n(b)\n", "(a)\n\n(b)\n");
    Same("@ (deref-me)\n#_ (gone)\n", "@(deref-me)\n#_(gone)\n");
    Unchanged("`(~ @x)\n");                // `~ @x` must not become `~@x`
    Unchanged("#inst \"2020-01-01\"\n");   // a tagged literal keeps its space
    Unchanged("{:a 1, :b 2}\n");           // commas between elements stay

    // --- left as they are -----------------------------------------------------------
    Unchanged("(def s \"a\n   b\")\n");    // a string's lines
    Unchanged("(foo\n    ;; a comment where it was\n bar)\n");
    Unchanged("(ns a.b\n  (:require [clojure.string :as str]))\n");
    // A comment before a closer keeps its line; the closer comes back.
    Same("(foo ; c\n)\n", "(foo ; c\n )\n");

    // --- refused ----------------------------------------------------------------------
    checks++;
    const cljfmt::Result bad = cljfmt::Format("(defn f [x]\n  (+ x 1)\n");
    CHECK(!bad.ok);
    CHECK(bad.text == "(defn f [x]\n  (+ x 1)\n");
    CHECK(bad.error_line == 1);

    std::printf("mep-clojure-format-test: %d checks passed\n", checks);
    return 0;
}
