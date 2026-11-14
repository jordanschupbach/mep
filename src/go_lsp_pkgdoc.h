#ifndef MEP_GO_LSP_PKGDOC_H
#define MEP_GO_LSP_PKGDOC_H

// Doc comments of Go packages outside the open file, for mep's Go server
// (hover on `pkg.Name` of a workspace or dependency package, `mep-go-lsp
// --doc` / `--exports`) and the Go UI mode's Help and Exports tabs
// (language_ui_go.h). `go doc` covers the standard library and the module
// cache when the go command is installed; this reads the same sources
// without it, with mep's go/parser port (go_parse.h):
//   - the main module: the go.mod above the directory (or GOFLAGS'
//     -modfile=), its module path mapped to its root;
//   - replace directives (a local directory, or another module@version);
//   - vendor/ (preferred under GOFLAGS -mod=vendor), then the module cache
//     (GOMODCACHE, else GOPATH's first entry or ~/go, + /pkg/mod) at the
//     version go.mod requires, with the cache's !lower escaping;
//   - GOROOT/src for the standard library (GOROOT, else beside `go` on
//     PATH), then GOPATH/src.
// Rosetta's cgo binding of datamunge ships its documentation this way: a
// Go package with godoc comments.

#include <string>
#include <utility>
#include <vector>

namespace gopkgdoc {

struct Entry {
    std::string kind;       // func method type const var field
    std::string name;       // Name, or Type.Name for a method or field
    std::string signature;  // the declaration's source (a func's header, a type's spec, a value's spec or group)
    std::string doc;        // the doc comment's text
    std::string file;
    int line = 0;  // 1-based
};

struct Package {
    bool ok = false;
    std::string dir;
    std::string name;         // the package clause's name
    std::string import_path;  // when known
    std::string doc;          // the package doc comment
    std::vector<Entry> entries;
    std::vector<std::pair<std::string, std::string>> imports;  // (name or "", path) of every file
};

/** @brief The package in DIR: its .go files, minus tests, with only exported names unless ALL. */
Package ReadPackage(const std::string &dir, bool all = false);

/** @brief The same from in-memory sources: (path, text) pairs. */
Package ReadSources(const std::vector<std::pair<std::string, std::string>> &files, bool all = false);

struct Module {
    bool ok = false;
    std::string root;  // the directory holding go.mod
    std::string path;  // its module path
    std::vector<std::pair<std::string, std::string>> require;  // path, version
    struct Replace {
        std::string old_path, old_version, new_path, new_version;
    };
    std::vector<Replace> replace;
};

/** @brief The module DIR belongs to (go.mod at or above DIR; GOFLAGS -modfile= wins). */
Module FindModule(const std::string &dir);

/** @brief go.mod's text parsed (module, require and replace, single lines and blocks). */
Module ParseGoMod(const std::string &text);

/** @brief The module cache's escaping of a path: upper-case letters become '!' + lower case. */
std::string EscapeModulePath(const std::string &path);

/** @brief The directory of import path PATH as seen from DIR, or "" (WHY, when given, says how). */
std::string ResolveImport(const std::string &path, const std::string &dir, std::string *why = nullptr);

/**
 * @brief NAME's documentation as Markdown, in `go doc`'s argument forms: `pkg`, `pkg.Name`,
 * `pkg.Type.Method`, `Name`, `Type.Method`, an import path (`example.com/m/datamunge.Frame`). `pkg`
 * is an import of the context files (or of DIR's package) by its name; a bare name is the package
 * in DIR. CONTEXT holds (path, text) of files whose imports count (an open, unsaved buffer).
 * Returns "" when nothing matches.
 */
std::string Doc(const std::string &name, const std::string &dir,
                const std::vector<std::pair<std::string, std::string>> &context = {});

/**
 * @brief The documentation of member MEMBER (a method or field, promoted ones too) of the value of Go
 * expression EXPR: a variable (`df` from `df, err := datamunge.NewDataFrame()`, `df :=
 * pkg.Must(...)`, `var df *pkg.T`, a parameter), a call `pkg.F(...)` or `x.M(...)` (its first
 * result), or `pkg.Must(...)` around one. Types come from the declarations in TEXT (the source up to
 * the expression) and the documented signatures; imports from CONTEXT (else DIR's package).
 * Returns "" when the type or member is unknown.
 */
std::string MemberDoc(const std::string &expr, const std::string &member, const std::string &dir,
                      const std::vector<std::pair<std::string, std::string>> &context, const std::string &text);

/** @brief The package's exported identifiers, one per line: kind TAB name TAB one-line signature TAB file:line. */
std::string ExportsTable(const Package &pkg);

}  // namespace gopkgdoc

#endif  // MEP_GO_LSP_PKGDOC_H
