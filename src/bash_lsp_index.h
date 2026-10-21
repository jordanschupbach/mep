#ifndef MEP_BASH_LSP_INDEX_H
#define MEP_BASH_LSP_INDEX_H

// Private to mep-bash-lsp's analysis files (bash_lsp*.cpp): the parse of a
// document plus every variable and function occurrence in it, resolved to
// a scope. See bash_lsp.h for the scoping rules.

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bash_parse.h"

namespace bash_lsp {

enum class OccKind { Read, Write, Declare };

struct Occurrence {
    std::string name;
    BashRange range;
    OccKind kind = OccKind::Read;
    const BashCommand *fn = nullptr;     // innermost enclosing function, null at top level
    const BashCommand *scope = nullptr;  // the function the name is local to, null for a global
    bool renameable = true;              // the name is written out literally here
    bool quoted = false;                 // a read inside double quotes
    bool in_arith = false;               // a read in arithmetic
    bool reads_too = false;              // a write that also reads: ${x:=default}, x+=1, ((x++))
    bool guarded = false;                // ${x:-...}, ${x+...}, ${x?...}, [[ -v x ]]: unset is expected
};

struct FunctionDef {
    std::string name;
    BashRange name_range;
    const BashCommand *cmd = nullptr;
    const BashCommand *parent = nullptr;  // the function it is defined in
    bool conditional = false;  // inside an if/case/loop/&&/|| rather than unconditional
};

// What a variable's first assignment says about it.
struct VarInfo {
    bool array = false;      // assigned with =( ) or declared -a/-A
    bool scalar = false;     // assigned a plain value somewhere
    bool readonly = false;
    bool exported = false;
    bool nameref = false;    // declare -n: reads and writes go elsewhere
    bool integer = false;
};

struct Index {
    std::vector<std::string> lines;
    BashParseResult parse;
    std::vector<Occurrence> vars;   // variable occurrences, in source order
    std::vector<Occurrence> funcs;  // function definitions and calls
    std::vector<FunctionDef> functions;
    std::map<const BashCommand *, std::set<std::string>> locals;
    // Keyed by scope (null = global) and name.
    std::map<std::pair<const BashCommand *, std::string>, VarInfo> info;

    // File-wide facts the name checks calibrate on.
    bool uses_eval = false;          // eval, or a string handed to bash -c / trap
    bool sources_files = false;      // source / .
    bool indirect_prefix = false;    // ${!prefix*}
    bool set_e = false;              // set -e / set -o errexit / #!... -e
    bool indirect_any = false;       // ${!x}: some variable is read through its name
    bool has_shebang = false;
    bool posix_sh = false;           // #!/bin/sh, dash, ash: no bashisms
    std::vector<std::string> indirect_names;  // names reached through ${!x}, declare -n
    // Names written as $name inside single-quoted strings: code handed to
    // something that runs it later (compgen -W, trap, a test harness).
    std::set<std::string> string_refs;

    /** @brief The occurrences (variables or functions) touching a position, innermost first. */
    const Occurrence *VarAt(int line, int col) const;
    const Occurrence *FuncAt(int line, int col) const;
    /** @brief Whether a function of this name is defined anywhere in the file. */
    bool DefinesFunction(const std::string &name) const;
};

/** @brief Parses and indexes a document. */
std::unique_ptr<Index> BuildIndex(const std::vector<std::string> &lines);

/** @brief The literal text of a word when it is one unquoted literal (no quotes, escapes or expansions). */
bool PlainWord(const BashWord &w, std::string *out);

/** @brief The first character offset of a line that is not a blank, or -1 for a blank line. */
int FirstNonBlank(const std::string &line);

/** @brief Whether a variable is one bash or the environment provides (PATH, HOME, BASH_SOURCE, ...). */
bool IsKnownShellVariable(const std::string &name);

/** @brief Walks every command, with its enclosing function and whether it runs conditionally. */
struct CommandWalker {
    virtual ~CommandWalker() = default;
    virtual void Visit(const BashCommand &cmd, const BashCommand *fn, bool conditional, const BashAndOr *andor,
                       const BashPipeline *pipeline) = 0;
};
void WalkCommands(const BashList &list, CommandWalker &w);

}  // namespace bash_lsp

#endif  // MEP_BASH_LSP_INDEX_H
