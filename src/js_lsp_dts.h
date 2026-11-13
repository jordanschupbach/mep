#ifndef MEP_JS_LSP_DTS_H
#define MEP_JS_LSP_DTS_H

// TypeScript declaration files for mep-js-lsp: the `.d.ts` TypeScript
// itself reads to type a module it cannot see -- a native Node addon
// (`.node`), typically, whose only documentation is the declarations
// generated beside it:
//
//     /** Shape kinds. */
//     export declare enum Kind { circle = 0, square = 1 }
//
//     export declare abstract class Shape {
//         /** Area of the shape. */
//         area(): number;
//     }
//
//     /** A circle with area 3r². */
//     export declare class Circle extends Shape {
//         /** @param radius distance from the centre */
//         constructor(radius: number);
//         static unit(): Circle;
//     }
//
//     /** Adds three integers. @param a first term */
//     export declare function add(a: number, b?: number, c?: number): number;
//     export declare function pick(arg0: number): string;   // overloads: one
//     export declare function pick(arg0: string): string;   // declaration each
//
// Read with js_parse's TypeScript dialect (as a `.d.ts` document is), and
// kept as the few things hover, completion and signature help need: each
// declaration with its signatures as written, its JSDoc as markdown, a
// class's `extends` and members, an enum's members, a namespace's contents,
// and what the file exports under which name (`export { A as B }`,
// `export default`, `export =`). Nothing is type-checked.
//
// Which file a `require(...)` / `import` of a JavaScript document reads is
// TypeScript's own lookup (moduleResolution "node10"/"bundler", types only),
// from the document's path:
//
//   - `$MEP_JS_TYPES` (directories separated by ':', ';' on Windows) first:
//     `<dir>/<specifier>.d.ts`, then `<dir>/<specifier>/index.d.ts`;
//   - a relative specifier (`./x`, `../x`, or an absolute path): `x.d.ts`
//     beside `x.js` (`x.d.mts` for `x.mjs`, `x.d.cts` for `x.cjs`, and
//     `x.d.ts` beside a native `x.node`), `x.d.ts` for an extensionless
//     `x`, then the directory `x/`: its package.json's `types` / `typings`
//     (or `exports["."].types`), else `x/index.d.ts`;
//   - a bare one (`pkg`, `@scope/pkg`, `pkg/sub`): the package itself when
//     the nearest package.json above the document is named `pkg` (a
//     package's own tests and examples), then `node_modules/pkg` and
//     `node_modules/@types/pkg` (`@types/scope__pkg`) in each directory up
//     from the document, a package's root read as a directory above and a
//     subpath as a relative specifier inside it.
//
// Parsed files are cached by path and modification time, so a declaration
// file of several megabytes is read once per change, whatever the number of
// edits to the documents that use it.

#include <map>
#include <memory>
#include <string>
#include <vector>

/** @brief One call or construct signature, as written. */
struct JsDtsSignature {
    std::string type_params;  // "<T>" or ""
    std::string params;       // "(a: number, b?: number)"
    std::string ret;          // "number"; "" for a constructor
    std::string doc;          // its JSDoc, as markdown
};

/** @brief A member of a class or interface, or an enum's member. */
struct JsDtsMember {
    enum KindT { Method, Property, EnumMember } kind = Method;
    std::string name;
    bool is_static = false;
    bool readonly = false;
    bool optional = false;
    bool abstract_ = false;
    std::string access;                // "protected", "private" or ""
    std::string type;                  // Property: its type; EnumMember: its value as written
    std::vector<JsDtsSignature> sigs;  // Method: every overload
    std::string doc;                   // Property / EnumMember: the JSDoc
};

struct JsDtsScope;

/** @brief A declared name, with every declaration of it merged (overloads, `interface` + `class`, + `namespace`). */
struct JsDtsDecl {
    enum KindT { Function, Class, Interface, Enum, Variable, Namespace, TypeAlias } kind = Variable;
    std::string name;
    std::string doc;
    bool abstract_ = false;            // Class
    bool const_enum = false;           // Enum
    std::string var_kind;              // Variable: "const", "let" or "var"
    std::string type_params;           // Class / Interface / TypeAlias: "<T = any>" or ""
    std::vector<std::string> bases;    // Class: its `extends`; Interface: each `extends`, as written
    std::vector<JsDtsSignature> sigs;  // Function: the overloads; Class: the constructors
    std::vector<JsDtsMember> members;  // Class / Interface members, Enum members, in file order
    std::string type;                  // Variable: its type; TypeAlias: what it names
    std::shared_ptr<JsDtsScope> ns;    // Namespace, or a namespace merged into a function/class/enum
    const JsDtsScope *scope = nullptr; // where it is declared (names in its types resolve from there)
    int line = 0;                      // 0-based line of its first declaration
};

/** @brief The declarations of a file, a namespace or a `declare module "x"` body. */
struct JsDtsScope {
    const JsDtsScope *parent = nullptr;
    std::map<std::string, JsDtsDecl> decls;      // by local name
    // Exported name -> local name; "default" for `export default X`, "export=" for `export = X`.
    std::map<std::string, std::string> exports;
    bool explicit_exports = false;  // has export syntax (else everything declared is visible)

    /** @brief The declaration `name` names here, or in an enclosing scope. */
    const JsDtsDecl *Find(const std::string &name) const;
    /** @brief What `name` is to an importer of this scope ("" names nothing). */
    const JsDtsDecl *Export(const std::string &name) const;
    /** @brief Every exported name and its declaration, sorted by name (no "default", no "export="). */
    std::vector<std::pair<std::string, const JsDtsDecl *>> Exports() const;
    /** @brief `export = X`'s X, or null. */
    const JsDtsDecl *ExportEquals() const;
};

struct JsDtsModule {
    std::string path;
    JsDtsScope root;
    std::map<std::string, std::shared_ptr<JsDtsScope>> ambient;  // `declare module "name" { ... }`

    /** @brief The class or interface a type names (`Circle`, `Shape | null`, `ns.Base<T>`), or null. */
    const JsDtsDecl *ClassOfType(const JsDtsScope *in, const std::string &type) const;
    /** @brief The member `name` of a class / interface, looking through `extends` (nearest first). */
    const JsDtsMember *Member(const JsDtsDecl *cls, const std::string &name, bool statics,
                              const JsDtsDecl **owner = nullptr) const;
    /** @brief Every member of a class / interface and its bases, nearest first, each name once. */
    std::vector<std::pair<const JsDtsDecl *, const JsDtsMember *>> Members(const JsDtsDecl *cls, bool statics) const;
};

/** @brief Parses a declaration file's text (exposed for tests). */
std::shared_ptr<const JsDtsModule> JsDtsParse(const std::string &text, const std::string &path);

/** @brief `$MEP_JS_TYPES`'s directories. */
std::vector<std::string> JsDtsTypeDirs();

/** @brief The declaration file for `specifier` imported by the document at `doc_path`, or "". */
std::string JsDtsResolve(const std::string &doc_path, const std::string &specifier);

/** @brief The parsed declaration file at `path` (cached by modification time), or null. */
std::shared_ptr<const JsDtsModule> JsDtsLoad(const std::string &path);

// --- rendering (hover markdown) ---------------------------------------------------------------------------------

/** @brief `name<T>(a: T): R` (`name(a: T)` for a constructor). */
std::string JsDtsSignatureLabel(const JsDtsSignature &s, const std::string &name);
std::string JsDtsDeclHover(const JsDtsModule &m, const JsDtsDecl &d);
std::string JsDtsMemberHover(const JsDtsModule &m, const JsDtsDecl &owner, const JsDtsMember &mem);
/** @brief What a module is, for the variable it is bound to: its file and its exports. */
std::string JsDtsModuleNote(const JsDtsModule &m, const JsDtsScope &scope);

#endif  // MEP_JS_LSP_DTS_H
