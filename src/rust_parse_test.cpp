// rust_parse_test.cpp -- tests for mep's Rust lexer + parser, plus the
// command-line modes tools/rust_parse_check.py drives:
//
//   mep-rust-parse-test                      # unit tests
//   mep-rust-parse-test --check FILE...      # FILE:LINE:COL: message per rejected file
//   mep-rust-parse-test --edition 2018 --check FILE...
//   mep-rust-parse-test --dump FILE          # S-expression tree with spans
//   mep-rust-parse-test --time FILE...       # parse everything, print the time
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "rust_parse.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ---------------------------------------------------------------------------
// Edition, by the oracle's rules (tools/ts_grammar_diff/rust_check.py): a
// compiletest `//@ edition:` header, else the nearest Cargo.toml, else 2021.

std::map<std::string, std::string> g_toml_cache;

const std::string& toml_text(const std::filesystem::path& p) {
    auto it = g_toml_cache.find(p.string());
    if (it != g_toml_cache.end()) return it->second;
    std::string s;
    read_file(p.string(), s);
    return g_toml_cache[p.string()] = s;
}

std::string cargo_edition(const std::string& path) {
    static const std::regex key(R"re((^|\n)[ \t]*edition[ \t]*=[ \t]*"(\d{4})")re");
    static const std::regex ws(R"((^|\n)[ \t]*edition(\s*\.\s*workspace\s*=\s*true|\s*=\s*\{\s*workspace\s*=\s*true))");
    bool want_workspace = false;
    std::error_code ec;
    std::filesystem::path d = std::filesystem::absolute(path, ec).parent_path();
    while (true) {
        std::filesystem::path toml = d / "Cargo.toml";
        if (std::filesystem::is_regular_file(toml, ec)) {
            const std::string& text = toml_text(toml);
            std::smatch m;
            if (want_workspace) {
                if (text.find("[workspace") != std::string::npos) {
                    size_t at = text.find("[workspace.package]");
                    if (at != std::string::npos) {
                        std::string rest = text.substr(at + 19);
                        if (std::regex_search(rest, m, key)) return m[2].str();
                    }
                }
            } else {
                if (std::regex_search(text, m, key)) return m[2].str();
                if (std::regex_search(text, ws))
                    want_workspace = true;
                else if (text.find("[package]") != std::string::npos)
                    return "2015";
            }
        }
        if (d.parent_path() == d || d.empty()) return "";
        d = d.parent_path();
    }
}

std::string file_edition(const std::string& path, const std::string& text) {
    static const std::regex header(R"((^|\n)[ \t]*//@[ \t]*(compile-flags:[^\n]*--edition[= ]|edition:[ \t]*)(\d{4}))");
    std::string head = text.substr(0, 4000);
    std::smatch m;
    if (std::regex_search(head, m, header)) return m[3].str();
    std::string e = cargo_edition(path);
    return e.empty() ? "2021" : e;
}

rust::Edition parse_edition(const std::string& s) {
    if (s == "2015") return rust::Edition::E2015;
    if (s == "2018") return rust::Edition::E2018;
    if (s == "2024") return rust::Edition::E2024;
    return rust::Edition::E2021;
}

// ---------------------------------------------------------------------------
// Helpers for the unit tests.

std::unique_ptr<rust::ParseResult> parse(const std::string& src, rust::Edition ed = rust::Edition::E2021) {
    rust::ParseOptions o;
    o.edition = ed;
    return rust::parse_file(src, o);
}

/// "LINE:COL: message" of the first diagnostic, or "" when accepted.
std::string first_error(const std::string& src, rust::Edition ed = rust::Edition::E2021) {
    auto r = parse(src, ed);
    if (r->diagnostics.empty()) return "";
    const rust::Diagnostic& d = r->diagnostics[0];
    return std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.message;
}

bool accepts(const std::string& src, rust::Edition ed = rust::Edition::E2021) {
    std::string e = first_error(src, ed);
    if (!e.empty()) std::fprintf(stderr, "  unexpected error: %s\n    in: %s\n", e.c_str(), src.substr(0, 200).c_str());
    return e.empty();
}

bool error_is(const std::string& src, const std::string& want, rust::Edition ed = rust::Edition::E2021) {
    std::string e = first_error(src, ed);
    if (e != want) std::fprintf(stderr, "  got:  %s\n  want: %s\n    in: %s\n", e.c_str(), want.c_str(), src.substr(0, 200).c_str());
    return e == want;
}

void test_lexer() {
    rust::LexOptions o;
    {
        auto r = rust::lex("fn main() { let x = r#\"a\"# ; }", o);
        CHECK(r.errors.empty());
        CHECK(r.tokens.size() == 12);  // fn main ( ) { let x = lit ; } + Eof
    }
    {
        auto r = rust::lex("a >>= b", o);
        CHECK(r.tokens.size() == 4);
        CHECK(r.tokens[1].kind == rust::Tok::ShrEq);
    }
    {
        auto r = rust::lex("/* a /* nested */ b */ x // c\n/// doc\ny", o);
        CHECK(r.comments.size() == 3);
        CHECK(r.tokens[0].kind == rust::Tok::Ident);
        CHECK(r.tokens[1].kind == rust::Tok::DocComment);
    }
    {
        auto r = rust::lex("'a 'b' '\\n' b'x' br\"y\" c\"z\" 1.0e10f64 0x_ff_u8 1.foo x.0.1", o);
        CHECK(r.errors.empty());
        CHECK(r.tokens[0].kind == rust::Tok::Lifetime);
        CHECK(r.tokens[1].kind == rust::Tok::Literal && r.tokens[1].lit == rust::LitKind::Char);
    }
    {
        auto r = rust::lex("\"unterminated", o);
        CHECK(r.fatal);
    }
    {
        auto r = rust::lex("r#abc r#crate", o);
        CHECK(!r.errors.empty());
    }
    {
        auto r = rust::lex("é_ident ü", o);
        CHECK(r.errors.empty());
        CHECK(r.tokens[0].kind == rust::Tok::Ident);
    }
    {
        // Unbalanced delimiters are repaired so the parser still gets a tree.
        auto r = rust::lex("fn f() { (", o);
        CHECK(r.fatal);
        CHECK(r.tokens.back().kind == rust::Tok::Eof);
    }
}

void test_accepts() {
    const char* ok[] = {
        "",
        "#!/usr/bin/env rust\nfn main() {}",
        "#![allow(dead_code)]\n//! crate doc\nuse std::{io::{self, Write as _}, *};\n",
        "pub(crate) struct S<'a, T: ?Sized + 'a, const N: usize = 3> where T: Clone { pub x: &'a T, y: [u8; N] }",
        "struct T(pub u8, i32); struct U; union X { a: u8 }",
        "enum E { A, B(u8), C { x: i32 } = 3, }",
        "trait Tr: Sized { type A: Copy = u8; const C: u8; fn f(&self) -> u8 { 0 } fn g(self: Box<Self>); }",
        "impl<T> Tr for Vec<T> where T: Tr {} impl !Send for S {} unsafe impl Sync for S {}",
        "extern \"C\" { fn printf(f: *const u8, ...) -> i32; static X: u8; type Opaque; }",
        "mod m { pub mod n; } extern crate core as c;",
        "macro_rules! m { ($x:expr) => { $x }; } m!(1); m! { a b c }",
        "const _: () = (); static mut Z: u32 = 0;",
        "type F = for<'a> fn(&'a u8) -> &'a u8; type G = dyn Fn(u8) -> u8 + Send; type H = impl Iterator<Item = u8>;",
        "fn f() { let x: Vec<u8> = Vec::<u8>::new(); let (a, mut b) = (1, 2); }",
        "fn f() { if let Some(x) = y { } else if z { } else { } }",
        "fn f() { match x { 0 | 1 => {}, 2..=5 if a => (), ref mut y @ Some(_) => y, _ => unreachable!() } }",
        "fn f() { loop { break 'a; } 'a: while x { continue 'a; } for i in 0..10 { } }",
        "fn f() { let c = |x: u8| -> u8 { x + 1 }; let d = move || x; let e = async move { 1 }; }",
        "fn f() { a.b().c::<u8>()?.d[0].0 .1; x as u8 as u16; -!*&&mut y; }",
        "fn f() { S { x, y: 1, ..Default::default() }; [0; 3]; [1, 2, 3]; (1,); () }",
        "async fn f() { x.await; }",
        "fn f() -> impl Fn() { || () }",
        "fn f<T>(x: T) where for<'a> &'a T: Into<u8> {}",
        "fn f() { let x = 1u8 << 2 >> 1; a < b && c > d; }",
        "fn f() { unsafe { *p = 1 }; const { 1 }; }",
        "pub fn f(#[attr] x: u8) {}",
        "fn f() { #[cfg(x)] let a = 1; }",
        "fn f() { x = if a { 1 } else { 2 }; }",
        "impl S { pub const fn new() -> Self { Self {} } }",
        "fn f() { let _ = r#\"raw\"#; let _ = b\"bytes\"; let _ = 'c'; let _ = 1e10; let _ = 0b1010; }",
        "fn f() { let Some(x) = y else { return }; }",
        "fn f() { m!{} m![]; m!() ; }",
        "fn f() { x.0.0; }",
    };
    for (const char* s : ok) CHECK(accepts(s));
    // Edition-dependent keywords.
    CHECK(accepts("fn async() {} fn dyn() {} fn try() {}", rust::Edition::E2015));
    CHECK(accepts("fn gen() {}", rust::Edition::E2021));
    CHECK(accepts("fn f() { if let Some(x) = y && x > 0 { } }", rust::Edition::E2024));
    CHECK(!first_error("fn gen() {}", rust::Edition::E2024).empty());
    CHECK(!first_error("fn async() {}", rust::Edition::E2018).empty());
}

void test_errors() {
    CHECK(error_is("fn f() {\n", "1:10: this file contains an unclosed delimiter"));
    CHECK(error_is("fn f() { let x = 1 }", "1:20: expected one of `.`, `;`, `?`, `else`, or an operator, found `}`"));
    CHECK(error_is("fn f() { if let Some(x) = y && x > 0 {} }", "1:13: let chains are only allowed in Rust 2024 or later"));
    CHECK(error_is("fn f() { m!{} m![] m!() ; }", "1:20: expected one of `.`, `;`, `?`, `}`, or an operator, found `m`"));
    CHECK(error_is("fn f() { 1 + }", "1:14: expected expression, found `}`"));
    CHECK(error_is("struct S { x: u8 y: u8 }", "1:17: expected `,`, or `}`, found `y`"));
    CHECK(error_is("fn f() { box 1; }", "1:10: `box_syntax` has been removed"));
    CHECK(!first_error("fn 1() {}").empty());
    CHECK(!first_error("fn f() { ) }").empty());
    CHECK(!first_error("let x = 1;").empty());
}

void test_depth_and_budget() {
    {
        std::string deep = "fn f() { let x = " + std::string(20000, '(') + "1" + std::string(20000, ')') + "; }";
        auto r = parse(deep);
        CHECK(!r->diagnostics.empty());
        CHECK(r->diagnostics[0].message.find("nesting") != std::string::npos);
    }
    {
        std::string deep = "type T = " + std::string(5000, '[') + "u8" + std::string(5000, ']') + ";";
        auto r = parse(deep);
        CHECK(!r->diagnostics.empty());
    }
    {
        std::string deep = "fn f() {" + std::string(5000, '{') + std::string(5000, '}') + "}";
        auto r = parse(deep);
        CHECK(!r->diagnostics.empty());
    }
    {
        std::string ok = "type T = " + std::string(100, '[') + "u8" + std::string(100, ']') + ";";
        CHECK(accepts(ok));
    }
}

void test_spans() {
    std::string src = "#[inline]\npub fn f(x: u8) -> u8 { x + 1 }\n";
    auto r = parse(src);
    CHECK(r->ok());
    CHECK(r->crate->items.size() == 1);
    if (r->crate->items.size() == 1) {
        const rust::Item* it = r->crate->items[0];
        CHECK(it->kind == rust::ItemKind::Fn);
        CHECK(r->text(it->span) == "pub fn f(x: u8) -> u8 { x + 1 }");
        CHECK(it->attrs.size() == 1);
        CHECK(r->text(it->span_with_attributes()).substr(0, 9) == "#[inline]");
        CHECK(it->ident.name == "f");
        CHECK(it->body != nullptr);
        if (it->body && it->body->stmts.size() == 1) {
            const rust::Stmt* s = it->body->stmts[0];
            CHECK(s->kind == rust::StmtKind::Expr);
            CHECK(r->text(s->expr->span) == "x + 1");
            CHECK(s->expr->kind == rust::ExprKind::Binary);
        }
    }
    {
        auto r2 = parse("fn f() { let x = 1; }");
        CHECK(r2->ok());
        const rust::Item* it = r2->crate->items[0];
        CHECK(it->body && it->body->stmts.size() == 1);
        if (it->body && it->body->stmts.size() == 1) CHECK(r2->text(it->body->stmts[0]->span) == "let x = 1;");
    }
}

void test_fragments() {
    // Re-parse a macro's argument tokens the way rustfmt does.
    auto r = parse("fn f() { println!(\"{}\", a + b, c.d()); }");
    CHECK(r->ok());
    const rust::Item* it = r->crate->items[0];
    if (it->body && it->body->stmts.size() == 1 && it->body->stmts[0]->kind == rust::StmtKind::MacCall) {
        const rust::MacCall* m = it->body->stmts[0]->mac->mac;
        rust::Fragment f = rust::parse_fragment(*r, m->args.tok_lo, m->args.tok_hi, rust::FragmentKind::ExprList);
        CHECK(f.ok);
        CHECK(f.exprs.size() == 3);
        if (f.exprs.size() == 3) CHECK(r->text(f.exprs[1]->span) == "a + b");
    } else {
        CHECK(false);
    }
    {
        rust::Fragment f;
        auto r2 = rust::parse_string_fragment("Vec<HashMap<u8, &'static str>>", rust::FragmentKind::Ty, f);
        CHECK(f.ok);
        CHECK(f.ty != nullptr);
    }
    {
        rust::Fragment f;
        auto r2 = rust::parse_string_fragment("fn a() {} struct B;", rust::FragmentKind::Items, f);
        CHECK(f.ok);
        CHECK(f.items.size() == 2);
    }
    {
        rust::Fragment f;
        auto r2 = rust::parse_string_fragment("1 +", rust::FragmentKind::Expr, f);
        CHECK(!f.ok);
    }
}

void test_dump() {
    auto r = parse("fn f() -> u8 { 1 }");
    std::string d = rust::dump_crate(*r);
    CHECK(d.find("(Fn @0..18") != std::string::npos);
    CHECK(d.find("(Lit @15..16 1)") != std::string::npos);
}

int run_tests() {
    test_lexer();
    test_accepts();
    test_errors();
    test_depth_and_budget();
    test_spans();
    test_fragments();
    test_dump();
    if (g_failures) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("rust parse tests: %d checks passed\n", g_checks);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string forced_edition;
    std::string mode;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--edition" && i + 1 < argc) {
            forced_edition = argv[++i];
        } else if (a == "--check" || a == "--dump" || a == "--time" || a == "--all" || a == "--macros") {
            mode = a;
        } else {
            files.push_back(a);
        }
    }
    if (mode.empty()) return run_tests();

    auto t0 = std::chrono::steady_clock::now();
    int rejected = 0;
    for (const std::string& path : files) {
        std::string text;
        if (!read_file(path, text)) {
            std::printf("%s:0:0: cannot read file\n", path.c_str());
            ++rejected;
            continue;
        }
        rust::ParseOptions o;
        o.edition = parse_edition(forced_edition.empty() ? file_edition(path, text) : forced_edition);
        auto r = rust::parse_file(std::move(text), o);
        if (mode == "--dump") {
            std::fputs(rust::dump_crate(*r).c_str(), stdout);
            for (const rust::Diagnostic& d : r->diagnostics)
                std::printf("%s:%u:%u: %s%s\n", path.c_str(), d.line, d.col, d.message.c_str(),
                            d.after_fatal ? " (after fatal)" : "");
        } else if (mode == "--check") {
            if (!r->diagnostics.empty()) {
                const rust::Diagnostic& d = r->diagnostics[0];
                std::printf("%s:%u:%u: %s\n", path.c_str(), d.line, d.col, d.message.c_str());
                ++rejected;
            }
        } else if (mode == "--macros") {
            // Re-parse every `name!(...)` argument list the ways rustfmt tries
            // (an exercise for parse_fragment; prints how many parsed).
            int n = 0, ok = 0;
            for (uint32_t i = 1; i + 1 < r->tokens.size(); ++i) {
                if (r->tokens[i - 1].kind != rust::Tok::Bang) continue;
                rust::Tok k = r->tokens[i].kind;
                if (k != rust::Tok::OpenParen && k != rust::Tok::OpenBracket && k != rust::Tok::OpenBrace) continue;
                uint32_t close = r->partner[i];
                if (close == UINT32_MAX || close <= i) continue;
                for (rust::FragmentKind fk : {rust::FragmentKind::ExprList, rust::FragmentKind::Items,
                                              rust::FragmentKind::Stmts, rust::FragmentKind::Ty, rust::FragmentKind::Pat}) {
                    rust::Fragment f = rust::parse_fragment(*r, i + 1, close, fk);
                    ++n;
                    if (f.ok) ++ok;
                }
            }
            std::printf("%s: %d/%d fragments parsed\n", path.c_str(), ok, n);
        } else if (mode == "--all") {
            for (const rust::Diagnostic& d : r->diagnostics)
                std::printf("%s:%u:%u: %s%s\n", path.c_str(), d.line, d.col, d.message.c_str(),
                            d.after_fatal ? " (after fatal)" : "");
        }
        std::fflush(stdout);
    }
    if (mode == "--time") {
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%zu files in %.3f s\n", files.size(), s);
    }
    return mode == "--check" && rejected ? 1 : 0;
}
