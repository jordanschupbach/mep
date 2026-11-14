// Windowless test for mep's own OCaml language server (`mep-ocaml-lsp`):
// drives the analysis functions of ocaml_lsp.h directly (no process, no
// JSON-RPC client), plus the odoc renderer and the module index of
// ocaml_lsp_internal.h. CHECK(), never assert(): the Release build strips
// assert(). Fixtures are written to a temporary directory. The tests that need
// an installed OCaml (its stdlib .mli files) skip cleanly without one; the
// rest run on the compiled-in stdlib fallback.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ocaml_lsp.h"
#include "ocaml_lsp_internal.h"

namespace fs = std::filesystem;

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

bool Has(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

void Write(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string ReadAll(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Pos {
    int line = -1, col = -1;
};
/** Position of the @p nth occurrence of @p needle (+ @p delta columns). */
Pos At(const std::string &text, const std::string &needle, int delta = 0, int nth = 1) {
    size_t p = std::string::npos, from = 0;
    for (int i = 0; i < nth; i++) {
        p = text.find(needle, from);
        if (p == std::string::npos) return Pos{};
        from = p + 1;
    }
    Pos out;
    out.line = 0;
    size_t ls = 0;
    for (size_t i = 0; i < p; i++) {
        if (text[i] == '\n') {
            out.line++;
            ls = i + 1;
        }
    }
    out.col = static_cast<int>(p - ls) + delta;
    return out;
}

std::string HoverText(const OcamlLspDocument &doc, const std::string &text, const std::string &needle, int delta = 0,
                      int nth = 1) {
    const Pos p = At(text, needle, delta, nth);
    const OcamlLspHoverInfo h = OcamlLspHover(doc, p.line, p.col);
    return h.found ? h.text : std::string();
}

/** No installed OCaml: the compiled-in stdlib is used. */
void UseFallbackStdlib() {
    setenv("MEP_OCAML_STDLIB", "", 1);
    setenv("MEP_OCAML_NO_FINDLIB", "1", 1);
    unsetenv("OCAMLPATH");
    unsetenv("OPAM_SWITCH_PREFIX");
    unsetenv("MEP_OCAML_PATH");
    unsetenv("OCAMLLIB");
    OcamlLspResetCaches();
}

// --- odoc ---------------------------------------------------------------------------------

void TestOdoc() {
    using mllsp::OdocToMarkdown;
    CHECK(OdocToMarkdown("Return [x] plus {b one}.") == "Return `x` plus **one**.");
    CHECK(OdocToMarkdown("{i a} and {e b}") == "*a* and *b*");
    CHECK(Has(OdocToMarkdown("See {!List.map} and {!val-iter}."), "`List.map` and `iter`"));
    CHECK(Has(OdocToMarkdown("{{!List.map} the map}"), "the map"));
    CHECK(Has(OdocToMarkdown("{{:https://ocaml.org} OCaml}"), "[OCaml](https://ocaml.org)"));
    const std::string code = OdocToMarkdown("Example:\n{[\n  let x = 1\n  in x\n]}\nDone.");
    CHECK(Has(code, "```ocaml\nlet x = 1\nin x\n```"));
    CHECK(Has(code, "Done."));
    const std::string ul = OdocToMarkdown("Items:\n{ul {li first [a]} {li second}}");
    CHECK(Has(ul, "- first `a`\n- second"));
    CHECK(Has(OdocToMarkdown("{ol {li one} {li two}}"), "1. one\n2. two"));
    CHECK(Has(OdocToMarkdown("{1 Title}\nbody"), "## Title"));
    CHECK(Has(OdocToMarkdown("{v raw [x] v}"), "```\nraw [x]\n```"));
    const std::string tags = OdocToMarkdown(
        "Divides.\n@param x the dividend\n@param y the divisor,\n  never zero\n@return the quotient\n"
        "@raise Division_by_zero if [y = 0]\n@see <https://x.org> the spec\n@since 4.08\n@deprecated use [div2]");
    CHECK(Has(tags, "Divides."));
    CHECK(Has(tags, "**Parameters**\n- `x`: the dividend\n- `y`: the divisor,"));
    CHECK(Has(tags, "**Returns:** the quotient"));
    CHECK(Has(tags, "**Raises**\n- `Division_by_zero`: if `y = 0`"));
    CHECK(Has(tags, "**See also:**\n- <https://x.org> the spec"));
    CHECK(Has(tags, "**Since:** 4.08"));
    CHECK(Has(tags, "**Deprecated.** use `div2`"));
    CHECK(OdocToMarkdown("a \\{ b \\}") == "a { b }");
    CHECK(mllsp::OdocSummary("First line\ncontinues.\n\nSecond paragraph.") == "First line continues.");
    CHECK(mllsp::DocCommentText("(** doc *)") == "doc");
    CHECK(mllsp::DocCommentText("(* not doc *)").empty());
    CHECK(mllsp::DocCommentText("(*** banner *)").empty());
    CHECK(mllsp::DocCommentText("(**/**)").empty());
}

void TestArrows() {
    std::string result;
    auto ps = mllsp::SplitArrows("('a -> 'b) -> 'a list -> 'b list", &result);
    CHECK(ps.size() == 2 && ps[0].text == "('a -> 'b)" && ps[1].text == "'a list" && result == "'b list");
    ps = mllsp::SplitArrows("?sep:string -> f:(int -> unit) -> int -> unit", &result);
    CHECK(ps.size() == 3);
    CHECK(ps[0].label == "sep" && ps[0].optional && ps[1].label == "f" && !ps[1].optional && ps[2].label.empty());
    ps = mllsp::SplitArrows("'a. 'a -> 'a", &result);
    CHECK(ps.size() == 1 && ps[0].text == "'a" && result == "'a");
    ps = mllsp::SplitArrows("int", &result);
    CHECK(ps.empty() && result == "int");
}

// --- the index: odoc attachment -------------------------------------------------------------

void TestDocAttachment() {
    const std::string mli =
        "(** The module preamble. *)\n"
        "\n"
        "(** Before [a]. *)\n"
        "val a : int\n"
        "\n"
        "val b : int -> int\n"
        "(** After [b]. *)\n"
        "\n"
        "val c : string (** Same line as [c]. *)\n"
        "\n"
        "type color =\n"
        "  | Red  (** The red one. *)\n"
        "  | Green\n"
        "      (** The green one. *)\n"
        "  | Blue (** The blue one. *)\n"
        "\n"
        "type point = {\n"
        "  x : float;  (** Abscissa. *)\n"
        "  y : float;  (** Ordinate. *)\n"
        "}\n"
        "\n"
        "(** A floating comment. *)\n"
        "\n"
        "val d : unit -> unit\n"
        "val e : int [@@ocaml.deprecated \"use d\"]\n";
    auto mod = mllsp::IndexSource(mli, true, "", "M");
    auto doc_of = [&](const std::string &name) -> std::string {
        for (const mllsp::Item &it : mod->items)
            if (it.name == name) return it.doc;
        return "<none>";
    };
    CHECK(mod->doc == "The module preamble.");
    CHECK(doc_of("a") == "Before [a].");
    CHECK(doc_of("b") == "After [b].");
    CHECK(doc_of("c") == "Same line as [c].");
    CHECK(doc_of("Red") == "The red one.");
    CHECK(doc_of("Green") == "The green one.");
    CHECK(doc_of("Blue") == "The blue one.");
    CHECK(doc_of("color").empty());
    CHECK(doc_of("x") == "Abscissa.");
    CHECK(doc_of("y") == "Ordinate.");
    CHECK(doc_of("d").empty());
    for (const mllsp::Item &it : mod->items) {
        if (it.name == "e") CHECK(it.is_deprecated && it.deprecated == "use d");
        if (it.name == "b") CHECK(it.type == "int -> int" && it.signature == "val b : int -> int");
        if (it.name == "color") CHECK(Has(it.signature, "type color =") && !Has(it.signature, "red one"));
    }
}

// --- locals ------------------------------------------------------------------------------------

void TestLocals() {
    const std::string src =
        "let total xs =\n"
        "  let sum = List.fold_left ( + ) 0 xs in\n"
        "  let unused = 3 in\n"
        "  let _ignored = 4 in\n"
        "  sum\n"
        "\n"
        "let classify n =\n"
        "  match n with\n"
        "  | Some v when v > 0 -> v\n"
        "  | Some v -> -v\n"
        "  | None -> 0\n"
        "\n"
        "let shadow x =\n"
        "  let x = x + 1 in\n"
        "  x * 2\n"
        "\n"
        "let labelled ~width ?(height = 2) () = width * height\n"
        "let call width = labelled ~width ()\n"
        "let () = for i = 1 to 3 do print_int i done\n";
    const auto doc = OcamlLspAnalyze(L(src), "");

    CHECK(Has(HoverText(*doc, src, "sum\n"), "let sum"));
    CHECK(Has(HoverText(*doc, src, "xs in"), "(parameter) xs"));
    CHECK(Has(HoverText(*doc, src, "v > 0"), "(pattern) v"));
    CHECK(Has(HoverText(*doc, src, "print_int i", 10), "(for) i"));

    Pos p = At(src, "sum\n");
    auto defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && defs[0].path.empty() && defs[0].range.line == 1 && defs[0].range.col == 6);
    // Match-case variables are per case.
    p = At(src, "-v");
    defs = OcamlLspDefinition(*doc, p.line, p.col + 1);
    CHECK(defs.size() == 1 && defs[0].range.line == 9 && defs[0].range.col == 9);
    // Shadowing: `x + 1` is the parameter, the body's `x` the inner let.
    p = At(src, "x + 1");
    defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && defs[0].range.line == 12 && defs[0].range.col == 11);
    p = At(src, "x * 2");
    defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && defs[0].range.line == 13 && defs[0].range.col == 6);

    // References and rename.
    p = At(src, "let sum", 4);
    auto refs = OcamlLspReferences(*doc, p.line, p.col);
    CHECK(refs.size() == 2 && refs[0].is_definition && !refs[1].is_definition && refs[1].range.line == 4);
    std::string err;
    auto edits = OcamlLspRename(*doc, p.line, p.col, "acc", &err);
    CHECK(err.empty() && edits.size() == 2 && edits[0].new_text == "acc");
    edits = OcamlLspRename(*doc, p.line, p.col, "Bad", &err);
    CHECK(!err.empty() && edits.empty());
    err.clear();
    edits = OcamlLspRename(*doc, p.line, p.col, "let", &err);
    CHECK(!err.empty());
    err.clear();
    // `~width` punned argument: renaming the variable keeps the label.
    p = At(src, "let call width", 9);
    edits = OcamlLspRename(*doc, p.line, p.col, "w", &err);
    CHECK(err.empty() && edits.size() == 2 && edits[1].new_text == "width:w");
    // A labelled parameter cannot be renamed.
    p = At(src, "~width ?", 1);
    CHECK(!OcamlLspPrepareRename(*doc, p.line, p.col).found);
    p = At(src, "let sum", 4);
    const OcamlLspRenameTarget rt = OcamlLspPrepareRename(*doc, p.line, p.col);
    CHECK(rt.found && rt.placeholder == "sum");

    // Diagnostics: one unused let-in variable, `_ignored` exempt.
    int unused = 0;
    for (const auto &d : OcamlLspDiagnostics(*doc)) {
        if (d.code == "unused-variable") {
            unused++;
            CHECK(d.range.line == 2 && d.unnecessary);
        }
        CHECK(d.code != "syntax-error");
    }
    CHECK(unused == 1);

    // Completion: locals in scope.
    const std::string src2 = "let f alpha beta =\n  let gamma = alpha + beta in\n  ga";
    const auto doc2 = OcamlLspAnalyze(L(src2), "");
    auto items = OcamlLspCompletions(*doc2, 2, 4);
    CHECK(!items.empty() && items[0].label == "gamma" && items[0].replace_start == 2 && items[0].replace_end == 4);
    items = OcamlLspCompletions(*doc2, 2, 2);
    bool alpha = false;
    for (const auto &it : items) alpha = alpha || it.label == "alpha";
    CHECK(alpha);
    // Nothing inside comments and strings.
    const auto doc3 = OcamlLspAnalyze(L("(* al *) let s = \"al\""), "");
    CHECK(OcamlLspCompletions(*doc3, 0, 5).empty());
    CHECK(OcamlLspCompletions(*doc3, 0, 20).empty());
}

// --- stdlib (compiled-in fallback) ----------------------------------------------------------------

void TestFallbackStdlib() {
    UseFallbackStdlib();
    const std::string src =
        "let () = print_endline \"hi\"\n"
        "let xs = List.map succ [1; 2]\n"
        "let n = List.length xs\n"
        "let s = String.concat \", \" []\n"
        "let o = Option.value ~default:0 None\n"
        "let () = List.\n";
    const auto doc = OcamlLspAnalyze(L(src), "");
    std::string h = HoverText(*doc, src, "print_endline");
    CHECK(Has(h, "val print_endline : string -> unit") && Has(h, "followed by a newline"));
    h = HoverText(*doc, src, "map succ", 1);
    CHECK(Has(h, "val map : ('a -> 'b) -> 'a list -> 'b list") && Has(h, "applies function"));
    h = HoverText(*doc, src, "List.map");
    CHECK(Has(h, "module List") && Has(h, "List operations"));
    const Pos p = At(src, "List.\n", 5);
    auto items = OcamlLspCompletions(*doc, p.line, p.col);
    bool map = false, fold = false;
    for (const auto &it : items) {
        map = map || (it.label == "map" && it.kind == OcamlLspKind::Function && Has(it.documentation, "applies"));
        fold = fold || it.label == "fold_left";
    }
    CHECK(map && fold);
    // Signature help.
    std::string line = "let ys = List.map (fun x -> x) ";
    auto d2 = OcamlLspAnalyze(L(line), "");
    auto sh = OcamlLspSignatureAt(*d2, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.label == "List.map : ('a -> 'b) -> 'a list -> 'b list" && sh.active_param == 1);
    CHECK(sh.params.size() == 2 && sh.params[1] == "'a list");
    line = "let ys = List.map ";
    d2 = OcamlLspAnalyze(L(line), "");
    sh = OcamlLspSignatureAt(*d2, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.active_param == 0);
    line = "let v = Option.value None ~default:";
    d2 = OcamlLspAnalyze(L(line), "");
    sh = OcamlLspSignatureAt(*d2, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.params.size() == 2 && sh.active_param == 1 && sh.params[1] == "default:'a");
    // A local function.
    line = "let add x y = x + y\nlet z = add 1 ";
    d2 = OcamlLspAnalyze(L(line), "");
    sh = OcamlLspSignatureAt(*d2, 1, 14);
    CHECK(sh.found && sh.label == "add x y" && sh.active_param == 1);
    // Unqualified, via open.
    const std::string src3 = "open List\nlet n = length [1]\n";
    const auto d3 = OcamlLspAnalyze(L(src3), "");
    CHECK(Has(HoverText(*d3, src3, "length"), "val length : 'a list -> int"));
    // No unbound-value complaints against the partial compiled-in stdlib.
    const auto d4 = OcamlLspAnalyze(L("let x = List.not_in_fallback 1\n"), "");
    for (const auto &d : OcamlLspDiagnostics(*d4)) CHECK(d.code != "unbound-value");
    // --doc.
    CHECK(Has(OcamlLspDocFor(*doc, "List.map"), "val map"));
    CHECK(Has(OcamlLspDocFor(*doc, "print_endline"), "val print_endline"));
    CHECK(Has(OcamlLspDocFor(*doc, "List"), "**Values:**"));
    CHECK(OcamlLspDocFor(*doc, "No_such_thing_anywhere").empty());
}

// --- the installed stdlib -----------------------------------------------------------------------------

void TestInstalledStdlib() {
    unsetenv("MEP_OCAML_STDLIB");
    unsetenv("MEP_OCAML_NO_FINDLIB");
    OcamlLspResetCaches();
    const std::string dir = mllsp::StdlibDir();
    if (dir.empty() || !fs::exists(fs::path(dir) / "list.mli")) {
        std::printf("ocaml_lsp_test: no installed OCaml stdlib, skipping installed-stdlib tests\n");
        UseFallbackStdlib();
        return;
    }
    const std::string src =
        "let xs = List.filter_map (fun x -> Some x) [1]\n"
        "let h = Hashtbl.create 16\n"
        "let () = Printf.printf \"%d\" 1\n"
        "let bad = List.no_such_function\n"
        "let ok = Stdlib.List.length []\n";
    const auto doc = OcamlLspAnalyze(L(src), "");
    std::string h = HoverText(*doc, src, "filter_map");
    CHECK(Has(h, "val filter_map : ('a -> 'b option) -> 'a list -> 'b list"));
    CHECK(Has(h, "**Since:**"));
    CHECK(Has(HoverText(*doc, src, "create"), "Hashtbl.create"));
    CHECK(Has(HoverText(*doc, src, "printf \""), "val printf"));
    CHECK(Has(HoverText(*doc, src, "length []"), "val length"));
    const Pos p = At(src, "filter_map");
    const auto defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && Has(defs[0].path, "list.ml"));
    const auto decl = OcamlLspDeclaration(*doc, p.line, p.col);
    CHECK(decl.size() == 1 && Has(decl[0].path, "list.mli"));
    bool unbound = false;
    for (const auto &d : OcamlLspDiagnostics(*doc)) {
        if (d.code == "unbound-value") {
            unbound = true;
            CHECK(d.range.line == 3 && Has(d.message, "List.no_such_function"));
        }
        CHECK(d.range.line == 3);  // nothing else is flagged
    }
    CHECK(unbound);
    const auto d2 = OcamlLspAnalyze(L("let x = Hasht"), "");
    bool hashtbl = false;
    for (const auto &it : OcamlLspCompletions(*d2, 0, 13)) hashtbl = hashtbl || it.label == "Hashtbl";
    CHECK(hashtbl);
    // Every installed stdlib interface indexes, and the deprecated ones are marked.
    int files = 0, deprecated = 0;
    for (const auto &e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".mli") continue;
        auto m = mllsp::LoadFile(e.path().string());
        CHECK(m != nullptr);
        files++;
        for (const auto &it : m->items) deprecated += it.is_deprecated ? 1 : 0;
    }
    CHECK(files > 30);
    std::printf("ocaml_lsp_test: installed stdlib: %d interfaces indexed, %d deprecated items\n", files, deprecated);
    UseFallbackStdlib();
}

// --- workspace modules -----------------------------------------------------------------------------

void TestWorkspace(const fs::path &root) {
    UseFallbackStdlib();
    Write(root / "dune-project", "(lang dune 3.0)\n");
    Write(root / "lib" / "geometry.mli",
          "(** Plane geometry. *)\n"
          "\n"
          "type point = { x : float; y : float }\n"
          "\n"
          "(** [distance a b] is the Euclidean distance.\n"
          "    @param a the first point\n"
          "    @param b the second point\n"
          "    @return a non-negative float *)\n"
          "val distance : point -> point -> float\n"
          "\n"
          "val origin : point\n"
          "(** The point [(0, 0)]. *)\n"
          "\n"
          "val scale : ?factor:float -> point -> point\n"
          "(** Scales a point. *)\n"
          "\n"
          "val old_dist : point -> point -> float\n"
          "[@@ocaml.deprecated \"use distance\"]\n"
          "\n"
          "module Shape : sig\n"
          "  type t = Circle of float | Square of float\n"
          "\n"
          "  val area : t -> float\n"
          "  (** The area of a shape. *)\n"
          "end\n");
    Write(root / "lib" / "geometry.ml",
          "type point = { x : float; y : float }\n"
          "let distance a b = sqrt (((a.x -. b.x) ** 2.) +. ((a.y -. b.y) ** 2.))\n"
          "let origin = { x = 0.; y = 0. }\n"
          "let scale ?(factor = 2.) p = { x = p.x *. factor; y = p.y *. factor }\n"
          "let old_dist = distance\n"
          "module Shape = struct\n"
          "  type t = Circle of float | Square of float\n"
          "  let area = function Circle r -> 3.14 *. r *. r | Square s -> s *. s\n"
          "end\n");
    const std::string main_src =
        "let d = Geometry.distance Geometry.origin Geometry.origin\n"
        "module G = Geometry\n"
        "let a = G.Shape.area (G.Shape.Circle 1.)\n"
        "open Geometry\n"
        "let p = scale ~factor:3. origin\n"
        "let q = Geometry.old_dist origin origin\n"
        "let r = Geometry.nope\n"
        "let s = Geometry.\n";
    const std::string main_path = (root / "bin" / "main.ml").string();
    Write(main_path, main_src);
    OcamlLspSetWorkspaceRoot(root.string());
    const auto doc = OcamlLspAnalyze(L(main_src), main_path);

    std::string h = HoverText(*doc, main_src, "distance");
    CHECK(Has(h, "val distance : point -> point -> float") && Has(h, "Euclidean distance"));
    CHECK(Has(h, "**Parameters**\n- `a`: the first point") && Has(h, "**Returns:** a non-negative float"));
    CHECK(Has(HoverText(*doc, main_src, "origin"), "The point `(0, 0)`."));
    h = HoverText(*doc, main_src, "area");
    CHECK(Has(h, "val area : t -> float") && Has(h, "The area of a shape."));
    CHECK(Has(HoverText(*doc, main_src, "Circle"), "Circle of float"));
    CHECK(Has(HoverText(*doc, main_src, "scale ~"), "Scales a point."));
    CHECK(Has(HoverText(*doc, main_src, "Geometry"), "Plane geometry."));

    // Definition: the implementation; declaration: the interface.
    Pos p = At(main_src, "distance");
    auto defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && Has(defs[0].path, "geometry.ml") && !Has(defs[0].path, ".mli") &&
          defs[0].range.line == 1 && defs[0].range.col == 4);
    auto decl = OcamlLspDeclaration(*doc, p.line, p.col);
    CHECK(decl.size() == 1 && Has(decl[0].path, "geometry.mli") && decl[0].range.line == 8);
    p = At(main_src, "Geometry");
    defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && Has(defs[0].path, "geometry.ml") && !Has(defs[0].path, ".mli"));
    p = At(main_src, "area");
    defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && Has(defs[0].path, "geometry.ml") && defs[0].range.line == 7);

    // Completion after `Geometry.`.
    p = At(main_src, "Geometry.\n", 9);
    auto items = OcamlLspCompletions(*doc, p.line, p.col);
    bool dist = false, shape = false;
    for (const auto &it : items) {
        dist = dist || (it.label == "distance" && it.detail == "point -> point -> float");
        shape = shape || (it.label == "Shape" && it.kind == OcamlLspKind::Module);
    }
    CHECK(dist && shape);
    const auto d2 = OcamlLspAnalyze(L("let x = Geo"), main_path);
    bool geo = false;
    for (const auto &it : OcamlLspCompletions(*d2, 0, 11)) geo = geo || it.label == "Geometry";
    CHECK(geo);

    // Signature help with @param docs and a labelled optional parameter.
    std::string line = "let z = Geometry.distance Geometry.origin ";
    auto d3 = OcamlLspAnalyze(L(line), main_path);
    auto sh = OcamlLspSignatureAt(*d3, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.active_param == 1 && sh.param_docs.size() == 2 && Has(sh.param_docs[1], "the second point"));
    line = "let z = Geometry.scale ~factor:";
    d3 = OcamlLspAnalyze(L(line), main_path);
    sh = OcamlLspSignatureAt(*d3, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.active_param == 0 && sh.params[0] == "?factor:float");

    // Diagnostics: deprecated, unbound.
    bool deprecated = false, unbound = false;
    for (const auto &d : OcamlLspDiagnostics(*doc)) {
        if (d.code == "deprecated") {
            deprecated = true;
            CHECK(d.range.line == 5 && d.deprecated && Has(d.message, "use distance"));
        }
        if (d.code == "unbound-value") {
            unbound = true;
            CHECK(d.range.line == 6 && d.severity == OcamlLspSeverity::Error);
        }
    }
    CHECK(deprecated && unbound);

    // The implementation, with its interface beside it: hovers show the interface's docs.
    const std::string impl_path = (root / "lib" / "geometry.ml").string();
    const std::string impl = ReadAll(impl_path);
    const auto di = OcamlLspAnalyze(L(impl), impl_path);
    h = HoverText(*di, impl, "distance");
    CHECK(Has(h, "val distance : point -> point -> float") && Has(h, "Euclidean"));
    p = At(impl, "distance");
    decl = OcamlLspDeclaration(*di, p.line, p.col);
    CHECK(decl.size() == 1 && Has(decl[0].path, "geometry.mli"));

    CHECK(Has(OcamlLspDocFor(*doc, "Geometry.distance"), "Euclidean"));
    CHECK(Has(OcamlLspDocFor(*doc, "Geometry.Shape.area"), "The area"));
    CHECK(Has(OcamlLspDocFor(*doc, "Geometry"), "Plane geometry"));
    OcamlLspSetWorkspaceRoot("");
}

// --- a library on MEP_OCAML_PATH: the Rosetta binding style -----------------------------------------

void TestLibraryPath(const fs::path &root) {
    UseFallbackStdlib();
    Write(root / "lib" / "datamunge" / "datamunge.mli",
          "(** Bindings to the datamunge statistics library. *)\n"
          "\n"
          "(** Descriptive statistics. *)\n"
          "module Stats : sig\n"
          "  (** [mean xs] is the arithmetic mean of [xs].\n"
          "      @param xs the samples (non-empty)\n"
          "      @raise Invalid_argument if [xs] is empty *)\n"
          "  val mean : float array -> float\n"
          "\n"
          "  (** [quantile ~p xs] is the [p]-quantile of [xs]. *)\n"
          "  val quantile : p:float -> float array -> float\n"
          "end\n"
          "\n"
          "(** A running counter. *)\n"
          "module Counter : sig\n"
          "  type t\n"
          "  (** The counter. *)\n"
          "\n"
          "  val create : int -> t\n"
          "  (** [create n] starts at [n]. *)\n"
          "\n"
          "  val add : t -> int -> int\n"
          "  (** [add c k] adds [k] and returns the new value. *)\n"
          "end\n");
    setenv("MEP_OCAML_PATH", (root / "lib").string().c_str(), 1);
    OcamlLspResetCaches();
    const std::string src =
        "let m = Datamunge.Stats.mean [| 1.; 2. |]\n"
        "let c = Datamunge.Counter.create 20\n"
        "open Datamunge\n"
        "let v = Counter.add c 22\n"
        "let q = Stats.quantile ~p:0.5 [||]\n";
    const std::string path = (root / "scratch" / "use.ml").string();
    const auto doc = OcamlLspAnalyze(L(src), path);
    std::string h = HoverText(*doc, src, "mean");
    CHECK(Has(h, "val mean : float array -> float") && Has(h, "arithmetic mean"));
    CHECK(Has(h, "- `xs`: the samples (non-empty)") && Has(h, "- `Invalid_argument`: if `xs` is empty"));
    CHECK(Has(HoverText(*doc, src, "create"), "starts at `n`"));
    CHECK(Has(HoverText(*doc, src, "add c"), "adds `k`"));
    CHECK(Has(HoverText(*doc, src, "Counter", 0, 1), "A running counter."));
    CHECK(Has(HoverText(*doc, src, "Datamunge"), "Bindings to the datamunge"));
    const Pos p = At(src, "mean");
    const auto defs = OcamlLspDefinition(*doc, p.line, p.col);
    CHECK(defs.size() == 1 && Has(defs[0].path, "datamunge.mli") && defs[0].range.line == 7);
    std::string line = "let q = Datamunge.Stats.quantile ~p:";
    auto d2 = OcamlLspAnalyze(L(line), path);
    auto sh = OcamlLspSignatureAt(*d2, 0, static_cast<int>(line.size()));
    CHECK(sh.found && sh.label == "Datamunge.Stats.quantile : p:float -> float array -> float" && sh.active_param == 0);
    // The .mli opened directly: its own symbols, docs on hover.
    const std::string mli_path = (root / "lib" / "datamunge" / "datamunge.mli").string();
    const std::string mli = ReadAll(mli_path);
    const auto dm = OcamlLspAnalyze(L(mli), mli_path);
    CHECK(Has(HoverText(*dm, mli, "mean :"), "arithmetic mean"));
    const auto syms = OcamlLspSymbols(*dm);
    bool stats = false, mean_child = false;
    for (const auto &s : syms) {
        if (s.name == "Stats" && s.kind == OcamlLspSymbolKind::Module) stats = true;
        if (s.name == "mean" && s.parent >= 0 && syms[static_cast<size_t>(s.parent)].name == "Stats") mean_child = true;
    }
    CHECK(stats && mean_child);
    for (const auto &d : OcamlLspDiagnostics(*dm)) CHECK(d.code != "syntax-error");
    unsetenv("MEP_OCAML_PATH");
    OcamlLspResetCaches();
}

// --- outline, folding, syntax errors, formatting ------------------------------------------------------

void TestOutline() {
    const std::string src =
        "(** Doc. *)\n"
        "type shape =\n"
        "  | Circle of float\n"
        "  | Rect of float * float\n"
        "\n"
        "exception Bad of string\n"
        "external c_sin : float -> float = \"sin\"\n"
        "\n"
        "module type S = sig\n"
        "  val name : string\n"
        "end\n"
        "\n"
        "module Impl : S = struct\n"
        "  let name = \"impl\"\n"
        "end\n"
        "\n"
        "class counter = object\n"
        "  val mutable n = 0\n"
        "  method incr = n <- n + 1\n"
        "end\n"
        "\n"
        "let area = function\n"
        "  | Circle r -> 3.14 *. r *. r\n"
        "  | Rect (w, h) -> w *. h\n";
    const auto doc = OcamlLspAnalyze(L(src), "/tmp/outline.ml");
    const auto syms = OcamlLspSymbols(*doc);
    auto find = [&](const std::string &n) -> const OcamlLspSymbol * {
        for (const auto &s : syms)
            if (s.name == n) return &s;
        return nullptr;
    };
    CHECK(find("shape") && find("shape")->kind == OcamlLspSymbolKind::Enum);
    CHECK(find("Circle") && find("Circle")->parent >= 0 &&
          syms[static_cast<size_t>(find("Circle")->parent)].name == "shape");
    CHECK(find("Bad") && find("Bad")->kind == OcamlLspSymbolKind::Event);
    CHECK(find("c_sin") && find("c_sin")->kind == OcamlLspSymbolKind::Function);
    CHECK(find("S") && find("S")->kind == OcamlLspSymbolKind::Interface);
    CHECK(find("Impl") && find("Impl")->kind == OcamlLspSymbolKind::Module);
    CHECK(find("counter") && find("counter")->kind == OcamlLspSymbolKind::Class);
    CHECK(find("incr") && find("incr")->kind == OcamlLspSymbolKind::Method);
    CHECK(find("area") && find("area")->range.line == 21);
    CHECK(Has(HoverText(*doc, src, "Circle r"), "Constructor of type `shape`"));
    const auto folds = OcamlLspFolds(*doc);
    bool type_fold = false, module_fold = false, fn_fold = false;
    for (const auto &f : folds) {
        type_fold = type_fold || (f.start_line == 1 && f.end_line == 3);
        module_fold = module_fold || (f.start_line == 12 && f.end_line == 14);
        fn_fold = fn_fold || (f.start_line == 21 && f.end_line == 23);
    }
    CHECK(type_fold && module_fold && fn_fold);

    const auto bad = OcamlLspAnalyze(L("let x = (1 + \nlet y = 2\n"), "");
    const auto diags = OcamlLspDiagnostics(*bad);
    CHECK(!diags.empty() && diags[0].code == "syntax-error" && diags[0].severity == OcamlLspSeverity::Error);
    CHECK(OcamlLspDiagnosticFails(diags[0]));
    CHECK(Has(OcamlLspFormatDiagnostic("a.ml", diags[0]), "a.ml:"));
    const auto mli = OcamlLspAnalyze(L("val f : int -> int\nmodule M : sig type t end\n"), "/tmp/x.mli");
    CHECK(OcamlLspDiagnostics(*mli).empty());
}

void TestFormatting() {
    std::string err;
    auto edits = OcamlLspFormat(L("let f x =\nx + 1\n\nlet g = 2\n"), 80, &err);
    CHECK(err.empty() && edits.size() == 1);
    CHECK(edits[0].range.line == 1 && edits[0].range.end_line == 2 && edits[0].new_text == "  x + 1\n");
    CHECK(OcamlLspFormat(L("let f x =\n  x + 1\n"), 80, &err).empty() && err.empty());
    edits = OcamlLspFormat(L("let s = \"unterminated\n"), 80, &err);
    CHECK(edits.empty() && !err.empty());
    err.clear();
    edits = OcamlLspFormatRange(L("let f x =\nx + 1\nlet g y =\ny\n"), 3, 3, 80, &err);
    CHECK(err.empty() && edits.size() == 1 && edits[0].range.line == 3 && edits[0].new_text == "  y");
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src =
        "(** Doc *)\n"
        "module M = struct\n"
        "  type t = A | B of { x : int }\n"
        "  let rec f ?(k = 1) ~l = function A -> k | B { x } -> x + l\n"
        "end\n"
        "open M\n"
        "let () = List.iter (fun s -> print_endline s) [\"a\"; {|q|}]\n"
        "let x = M.f ~l:2 (B { x = 1 }) |> string_of_int\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const auto lines = L(src.substr(0, n));
        const auto doc = OcamlLspAnalyze(lines, "");
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        OcamlLspDiagnostics(*doc);
        OcamlLspCompletions(*doc, last, col);
        OcamlLspHover(*doc, last, col);
        OcamlLspSignatureAt(*doc, last, col);
        OcamlLspDefinition(*doc, last, col);
        OcamlLspReferences(*doc, last, col);
        OcamlLspSymbols(*doc);
        OcamlLspFolds(*doc);
        for (int c = 0; c <= col; c += 3) OcamlLspHover(*doc, last, c);
    }
    CHECK(true);
}

}  // namespace

int main() {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / ("mep-ocaml-lsp-test-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    UseFallbackStdlib();
    TestOdoc();
    TestArrows();
    TestDocAttachment();
    TestLocals();
    TestFallbackStdlib();
    TestWorkspace(tmp / "ws");
    TestLibraryPath(tmp / "libs");
    TestOutline();
    TestFormatting();
    TestRobustness();
    TestInstalledStdlib();
    fs::remove_all(tmp, ec);
    std::printf("ocaml_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
