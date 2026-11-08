// mep's Rust formatter, driven directly.
//
// `mep-rust-format-test` runs the checks below;
// `mep-rust-format-test --format [-w N] [--indent N] [--edition E|auto] [--no-check] FILE...`
// prints each file formatted ('-' reads stdin), for tools/format_compare.py.
// `--edition auto` (the default for files) takes the edition from the file's
// compiletest header or its crate's Cargo.toml, like tools/ts_grammar_diff/rust_check.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "rust_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    rustfmt::Options opts;
    bool auto_edition = true;
    int status = 0;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            opts.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            opts.indent = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--edition" && k + 1 < argc) {
            const std::string e = argv[++k];
            auto_edition = e == "auto";
            if (!auto_edition) opts.edition = std::atoi(e.c_str());
            continue;
        }
        if (arg == "--no-check") {
            opts.no_check = true;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const std::string text = ss.str();
        rustfmt::Options o = opts;
        if (auto_edition && arg != "-") o.edition = rustfmt::DetectEdition(arg, text);
        const rustfmt::Result r = rustfmt::Format(text, o);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Formats `in` (edition 2021 unless given) and checks the result, and that
// formatting it again changes nothing.
void Expect(const std::string &in, const std::string &want, int edition = 2021) {
    rustfmt::Options o;
    o.edition = edition;
    const rustfmt::Result r = rustfmt::Format(in, o);
    if (!r.ok || r.text != want) {
        std::fprintf(stderr, "--- input\n%s--- want\n%s--- got (%s)\n%s\n", in.c_str(), want.c_str(),
                     r.ok ? "ok" : r.error.c_str(), r.text.c_str());
    }
    CHECK(r.ok);
    CHECK(r.text == want);
    const rustfmt::Result again = rustfmt::Format(r.text, o);
    CHECK(again.ok && again.text == r.text);
}

void Refuses(const std::string &in) {
    const rustfmt::Result r = rustfmt::Format(in);
    CHECK(!r.ok);
    CHECK(r.text == in);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    // Files and whitespace.
    Expect("", "\n");
    Expect("\n\n\n", "\n");
    Expect("// only comment\n\n\n", "// only comment\n");
    Expect("\xEF\xBB\xBF" "fn a(){}\r\nfn b(){}\r\n", "fn a() {}\nfn b() {}\n");
    Expect("fn main(){let x=1;\n// hi\n}\n", "fn main() {\n    let x = 1;\n    // hi\n}\n");

    // The token changes rustfmt makes (and the self-check must allow).
    Expect("#[derive(Debug)]\n#[derive(Clone,)]\nstruct A;\n", "#[derive(Debug, Clone)]\nstruct A;\n");
    Expect("extern { fn f(); }\n", "extern \"C\" {\n    fn f();\n}\n");
    Expect("fn main() {\n    let c = | | { 1 };\n    let d = |x| { x + 1 };\n}\n",
           "fn main() {\n    let c = || 1;\n    let d = |x| x + 1;\n}\n");
    Expect("fn main() {\n    match x { 1 => { a() } 2 => b(), 3 => { c(); } _ => {}, }\n}\n",
           "fn main() {\n    match x {\n        1 => a(),\n        2 => b(),\n        3 => {\n            c();\n"
           "        }\n        _ => {}\n    }\n}\n");
    Expect("fn main() {\n    loop {};\n    ;\n    f(a,);\n    let z = (a,);\n    return 3\n}\n",
           "fn main() {\n    loop {}\n    f(a);\n    let z = (a,);\n    return 3;\n}\n");
    Expect("use b::{c, a};\nuse a;\n", "use a;\nuse b::{a, c};\n");
    Expect("fn h<T,>() where T: Clone, {}\n", "fn h<T>()\nwhere\n    T: Clone,\n{\n}\n");
    Expect("mod m { pub(in crate) fn x() {} }\n", "mod m {\n    pub(crate) fn x() {}\n}\n");
    Expect("macro_rules! m { () => {}; (x) => {} }\n", "macro_rules! m {\n    () => {};\n    (x) => {};\n}\n");
    Expect("fn main() { let y = ((1 + 2)); }\n", "fn main() {\n    let y = (1 + 2);\n}\n");

    // Expressions, items, style editions.
    Expect("fn f(a:i32,b:i32)->i32{a+b}\n", "fn f(a: i32, b: i32) -> i32 {\n    a + b\n}\n");
    Expect("struct P{x:i32,y:i32}\n", "struct P {\n    x: i32,\n    y: i32,\n}\n");
    Expect("enum E{A,B(i32),C{x:u8}}\n", "enum E {\n    A,\n    B(i32),\n    C { x: u8 },\n}\n");
    Expect("impl<T> Tr for S<T> where T: Copy { fn f(&self) {} }\n",
           "impl<T> Tr for S<T>\nwhere\n    T: Copy,\n{\n    fn f(&self) {}\n}\n");
    Expect("fn main() { let v = vec![1,2,3]; println!(\"{}\", v.len()); }\n",
           "fn main() {\n    let v = vec![1, 2, 3];\n    println!(\"{}\", v.len());\n}\n");
    Expect("fn main() { if a { b } else { c } }\n",
           "fn main() {\n    if a {\n        b\n    } else {\n        c\n    }\n}\n");
    Expect("fn main() { x.0.1; }\n", "fn main() {\n    x.0 .1;\n}\n", 2021);
    Expect("fn main() { x.0.1; }\n", "fn main() {\n    x.0.1;\n}\n", 2024);
    Expect("fn main() {\n    let x = foo.bar().baz().qux().quux().corge().grault().garply().waldo().fred().plugh();\n}\n",
           "fn main() {\n    let x = foo\n        .bar()\n        .baz()\n        .qux()\n        .quux()\n        .corge()\n"
           "        .grault()\n        .garply()\n        .waldo()\n        .fred()\n        .plugh();\n}\n");
    Expect("#[rustfmt::skip]\nfn  f ( ) { }\nfn  g ( ) { }\n", "#[rustfmt::skip]\nfn  f ( ) { }\nfn g() {}\n");

    // Refusals leave the text alone.
    Refuses("fn main( {\n");
    Refuses("fn f() { let = ; }\n");

    // Deep nesting must not crash.
    {
        std::string deep = "fn main() { let x = ";
        for (int i = 0; i < 5000; i++) deep += "(";
        deep += "1";
        for (int i = 0; i < 5000; i++) deep += ")";
        deep += "; }\n";
        const rustfmt::Result r = rustfmt::Format(deep);
        CHECK(!r.ok || !r.text.empty());
    }

    // Idempotence: formatting formatted text changes nothing.
    const std::string once = rustfmt::Format("fn main(){}\n").text;
    CHECK(rustfmt::Format(once).text == once);
    std::printf("rust_format_test: %d checks passed\n", g_checks);
    return 0;
}
