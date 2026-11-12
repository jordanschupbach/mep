#ifndef MEP_LUA_LSP_META_H
#define MEP_LUA_LSP_META_H

// LuaCATS "meta" stubs for mep-lua-lsp: the files lua-language-server reads
// to document a module it cannot see (a native C module, typically), in the
// format
//
//     ---@meta datamunge
//     ---@class datamunge
//     local datamunge = {}
//
//     ---Adds two numbers.
//     ---@param a integer first term
//     ---@param b? integer second term
//     ---@return integer # the sum
//     function datamunge.add(a, b) end
//
//     ---@class datamunge.Dual: datamunge.Base
//     ---@field x number
//     ---@overload fun(value: number): datamunge.Dual
//     datamunge.Dual = {}
//     function datamunge.Dual:multiply(other) end
//
//     ---@enum datamunge.Alternative
//     datamunge.Alternative = { Less = 0, Greater = 1 }
//
//     return datamunge
//
// Only the annotations, never run. Where they come from is the
// lua-language-server convention: the `workspace.library` directories of the
// nearest `.luarc.json` / `.luarc.jsonc` above a document (relative to that
// file's directory), plus `$MEP_LUA_LIBRARY` (directories separated by ':',
// searched first). `require("a.b")` finds `<lib>/a/b.lua` or
// `<lib>/a/b/init.lua` when the file says `---@meta` near its top.
//
// Parsed stubs are cached by path and modification time, so a stub of
// several megabytes is read once per change, whatever the number of edits.

#include <map>
#include <memory>
#include <string>
#include <vector>

struct LuaMetaParam {
    std::string name;
    std::string type;  // "" when not annotated
    std::string doc;
    bool optional = false;
};

struct LuaMetaReturn {
    std::string type;
    std::string name;
    std::string doc;
};

struct LuaMetaFunc {
    std::string owner;  // the type it belongs to ("datamunge", "datamunge.Dual")
    std::string name;
    bool method = false;  // defined with ':' (an implicit self)
    std::vector<LuaMetaParam> params;
    std::vector<LuaMetaReturn> returns;
    std::vector<std::string> overloads;  // extra "fun(...): T" signatures
    std::string doc;
    bool deprecated = false;
    int line = 0;  // 0-based line in the stub
};

struct LuaMetaField {
    std::string name;
    std::string type;
    std::string doc;
};

struct LuaMetaClass {
    std::string name;
    std::vector<std::string> bases;
    std::string doc;
    std::vector<LuaMetaField> fields;
    std::vector<std::string> overloads;  // "fun(...): T": calling the class table constructs
    int line = 0;
};

struct LuaMetaEnum {
    std::string name;
    std::string doc;
    std::vector<std::pair<std::string, std::string>> members;  // name, value as written
    int line = 0;
};

/** @brief A member of a type: what `T.name` / `obj:name` can be. */
struct LuaMetaMember {
    enum KindT { Function, Field, Class, Enum } kind = Function;
    std::string name;
    std::string type;               // Field: its type; Class / Enum: the type's name
    std::string doc;                // Field: its doc
    const LuaMetaFunc *func = nullptr;  // Function
};

struct LuaMetaModule {
    std::string path;
    std::string meta_name;    // ---@meta NAME
    std::string root_type;    // the type of what the stub returns (the module table)
    std::map<std::string, LuaMetaClass> classes;
    std::map<std::string, LuaMetaEnum> enums;
    std::vector<std::unique_ptr<LuaMetaFunc>> funcs;
    std::map<std::string, std::vector<LuaMetaMember>> members;  // by owner type, in stub order

    const LuaMetaClass *Class(const std::string &name) const;
    const LuaMetaEnum *Enum(const std::string &name) const;
    /** @brief The members `name` of `type`, following `---@class T: Base` (own members first). */
    const LuaMetaMember *Member(const std::string &type, const std::string &name) const;
    /** @brief Every member of `type` and its bases, nearest first, each name once. */
    std::vector<const LuaMetaMember *> Members(const std::string &type) const;
};

/** @brief Parses a stub's text (exposed for tests). */
std::shared_ptr<const LuaMetaModule> LuaMetaParse(const std::string &text, const std::string &path);

/** @brief The library directories for a document: $MEP_LUA_LIBRARY, then the nearest .luarc.json's. */
std::vector<std::string> LuaMetaLibraryDirs(const std::string &doc_path);

/** @brief The stub for `require(name)` in those directories (parsed, cached by mtime), or null. */
std::shared_ptr<const LuaMetaModule> LuaMetaFind(const std::vector<std::string> &dirs, const std::string &name);

// --- rendering --------------------------------------------------------------------------------------------------

/** @brief The type with `?`, `[]`, generics and unions reduced to the one class it names, or "". */
std::string LuaMetaClassOfType(const std::string &type);
/** @brief `b?: integer`, as a signature shows a parameter. */
std::string LuaMetaParamText(const LuaMetaParam &p);
/** @brief `name(a: T, b?: U)` (the label signature help shows). */
std::string LuaMetaSignature(const LuaMetaFunc &f, const std::string &display);
/** @brief Hover markdown for a function shown as `display` (e.g. "m.add" or "x:multiply"). */
std::string LuaMetaFuncHover(const LuaMetaModule &m, const LuaMetaFunc &f, const std::string &display);
std::string LuaMetaClassHover(const LuaMetaModule &m, const LuaMetaClass &c);
std::string LuaMetaEnumHover(const LuaMetaModule &m, const LuaMetaEnum &e);
/** @brief The parameter names and types out of a `fun(a: T, b?: U): R` overload. */
std::vector<LuaMetaParam> LuaMetaFunParams(const std::string &fun);
/** @brief What a `fun(...): R` overload returns ("" for none). */
std::string LuaMetaFunReturn(const std::string &fun);

#endif  // MEP_LUA_LSP_META_H
