// Windowless test for lua_lsp.cpp -- the analysis half of mep's own
// Lua language server (`mep-lua-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert().
//
// The parser's agreement with luac is checked against the real compiler by
// tools/lua_parse_check.py over a corpus; the luac cases here pin a few
// messages so a regression shows up without luac installed.
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lua_lsp.h"
#include "lua_lsp_meta.h"

#if defined(_WIN32)
#include <process.h>
#include <stdlib.h>
#define getpid _getpid
static int setenv(const char *k, const char *v, int) { return _putenv_s(k, v); }
static int unsetenv(const char *k) { return _putenv_s(k, ""); }
#else
#include <unistd.h>
#endif

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

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

std::vector<LuaLspDiagnostic> Diags(const std::string &src) { return LuaLspDiagnostics(*LuaLspAnalyze(L(src))); }

int Count(const std::vector<LuaLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const auto &d : ds) {
        if (d.code == code) n++;
    }
    return n;
}

const LuaLspDiagnostic *Find(const std::vector<LuaLspDiagnostic> &ds, const std::string &code) {
    for (const auto &d : ds) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

void TestLuac() {
    CHECK(LuaLspLuacCheck("local x = 1\nreturn x\n", "t.lua").empty());
    CHECK(LuaLspLuacCheck("x = = 1", "t.lua") == "t.lua:1: unexpected symbol near '='");
    CHECK(LuaLspLuacCheck("if x then\nprint(1)\n", "t.lua") == "t.lua:3: 'end' expected (to close 'if' at line 1) near <eof>");
    CHECK(LuaLspLuacCheck("local s = \"abc\n", "t.lua") == "t.lua:1: unfinished string near '\"abc'");
    CHECK(LuaLspLuacCheck("goto nowhere", "t.lua") == "t.lua:1: no visible label 'nowhere' for <goto> at line 1");
    CHECK(LuaLspLuacCheck("local x <const> = 1\nx = 2", "t.lua") ==
          "t.lua:2: attempt to assign to const variable 'x'");
    CHECK(LuaLspLuacCheck("local x <foo> = 1", "t.lua") == "t.lua:1: unknown attribute 'foo'");
    CHECK(LuaLspLuacCheck("function f() return ... end", "t.lua") ==
          "t.lua:1: cannot use '...' outside a vararg function near '...'");
    CHECK(LuaLspLuacCheck("for i = 1, 2 do break end\nbreak", "t.lua") == "t.lua:2: break outside loop at line 2");
    CHECK(LuaLspLuacCheck("x = 0x", "t.lua") == "t.lua:1: malformed number near '0x'");
    // Shebang lines are skipped, as luaL_loadfile does.
    CHECK(LuaLspLuacCheck("#!/usr/bin/lua\nprint(1)\n", "t.lua").empty());
}

void TestSyntaxDiagnostic() {
    const auto ds = Diags("local a = 1\nlocal b = = 2\nprint(a, b)\n");
    CHECK(!ds.empty() && ds[0].severity == LuaLspSeverity::Error);
    CHECK(ds[0].range.line == 1 && ds[0].message == "unexpected symbol near '='");
    // No unused-* on a repaired tree.
    CHECK(Count(ds, "unused-local") == 0);
}

void TestUnused() {
    auto ds = Diags("local used = 1\nlocal unused = 2\nlocal _ignored = 3\nprint(used)\n");
    CHECK(Count(ds, "unused-local") == 1);
    const auto *d = Find(ds, "unused-local");
    CHECK(d && d->range.line == 1 && d->range.col == 6 && d->unnecessary);
    CHECK(d && d->severity == LuaLspSeverity::Warning);

    // Assigned but never read.
    ds = Diags("local x\nx = 5\n");
    CHECK(Count(ds, "unused-value") == 1);

    // An argument is reported only when nothing after it is used, and only for a function with its own signature.
    ds = Diags("local function f(a, b, c)\n  return b\nend\nreturn f\n");
    CHECK(Count(ds, "unused-argument") == 1);
    d = Find(ds, "unused-argument");
    CHECK(d && d->message == "unused argument 'c'" && d->severity == LuaLspSeverity::Hint);
    ds = Diags("local t = {}\nfunction t.cb(ev) end\nfunction t:m(x) end\ncall(function(y) end)\nreturn t\n");
    CHECK(Count(ds, "unused-argument") == 0);

    // A loop variable.
    ds = Diags("for i, v in ipairs(t) do print(v) end\n");
    CHECK(Count(ds, "unused-loop-variable") == 1);
    // A to-be-closed variable is used by being closed.
    ds = Diags("do local f <close> = io.open('x') end\n");
    CHECK(Count(ds, "unused-local") == 0);
    // Used from a nested function.
    ds = Diags("local n = 0\nreturn function() n = n + 1 return n end\n");
    CHECK(Count(ds, "unused-local") == 0 && Count(ds, "unused-value") == 0);
}

void TestShadowing() {
    auto ds = Diags("local x = 1\nprint(x)\ndo\n  local x = 2\n  print(x)\nend\n");
    CHECK(Count(ds, "shadowed-local") == 1);
    ds = Diags("local x = 1\nprint(x)\nlocal x = 2\nprint(x)\n");
    CHECK(Count(ds, "redefined-local") == 1);
    // `local x = x or 1` reads the outer x: deliberate.
    ds = Diags("local x = 1\nprint(x)\ndo local x = x or 2 print(x) end\n");
    CHECK(Count(ds, "shadowed-local") == 0);
    // A parameter named like an outer local: a different function, ordinary.
    ds = Diags("local path = 1\nprint(path)\nlocal function f(path) return path end\nreturn f\n");
    CHECK(Count(ds, "shadowed-local") == 0);
}

void TestGlobals() {
    // A typo of a known name.
    auto ds = Diags("pirnt('hi')\n");
    const auto *d = Find(ds, "undefined-global");
    CHECK(d && d->message == "undefined global 'pirnt'; did you mean 'print'?");
    ds = Diags("local config = {}\nreturn confgi\n");
    d = Find(ds, "undefined-global");
    CHECK(d && d->message == "undefined global 'confgi'; did you mean 'config'?");
    // An unknown name that is not a near miss: some other file or the host defines it.
    ds = Diags("describe('x', function() it('y', function() end) end)\nfoo_from_elsewhere()\n");
    CHECK(Count(ds, "undefined-global") == 0);
    // Defined in this file.
    ds = Diags("function helper() end\nhelper()\n");
    CHECK(Count(ds, "undefined-global") == 0);
    // mep's API is known.
    ds = Diags("mep.map('n', 'x', function() end)\n");
    CHECK(Count(ds, "undefined-global") == 0);
    // luacheck inline declarations.
    ds = Diags("-- luacheck: globals prnt\nprnt()\n");
    CHECK(Count(ds, "undefined-global") == 0);

    // Names from other Lua versions, unless the file tests for them.
    ds = Diags("local a, b = unpack(t)\nreturn a, b\n");
    d = Find(ds, "deprecated-global");
    CHECK(d && d->deprecated && d->severity == LuaLspSeverity::Information);
    ds = Diags("local unpack = table.unpack or unpack\nreturn unpack(t)\n");
    CHECK(Count(ds, "deprecated-global") == 0);
    ds = Diags("local env = getfenv and getfenv() or _ENV\nreturn env\n");
    CHECK(Count(ds, "deprecated-global") == 0);

    // Setting a global in a module.
    ds = Diags("local M = {}\nhelper = 1\nfunction M.f() end\nreturn M\n");
    CHECK(Count(ds, "global-in-module") == 1);
    ds = Diags("helper = 1\nreturn 0\n");
    CHECK(Count(ds, "global-in-module") == 0);
}

void TestStatementLints() {
    auto ds = Diags("local a, b = 1, 2, 3\nreturn a, b\n");
    CHECK(Count(ds, "unbalanced-assignment") == 1);
    ds = Diags("local a, b = 1\nreturn a, b\n");
    CHECK(Count(ds, "unbalanced-assignment") == 1);
    ds = Diags("local a, b = f()\nlocal c, d = nil\nreturn a, b, c, d\n");
    CHECK(Count(ds, "unbalanced-assignment") == 0);

    ds = Diags("local t = {a = 1, b = 2, a = 3, [\"b\"] = 4, [1] = 5, 6}\nreturn t\n");
    CHECK(Count(ds, "duplicate-key") == 2);

    ds = Diags("for i = 1, 3 do\n  break\n  print(i)\nend\n");
    CHECK(Count(ds, "unreachable-code") == 1);
    ds = Diags("for i = 1, 3 do\n  goto continue\n  print(i)\n  ::continue::\nend\n");
    CHECK(Count(ds, "unreachable-code") == 1);

    ds = Diags("if x then\nend\nif y then\n  -- nothing yet\nend\n");
    CHECK(Count(ds, "empty-block") == 1);

    ds = Diags("local s = string.format('%d %s', 1)\nlocal t = ('%5.2f%%'):format(1, 2)\nreturn s, t\n");
    CHECK(Count(ds, "format-arguments") == 2);
    ds = Diags("local s = string.format('%d %s', f())\nreturn s\n");
    CHECK(Count(ds, "format-arguments") == 0);

    ds = Diags("local x = 1\nx = x\nreturn x\n");
    CHECK(Count(ds, "self-assignment") == 1);

    ds = Diags("::top::\nprint(1)\n");
    CHECK(Count(ds, "unused-label") == 1);

    // `-- luacheck: ignore` on a line silences it.
    ds = Diags("local unused = 1 -- luacheck: ignore\n");
    CHECK(Count(ds, "unused-local") == 0);
}

void TestHover() {
    const auto doc = LuaLspAnalyze(L("-- Adds two numbers.\nlocal function add(a, b)\n  return a + b\nend\n"
                                     "print(add(1, 2))\nlocal s = string.format('%d', 1)\nlocal n = mep.get_line(1)\n"
                                     "local M = {}\n--- Greets.\nfunction M.greet(name) return name end\nM.greet('x')\n"
                                     "local u = s:upper()\n"));
    auto h = LuaLspHover(*doc, 4, 7);  // add
    CHECK(h.found && h.text.find("local function add(a, b)") != std::string::npos);
    CHECK(h.text.find("Adds two numbers.") != std::string::npos);
    h = LuaLspHover(*doc, 2, 9);  // a (parameter)
    CHECK(h.found && h.text.find("(parameter) a") != std::string::npos);
    h = LuaLspHover(*doc, 4, 1);  // print
    CHECK(h.found && h.text.find("function print(...)") != std::string::npos);
    h = LuaLspHover(*doc, 5, 18);  // format
    CHECK(h.found && h.text.find("function string.format(formatstring, ...)") != std::string::npos);
    CHECK(h.text.find("sprintf") != std::string::npos);
    h = LuaLspHover(*doc, 6, 15);  // get_line
    CHECK(h.found && h.text.find("function mep.get_line(row)") != std::string::npos);
    h = LuaLspHover(*doc, 10, 3);  // M.greet use
    CHECK(h.found && h.text.find("function M.greet(name)") != std::string::npos);
    CHECK(h.text.find("Greets.") != std::string::npos);
    h = LuaLspHover(*doc, 11, 13);  // s:upper
    CHECK(h.found && h.text.find("string.upper") != std::string::npos);
    h = LuaLspHover(*doc, 0, 3);  // in a comment
    CHECK(!h.found);
}

void TestCompletion() {
    auto doc = LuaLspAnalyze(L("local alpha = 1\nlocal function beta() end\nal"));
    auto items = LuaLspCompletions(*doc, 2, 2);
    CHECK(!items.empty() && items[0].label == "alpha");
    CHECK(items[0].replace_start == 0 && items[0].replace_end == 2);

    doc = LuaLspAnalyze(L("string.fo"));
    items = LuaLspCompletions(*doc, 0, 9);
    bool format = false;
    for (const auto &it : items) format = format || it.label == "format";
    CHECK(format && items.size() == 1);

    doc = LuaLspAnalyze(L("mep.get_"));
    items = LuaLspCompletions(*doc, 0, 8);
    bool get_line = false;
    for (const auto &it : items) get_line = get_line || it.label == "get_line";
    CHECK(get_line);

    // Members this file assigns, through a local root.
    doc = LuaLspAnalyze(L("local M = {size = 1}\nfunction M.run() end\nM."));
    items = LuaLspCompletions(*doc, 2, 2);
    bool run = false, size = false;
    for (const auto &it : items) {
        run = run || it.label == "run";
        size = size || it.label == "size";
    }
    CHECK(run && size);

    // Keywords, and nothing inside a string or comment.
    doc = LuaLspAnalyze(L("loc"));
    items = LuaLspCompletions(*doc, 0, 3);
    CHECK(!items.empty() && items[0].label == "local");
    doc = LuaLspAnalyze(L("print('loc"));
    CHECK(LuaLspCompletions(*doc, 0, 10).empty());
    doc = LuaLspAnalyze(L("-- loc"));
    CHECK(LuaLspCompletions(*doc, 0, 6).empty());
    // A local not yet in scope is not offered.
    doc = LuaLspAnalyze(L("lat\nlocal later = 1"));
    items = LuaLspCompletions(*doc, 0, 3);
    for (const auto &it : items) CHECK(!(it.label == "later" && it.kind == LuaLspKind::Variable));
}

void TestSignature() {
    auto doc = LuaLspAnalyze(L("string.format('%d', "));
    auto sh = LuaLspSignatureAt(*doc, 0, 20);
    CHECK(sh.found && sh.label == "string.format(formatstring, ...)" && sh.active_param == 1);
    CHECK(sh.params.size() == 2 && sh.params[0] == "formatstring");

    doc = LuaLspAnalyze(L("local function add(a, b) return a + b end\nadd(1, "));
    sh = LuaLspSignatureAt(*doc, 1, 7);
    CHECK(sh.found && sh.label == "add(a, b)" && sh.active_param == 1);

    doc = LuaLspAnalyze(L("local s = 'x'\ns:sub("));
    sh = LuaLspSignatureAt(*doc, 1, 6);
    CHECK(sh.found && sh.params.size() == 2 && sh.params[0] == "i" && sh.active_param == 0);

    doc = LuaLspAnalyze(L("print(f(1), "));
    sh = LuaLspSignatureAt(*doc, 0, 12);
    CHECK(sh.found && sh.label == "print(...)" && sh.active_param == 1);
}

void TestNavigation() {
    const auto doc = LuaLspAnalyze(L("local x = 1\nlocal function f()\n  return x + 1\nend\nx = f()\n"
                                     "for i = 1, 2 do goto skip ::skip:: end\nglobal_thing = 1\nprint(global_thing)\n"));
    auto defs = LuaLspDefinition(*doc, 2, 9);  // x in f
    CHECK(defs.size() == 1 && defs[0].range.line == 0 && defs[0].range.col == 6);
    auto refs = LuaLspReferences(*doc, 0, 6);
    CHECK(refs.size() == 3 && refs[0].is_definition);
    defs = LuaLspDefinition(*doc, 5, 22);  // goto skip
    CHECK(defs.size() == 1 && defs[0].range.line == 5 && defs[0].range.col == 28);
    defs = LuaLspDefinition(*doc, 7, 8);  // global_thing
    CHECK(defs.size() == 1 && defs[0].range.line == 6);
    refs = LuaLspReferences(*doc, 7, 8);
    CHECK(refs.size() == 2);

    // Rename a local, and refuse captures.
    std::string err;
    auto edits = LuaLspRename(*doc, 0, 6, "y", &err);
    CHECK(err.empty() && edits.size() == 3);
    CHECK(LuaLspPrepareRename(*doc, 0, 6).found);
    CHECK(!LuaLspPrepareRename(*doc, 7, 1).found);  // print: a global
    edits = LuaLspRename(*doc, 0, 6, "f", &err);
    CHECK(edits.empty() && !err.empty());
    err.clear();
    edits = LuaLspRename(*doc, 0, 6, "print", &err);
    CHECK(edits.empty() && !err.empty());
    err.clear();
    edits = LuaLspRename(*doc, 0, 6, "end", &err);
    CHECK(edits.empty() && !err.empty());
}

void TestSymbolsFolds() {
    const auto doc = LuaLspAnalyze(L("local M = {}\n\nfunction M.new(x)\n  local function helper() end\n  return x\nend\n\n"
                                     "function M:method()\nend\n\n-- one\n-- two\n-- three\nlocal t = {\n  a = 1,\n  "
                                     "b = function() end,\n}\nreturn M\n"));
    const auto syms = LuaLspSymbols(*doc);
    bool newf = false, method = false, helper = false, field_b = false;
    for (const auto &s : syms) {
        if (s.name == "M.new" && s.kind == LuaLspSymbolKind::Function) newf = true;
        if (s.name == "M:method" && s.kind == LuaLspSymbolKind::Method) method = true;
        if (s.name == "helper" && s.parent >= 0 && syms[static_cast<size_t>(s.parent)].name == "M.new") helper = true;
        if (s.name == "b" && s.parent >= 0 && syms[static_cast<size_t>(s.parent)].name == "t") field_b = true;
    }
    CHECK(newf && method && helper && field_b);
    const auto folds = LuaLspFolds(*doc);
    bool fn_fold = false, comment_fold = false, table_fold = false;
    for (const auto &f : folds) {
        if (f.start_line == 2 && f.end_line == 4) fn_fold = true;
        if (f.start_line == 10 && f.end_line == 12 && f.kind == "comment") comment_fold = true;
        if (f.start_line == 13 && f.end_line == 15) table_fold = true;
    }
    CHECK(fn_fold && comment_fold && table_fold);
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src =
        "#!/usr/bin/env lua\n-- A module.\nlocal M = {}\nlocal fmt <const> = string.format\n\n"
        "--- Makes one.\nfunction M.new(name, ...)\n  local self = setmetatable({name = name, n = select('#', ...)}, "
        "{__index = M})\n  return self\nend\n\nfunction M:greet(greeting)\n  for i = 1, self.n do\n    if i % 2 == 0 "
        "then goto continue end\n    print(fmt('%s, %s! (%d)', greeting or 'hi', self.name, i))\n    "
        "::continue::\n  end\n  local t = {[1] = 'a'; 'b', x = [[long\nstring]], y = 0x1p4, z = 3 // 2}\n  "
        "repeat local k = next(t) until not k\n  while false do end\n  return #t, t.x:upper(), ~1 << 2\nend\n\n"
        "return M\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const auto lines = L(src.substr(0, n));
        const auto doc = LuaLspAnalyze(lines);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        LuaLspDiagnostics(*doc);
        LuaLspCompletions(*doc, last, col);
        LuaLspHover(*doc, last, col);
        LuaLspSignatureAt(*doc, last, col);
        LuaLspDefinition(*doc, last, col);
        LuaLspReferences(*doc, last, col);
        LuaLspPrepareRename(*doc, last, col);
        LuaLspSymbols(*doc);
        LuaLspFolds(*doc);
    }
    // The whole file is clean.
    const auto ds = Diags(src);
    for (const auto &d : ds) std::fprintf(stderr, "  %s\n", LuaLspFormatDiagnostic("robust.lua", d).c_str());
    CHECK(ds.empty());
}

// Formatting edits only the lines that change, and a formatted file (with
// or without its final newline) gets no edit at all.
void TestFormat() {
    std::string err;
    CHECK(LuaLspFormat({"local x = 1", ""}, 0, &err).empty());
    CHECK(LuaLspFormat({"local x = 1"}, 0, &err).empty());
    const auto e = LuaLspFormat({"local x = 1", "local y  =  2", "return x + y", ""}, 0, &err);
    CHECK(e.size() == 1);
    CHECK(!e.empty() && e[0].range.line == 1 && e[0].range.end_line == 2 && e[0].new_text == "local y = 2\n");
    CHECK(LuaLspFormat({"local = 1"}, 0, &err).empty() && !err.empty());
}


// ---- LuaCATS meta stubs (lua_lsp_meta.h) -------------------------------------------------------------------------

const char *const kStub = R"(---@meta datamunge
-- Generated by Rosetta: never loaded at run time.

---@class datamunge
local datamunge = {}

---A mutable cell for C++ reference parameters.
---@param value any The initial value.
---@return datamunge.ref
function datamunge.ref(value) end

---@class datamunge.ref
---@field value any

---Adds three integers, Socha & Dorigo style.
---
---Longer description paragraph.
---@param a integer first term
---@param b? integer second term (default: 10)
---@param c? integer third term (default: 100)
---@return integer # the sum
function datamunge.add(a, b, c) end

---Hypothesis test sidedness.
---@enum datamunge.Alternative
datamunge.Alternative = {
    TwoSided = 0,
    Less = 1,
    Greater = 2,
}

---A dual number value + derivative*eps.
---@class datamunge.Dual
---@overload fun(value: number, derivative?: number): datamunge.Dual
datamunge.Dual = {}

---The real part: the function value.
---@return number
function datamunge.Dual:value() end

---Product: this * `other`, with derivative a'b + ab' (product rule).
---@param other datamunge.Dual Right operand.
---@return datamunge.Dual # A new Dual; neither operand changes.
function datamunge.Dual:multiply(other) end

---Sum.
---@param other datamunge.Dual|number
---@return datamunge.Dual
function datamunge.Dual:add(other) end

---Reverse-mode tape.
---@class datamunge.Tape
---@overload fun(): datamunge.Tape
datamunge.Tape = {}

---Reverse sweep from `output`.
---@param output datamunge.Var A Var recorded on this tape.
---@return number[] # One adjoint per node.
function datamunge.Tape:backward(output) end

---Records a new independent leaf variable on `tape`.
---@class datamunge.Var
---@overload fun(tape: datamunge.Tape, value: number): datamunge.Var
datamunge.Var = {}

---This node's index on its Tape.
---@return integer
function datamunge.Var:index() end

---A 2-D vector.
---@class datamunge.Vec2
---@field x number Horizontal component.
---@field y number Vertical component.
---@overload fun(x: number, y: number): datamunge.Vec2
---@overload fun(): datamunge.Vec2
datamunge.Vec2 = {}

---Base shape.
---@class datamunge.Shape
datamunge.Shape = {}

---Area of the shape.
---@return number
function datamunge.Shape:area() end

---A circle.
---@class datamunge.Circle: datamunge.Shape
---@overload fun(radius: number): datamunge.Circle
datamunge.Circle = {}

---Builds a unit circle (static method).
---@return datamunge.Circle
function datamunge.Circle.unit() end

return datamunge
)";

bool Has(const std::vector<LuaLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

bool Contains(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

void TestMetaParse() {
    const auto m = LuaMetaParse(kStub, "datamunge.lua");
    CHECK(m->meta_name == "datamunge" && m->root_type == "datamunge");
    const LuaMetaMember *add = m->Member("datamunge", "add");
    CHECK(add && add->kind == LuaMetaMember::Function && add->func->params.size() == 3);
    CHECK(add->func->params[1].name == "b" && add->func->params[1].optional && add->func->params[1].type == "integer");
    CHECK(add->func->returns.size() == 1 && add->func->returns[0].doc == "the sum");
    CHECK(add->func->doc == "Adds three integers, Socha & Dorigo style.\n\nLonger description paragraph.");
    const LuaMetaEnum *alt = m->Enum("datamunge.Alternative");
    CHECK(alt && alt->members.size() == 3 && alt->members[1].first == "Less" && alt->members[1].second == "1");
    const LuaMetaClass *circle = m->Class("datamunge.Circle");
    CHECK(circle && circle->bases.size() == 1 && circle->bases[0] == "datamunge.Shape");
    CHECK(m->Member("datamunge.Circle", "area") != nullptr);  // inherited
    const LuaMetaClass *vec = m->Class("datamunge.Vec2");
    CHECK(vec && vec->fields.size() == 2 && vec->overloads.size() == 2);
    CHECK(LuaMetaFunReturn(vec->overloads[0]) == "datamunge.Vec2");
    CHECK(LuaMetaFunParams("fun(value: number, derivative?: number): datamunge.Dual").size() == 2);
    // `ref` is both a function of the module and a class.
    CHECK(m->Member("datamunge", "ref")->kind == LuaMetaMember::Function && m->Class("datamunge.ref"));
    CHECK(LuaMetaClassOfType("datamunge.Dual?") == "datamunge.Dual");
    CHECK(LuaMetaClassOfType("nil|datamunge.Dual") == "datamunge.Dual");
    CHECK(LuaMetaClassOfType("number[]").empty());
}

void WriteFile(const std::filesystem::path &p, const std::string &text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

void TestMetaStubs() {
    namespace fs = std::filesystem;
    const char *tmp_env = std::getenv("TMPDIR");
    const fs::path root = fs::path(tmp_env && *tmp_env ? tmp_env : "/tmp") / ("mep-lua-lsp-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    WriteFile(root / "build/lua/prefix/share/lua/library/datamunge.lua", kStub);
    WriteFile(root / ".luarc.json", "{\"workspace.library\": [\"build/lua/prefix/share/lua/library\"]}\n");
    // An ordinary module in the library is code, not a stub: ignored.
    WriteFile(root / "build/lua/prefix/share/lua/library/plain.lua", "return {}\n");
    const std::string path = (root / "src/example.lua").string();

    const auto dirs = LuaMetaLibraryDirs(path);
    CHECK(!dirs.empty() && Contains(dirs.back(), "share/lua/library"));
    CHECK(LuaMetaFind(dirs, "datamunge") != nullptr);
    CHECK(LuaMetaFind(dirs, "plain") == nullptr);
    CHECK(LuaMetaFind(dirs, "../x") == nullptr);

    const std::string src =
        "local m = require(\"datamunge\")\n"                       // 0
        "local x = m.Dual(2, 1)\n"                                 // 1
        "local y = x:multiply(x)\n"                                // 2
        "local z = x:multiply(y):add(x)\n"                         // 3
        "local t = m.Tape()\n"                                     // 4
        "local v = m.Var(t, 2)\n"                                  // 5
        "print(m.add(1, 2), m.Alternative.Less, v:index(), z)\n"   // 6
        "local c = m.Circle.unit()\n"                              // 7
        "print(c:area(), t, m.Whatever)\n";                        // 8
    const auto doc = LuaLspAnalyze(L(src), path);

    // Hover.
    auto h = LuaLspHover(*doc, 6, 9);  // m.add
    std::printf("--- hover m.add ---\n%s\n", h.text.c_str());
    CHECK(h.found && Contains(h.text, "function datamunge.add(a: integer, b?: integer, c?: integer) -> integer"));
    CHECK(Contains(h.text, "Socha & Dorigo") && Contains(h.text, "first term") && Contains(h.text, "the sum"));
    h = LuaLspHover(*doc, 2, 14);  // x:multiply
    std::printf("--- hover x:multiply ---\n%s\n", h.text.c_str());
    CHECK(h.found && Contains(h.text, "function datamunge.Dual:multiply(other: datamunge.Dual) -> datamunge.Dual"));
    CHECK(Contains(h.text, "product rule"));
    h = LuaLspHover(*doc, 3, 25);  // x:multiply(y):add
    CHECK(h.found && Contains(h.text, "function datamunge.Dual:add("));
    h = LuaLspHover(*doc, 1, 13);  // m.Dual
    CHECK(h.found && Contains(h.text, "class datamunge.Dual") && Contains(h.text, "datamunge.Dual(value: number, derivative?: number)"));
    CHECK(Contains(h.text, "`multiply`"));
    h = LuaLspHover(*doc, 6, 23);  // m.Alternative
    CHECK(h.found && Contains(h.text, "enum datamunge.Alternative") && Contains(h.text, "Greater = 2"));
    h = LuaLspHover(*doc, 6, 33);  // .Less
    CHECK(h.found && Contains(h.text, "datamunge.Alternative.Less = 1") && Contains(h.text, "sidedness"));
    h = LuaLspHover(*doc, 6, 42);  // v:index
    CHECK(h.found && Contains(h.text, "datamunge.Var:index() -> integer"));
    h = LuaLspHover(*doc, 8, 9);  // c:area, inherited through datamunge.Circle: datamunge.Shape
    CHECK(h.found && Contains(h.text, "datamunge.Shape:area() -> number"));
    h = LuaLspHover(*doc, 3, 6);  // the local z
    CHECK(h.found && Contains(h.text, "local z: datamunge.Dual"));
    h = LuaLspHover(*doc, 0, 6);  // the local m
    CHECK(h.found && Contains(h.text, "local m: datamunge"));
    // A receiver of unknown type (a parameter): the stubs' methods of that name.
    {
        const std::string line = "local function f(p) return p:multiply(p):index() end";
        const auto pdoc = LuaLspAnalyze(L("local m = require(\"datamunge\")\n" + line + "\n"), path);
        h = LuaLspHover(*pdoc, 1, static_cast<int>(line.find("multiply")) + 1);
        CHECK(h.found && Contains(h.text, "datamunge.Dual:multiply(other: datamunge.Dual)"));
        h = LuaLspHover(*pdoc, 1, static_cast<int>(line.find("index")) + 1);
        CHECK(h.found && Contains(h.text, "datamunge.Var:index() -> integer"));
        const auto nodoc = LuaLspAnalyze(L(line + "\n"), path);  // no require: no stubs
        CHECK(!LuaLspHover(*nodoc, 0, static_cast<int>(line.find("multiply")) + 1).found);
    }

    // No diagnostics about stub members (nor anything else in this file).
    const auto ds = LuaLspDiagnostics(*doc);
    for (const auto &d : ds) std::printf("unexpected diagnostic: %s\n", d.message.c_str());
    CHECK(ds.empty());

    // Completion.
    auto cdoc = LuaLspAnalyze(L(src + "m."), path);
    auto items = LuaLspCompletions(*cdoc, 9, 2);
    CHECK(Has(items, "add") && Has(items, "Dual") && Has(items, "Alternative") && Has(items, "ref") && Has(items, "Circle"));
    for (const auto &it : items) {
        if (it.label == "add") CHECK(it.kind == LuaLspKind::Function && Contains(it.documentation, "Socha"));
        if (it.label == "Dual") CHECK(it.kind == LuaLspKind::Class);
        if (it.label == "Alternative") CHECK(it.kind == LuaLspKind::Enum);
    }
    cdoc = LuaLspAnalyze(L(src + "x:"), path);
    items = LuaLspCompletions(*cdoc, 9, 2);
    CHECK(Has(items, "multiply") && Has(items, "value") && Has(items, "add") && !Has(items, "upper"));
    cdoc = LuaLspAnalyze(L(src + "local w = x:multiply(y):mu"), path);
    items = LuaLspCompletions(*cdoc, 9, 26);
    CHECK(items.size() == 1 && items[0].label == "multiply" && items[0].replace_start == 24);
    cdoc = LuaLspAnalyze(L(src + "m.Alternative."), path);
    items = LuaLspCompletions(*cdoc, 9, 14);
    CHECK(items.size() == 3 && Has(items, "TwoSided") && items[0].kind == LuaLspKind::EnumMember);
    cdoc = LuaLspAnalyze(L(src + "m.Circle."), path);
    items = LuaLspCompletions(*cdoc, 9, 9);
    CHECK(Has(items, "unit") && Has(items, "area"));
    cdoc = LuaLspAnalyze(L(src + "c:"), path);
    items = LuaLspCompletions(*cdoc, 9, 2);
    CHECK(Has(items, "area") && !Has(items, "unit"));
    cdoc = LuaLspAnalyze(L(src + "local p = m.Vec2(1, 2)\np."), path);
    items = LuaLspCompletions(*cdoc, 10, 2);
    CHECK(Has(items, "x") && Has(items, "y"));
    // A require typed straight into a chain.
    cdoc = LuaLspAnalyze(L("require('datamunge').Du"), path);
    items = LuaLspCompletions(*cdoc, 0, 23);
    CHECK(items.size() == 1 && items[0].label == "Dual");

    // Signature help.
    auto sdoc = LuaLspAnalyze(L(src + "m.add(1, "), path);
    auto sh = LuaLspSignatureAt(*sdoc, 9, 9);
    CHECK(sh.found && sh.label == "datamunge.add(a: integer, b?: integer, c?: integer) -> integer");
    CHECK(sh.active_param == 1 && sh.params.size() == 3 && sh.params[1] == "b?: integer");
    sdoc = LuaLspAnalyze(L(src + "x:multiply(y):add("), path);
    sh = LuaLspSignatureAt(*sdoc, 9, 18);
    CHECK(sh.found && sh.label == "datamunge.Dual:add(other: datamunge.Dual|number) -> datamunge.Dual");
    sdoc = LuaLspAnalyze(L(src + "m.Vec2(1, "), path);
    sh = LuaLspSignatureAt(*sdoc, 9, 10);
    CHECK(sh.found && sh.label == "datamunge.Vec2(x: number, y: number)" && sh.active_param == 1);
    sdoc = LuaLspAnalyze(L(src + "m.Dual.multiply("), path);
    sh = LuaLspSignatureAt(*sdoc, 9, 16);
    CHECK(sh.found && sh.params.size() == 2 && sh.params[0] == "self: datamunge.Dual");

    // Without a .luarc.json the same file knows nothing of datamunge, and says so quietly.
    const auto bare = LuaLspAnalyze(L(src));
    CHECK(!LuaLspHover(*bare, 6, 9).found);
    CHECK(LuaLspDiagnostics(*bare).empty());

    // $MEP_LUA_LIBRARY works without a .luarc.json.
    const std::string lib = (root / "build/lua/prefix/share/lua/library").string();
    setenv("MEP_LUA_LIBRARY", lib.c_str(), 1);
    const auto envdoc = LuaLspAnalyze(L(src), (root.parent_path() / "elsewhere.lua").string());
    CHECK(LuaLspHover(*envdoc, 6, 9).found);
    unsetenv("MEP_LUA_LIBRARY");

    // A changed stub is re-read.
    WriteFile(root / "build/lua/prefix/share/lua/library/datamunge.lua",
              std::string(kStub).replace(std::string(kStub).find("Adds three integers"), 19, "Sums three integers"));
    std::filesystem::last_write_time(root / "build/lua/prefix/share/lua/library/datamunge.lua",
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    const auto doc2 = LuaLspAnalyze(L(src), path);
    CHECK(Contains(LuaLspHover(*doc2, 6, 9).text, "Sums three integers"));

    // The nested form, with comments (.luarc.jsonc style), and an absolute path.
    fs::remove(root / ".luarc.json");
    WriteFile(root / ".luarc.jsonc", "{\n  // libraries\n  \"workspace\": {\"library\": [\"" + lib + "\",]}\n}\n");
    const auto doc3 = LuaLspAnalyze(L(src), path);
    CHECK(LuaLspHover(*doc3, 6, 9).found);
    fs::remove_all(root);
}

}  // namespace

int main() {
    TestLuac();
    TestFormat();
    TestSyntaxDiagnostic();
    TestUnused();
    TestShadowing();
    TestGlobals();
    TestStatementLints();
    TestHover();
    TestCompletion();
    TestSignature();
    TestNavigation();
    TestSymbolsFolds();
    TestRobustness();
    TestMetaParse();
    TestMetaStubs();
    std::printf("lua_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
