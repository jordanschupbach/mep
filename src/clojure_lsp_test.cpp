// Coverage for mep's own Clojure language server: the reader
// (clojure_reader.h), the analysis and every feature (clojure_lsp.h),
// driven directly -- no process, no JSON-RPC, no JVM.
//
// `mep-clojure-lsp-test --read-check` reads the paths named on stdin and
// prints each one's first reader error (`path\tERR\tline\tmessage`, or
// `path\tOK`), the format tools/clojure_reader_check.clj prints for the
// JVM's reader -- the two outputs are compared on a corpus.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "clojure_lsp.h"
#include "clojure_reader.h"

namespace {

int checks = 0;

void Check(bool condition, const char *expression, int line) {
    checks++;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

Lines L(const std::string &text) {
    Lines out;
    size_t p = 0;
    for (;;) {
        const size_t nl = text.find('\n', p);
        if (nl == std::string::npos) {
            out.push_back(text.substr(p));
            break;
        }
        out.push_back(text.substr(p, nl - p));
        p = nl + 1;
    }
    return out;
}

// The first reader error's message, or "" when the text reads.
std::string ReadError(const std::string &text) {
    const CljReadResult r = CljRead(L(text), false);
    return r.errors.empty() ? "" : r.errors[0].message;
}

std::vector<std::string> Codes(const std::string &text, const std::string &path = "src/a/b.clj") {
    std::vector<std::string> out;
    for (const CljLspDiagnostic &d : CljLspDiagnostics(L(text), path)) out.push_back(d.code);
    return out;
}

bool Has(const std::vector<std::string> &v, const std::string &s) {
    for (const std::string &x : v) {
        if (x == s) return true;
    }
    return false;
}

bool HasLabel(const std::vector<CljLspCompletionItem> &items, const std::string &label) {
    for (const CljLspCompletionItem &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

const std::string kNs = "(ns a.b\n  (:require [clojure.string :as str]))\n";
const std::string kBare = "(ns a.b)\n";

// --- the reader ------------------------------------------------------------------------

void TestReader() {
    // Clojure 1.12's own messages.
    CHECK(ReadError("(defn f [x]\n") == "EOF while reading, starting at line 1");
    CHECK(ReadError(")") == "Unmatched delimiter: )");
    CHECK(ReadError("(]") == "Unmatched delimiter: ]");
    CHECK(ReadError("{:a 1 :b}") == "Map literal must contain an even number of forms");
    CHECK(ReadError("{:a 1 :a 2}") == "Duplicate key: :a");
    CHECK(ReadError("#{1 1}") == "Duplicate key: 1");
    CHECK(ReadError("a:") == "Invalid token: a:");
    CHECK(ReadError("::") == "Invalid token: ::");
    CHECK(ReadError("08") == "Invalid number: 08");
    CHECK(ReadError("1.5.5") == "Invalid number: 1.5.5");
    CHECK(ReadError("\\abc") == "Unsupported character: \\abc");
    CHECK(ReadError("\"\\q\"") == "Unsupported escape character: \\q");
    CHECK(ReadError("\"abc") == "EOF while reading string");
    CHECK(ReadError("^1 x") == "Metadata must be Symbol,Keyword,String,Vector or Map");
    CHECK(ReadError("^:a 1") == "Metadata can only be applied to IMetas");
    CHECK(ReadError("#?(clj 1)") == "Feature should be a keyword: clj");
    CHECK(ReadError("#?(:clj)") == "read-cond starting on line 1 requires an even number of forms");
    CHECK(ReadError("#?(:cljs)").empty());  // a branch not taken needs no form
    CHECK(ReadError("`~@x") == "splice not in list");
    CHECK(ReadError("#(#(%))") == "Nested #()s are not allowed");
    CHECK(ReadError("##Foo") == "Unknown symbolic value: ##Foo");
    CHECK(ReadError("#\"(a\"") == "Unclosed group near index 2");
    CHECK(ReadError("#uuid 1") == "#uuid data reader expected string");
    // What reads.
    CHECK(ReadError(":1 clojure.core// #inst \"2020\" #foo/bar 1 [#?@(:clj [1 2])] 3#?(:clj 4)").empty());
    CHECK(ReadError("{#?(:clj :a :cljs :b) 1 #?(:cljs :a :clj :c) 2}").empty());  // the :clj branch decides
    CHECK(ReadError("{#?(:clj :a) 1 #?(:cljs :b) 2}") == "Map literal must contain an even number of forms");
    CHECK(ReadError("(a #_ b c) #! shebang\n 2r1010 36rZZ 1/2 1e5M 0x1F \\u00e9 \\newline").empty());
    // Recovery: a forgotten paren is closed at the next (def at column 0.
    const CljReadResult r = CljRead(L("(defn f [x]\n  (+ x 1)\n(defn g [] 2)"));
    CHECK(r.errors.size() == 1);
    CHECK(r.top.size() == 2);
}

// --- diagnostics -------------------------------------------------------------------------

void TestDiagnostics() {
    // The compiler's errors.
    CHECK(Has(Codes(kNs + "(defn f [] (frobnicate 1))"), "unresolved-symbol"));
    CHECK(Has(Codes(kNs + "(defn f [] (strr/join []))"), "unresolved-namespace"));
    CHECK(Has(Codes(kNs + "(defn f [] (str/joinn []))"), "unresolved-var"));
    CHECK(Has(Codes(kNs + "(defn f [] (g))\n(defn g [] 1)"), "use-before-definition"));
    CHECK(!Has(Codes(kNs + "(declare g)\n(defn f [] (g))\n(defn g [] 1)"), "use-before-definition"));
    CHECK(Has(Codes(kNs + "(inc 1 2)"), "invalid-arity"));
    CHECK(Has(Codes(kNs + "(defn f [x] x)\n(f 1 2)"), "invalid-arity"));
    CHECK(!Has(Codes(kNs + "(-> 1 (+ 2) inc)"), "invalid-arity"));  // threading inserts the argument
    CHECK(Has(Codes(kNs + "(let [a] a)"), "binding-shape"));
    CHECK(Has(Codes(kNs + "(if)"), "if-shape"));
    CHECK(Has(Codes(kNs + "(defn f x)"), "fn-shape"));
    CHECK(Has(Codes("#?(:clj 1)", "src/x.clj"), "reader-conditional"));
    CHECK(!Has(Codes("(ns x)\n#?(:clj 1)", "src/x.cljc"), "reader-conditional"));
    CHECK(Has(Codes("(ns wrong.name)", "src/a/b.clj"), "namespace-name-mismatch"));
    // Names the file does not define are fine when they come from a macro it cannot see,
    // ClojureScript, Java, or what a form binds.
    CHECK(Codes(kBare + "(defn f [{:keys [a b]} & more] (for [x more :let [y x]] [a b y]))").empty());
    CHECK(Codes(kBare + "(defn f [] (String/valueOf 1) (.toUpperCase \"a\") (java.util.Date.))").empty());
    CHECK(Codes(kBare + "(defn f [] (try 1 (catch Exception e (str e))))").empty());
    CHECK(!Has(Codes("(ns a.b (:require [lib.m :as m]))\n(defn f [] (m/go-loop [x 1] (recur x)))"), "unresolved-symbol"));
    CHECK(Codes("(ns a.b)\n(defn f [] (js/alert 1))", "src/a/b.cljs").empty());
    CHECK(Codes(kBare + "(defmacro m [x] `(foo ~x ~'bar))\n(m 1)").empty());
    // A use above a def that takes over a core name means core's.
    CHECK(!Has(Codes(kNs + "(defn g [x] (name x))\n(defn name [x] x)"), "use-before-definition"));
    // Unused things.
    CHECK(Has(Codes(kNs + "(defn f [x] (let [y 1] x))"), "unused-binding"));
    CHECK(!Has(Codes(kNs + "(defn f [x] (let [_y 1] x))"), "unused-binding"));
    CHECK(Has(Codes("(ns a.b (:require [clojure.set :as set]))\n(defn f [] 1)"), "unused-namespace"));
    CHECK(Has(Codes("(ns a.b (:require [clojure.set :refer [union difference]]))\n(defn f [] (union))"),
              "unused-referred-var"));
    CHECK(Has(Codes(kNs + "(defn- h [] 1)"), "unused-private-var"));
    CHECK(Has(Codes(kNs + "(def a 1)\n(def a 2)"), "redefined-var"));
    CHECK(Has(Codes(kNs + "(defn f [x] \"doc\" x)"), "misplaced-docstring"));
    CHECK(Has(Codes(kNs + "(replicate 2 :x)"), "deprecated-var"));
    CHECK(Has(Codes(kNs + "(defn f [] (do 1))"), "redundant-do"));
    CHECK(Has(Codes(kNs + "(defn f [x] (not (empty? x)))"), "not-empty"));
    // Clojure's messages.
    const std::vector<CljLspDiagnostic> d = CljLspDiagnostics(L(kBare + "(frobnicate)"), "src/a/b.clj");
    CHECK(d.size() == 1 && d[0].message == "Unable to resolve symbol: frobnicate in this context");
    CHECK(CljLspFormatDiagnostic("x.clj", d[0]) == "x.clj:2:2: error: Unable to resolve symbol: frobnicate in this context [unresolved-symbol]");
    CHECK(CljLspDiagnosticFails(d[0]));
}

// --- features ------------------------------------------------------------------------------

void TestCompletion() {
    const std::string src = kNs + "(defn helper [x] x)\n(defn f [alpha beta]\n  (let [gamma 1]\n    (he al ga str/jo ma :ke)))";
    const Lines l = L(src);
    const int line = 5;
    const std::string &t = l[static_cast<size_t>(line)];
    auto at = [&](const char *needle) { return static_cast<int>(t.find(needle) + std::strlen(needle)); };
    CHECK(HasLabel(CljLspCompletions(l, line, at("(he")), "helper"));
    CHECK(HasLabel(CljLspCompletions(l, line, at(" al")), "alpha"));
    CHECK(HasLabel(CljLspCompletions(l, line, at(" ga")), "gamma"));
    CHECK(HasLabel(CljLspCompletions(l, line, at("str/jo")), "str/join"));
    CHECK(HasLabel(CljLspCompletions(l, line, at(" ma")), "map"));
    CHECK(HasLabel(CljLspCompletions(l, line, at(":ke")), ":keys"));
    // Locals come first.
    const std::vector<CljLspCompletionItem> g = CljLspCompletions(l, line, at(" ga"));
    CHECK(!g.empty() && g[0].label == "gamma");
    // Namespaces inside :require.
    const Lines r = L("(ns a.b\n  (:require [clojure.s]))");
    CHECK(HasLabel(CljLspCompletions(r, 1, static_cast<int>(r[1].find("clojure.s") + 9)), "clojure.string"));
}

void TestHoverAndSignatures() {
    const std::string src = kNs + "(defn add\n  \"Adds.\"\n  ([a] a)\n  ([a b] (+ a b)))\n(add 1 2)\n(str/join \",\" [])";
    const Lines l = L(src);
    CljLspHoverInfo h = CljLspHover(l, 6, 1);
    CHECK(h.found && h.text.find("(add a b)") != std::string::npos && h.text.find("Adds.") != std::string::npos);
    h = CljLspHover(l, 7, 2);
    CHECK(h.found && h.text.find("clojure.string") != std::string::npos && h.text.find("(join coll)") != std::string::npos);
    h = CljLspHover(l, 6, 7);  // the `2`: nothing
    CHECK(!h.found);
    const CljLspSignatureHelp s = CljLspSignatures(l, 6, 7);
    CHECK(s.found && s.signatures.size() == 2 && s.active_signature == 1 && s.active_param == 1);
    CHECK(s.signatures[1].label == "(add a b)");
    const CljLspSignatureHelp c = CljLspSignatures(L("(assoc {} :a "), 0, 13);
    CHECK(c.found && c.signatures[0].label == "(assoc map key val)" && c.active_param == 2);
}

void TestSymbolsAndFolds() {
    const std::string src = kNs + "(defprotocol Shape\n  (area [s]))\n(defrecord Circle [r])\n(defmacro m [x] x)\n(def ^:private pi 3.14)\n(defn f [] 1)";
    const std::vector<CljLspSymbol> syms = CljLspSymbols(L(src));
    CHECK(syms.size() == 7);
    CHECK(syms[0].name == "a.b" && syms[0].kind == CljLspSymbolKind::Namespace);
    CHECK(syms[1].name == "Shape" && syms[1].kind == CljLspSymbolKind::Interface);
    CHECK(syms[2].name == "area" && syms[2].parent == 1);
    CHECK(syms[3].name == "Circle" && syms[3].kind == CljLspSymbolKind::Struct);
    CHECK(syms[4].name == "m" && syms[4].detail == "macro");
    const std::vector<CljLspFold> folds = CljLspFolds(L(";; a\n;; b\n(defn f []\n  1)"));
    CHECK(folds.size() == 2);
}

void TestNavigation() {
    const std::string src = kNs + "(defn helper [x] x)\n(defn f [a]\n  (let [b (helper a)]\n    (helper b)))";
    const Lines l = L(src);
    // helper's uses -> its definition.
    // (lines: 2 "(defn helper [x] x)", 3 "(defn f [a]", 4 "  (let [b (helper a)]", 5 "    (helper b)))")
    CljLspLocation d = CljLspDefinition(l, 5, 6);
    CHECK(d.found && d.range.line == 2 && d.range.col == 6);
    // A local -> its binding.
    d = CljLspDefinition(l, 5, 12);
    CHECK(d.found && d.range.line == 4 && d.range.col == 8);
    CHECK(CljLspReferences(l, 2, 7).size() == 3);
    // Rename: a var, a local.
    std::string err;
    std::vector<CljLspTextEdit> e = CljLspRename(l, 2, 7, "assist", &err);
    CHECK(err.empty() && e.size() == 3);
    e = CljLspRename(l, 3, 9, "alpha", &err);
    CHECK(err.empty() && e.size() == 2);
    e = CljLspRename(l, 2, 7, "x y", &err);
    CHECK(e.empty() && !err.empty());
    CHECK(!CljLspPrepareRename(l, 4, 3).found);  // `let`: core's
    // Across files: the project index.
    CljProject p;
    CljProjectAddFile(&p, "/w/src/lib/util.clj", L("(ns lib.util)\n(defn twice\n  \"Doubles.\"\n  [x] (* 2 x))"));
    const Lines use = L("(ns app.core (:require [lib.util :as u]))\n(u/twice 1 2)");
    d = CljLspDefinition(use, 1, 3, "/w/src/app/core.clj", &p);
    CHECK(d.found && d.path == "/w/src/lib/util.clj" && d.range.line == 1);
    CHECK(CljLspHover(use, 1, 3, "/w/src/app/core.clj", &p).text.find("Doubles.") != std::string::npos);
    bool arity = false;
    for (const CljLspDiagnostic &x : CljLspDiagnostics(use, "/w/src/app/core.clj", &p)) arity = arity || x.code == "invalid-arity";
    CHECK(arity);
    CHECK(HasLabel(CljLspCompletions(L("(ns app.core (:require [lib.util :as u]))\n(u/tw"), 1, 6, "", &p), "u/twice"));
}

void TestVocabulary() {
    const CljLspVar *v = CljLspCoreVar("clojure.core", "map");
    CHECK(v && v->kind == "function" && v->arglists.find("[f coll]") != std::string::npos);
    CHECK(CljLspCoreVar("", "if") && CljLspCoreVar("", "if")->kind == "special");
    CHECK(CljLspCoreVar("clojure.core", "when")->kind == "macro");
    CHECK(CljLspCoreVar("clojure.core", "deref-future") && CljLspCoreVar("clojure.core", "deref-future")->is_private);
    CHECK(CljLspNamespaceVars("clojure.string").size() > 15);
}

int ReadCheck() {
    std::string path;
    while (std::getline(std::cin, path)) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const CljReadResult r = CljRead(L(ss.str()), false);
        if (r.errors.empty()) {
            std::cout << path << "\tOK\n";
        } else {
            std::cout << path << "\tERR\t" << r.errors[0].range.line + 1 << "\t" << r.errors[0].message << "\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--read-check") == 0) return ReadCheck();
    TestReader();
    TestDiagnostics();
    TestCompletion();
    TestHoverAndSignatures();
    TestSymbolsAndFolds();
    TestNavigation();
    TestVocabulary();
    std::printf("clojure lsp tests passed (%d checks)\n", checks);
    return 0;
}
