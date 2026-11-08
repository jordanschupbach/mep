// rust_ast.h -- mep's Rust syntax tree, mirroring rustc_ast::ast.
//
// Shape and spans follow rustc_ast/src/ast.rs (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0), the tree rustc_parse produces before macro expansion: no node
// ids, no tokens, no resolution. rustc's enums with payloads become one
// struct per category with a `kind` tag; each field documents the kinds that
// use it, and unused fields stay null / empty.
//
// Span conventions are rustc's, because rustfmt finds comments by looking at
// the source between spans:
//  - An item's span starts at its visibility / first qualifier and does NOT
//    include its outer attributes; attributes carry their own spans
//    (`span_with_attributes()` adds them back). Same for statements, fields,
//    variants, params, arms, generic params, expressions with attributes.
//  - Spans are half-open byte ranges into the parsed text.
//  - Token ranges (macro bodies, attribute args) are half-open index ranges
//    into ParseResult::tokens, excluding the delimiters themselves.
//
// Nodes live in an Arena owned by the ParseResult; pointers stay valid as
// long as it does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "rust_lex.h"

namespace rust {

// ---------------------------------------------------------------------------
// Arena: bump allocation with destructors run in reverse order.

class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() {
        for (size_t i = dtors_.size(); i-- > 0;) dtors_[i].fn(dtors_[i].p);
    }
    template <class T, class... A>
    T* make(A&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        T* p = new (mem) T(std::forward<A>(args)...);
        if (!std::is_trivially_destructible<T>::value) dtors_.push_back({p, [](void* q) { static_cast<T*>(q)->~T(); }});
        return p;
    }

private:
    void* alloc(size_t n, size_t align) {
        size_t off = (used_ + align - 1) & ~(align - 1);
        if (blocks_.empty() || off + n > cap_) {
            size_t sz = n + align > kBlock ? n + align : kBlock;
            blocks_.emplace_back(new unsigned char[sz]);
            cap_ = sz;
            used_ = 0;
            off = (reinterpret_cast<uintptr_t>(blocks_.back().get()) % align) == 0
                      ? 0
                      : align - reinterpret_cast<uintptr_t>(blocks_.back().get()) % align;
        }
        void* p = blocks_.back().get() + off;
        used_ = off + n;
        return p;
    }
    struct Dtor {
        void* p;
        void (*fn)(void*);
    };
    static constexpr size_t kBlock = 64 * 1024;
    std::vector<std::unique_ptr<unsigned char[]>> blocks_;
    size_t used_ = 0;
    size_t cap_ = 0;
    std::vector<Dtor> dtors_;
};

struct Expr;
struct Pat;
struct Ty;
struct Item;
struct Stmt;
struct Block;
struct GenericArgs;
struct GenericParam;
struct FnDecl;
struct MacCall;
struct Path;
struct UseTree;
struct WherePredicate;

// ---------------------------------------------------------------------------
// Small leaf types.

/// A name. `name` excludes `r#` (see `raw`); for lifetimes it includes the `'`.
struct Ident {
    std::string_view name;
    Span span;
    bool raw = false;
    bool empty() const { return name.empty(); }
};

/// `'a`: `ident.name` is "'a".
struct Lifetime {
    Ident ident;
};

/// A loop / block label `'a:` (the ident is the lifetime, without the colon).
struct Label {
    Ident ident;
};

enum class Mutability : uint8_t { Not, Mut };
enum class Pinnedness : uint8_t { Not, Pinned };

struct Safety {
    enum Kind : uint8_t { Default, Unsafe, Safe } kind = Default;
    Span span;  // of the keyword
};

/// ast::Const
struct Constness {
    bool yes = false;
    Span span;
};

struct Defaultness {
    enum Kind : uint8_t { Implicit, Default, Final } kind = Implicit;
    Span span;
};

struct CoroutineKind {
    enum Kind : uint8_t { None, Async, Gen, AsyncGen } kind = None;
    Span span;  // `async`, `gen` or `async gen`
};

enum class StrStyle : uint8_t { Cooked, Raw };

/// ast::StrLit (an ABI string).
struct StrLit {
    std::string_view symbol;  // contents without quotes
    std::string_view suffix;
    StrStyle style = StrStyle::Cooked;
    uint8_t n_hashes = 0;
    Span span;
};

struct Extern {
    enum Kind : uint8_t { None, Implicit, Explicit } kind = None;
    StrLit abi;  // Explicit
    Span span;   // `extern` plus the ABI string
};

struct FnHeader {
    Constness constness;
    CoroutineKind coroutine_kind;
    Safety safety;
    Extern ext;
};

/// token::Lit plus its span: the literal as written.
struct Lit {
    LitKind kind = LitKind::Err;
    std::string_view symbol;  // rustc's symbol: contents without quotes / prefix, or the number text
    std::string_view suffix;  // empty if none
    uint8_t n_hashes = 0;     // raw strings
    Span span;
};

enum class AttrStyle : uint8_t { Outer, Inner };
enum class DelimKind : uint8_t { Paren, Brace, Bracket };

/// A delimited token tree: `( ... )` / `[ ... ]` / `{ ... }`.
struct DelimArgs {
    DelimKind delim = DelimKind::Paren;
    Span open;               // the opening delimiter
    Span close;              // the closing delimiter
    uint32_t tok_lo = 0;     // inner tokens: ParseResult::tokens[tok_lo, tok_hi)
    uint32_t tok_hi = 0;
    Span span() const { return open.to(close); }
};

/// MacCall: `path!(...)`. The macro's own span lives on the enclosing node.
struct MacCall {
    Path* path = nullptr;
    DelimArgs args;
};

struct AttrArgs {
    enum Kind : uint8_t { Empty, Delimited, Eq } kind = Empty;
    DelimArgs delim;          // Delimited
    Span eq_span;             // Eq
    Expr* expr = nullptr;     // Eq
};

struct Attribute {
    AttrStyle style = AttrStyle::Outer;
    Span span;
    /// A sugared doc comment (`/// x`, `//! x`, `/** x */`, `/*! x */`).
    bool is_doc_comment = false;
    CommentKind comment_kind = CommentKind::Line;  // doc comments
    std::string_view doc;                          // doc comments: text after the marker
    // Normal attributes `#[unsafe(path args)]`:
    Safety unsafety;
    Path* path = nullptr;
    AttrArgs args;
};
using AttrVec = std::vector<Attribute>;

// ---------------------------------------------------------------------------
// Paths and generics.

struct PathSegment {
    Ident ident;
    GenericArgs* args = nullptr;
};

struct Path {
    Span span;
    std::vector<PathSegment> segments;
};

/// `<Ty as Trait>::x`: `position` is the number of segments of the path that
/// belong to the trait (0 for `<Ty>::x`).
struct QSelf {
    Ty* ty = nullptr;
    Span path_span;
    size_t position = 0;
};

/// ast::AnonConst
struct AnonConst {
    Expr* value = nullptr;
    bool direct = false;  // MgcaDisambiguation::Direct
};

struct FnRetTy {
    Ty* ty = nullptr;  // null: FnRetTy::Default
    Span span;         // Default: the empty span where `-> T` would go
    bool is_default() const { return ty == nullptr; }
};

struct GenericBound;
using GenericBounds = std::vector<GenericBound>;

struct GenericArg {
    enum Kind : uint8_t { Lifetime, Type, Const } kind = Type;
    rust::Lifetime lifetime;  // Lifetime
    Ty* ty = nullptr;         // Type
    AnonConst konst;          // Const
};

/// AssocItemConstraint: `Item = T`, `Item: Bound`, `Item<'a> = T`.
struct AssocItemConstraint {
    Ident ident;
    GenericArgs* gen_args = nullptr;
    bool is_equality = true;
    Ty* term_ty = nullptr;   // Equality with a type term
    AnonConst term_const;    // Equality with a const term (term_ty null)
    GenericBounds* bounds = nullptr;  // Bound
    Span span;
};

struct AngleBracketedArg {
    bool is_constraint = false;
    GenericArg arg;
    AssocItemConstraint* constraint = nullptr;
};

struct GenericArgs {
    enum Kind : uint8_t { AngleBracketed, Parenthesized, ParenthesizedElided } kind = AngleBracketed;
    Span span;
    std::vector<AngleBracketedArg> args;  // AngleBracketed
    std::vector<Ty*> inputs;              // Parenthesized
    Span inputs_span;                     // Parenthesized
    FnRetTy output;                       // Parenthesized
};

enum class BoundPolarityKind : uint8_t { Positive, Negative, Maybe };
enum class BoundConstnessKind : uint8_t { Never, Always, Maybe };

struct TraitBoundModifiers {
    BoundConstnessKind constness = BoundConstnessKind::Never;
    Span constness_span;  // `const`, `[const]`, `~const`
    bool async_ = false;
    Span async_span;
    BoundPolarityKind polarity = BoundPolarityKind::Positive;
    Span polarity_span;  // `!` or `?`
};

struct PolyTraitRef {
    std::vector<GenericParam*> bound_generic_params;  // for<'a>
    TraitBoundModifiers modifiers;
    Path* trait_ref = nullptr;
    Span span;
    bool parens = false;  // `(Trait)`
};

struct PreciseCapturingArg {
    bool is_lifetime = false;
    rust::Lifetime lifetime;
    Path* path = nullptr;
};

struct GenericBound {
    enum Kind : uint8_t { Trait, Outlives, Use } kind = Trait;
    PolyTraitRef trait;                      // Trait
    rust::Lifetime lifetime;                 // Outlives
    std::vector<PreciseCapturingArg> use_args;  // Use: use<'a, T>
    Span use_span;                           // Use
    Span span() const;
};

struct GenericParam {
    enum Kind : uint8_t { Lifetime, Type, Const } kind = Type;
    Ident ident;
    AttrVec attrs;
    GenericBounds bounds;
    Ty* default_ty = nullptr;      // Type
    Ty* const_ty = nullptr;        // Const
    Span const_span;               // Const: from `const` to the end of the type
    AnonConst* const_default = nullptr;  // Const
    bool has_colon = false;
    Span colon_span;
    Span span() const;
};

struct WhereClause {
    bool has_where_token = false;
    std::vector<WherePredicate*> predicates;
    Span span;
};

struct WherePredicate {
    enum Kind : uint8_t { Bound, Region, Eq } kind = Bound;
    AttrVec attrs;
    Span span;
    std::vector<GenericParam*> bound_generic_params;  // Bound: for<'a>
    Ty* bounded_ty = nullptr;                         // Bound
    GenericBounds bounds;                             // Bound, Region
    rust::Lifetime lifetime;                          // Region
    Ty* lhs_ty = nullptr;                             // Eq
    Ty* rhs_ty = nullptr;                             // Eq
};

struct Generics {
    std::vector<GenericParam*> params;
    WhereClause where_clause;
    Span span;  // of `<...>` (empty at the name's end when there is none)
};

// ---------------------------------------------------------------------------
// Types.

enum class TyKind : uint8_t {
    Slice,          // [inner]
    Array,          // [inner; len]
    Ptr,            // *const inner / *mut inner (mutbl)
    Ref,            // &'a mut inner (lifetime, mutbl)
    PinnedRef,      // &'a pin mut inner
    FnPtr,          // fn_ptr
    UnsafeBinder,   // unsafe<'a> inner (generic_params)
    Never,          // !
    Tup,            // (elems...)
    Path,           // qself? path
    TraitObject,    // dyn bounds / bare bounds (dyn_syntax)
    ImplTrait,      // impl bounds
    Paren,          // (inner)
    Typeof,         // typeof(len) -- rustc reports it, kept for recovery
    Infer,          // _
    ImplicitSelf,   // the type of a `self` param
    MacCall,        // mac
    CVarArgs,       // ... in a foreign fn
    Pat,            // inner is pat_ty
    FieldOf,        // builtin # field_of(inner, variant?.field)
    Err,
    Dummy,
};

struct FnPtrTy {
    Safety safety;
    Extern ext;
    std::vector<GenericParam*> generic_params;
    FnDecl* decl = nullptr;
    Span decl_span;
};

struct TyPat;

struct Ty {
    TyKind kind = TyKind::Err;
    Span span;
    Ty* inner = nullptr;            // Slice, Array, Ptr, Ref, PinnedRef, UnsafeBinder, Paren, Pat, FieldOf
    AnonConst len;                  // Array, Typeof
    Mutability mutbl = Mutability::Not;  // Ptr, Ref, PinnedRef
    bool has_lifetime = false;      // Ref, PinnedRef
    rust::Lifetime lifetime;        // Ref, PinnedRef
    FnPtrTy* fn_ptr = nullptr;      // FnPtr
    std::vector<GenericParam*> generic_params;  // UnsafeBinder
    std::vector<Ty*> elems;         // Tup
    QSelf* qself = nullptr;         // Path
    rust::Path* path = nullptr;     // Path
    GenericBounds bounds;           // TraitObject, ImplTrait
    bool dyn_syntax = false;        // TraitObject: `dyn` written
    rust::MacCall* mac = nullptr;   // MacCall
    TyPat* pat = nullptr;           // Pat
    bool has_variant = false;       // FieldOf
    Ident variant;                  // FieldOf
    Ident field;                    // FieldOf
};

enum class RangeEndKind : uint8_t { Excluded, IncludedDotDotDot, IncludedDotDotEq };

struct TyPat {
    enum Kind : uint8_t { Range, NotNull, Or, Err } kind = Err;
    Span span;
    AnonConst* start = nullptr;   // Range
    AnonConst* end = nullptr;     // Range
    RangeEndKind range_end = RangeEndKind::Excluded;
    Span range_end_span;
    std::vector<TyPat*> alts;     // Or
};

// ---------------------------------------------------------------------------
// Patterns.

enum class PatKind : uint8_t {
    Missing,      // an elided parameter pattern (`fn f(u8)` in 2015 trait items)
    Wild,         // _
    Ident,        // ref? mut? ident (@ sub)?   (by_ref, by_ref_mutbl, pinned, mutbl, ident, sub)
    Struct,       // qself? path { fields, .. }  (fields, rest)
    TupleStruct,  // qself? path(elems)
    Or,           // elems with `|`
    Path,         // qself? path
    Tuple,        // (elems)
    Box,          // box sub
    Deref,        // deref!(sub)
    Ref,          // &mut sub / &pin mut sub (mutbl, pinned)
    Expr,         // a literal / const block / path expression (expr)
    Range,        // lo? .. hi? (range_lo, range_hi, range_end)
    Slice,        // [elems]
    Rest,         // ..
    Never,        // !
    Guard,        // (sub if guard)
    Paren,        // (sub)
    MacCall,      // mac
    Err,
};

struct PatField {
    Ident ident;
    Pat* pat = nullptr;
    bool is_shorthand = false;
    AttrVec attrs;
    Span span;
};

struct Guard;

struct Pat {
    PatKind kind = PatKind::Err;
    Span span;
    // Ident
    bool by_ref = false;
    Mutability by_ref_mutbl = Mutability::Not;
    Pinnedness pinned = Pinnedness::Not;  // Ident (ref pin), Ref (&pin)
    Mutability mutbl = Mutability::Not;   // Ident (`mut x`), Ref
    rust::Ident ident;
    Pat* sub = nullptr;                   // Ident (@ sub), Box, Deref, Ref, Paren, Guard
    // Struct / TupleStruct / Path
    QSelf* qself = nullptr;
    rust::Path* path = nullptr;
    std::vector<PatField> fields;         // Struct
    enum class Rest : uint8_t { None, Rest, Recovered } rest = Rest::None;  // Struct
    Span rest_span;                       // Struct, Rest::Rest
    std::vector<Pat*> elems;              // TupleStruct, Or, Tuple, Slice
    Expr* expr = nullptr;                 // Expr
    Expr* range_lo = nullptr;             // Range
    Expr* range_hi = nullptr;             // Range
    RangeEndKind range_end = RangeEndKind::Excluded;
    Span range_end_span;
    rust::Guard* guard = nullptr;         // Guard
    rust::MacCall* mac = nullptr;         // MacCall
};

// ---------------------------------------------------------------------------
// Expressions.

enum class BinOpKind : uint8_t { Add, Sub, Mul, Div, Rem, And, Or, BitXor, BitAnd, BitOr, Shl, Shr, Eq, Lt, Le, Ne, Ge, Gt };
enum class AssignOpKind : uint8_t { AddAssign, SubAssign, MulAssign, DivAssign, RemAssign, BitXorAssign, BitAndAssign, BitOrAssign, ShlAssign, ShrAssign };
enum class UnOp : uint8_t { Deref, Not, Neg };
enum class BorrowKind : uint8_t { Ref, Raw, Pin };
enum class RangeLimits : uint8_t { HalfOpen, Closed };
enum class CaptureBy : uint8_t { Ref, Value, Use };
enum class Movability : uint8_t { Movable, Static };
enum class MatchKind : uint8_t { Prefix, Postfix };
enum class YieldKind : uint8_t { Prefix, Postfix };
enum class ForLoopKind : uint8_t { For, ForAwait };
enum class GenBlockKind : uint8_t { Async, Gen, AsyncGen };
enum class UnsafeBinderCastKind : uint8_t { Wrap, Unwrap };
enum class BlockCheckMode : uint8_t { Default, Unsafe };

enum class ExprKind : uint8_t {
    Array,        // [exprs...]
    ConstBlock,   // const { block }: anon_const.value is the block expr
    Call,         // lhs(exprs...)
    MethodCall,   // lhs.seg::<..>(exprs...): seg, lhs = receiver, method_span = `seg(args)`
    Tup,          // (exprs...)
    Binary,       // lhs binop rhs (op_span)
    Unary,        // unop lhs
    Move,         // move(lhs) -- `move` expressions (unstable)
    Lit,          // lit
    Cast,         // lhs as ty
    Type,         // lhs: ty (builtin # type_ascribe)
    Let,          // let pat = lhs (let_span is `let pat = lhs` without attrs)
    If,           // if lhs block (else rhs)?
    While,        // label? while lhs block
    ForLoop,      // label? for pat in lhs block (for_kind)
    Loop,         // label? loop block (loop_span: the `loop` keyword)
    Match,        // lhs match { arms }   (match_kind)
    Closure,      // closure
    Block,        // label? unsafe? block
    Gen,          // async/gen move? block (capture, gen_kind, kw_span)
    Await,        // lhs.await (kw_span)
    Use,          // lhs.use (kw_span)
    TryBlock,     // try block / try bikeshed ty block
    Assign,       // lhs = rhs (op_span)
    AssignOp,     // lhs op= rhs (assignop, op_span)
    Field,        // lhs.ident
    Index,        // lhs[rhs] (bracket_span)
    Range,        // lhs? limits rhs?
    Underscore,   // _
    Path,         // qself? path
    AddrOf,       // &raw? mut? lhs (borrow, mutbl)
    Break,        // break label? lhs?
    Continue,     // continue label?
    Ret,          // return lhs?
    InlineAsm,    // (not produced by the parser: asm! stays a MacCall)
    OffsetOf,     // builtin # offset_of(ty, fields...)
    MacCall,      // mac
    Struct,       // qself? path { fields, ..rest }
    Repeat,       // [lhs; anon_const]
    Paren,        // (lhs)
    Try,          // lhs?
    Yield,        // yield lhs? / lhs.yield (yield_kind)
    Yeet,         // do yeet lhs?
    Become,       // become lhs
    IncludedBytes,
    FormatArgs,   // (not produced by the parser)
    UnsafeBinderCast,  // builtin # wrap_binder(lhs, ty?)
    Err,
    Dummy,
};

enum class StructRestKind : uint8_t { None, Base, Rest, NoneWithError };

struct ExprField {
    AttrVec attrs;
    Span span;
    rust::Ident ident;
    Expr* expr = nullptr;
    bool is_shorthand = false;
};

struct Guard {
    Expr* cond = nullptr;
    Span span_with_leading_if;
};

struct Arm {
    AttrVec attrs;
    Pat* pat = nullptr;
    Guard* guard = nullptr;
    Expr* body = nullptr;  // null for a never pattern arm `!,`
    Span span;
    bool has_comma = false;  // a `,` follows the arm (convenience for formatters)
};

struct ClosureBinder {
    bool present = false;
    Span span;  // for<..>
    std::vector<GenericParam*> generic_params;
};

struct Closure {
    ClosureBinder binder;
    CaptureBy capture = CaptureBy::Ref;
    Span capture_kw;  // `move` / `use`
    Constness constness;
    CoroutineKind coroutine_kind;
    Movability movability = Movability::Movable;
    FnDecl* fn_decl = nullptr;
    Expr* body = nullptr;
    Span fn_decl_span;  // from the first qualifier to the end of the return type
    Span fn_arg_span;   // |args|
};

struct Expr {
    ExprKind kind = ExprKind::Err;
    Span span;
    AttrVec attrs;
    Expr* lhs = nullptr;      // operand / receiver / callee / condition / scrutinee / base / inner
    Expr* rhs = nullptr;      // right operand, index, else branch (If), range end
    std::vector<Expr*> exprs; // Array, Call / MethodCall args, Tup
    BinOpKind binop = BinOpKind::Add;
    AssignOpKind assignop = AssignOpKind::AddAssign;
    UnOp unop = UnOp::Deref;
    Span op_span;             // Binary / AssignOp / Assign operator; Index brackets; Let `let`
    rust::Lit lit;            // Lit
    rust::Ty* ty = nullptr;   // Cast, Type, OffsetOf, TryBlock bikeshed, UnsafeBinderCast
    rust::Pat* pat = nullptr; // Let, ForLoop
    rust::Block* block = nullptr;  // If, While, ForLoop, Loop, Block, Gen, TryBlock
    bool has_label = false;
    rust::Label label;        // While, ForLoop, Loop, Block, Break, Continue
    Span kw_span;             // Loop `loop`, Await/Use `await`/`use`, Gen `async`..., Move `move`
    ForLoopKind for_kind = ForLoopKind::For;
    MatchKind match_kind = MatchKind::Prefix;
    std::vector<Arm> arms;    // Match
    rust::Closure* closure = nullptr;  // Closure
    CaptureBy capture = CaptureBy::Ref;  // Gen
    GenBlockKind gen_kind = GenBlockKind::Async;  // Gen
    RangeLimits limits = RangeLimits::HalfOpen;   // Range
    QSelf* qself = nullptr;   // Path, Struct
    rust::Path* path = nullptr;  // Path, Struct
    PathSegment seg;          // MethodCall
    rust::Ident ident;        // Field: the field name
    Span method_span;         // MethodCall: from the segment to the `)`
    BorrowKind borrow = BorrowKind::Ref;   // AddrOf
    Mutability mutbl = Mutability::Not;    // AddrOf
    std::vector<rust::Ident> fields;       // OffsetOf
    rust::MacCall* mac = nullptr;          // MacCall
    std::vector<ExprField> struct_fields;  // Struct
    StructRestKind rest = StructRestKind::None;  // Struct (Base: rhs is the base)
    Span rest_span;           // Struct: the `..`
    AnonConst anon_const;     // ConstBlock, Repeat
    YieldKind yield_kind = YieldKind::Prefix;
    UnsafeBinderCastKind binder_cast = UnsafeBinderCastKind::Wrap;
    bool recovered = false;   // Let: Recovered::Yes
};

// ---------------------------------------------------------------------------
// Statements and blocks.

enum class StmtKind : uint8_t { Let, Item, Expr, Semi, Empty, MacCall };
enum class MacStmtStyle : uint8_t { Semicolon, Braces, NoBraces };
enum class LocalKind : uint8_t { Decl, Init, InitElse };

struct Local {
    bool is_super = false;
    Span super_span;
    Pat* pat = nullptr;
    Ty* ty = nullptr;
    bool has_colon = false;
    Span colon_span;
    LocalKind kind = LocalKind::Decl;
    Expr* init = nullptr;    // Init, InitElse
    Block* els = nullptr;    // InitElse
    Span span;               // `let` (or `super`) through the `;` when there is one (rustc's Local span)
    AttrVec attrs;
};

struct MacCallStmt {
    rust::MacCall* mac = nullptr;
    MacStmtStyle style = MacStmtStyle::NoBraces;
    AttrVec attrs;
};

struct Stmt {
    StmtKind kind = StmtKind::Empty;
    Span span;
    Local* local = nullptr;         // Let
    rust::Item* item = nullptr;     // Item
    rust::Expr* expr = nullptr;     // Expr, Semi
    MacCallStmt* mac = nullptr;     // MacCall
};

struct Block {
    std::vector<Stmt*> stmts;
    BlockCheckMode rules = BlockCheckMode::Default;
    Span span;  // `{` .. `}` (an `unsafe` keyword is outside it)
};

// ---------------------------------------------------------------------------
// Functions.

struct Param {
    AttrVec attrs;
    Ty* ty = nullptr;
    Pat* pat = nullptr;
    Span span;
    /// A `self` parameter (`self`, `mut self`, `&self`, `&'a mut self`,
    /// `&pin mut self`, `self: T`): pat is Ident(self) and ty is ImplicitSelf,
    /// Ref(ImplicitSelf), PinnedRef(ImplicitSelf) or the explicit type.
    bool is_self() const;
};

struct FnDecl {
    std::vector<Param*> inputs;
    FnRetTy output;
    bool c_variadic() const;  // last input's type is CVarArgs
};

struct FnSig {
    FnHeader header;
    FnDecl* decl = nullptr;
    Span span;  // from the first qualifier to the end of the return type / where clause start
};

struct FnContract {
    Expr* requires_ = nullptr;
    Expr* ensures = nullptr;
};

// ---------------------------------------------------------------------------
// Items.

struct Visibility {
    enum Kind : uint8_t { Public, Restricted, Inherited } kind = Inherited;
    Span span;               // Inherited: empty at the item's start
    rust::Path* path = nullptr;  // Restricted
    bool shorthand = false;  // Restricted: pub(crate) / pub(self) / pub(super) without `in`
};

struct ImplRestriction {
    bool restricted = false;
    rust::Path* path = nullptr;
    bool shorthand = false;
    Span span;
};

struct FieldDef {
    AttrVec attrs;
    Span span;
    Visibility vis;
    Safety safety;
    bool has_ident = false;  // named field
    Ident ident;
    Ty* ty = nullptr;
    AnonConst* default_ = nullptr;  // `field: T = expr`
};

struct VariantData {
    enum Kind : uint8_t { Struct, Tuple, Unit } kind = Unit;
    std::vector<FieldDef*> fields;
    bool recovered = false;  // Struct
};

struct Variant {
    AttrVec attrs;
    Span span;
    Visibility vis;
    Ident ident;
    VariantData data;
    AnonConst* disr_expr = nullptr;
};

struct UseTree {
    enum Kind : uint8_t { Simple, Nested, Glob } kind = Simple;
    rust::Path* prefix = nullptr;
    bool has_rename = false;
    Ident rename;                    // Simple: `as rename` (may be `_`)
    std::vector<UseTree*> items;     // Nested
    Span nested_span;                // Nested: `{ ... }`
    Span glob_span;                  // Glob: `*`
    Span span;                       // the whole tree
};

enum class ItemKind : uint8_t {
    ExternCrate,  // extern crate orig_name as ident;  (has_orig_name / orig_name)
    Use,          // use_tree
    Static,       // safety? static mut? ident: ty = expr;
    Const,        // defaultness const ident<generics>: ty = expr;  (type_const for `type const`)
    ConstBlock,   // const { block }  (an item-level const block)
    Fn,           // defaultness sig ident<generics>(..) body
    Mod,          // safety mod ident { items } / mod ident;  (mod_inline)
    ForeignMod,   // unsafe? extern "abi" { items }
    GlobalAsm,    // (not produced by the parser: global_asm! stays a MacCall)
    TyAlias,      // defaultness type ident<generics>: bounds where .. = ty where ..;
    Enum,         // enum ident<generics> { variants }
    Struct,       // struct ident<generics> data
    Union,        // union ident<generics> data
    Trait,        // impl_restriction const? unsafe? auto? trait ident<generics>: bounds { items }
    TraitAlias,   // const? trait ident<generics> = bounds;
    Impl,         // impl<generics> const? (unsafe? !? Trait for)? self_ty { items }
    MacCall,      // mac (with or without `;`)
    MacroDef,     // macro_rules! ident { body } / macro ident(..) { .. }
    Delegation,   // reuse qself? path as rename { body }
    DelegationMac,  // reuse prefix::{a, b as c} / reuse prefix::*
};

/// Which list an item came from: rustc's Item / AssocItem / ForeignItem.
enum class ItemContext : uint8_t { Free, Trait, Impl, Foreign };

struct DelegationSuffix {
    Ident ident;
    bool has_rename = false;
    Ident rename;
};

struct Item {
    ItemKind kind = ItemKind::MacCall;
    ItemContext ctx = ItemContext::Free;
    AttrVec attrs;
    Span span;
    Visibility vis;
    Ident ident;                    // every kind with a name
    Generics generics;              // Const, Fn, TyAlias, Enum, Struct, Union, Trait, TraitAlias, Impl
    Defaultness defaultness;        // Const, Fn, TyAlias, Impl (of_trait)
    Safety safety;                  // Static, Mod, ForeignMod, Trait, Impl (of_trait)
    Constness constness;            // Trait, TraitAlias, Impl
    Mutability mutbl = Mutability::Not;  // Static
    Ty* ty = nullptr;               // Static, Const, TyAlias (optional), Impl self_ty
    Expr* expr = nullptr;           // Static, Const (optional)
    bool type_const = false;        // Const: `type const X: T = ..;`
    // ExternCrate
    bool has_orig_name = false;
    Ident orig_name;
    // Use
    UseTree* use_tree = nullptr;
    // Fn
    FnSig sig;
    Block* body = nullptr;          // Fn (null for `;`), ConstBlock, Delegation
    FnContract* contract = nullptr;
    // Mod
    bool mod_inline = false;        // `mod x { ... }` (false for `mod x;`)
    Span mod_inner_span;            // between the braces (Mod) / the whole file (crate)
    bool had_parse_error = false;
    // Mod, ForeignMod, Trait, Impl
    std::vector<Item*> items;
    // ForeignMod
    Span extern_span;
    bool has_abi = false;
    StrLit abi;
    // TyAlias, Trait, TraitAlias
    GenericBounds bounds;
    WhereClause after_where_clause; // TyAlias: `where` after `= ty`
    // Enum
    std::vector<Variant*> variants;
    // Struct, Union
    VariantData data;
    // Trait
    ImplRestriction impl_restriction;
    bool is_auto = false;
    // Impl
    bool of_trait = false;          // `impl Trait for T`
    bool negative = false;          // `impl !Trait for T`
    Span negative_span;
    Path* trait_ref = nullptr;
    // MacCall
    MacCall* mac = nullptr;
    // MacroDef
    DelimArgs macro_body;           // the body tokens; for `macro m(..) {..}` the whole `(..) {..}` lives in tok range [body.tok_lo-1, ...]
    bool macro_rules = false;
    // Delegation / DelegationMac
    QSelf* qself = nullptr;
    Path* path = nullptr;           // Delegation path / DelegationMac prefix
    bool has_rename = false;
    Ident rename;
    bool glob = false;              // DelegationMac `::*`
    Span glob_span;
    std::vector<DelegationSuffix> suffixes;

    Span span_with_attributes() const {
        Span s = span;
        for (const Attribute& a : attrs) s = s.to(a.span);
        return s;
    }
};

struct Crate {
    AttrVec attrs;
    std::vector<Item*> items;
    Span inner_span;  // the whole file
};

// ---------------------------------------------------------------------------
// Inline helpers.

inline Span GenericBound::span() const {
    switch (kind) {
    case Trait: return trait.span;
    case Outlives: return lifetime.ident.span;
    case Use: return use_span;
    }
    return Span();
}

const char* binop_str(BinOpKind k);
const char* assignop_str(AssignOpKind k);
const char* unop_str(UnOp k);
const char* expr_kind_name(ExprKind k);
const char* pat_kind_name(PatKind k);
const char* ty_kind_name(TyKind k);
const char* item_kind_name(ItemKind k);

}  // namespace rust
