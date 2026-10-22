#ifndef MEP_CLOJURE_LSP_ANALYSIS_H
#define MEP_CLOJURE_LSP_ANALYSIS_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "clojure_lsp.h"
#include "clojure_reader.h"

// The shared analysis behind every feature of mep-clojure-lsp: one walk
// over the reader's tree that knows what clojure.core's special forms and
// macros bind and evaluate, producing the file's namespace model, its vars,
// every binding with its scope, and every symbol occurrence resolved to
// what it names. clojure_lsp.cpp (features) and clojure_lsp_diagnostics.cpp
// (lints) read the result; nothing here reports anything itself except the
// shape errors the walk meets (an odd binding vector, `(if)` with one form).

namespace clj {

// What an occurrence of a symbol names.
enum class Target {
    Local,       // a binding in scope (`binding` holds which)
    FileVar,     // a var this file defines (`var` holds which)
    CoreVar,     // clojure.core or another shipped namespace, or a special form
    ProjectVar,  // a var another file of the project defines
    OtherVar,    // `alias/x` or a referred name in a namespace this server does not know
    Class,       // a Java class, interop (`.method`, `Foo.`, `Foo/bar`), or a record/type name
    Special,     // a name only meaningful in place: `&`, `catch`, `finally`, `%`
    Unresolved,
};

enum class Platform { Clj, Cljs, Both };

struct Binding {
    std::string name;
    int node = -1;  // the symbol that binds it
    CljRange range;
    CljRange scope;  // where it is visible
    std::string kind;  // "let", "param", "fn-name", "for", "catch", "field", "macro"
    int uses = 0;
    bool soft = false;    // bound by an unknown macro's vector (no unused report)
    bool exempt = false;  // `_x`, `&form`, a protocol impl's or defmethod's params...
    bool destructured_key = false;  // from {:keys [...]}
};

struct FileVar {
    std::string name;
    std::string kind;  // "defn", "defmacro", "def", "defonce", "defmulti", "defmethod", "defprotocol", "protocol-method",
                       // "defrecord", "deftype", "definterface", "declare", "defstruct", "deftest", "def-macro" (a library's def...)
    int name_node = -1;
    int form = -1;  // the defining form
    int top = -1;   // index of the top-level form it is in
    CljRange range;  // the name
    CljRange form_range;
    std::vector<std::string> arglists;  // "[x y]", "[x & more]" (an :arglists in the metadata wins)
    std::vector<std::pair<int, bool>> arities;  // from the real parameter vectors: fixed count, variadic
    std::string doc;
    bool is_private = false;
    bool declared_only = false;  // a `declare`
    bool in_comment = false;
    bool other_platform = false;  // defined in a :cljs (or other non-JVM) reader-conditional branch
    std::string parent;  // a protocol method's protocol, a record's constructor's record
    int uses = 0;
};

struct Occurrence {
    std::string text;  // as written
    int node = -1;
    CljRange range;
    Target target = Target::Unresolved;
    int binding = -1;  // Local
    int var = -1;      // FileVar
    std::string ns;    // CoreVar / ProjectVar / OtherVar: the namespace; Class: the class
    std::string name;  // the var's name
    bool is_definition = false;
    bool in_comment = false;     // inside (comment ...)
    bool checked = true;         // the name checks apply here (Clojure code, not quoted, not inside an unknown macro's vector)
    int top = -1;
    bool alias_unknown = false;  // `x/y` with `x` neither an alias nor a namespace
    bool not_public = false;     // `ns/x` naming a private var of a shipped namespace
};

// A call `(f a b)`, for argument counts.
struct Call {
    int node = -1;  // the list
    int head = -1;  // the occurrence of the head symbol
    int argc = 0;   // after threading macros inserted theirs
    bool checked = true;
};

struct Require {
    std::string ns;
    std::string alias;
    int node = -1;        // the namespace symbol
    int alias_node = -1;  // the alias symbol
    std::vector<int> refer_nodes;
    bool refer_all = false;
    bool is_use = false;
    bool side_effect = false;  // no alias, no refer: loaded for what it does
};

struct Analysis {
    CljReadResult read;
    std::vector<std::string> lines;
    std::string path;
    std::string ext;  // "clj", "cljc", "cljs", "edn", "bb", ...
    const CljProject *project = nullptr;

    std::string ns_name = "user";
    int ns_form = -1;
    int ns_name_node = -1;
    bool has_in_ns = false;  // (in-ns 'x) without this file defining x

    std::map<std::string, std::string> aliases;  // alias -> namespace
    std::map<std::string, std::string> refers;   // name -> namespace
    std::map<std::string, std::string> renames;  // local name -> core name (:refer-clojure :rename)
    std::vector<Require> reqs;
    std::set<std::string> refer_all;    // namespaces referred whole (:refer :all, :use)
    std::set<std::string> core_excluded;
    bool core_only = false;
    std::set<std::string> core_only_names;
    std::set<std::string> imports;      // simple class names imported

    std::vector<FileVar> vars;
    std::multimap<std::string, int> var_index;  // name -> vars index
    std::vector<Binding> bindings;
    std::vector<Occurrence> occs;
    std::vector<Call> calls;
    std::vector<CljLspDiagnostic> shape;  // errors the walk met
    std::vector<int> keywords;            // keyword nodes, for completion
    std::vector<int> comment_forms;       // (comment ...) forms
    std::set<int> fn_bodies;              // nodes inside a fn body (for def-in-fn)
    bool clj_checks = true;               // the name checks are worth running in this file
    bool own_vocab_ns = false;            // the file is one of Clojure's own namespaces (clojure.core...)
};

/** @brief Reads and analyzes a file. */
Analysis Analyze(const std::vector<std::string> &lines, const std::string &path, const CljProject *project);

/** @brief The occurrence whose symbol covers (line, col), or -1. */
int OccurrenceAt(const Analysis &a, int line, int col);

/** @brief The innermost list containing (line, col), or -1. */
int ListAt(const Analysis &a, int line, int col);

/** @brief The names in scope at a position: locals innermost first. */
std::vector<int> LocalsAt(const Analysis &a, int line, int col);

/** @brief The file vars named `name` (a protocol method, a def, ...), first definition first. */
std::vector<int> FileVarsNamed(const Analysis &a, const std::string &name);

/** @brief The extension of a path, lowercased ("clj", "cljc", ...); "clj" when there is none. */
std::string Extension(const std::string &path);

/** @brief The text of a node as written. */
std::string NodeText(const Analysis &a, int node);

/** @brief Whether a position comes before another. */
bool Before(const CljRange &a, const CljRange &b);

}  // namespace clj

#endif  // MEP_CLOJURE_LSP_ANALYSIS_H
