#!/usr/bin/env python3
"""Check the Lua language UI mode (src/language_ui_lua.h) without a window.

Two halves, both run with a real `lua`:
  - the editor side: the chunk loaded against a stubbed `mep` table --
    statement-at-cursor detection, the top-level-local rewrite, what
    <leader>re / <leader>rr / gh type into the console, the console argv
    (including a REPL: override), and the panel poll;
  - the console side: the init script the mode writes, run in a real
    interpreter, then mep_eval / mep_run / mep_help / mep_view driven the way
    the editor drives them, checking the panel files they write.
No packages needed beyond the standard library. GUI coverage is manual.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_lua.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]
INIT = CHUNK.split('[==[\n', 1)[1].split(']==]', 1)[0]

STUBS = r"""
local files, sections, sent, notes = {}, {}, {}, {}
local buffer, tab = 1, 1
local source = {}
mep = {opt = {language_ui_poll_interval = 1}, language_ui_extensions = {}, language_ui_modes = {},
  language_ui_eval_handlers = {}}
local providers = {}
function mep.help_register_provider(name, fn) providers[name] = fn end
function mep.current_tab_id() return tab end
function mep.current_buffer() return buffer end
function mep.is_terminal_buffer(b) return b == 2 end
function mep.terminal_write(_, text) sent[#sent + 1] = text end
function mep.notify(msg) notes[#notes + 1] = msg end
function mep.language_ui_session_dir(s) return '/tmp/session_' .. s end
function mep.language_ui_write_file(path, text) files[path] = text end
function mep.language_ui_read_file(path) return files[path] end
function mep.language_ui_cmd(default)
  if mep.language_ui_repl_override then return {'sh', '-c', mep.language_ui_repl_override .. ' "$@"', 'mep-repl'} end
  local a = {} for _, x in ipairs(default) do a[#a + 1] = x end return a
end
function mep.language_ui_layout(opts) opts.console() buffer = 2 return {editor_pane = 10, top_pane = 20, bottom_pane = 30, console_buf = 2} end
function mep.terminal_here_argv(argv) mep.started = argv end
function mep.sidebar_create(name) return name end
function mep.sidebar_set_sections(id, value) sections[id] = value end
function mep.language_ui_open_tabs(_, ids) local b = {} for i, id in ipairs(ids) do b[id] = 100 + i end return b end
function mep.language_ui_render_textbox(id, value) sections[id] = value end
function mep.language_ui_teardown() end
function mep.pane_focus() return true end
function mep.reveal_buffer(b) mep.revealed = b end
function mep.jump_to_buffer(b) mep.jumped = b end
function mep.lsp_hover() mep.hovered = true end
function mep.on_frame() end
function mep.now() return 0 end
function mep.filename() return '/proj/mod.lua' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'lua' end
local help_sym = 'dm.read_csv'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r"""
assert(mep.language_ui_extensions.lua == 'lua')
assert(mep.language_ui_modes.lua and mep.language_ui_eval_handlers.lua and providers.lua)

-- The top-level-local rewrite: line for line, column 0 only, not in long strings.
local g = mep.lua_ui_globalize({
  'local dm = require("datamunge")',
  'local function f(x)',
  '  local y = x',
  '  return y',
  'end',
  'local a, b <const> = 1, 2',
  'local c',
  'local d, e -- two',
  'local s = [[',
  'local inside = 1',
  ']]',
  'local x == 1',
})
assert(g[1] == 'dm = require("datamunge")', g[1])
assert(g[2] == 'function f(x)', g[2])
assert(g[3] == '  local y = x')
assert(g[6] == 'a, b = 1, 2', g[6])
assert(g[7] == 'c = nil', g[7])
assert(g[8] == 'd, e = nil -- two', g[8])
assert(g[9] == 's = [[', g[9])
assert(g[10] == 'local inside = 1', g[10])
assert(g[12] == 'local x == 1')
assert(#g == 12)

-- Statement at the cursor.
source = {
  '#!/usr/bin/env lua',          -- 1
  'local dm = require("dm")',    -- 2
  '',                            -- 3
  '-- doubles',                  -- 4
  'local function double(x)',    -- 5
  '  if x then',                 -- 6
  '    return x * 2',            -- 7
  '  end',                       -- 8
  'end',                         -- 9
  '',                            -- 10
  'local t = {',                 -- 11
  '  a = 1,',                    -- 12
  '}',                           -- 13
  'local long = 1 +',            -- 14
  '2',                           -- 15
  'print(double(2))',            -- 16
}
local function block(r) return mep.lua_ui_block_at(source, r) end
local s, e = block(2) assert(s == 2 and e == 2, s .. ',' .. e)
s, e = block(7) assert(s == 5 and e == 9)
s, e = block(9) assert(s == 5 and e == 9)
s, e = block(10) assert(s == 5 and e == 9)
s, e = block(12) assert(s == 11 and e == 13)
s, e = block(15) assert(s == 14 and e == 15)
s, e = block(16) assert(s == 16 and e == 16)
assert(block(1) == nil)

-- Opening: argv, panels.
mep.lua_ui_open()
assert(mep.started[1] == 'lua' and mep.started[2] == '-i' and mep.started[3] == '/tmp/session_luaui/mep_init.lua')
assert(files['/tmp/session_luaui/mep_init.lua']:find("^local __mep_dir = \"/tmp/session_luaui\"\n"))
assert(sections.Globals and sections.Modules and sections.Inspect and sections.Help)

-- <leader>re: the function at the cursor, padded, locals made global.
cursor_row = 7
mep.language_ui_eval_handlers.lua()
assert(sent[#sent] == 'mep_eval("/tmp/session_luaui/eval.lua", "@/proj/mod.lua")\n', sent[#sent])
local text = files['/tmp/session_luaui/eval.lua']
assert(text == '\n\n\n\nfunction double(x)\n  if x then\n    return x * 2\n  end\nend\n', text)

-- <leader>rr: the whole buffer through mep_run under the file's name.
local mode_handle = mep.lua_ui_open()
mode_handle.run_source('/proj/mod.lua')
assert(sent[#sent] == 'mep_run("/tmp/session_luaui/run.lua", "@/proj/mod.lua")\n', sent[#sent])
assert(files['/tmp/session_luaui/run.lua']:find('\ndm = require%("dm"%)\n'))
mep.opt.lua_ui_globalize_locals = false
mode_handle.run_source('/proj/mod.lua')
assert(files['/tmp/session_luaui/run.lua']:find('\nlocal dm = require%("dm"%)\n'))
mep.opt.lua_ui_globalize_locals = true

-- gh: help into the Help tab, plus the LSP hover from a .lua buffer.
assert(providers.lua() == true)
assert(sent[#sent] == 'mep_help("dm.read_csv")\n')
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files.
files['/tmp/session_luaui/globals.txt'] = 'x                      number    42\n'
files['/tmp/session_luaui/help.txt'] = 'double(x)\n'
mep.lua_ui_poll({paths = {
  globals = '/tmp/session_luaui/globals.txt', modules = '/tmp/session_luaui/modules.txt',
  inspect = '/tmp/session_luaui/inspect.txt', help = '/tmp/session_luaui/help.txt'}, help_buf = 7})
assert(mep.revealed == 7)

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop --command lua'
mep.lua_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-i' and mep.started[6]:match('mep_init.lua$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.lua()
assert(#sent == before and notes[#notes]:find('No Lua console'))
print('editor side passed')
"""

CONSOLE = r"""
local dir = assert(os.getenv('MEP_TEST_DIR'))
local function slurp(name) local f = assert(io.open(dir .. '/' .. name)) local s = f:read('*a') f:close() return s end
local function spit(name, s) local f = assert(io.open(dir .. '/' .. name, 'w')) f:write(s) f:close() end
dofile(dir .. '/mep_init.lua')
assert(type(mep_eval) == 'function' and getmetatable(_PROMPT))
-- The prompt hook refreshes the panels.
x_from_prompt = 7
tostring(_PROMPT)
assert(slurp('globals.txt'):find('x_from_prompt%s+number%s+7'))
-- A block sent padded to its source lines: errors name the source line.
spit('eval.lua', '\n\nfunction double(x)\n  return x * 2\nend\n')
mep_eval(dir .. '/eval.lua', '@/proj/mod.lua')
assert(double(4) == 8)
assert(slurp('globals.txt'):find('double%s+function%s+/proj/mod.lua:3'))
spit('eval.lua', '\n\n\nerror("boom")\n')
mep_eval(dir .. '/eval.lua', '@/proj/mod.lua')
-- An expression's value is printed (checked by the caller).
spit('eval.lua', 'double(21)\n')
mep_eval(dir .. '/eval.lua', '=eval')
-- mep_run: a file with a shebang, returning a value.
spit('run.lua', '#!/usr/bin/env lua\nM = {name = "m"}\nfunction M.greet(who)\n  return "hi " .. who\nend\nreturn M\n')
mep_run(dir .. '/run.lua')
assert(M.greet('x') == 'hi x')
-- Help: a Lua function's signature and source, a module's fields, a C function.
spit('src.lua', '--- Says hi.\n-- @param who string\nfunction greet2(who, ...)\n  return who\nend\n')
dofile(dir .. '/src.lua')
mep_help('greet2')
local h = slurp('help.txt')
assert(h:find('greet2%(who, %.%.%.%)'), h)
assert(h:find('%-%-%- Says hi%.'), h)
mep_help('M')
h = slurp('help.txt')
assert(h:find('greet%(who%)') and h:find('name%s+string'), h)
mep_help('string.format')
h = slurp('help.txt')
assert(h:find('C function') and h:find('manual.html#pdf%-string%.format'), h)
mep_help('nope.nothing')
assert(slurp('help.txt'):find('not a global'))
-- Inspect.
mep_view('M')
assert(slurp('inspect.txt'):find('%[name%] string "m"'))
-- Modules: what this session required.
package.loaded['fake.mod'] = {}
tostring(_PROMPT)
assert(slurp('modules.txt'):find('fake%.mod'))
print('console side passed')
"""


class LuaUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-luaui-') as directory:
            init = Path(directory) / 'mep_init.lua'
            init.write_text('local __mep_dir = ' + '"' + directory + '"\n' + INIT)
            p = subprocess.run([LUA_EXE, '-'], input=CONSOLE, text=True, capture_output=True, timeout=10,
                               env={'MEP_TEST_DIR': directory, 'PATH': '/usr/bin:/bin'})
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('/proj/mod.lua:4: boom', p.stderr)
            self.assertNotIn('mep_init.lua', p.stderr)
            self.assertIn('\n42\n', p.stdout)

    def test_interactive_console(self):
        # The real REPL under a pty: `lua -i mep_init.lua` stays open, runs
        # what is typed, and the prompt hook refreshes the panels.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-luaui-') as directory:
            init = Path(directory) / 'mep_init.lua'
            init.write_text('local __mep_dir = ' + '"' + directory + '"\n' + INIT)
            p = subprocess.run([script, '-qfec', f'{LUA_EXE} -i {init}', '/dev/null'],
                               input='typed_value = 99\nprint(typed_value + 1)\nos.exit(0)\n',
                               text=True, capture_output=True, timeout=10)
            self.assertIn('100', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            self.assertRegex((Path(directory) / 'globals.txt').read_text(), r'typed_value\s+number\s+99')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    args, remaining = parser.parse_known_args()
    if not args.lua:
        print('lua not installed: skipping the Lua UI check')
        sys.exit(77)
    LUA_EXE = args.lua
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
