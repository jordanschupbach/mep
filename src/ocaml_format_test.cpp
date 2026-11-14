// mep's OCaml formatter, driven directly.
//
// `mep-ocaml-format-test` runs the checks below (layout cases, token safety,
// refusals, idempotence; and, when an OCaml toolchain's stdlib sources are found
// through `ocamlfind ocamlc -where` / `ocamlc -where` or MEP_OCAML_STDLIB, every
// stdlib .ml/.mli formatted, checked unchanged in tokens and a fixed point).
// `mep-ocaml-format-test --format [-w N] [--indent N] [--no-break] FILE...` prints
// each file formatted ('-' reads stdin), for tools/format_compare.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ocaml_format.h"

namespace {
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    g_failed++;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    ocamlfmt::Options opts;
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
        if (arg == "--no-break") {
            opts.break_lines = false;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        const ocamlfmt::Result r = ocamlfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

struct Case {
    const char *name;
    const char *in;
    const char *out;
};

// Layout cases: each `out` is laid out the way ocamlformat (profile=default, margin 80)
// lays out the same code broken the same way, and is itself unchanged by the formatter.
const Case kCases[] = {
    {"let body", "let f x =\nx + 1\n", "let f x =\n  x + 1\n"},
    {"let in", "let f x =\nlet y = x in\nlet z = y in\nz\n",
     "let f x =\n  let y = x in\n  let z = y in\n  z\n"},
    {"match arms", "let f x =\nmatch x with\n| A -> 1\n| B ->\nlet y = 2 in\ny+1\n",
     "let f x =\n  match x with\n  | A -> 1\n  | B ->\n      let y = 2 in\n      y + 1\n"},
    {"function", "let g = function\n| None -> 0\n| Some x -> x\n", "let g = function\n  | None -> 0\n  | Some x -> x\n"},
    {"if then else", "let h x =\nif x then\nfoo ()\nelse\nbar ()\n",
     "let h x =\n  if x then\n    foo ()\n  else\n    bar ()\n"},
    {"else if", "let h x =\nif a then 1\nelse if b then 2\nelse 3\n", "let h x =\n  if a then 1\n  else if b then 2\n  else 3\n"},
    {"struct", "module M = struct\nlet a = 1\ntype t = int\nend\n", "module M = struct\n  let a = 1\n  type t = int\nend\n"},
    {"sig", "module type S = sig\nval f : int -> int\nval g : int\nend\n",
     "module type S = sig\n  val f : int -> int\n  val g : int\nend\n"},
    {"begin end", "let () =\nif c then begin\nfoo ();\nbar ()\nend\n", "let () =\n  if c then begin\n    foo ();\n    bar ()\n  end\n"},
    {"begin match", "let () =\nbegin match x with\n| A -> ()\n| B -> ()\nend\n",
     "let () =\n  begin match x with\n  | A -> ()\n  | B -> ()\n  end\n"},
    {"for loop", "let () =\nfor i = 0 to 3 do\nprint_int i\ndone\n", "let () =\n  for i = 0 to 3 do\n    print_int i\n  done\n"},
    {"while loop", "let () =\nwhile !r do\ndecr r\ndone\n", "let () =\n  while !r do\n    decr r\n  done\n"},
    {"try with", "let f () =\ntry\nfoo ()\nwith\n| Not_found -> 0\n| Exit -> 1\n",
     "let f () =\n  try\n    foo ()\n  with\n  | Not_found -> 0\n  | Exit -> 1\n"},
    {"try single arm", "let f () =\ntry foo ()\nwith Not_found ->\nlet x = 1 in\nx\n",
     "let f () =\n  try foo ()\n  with Not_found ->\n    let x = 1 in\n    x\n"},
    {"fun body", "let z =\nList.iter (fun x ->\nprint_int x)\nl\n", "let z =\n  List.iter (fun x ->\n    print_int x)\n    l\n"},
    {"pipeline", "let () =\nfoo\n|> bar\n|> baz\n", "let () =\n  foo\n  |> bar\n  |> baz\n"},
    {"sequence after let in else", "let f a =\nif a then 0\nelse\nlet i = ref 0 in\nincr i;\n!i\n",
     "let f a =\n  if a then 0\n  else\n    let i = ref 0 in\n    incr i;\n    !i\n"},
    {"variant type", "type t =\n| A\n| B of int\n", "type t =\n  | A\n  | B of int\n"},
    {"record type", "type t = {\nx : int;\ny : float;\n}\n", "type t = {\n  x : int;\n  y : float;\n}\n"},
    {"gadt", "type _ t =\n| C :\nint\n-> int t\n", "type _ t =\n  | C :\n      int\n      -> int t\n"},
    {"assignment", "let () =\nr :=\n!r + 1;\nx.f <-\n2\n", "let () =\n  r :=\n    !r + 1;\n  x.f <-\n    2\n"},
    {"item attribute", "external f : int -> int = \"f\"\n  [@@noalloc]\n", "external f : int -> int = \"f\"\n[@@noalloc]\n"},
    {"nested match in parens", "let f x =\n(match x with\n| A -> 1\n| B -> 2)\n",
     "let f x =\n  (match x with\n   | A -> 1\n   | B -> 2)\n"},
    {"paren function", "let f =\nString.map\n(function\n| 'a' -> 'b'\n| c -> c)\ns\n",
     "let f =\n  String.map\n    (function\n      | 'a' -> 'b'\n      | c -> c)\n    s\n"},
    {"application continuation", "let x =\nf a\nb\n", "let x =\n  f a\n    b\n"},
    {"let head continuation", "let f aaaa bbbb\ncccc =\n1\n", "let f aaaa bbbb\n    cccc =\n  1\n"},
    {"class object", "class c = object\nval mutable n = 0\nmethod incr = n <- n + 1\nend\n",
     "class c = object\n  val mutable n = 0\n  method incr = n <- n + 1\nend\n"},
    {"blank lines collapse", "let a = 1\n\n\n\nlet b = 2\n", "let a = 1\n\nlet b = 2\n"},
    {"trailing blanks", "let a = 1   \nlet b = 2\t\n", "let a = 1\nlet b = 2\n"},
    {"comment before arm", "let f = function\n(* first *)\n| A -> 1\n| B -> 2\n",
     "let f = function\n  (* first *)\n  | A -> 1\n  | B -> 2\n"},
    {"comment before end", "module M = struct\nlet x = 1\n(* last *)\nend\n", "module M = struct\n  let x = 1\n  (* last *)\nend\n"},
    {"doc comment of a constructor", "type t =\n| A of int\n(** an A *)\n| B\n", "type t =\n  | A of int\n      (** an A *)\n  | B\n"},
    {"multi-line comment moves", "let f x =\n(* one\n   two *)\nx\n", "let f x =\n  (* one\n     two *)\n  x\n"},
    // Spacing.
    {"operators", "let x = a+b*c-d\n", "let x = a + b * c - d\n"},
    {"float operators", "let x = a+.b*.c\n", "let x = a +. b *. c\n"},
    {"comma and semicolon", "let l = [1;2;3] and t = (a,b)\n", "let l = [ 1; 2; 3 ] and t = (a, b)\n"},
    {"record padding", "let r = {x = 1; y = 2}\n", "let r = { x = 1; y = 2 }\n"},
    {"array padding", "let a = [|1;2|]\n", "let a = [| 1; 2 |]\n"},
    {"cons and append", "let l = x::xs@ys\n", "let l = x :: xs @ ys\n"},
    {"arrow", "let f = fun x->x\n", "let f = fun x -> x\n"},
    {"labels untouched", "let _ = f ~x:1 ?y:None ~z\n", "let _ = f ~x:1 ?y:None ~z\n"},
    {"labelled arrow type", "val f : x:int -> ?y:int -> int\n", "val f : x:int -> ?y:int -> int\n"},
    {"unary minus", "let x = f (-1) - -2\n", "let x = f (-1) - -2\n"},
    {"operator section", "let s = List.fold_left ( + ) 0\nlet p = ( * )\n", "let s = List.fold_left ( + ) 0\nlet p = ( * )\n"},
    {"deref and not-equal", "let b = !r != 0\n", "let b = !r != 0\n"},
    {"type annotation", "let f (x:int) : int = x\n", "let f (x:int) : int = x\n"},
    {"spaced annotation", "let f (x :int) = x\n", "let f (x : int) = x\n"},
    {"bigarray access untouched", "let v = a.{i} + b.(j) + s.[k]\n", "let v = a.{i} + b.(j) + s.[k]\n"},
    {"attribute brackets", "let x = 1 [@@inline]\nlet y = [%ext foo]\n", "let x = 1 [@@inline]\nlet y = [%ext foo]\n"},
    {"binding operators", "let* x = m in\nlet+ y = n in\nx+y\n", "let* x = m in\nlet+ y = n in\nx + y\n"},
    {"polymorphic variants", "type t = [`A|`B of int]\n", "type t = [ `A | `B of int ]\n"},
    {"chars and type vars", "let c = '\\'' and d = 'a' and e : 'a list = []\n",
     "let c = '\\'' and d = 'a' and e : 'a list = []\n"},
    {"empty", "", ""},
};

void TestCases() {
    for (const Case &c : kCases) {
        const ocamlfmt::Result r = ocamlfmt::Format(c.in);
        const bool ok = r.ok && r.text == c.out;
        if (!ok) {
            std::fprintf(stderr, "case `%s`:\n--- want\n%s--- got (%s)\n%s---\n", c.name, c.out,
                         r.ok ? "ok" : r.error.c_str(), r.text.c_str());
        }
        CHECK(ok);
        // The expected output is a fixed point.
        const ocamlfmt::Result again = ocamlfmt::Format(c.out);
        if (!(again.ok && again.text == c.out))
            std::fprintf(stderr, "case `%s` is not a fixed point:\n%s", c.name, again.text.c_str());
        CHECK(again.ok && again.text == c.out);
    }
}

void TestBreaking() {
    // A long binding breaks after `=`.
    const std::string long_let =
        "let some_long_name = some_function_with_a_long_name argument_number_one argument_number_two\n";
    ocamlfmt::Result r = ocamlfmt::Format(long_let);
    CHECK(r.ok);
    CHECK(r.text == "let some_long_name =\n  some_function_with_a_long_name argument_number_one argument_number_two\n");
    // A long pipeline breaks before each `|>`.
    r = ocamlfmt::Format("let () =\n  xs |> List.map (fun x -> x + 1) |> List.filter (fun x -> x > 0) |> List.iter print_int\n");
    CHECK(r.ok);
    CHECK(r.text == "let () =\n  xs\n  |> List.map (fun x -> x + 1)\n  |> List.filter (fun x -> x > 0)\n  |> List.iter print_int\n");
    // A long arm breaks after `->`.
    r = ocamlfmt::Format("let f = function\n  | Some_constructor x -> some_function_with_a_long_name x argument_number_two arg3\n  | None -> 0\n");
    CHECK(r.ok);
    CHECK(r.text == "let f = function\n  | Some_constructor x ->\n      some_function_with_a_long_name x argument_number_two arg3\n  | None -> 0\n");
    // A long if breaks before `else`.
    r = ocamlfmt::Format("let f x =\n  if some_condition x && another_condition x then first_result x else second_result x\n");
    CHECK(r.ok);
    CHECK(r.text == "let f x =\n  if some_condition x && another_condition x then first_result x\n  else second_result x\n");
    // Lines are never joined, and a short line is never broken.
    r = ocamlfmt::Format("let x =\n  1\n");
    CHECK(r.ok && r.text == "let x =\n  1\n");
    // Range formatting keeps the line structure.
    ocamlfmt::Options keep;
    keep.break_lines = false;
    r = ocamlfmt::Format(long_let, keep);
    CHECK(r.ok && r.text == long_let);
    // Width is honoured.
    ocamlfmt::Options narrow;
    narrow.width = 30;
    r = ocamlfmt::Format("let value = compute first second third\n", narrow);
    CHECK(r.ok && r.text == "let value =\n  compute first second third\n");
}

void TestSafety() {
    // Strings and comments keep their insides; a multi-line string's lines are never re-indented.
    const std::string s = "let s = \"a\n    b\"\nlet t = {|x\n  y|}\n(* a \"*)\" b *)\nlet u = 1\n";
    ocamlfmt::Result r = ocamlfmt::Format(s);
    CHECK(r.ok);
    CHECK(r.text == s);
    // Unbalanced code is refused, with the input returned unchanged.
    r = ocamlfmt::Format("let f x = (x + 1\n");
    CHECK(!r.ok && r.text == "let f x = (x + 1\n" && !r.error.empty());
    r = ocamlfmt::Format("let f x = x + 1)\n");
    CHECK(!r.ok && r.error_line == 1);
    r = ocamlfmt::Format("module M = struct\nlet x = 1\n");
    CHECK(!r.ok);
    r = ocamlfmt::Format("let x = 1\nend\n");
    CHECK(!r.ok && r.error_line == 2);
    r = ocamlfmt::Format("let s = \"unterminated\n");
    CHECK(!r.ok && r.error.find("unterminated string") != std::string::npos);
    r = ocamlfmt::Format("(* unterminated\n");
    CHECK(!r.ok && r.error.find("unterminated comment") != std::string::npos);
    // Toplevel directives stay where they are.
    r = ocamlfmt::Format("#use \"topfind\";;\n#require \"str\";;\nlet x = 1\n");
    CHECK(r.ok && r.text == "#use \"topfind\";;\n#require \"str\";;\nlet x = 1\n");
    // The token comparison itself.
    CHECK(ocamlfmt::SameTokens("let x=1", "let  x = 1"));
    CHECK(!ocamlfmt::SameTokens("let x = a+.b", "let x = a + . b"));
    CHECK(!ocamlfmt::SameTokens("let x = 1", "let x = 2"));
    CHECK(ocamlfmt::SameTokens("(* a\n   b *)", "(* a\n       b *)"));
    CHECK(!ocamlfmt::SameTokens("(* a b *)", "(* a  b *)"));
    // The lexer agrees with OCaml's on operator boundaries (a space must never split one).
    std::vector<ocamlfmt::Token> toks;
    std::string err;
    int line = 0;
    CHECK(ocamlfmt::Lex("a+.b |>f >>= g ~x: ?y:z '\\'' 'a", &toks, &err, &line));
    CHECK(toks.size() == 12);
    CHECK(toks.size() > 1 && toks[1].text == "+.");
    CHECK(toks.size() > 3 && toks[3].text == "|>");
    CHECK(toks.size() > 5 && toks[5].text == ">>=");
    CHECK(toks.size() > 7 && toks[7].text == "~x:" && toks[7].kind == ocamlfmt::TokKind::Label);
    CHECK(toks.size() > 10 && toks[10].text == "'\\''" && toks[10].kind == ocamlfmt::TokKind::Char);
    CHECK(toks.size() > 11 && toks[11].kind == ocamlfmt::TokKind::TyVar);
}

std::string ReadFile(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string RunCapture(const std::string &cmd) {
#if defined(_WIN32)
    (void)cmd;
    return std::string();
#else
    std::string out;
    if (FILE *p = popen(cmd.c_str(), "r")) {
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

// Every .ml/.mli of an installed stdlib: formatted (or refused), same tokens, a fixed point.
void TestStdlib() {
    std::string dir;
    if (const char *e = std::getenv("MEP_OCAML_STDLIB")) dir = e;
    if (dir.empty()) dir = RunCapture("ocamlfind ocamlc -where 2>/dev/null");
    if (dir.empty()) dir = RunCapture("ocamlc -where 2>/dev/null");
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
        std::printf("ocaml_format_test: no OCaml stdlib sources found (ocamlc -where); skipping the stdlib corpus\n");
        return;
    }
    int files = 0, refused = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string ext = entry.path().extension().string();
        if (ext != ".ml" && ext != ".mli") continue;
        const std::string src = ReadFile(entry.path());
        const ocamlfmt::Result r = ocamlfmt::Format(src);
        files++;
        if (!r.ok) {
            refused++;
            std::fprintf(stderr, "stdlib %s refused: %s\n", entry.path().filename().string().c_str(), r.error.c_str());
            continue;
        }
        CHECK(ocamlfmt::SameTokens(src, r.text));
        const ocamlfmt::Result again = ocamlfmt::Format(r.text);
        if (!(again.ok && again.text == r.text))
            std::fprintf(stderr, "stdlib %s: not a fixed point\n", entry.path().filename().string().c_str());
        CHECK(again.ok && again.text == r.text);
    }
    CHECK(refused == 0);
    std::printf("ocaml_format_test: stdlib corpus %s: %d files, %d refused\n", dir.c_str(), files, refused);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    TestCases();
    TestBreaking();
    TestSafety();
    TestStdlib();
    if (g_failed) {
        std::printf("ocaml_format_test: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("ocaml_format_test: %d checks passed\n", g_checks);
    return 0;
}
