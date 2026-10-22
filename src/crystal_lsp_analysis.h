#pragma once

// The shared analysis behind crystal_lsp.h's features: the parse (with
// recovery for a file being edited), the scopes and every local's
// occurrences, the names the file defines, and a reader's type inference.
// Internal to mep-crystal-lsp.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "crystal_ast.h"
#include "crystal_lsp.h"

namespace crlsp {

using crystal::Node;
using crystal::NodeKind;

enum class ScopeKind { File, Type, Def, Block, Proc };

struct Scope {
    ScopeKind kind = ScopeKind::File;
    int parent = -1;
    CrRange range;              // where it is (for the cursor)
    std::string owner;          // enclosing type path ("" at the top level)
    std::map<std::string, int> vars;  // name -> local id
    bool has_macro = false;           // macro code inside: names may be used there unseen
};

struct Occurrence {
    CrRange range;
    bool write = false;
    bool def = false;  // the binding occurrence
};

struct Local {
    std::string name;
    int scope = 0;
    CrRange def_range;
    std::vector<Occurrence> occs;
    int reads = 0;
    bool is_param = false;        // a def's (or proc's) parameter
    bool is_block_param = false;  // a block's
    bool abstract_owner = false;  // a parameter of an abstract def or one with an empty body
    Node *declared_type = nullptr;
    Node *first_value = nullptr;
    int bind_kind = 0;  // first bound by: 0 an assignment, 1 a multiple assignment, 2 `rescue x`, 3 `out x`
};

enum class NameKind { Call, DefName, Path, InstanceVar, ClassVar };

/** @brief A named occurrence other than a local: a call, a def's name, a type path, an @ivar. */
struct NameOcc {
    NameKind kind = NameKind::Call;
    std::string name;   // method name, "Foo::Bar", "@x"
    std::string owner;  // enclosing type path
    CrRange range;
    Node *node = nullptr;
    bool is_def = false;
    bool write = false;  // an @ivar assignment
    int scope = 0;
};

/** @brief A definition the file makes (or one in the workspace: see CrProjectDef). */
struct Def {
    std::string kind;   // as CrProjectDef::kind, plus "getter"/"setter" for macro-made accessors
    std::string owner;
    std::string name;
    std::string signature;
    std::string doc;
    std::string ret;
    std::string superclass;
    std::vector<std::string> includes;
    bool class_method = false;
    bool is_abstract = false;
    CrRange range;       // the name
    CrRange full_range;  // the whole definition
    Node *node = nullptr;
    int parent = -1;     // enclosing type's index in defs (outline nesting)
};

struct Analysis {
    std::vector<std::string> lines;
    std::string text;
    crystal::Ast ast;
    Node *root = nullptr;
    bool ok = false;        // the file as it stands parses
    bool repaired = false;  // root comes from a copy with broken lines blanked
    std::string error;
    int error_line = 0;  // 0-based
    int error_col = 0;   // byte
    std::vector<std::pair<CrRange, std::string>> warnings;

    std::vector<Scope> scopes;
    std::vector<Local> locals;
    std::vector<std::pair<CrRange, int>> local_occs;  // occurrence -> local id
    std::vector<NameOcc> names;
    std::vector<Def> defs;
    std::vector<std::pair<int, int>> unreachable;  // (line, byte col) of code after a jump: filled by Analyze
    std::vector<CrRange> unreachable_ranges;
    std::vector<std::pair<CrRange, std::string>> shadows;  // a block parameter hiding an outer local
    std::vector<Node *> require_nodes;

    /** @brief 0-based byte column of a 1-based character column on a 1-based line. */
    int ByteCol(int line1, int col1) const;
    /** @brief The range [loc, end] of 1-based character positions (end inclusive), as a byte range. */
    CrRange RangeOf(crystal::Location loc, crystal::Location end) const;
    /** @brief A range starting at loc spanning `chars` characters. */
    CrRange RangeAt(crystal::Location loc, int chars) const;
    CrRange NodeRange(const Node *n) const;
};

/** @brief Parses and analyzes. When the file does not parse, the error is kept and the tree comes from a copy
 *         with the offending lines blanked (up to a bound), so outline and navigation keep working. */
std::unique_ptr<Analysis> Analyze(const std::vector<std::string> &lines);

bool Contains(const CrRange &r, int line, int col);
bool Before(int l1, int c1, int l2, int c2);

/** @brief The innermost scope containing a position. */
int ScopeAt(const Analysis &a, int line, int col);

/** @brief A type expression as Crystal writes it ("Array(String)", "Int32 | Nil"...). */
std::string TypeText(const Node *n);

/** @brief A reader's type of an expression ("" when not visible). */
std::string InferType(const Analysis &a, const Node *n, int depth = 0);

/** @brief The header of a definition as written: "def split(separator : String) : Array(String)". */
std::string HeaderText(const Analysis &a, const Node *n);

/** @brief "Array(String)?" -> "Array", "Foo::Bar | Nil" -> "Foo::Bar". */
std::string BaseType(const std::string &type);

}  // namespace crlsp
