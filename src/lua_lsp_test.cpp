// Windowless test for lua_lsp.cpp -- the analysis half of mep's own
// Lua language server (`mep-lua-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert().
//
// The parser's agreement with luac is checked against the real compiler by
// tools/lua_parse_check.py over a corpus; the luac cases here pin a few
// messages so a regression shows up without luac installed.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lua_lsp.h"

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
    std::printf("lua_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
