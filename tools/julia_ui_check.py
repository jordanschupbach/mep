#!/usr/bin/env python3
"""Check the Julia language UI mode (src/language_ui_julia.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the block at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (including a
    REPL: override), and the panel poll;
  - the console side: the init script the mode writes, loaded into a real
    `julia`, then MepUI.eval_file / run / help / view driven the way the
    editor drives them, checking the panel files they write;
  - the interactive REPL under a pty (script(1)): `julia -i -L mep_init.jl`
    stays open, runs what is typed, and its prompt hook refreshes the panels.
Needs lua and julia on PATH (no packages). GUI coverage is manual.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_julia.h').read_text()
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
function mep.filename() return '/proj/model.jl' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'julia' end
local help_sym = 'Datamunge.fit'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.jl == 'julia')
assert(mep.language_ui_modes.julia and mep.language_ui_eval_handlers.julia and providers.julia)
assert(mep.julia_ui_quote('a"b$c\\') == '"a\\"b\\$c\\\\"', mep.julia_ui_quote('a"b$c\\'))

-- Statement at the cursor.
source = {
  'using Datamunge',              -- 1
  '',                             -- 2
  '"""',                          -- 3
  '    double(x)',                -- 4
  '',                             -- 5
  'end of a docstring line',      -- 6
  '"""',                          -- 7
  'function double(x)',           -- 8
  '    if x > 0',                 -- 9
  '        2x',                   -- 10
  '    else',                     -- 11
  '        0',                    -- 12
  '    end',                      -- 13
  'end',                          -- 14
  '',                             -- 15
  'v = [1,',                      -- 16
  '2]',                           -- 17
  'total = 1 +',                  -- 18
  '2',                            -- 19
  '# comment',                    -- 20
  'println(double(2))',           -- 21
  'struct P',                     -- 22
  '    x::Int',                   -- 23
  'end',                          -- 24
}
local function block(r) return mep.julia_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, s .. ',' .. e)
s, e = block(4) assert(s == 3 and e == 14, s .. ',' .. e)
s, e = block(6) assert(s == 3 and e == 14, s .. ',' .. e)
s, e = block(10) assert(s == 3 and e == 14, s .. ',' .. e)
s, e = block(14) assert(s == 3 and e == 14, s .. ',' .. e)
s, e = block(17) assert(s == 16 and e == 17, s .. ',' .. e)
s, e = block(19) assert(s == 18 and e == 19, s .. ',' .. e)
s, e = block(21) assert(s == 21 and e == 21, s .. ',' .. e)
s, e = block(23) assert(s == 22 and e == 24, s .. ',' .. e)

-- Opening: argv, panels.
mep.julia_ui_open()
assert(mep.started[1] == 'julia' and mep.started[2] == '-i' and mep.started[3] == '-L'
  and mep.started[4] == '/tmp/session_juliaui/mep_init.jl', table.concat(mep.started, ' '))
assert(files['/tmp/session_juliaui/mep_init.jl']:find('^const __mep_dir = "/tmp/session_juliaui"\nmodule MepUI'))
assert(sections.Variables and sections.Modules and sections.Inspect and sections.Help)

-- <leader>re: the function at the cursor (with its docstring), padded to its lines.
cursor_row = 10
mep.language_ui_eval_handlers.julia()
assert(sent[#sent] == 'MepUI.eval_file("/tmp/session_juliaui/eval.jl", "/proj/model.jl")\n', sent[#sent])
local text = files['/tmp/session_juliaui/eval.jl']
assert(text:sub(1, 3) == '\n\n"' and text:find('function double%(x%)') and select(2, text:gsub('\n', '')) == 14, text)

-- <leader>rr: the whole buffer through MepUI.run under the file's name.
local mode_handle = mep.julia_ui_open()
mode_handle.run_source('/proj/model.jl')
assert(sent[#sent] == 'MepUI.run("/tmp/session_juliaui/run.jl", "/proj/model.jl")\n', sent[#sent])
assert(files['/tmp/session_juliaui/run.jl']:find('^using Datamunge\n'))

-- gh: help into the Help tab, plus the LSP hover from a .jl buffer.
assert(providers.julia() == true)
assert(sent[#sent] == 'MepUI.help("Datamunge.fit")\n', sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files.
files['/tmp/session_juliaui/variables.txt'] = 'x                      Int64      42\n'
files['/tmp/session_juliaui/help.txt'] = 'double(x)\n'
mep.julia_ui_poll({paths = {
  variables = '/tmp/session_juliaui/variables.txt', modules = '/tmp/session_juliaui/modules.txt',
  inspect = '/tmp/session_juliaui/inspect.txt', help = '/tmp/session_juliaui/help.txt'}, help_buf = 7})
assert(mep.revealed == 7)

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop .#julia --command julia'
mep.julia_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-i' and mep.started[6] == '-L' and mep.started[7]:match('mep_init.jl$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.julia()
assert(#sent == before and notes[#notes]:find('No Julia console'))
print('editor side passed')
'''

CONSOLE = r'''
dir = ENV["MEP_TEST_DIR"]
slurp(name) = read(joinpath(dir, name), String)
spit(name, s) = write(joinpath(dir, name), s)
@assert isdefined(Main, :MepUI)
# Panels: what the session defines.
x_from_session = 7
MepUI.refresh()
@assert occursin(r"x_from_session\s+Int64\s+7", slurp("variables.txt")) slurp("variables.txt")
# A block sent padded to its source lines: errors name the source file and line.
spit("eval.jl", "\n\n\"\"\"\n    double(x)\n\nTwice `x`.\n\"\"\"\ndouble(x) = 2x\n")
MepUI.eval_file(joinpath(dir, "eval.jl"), "/proj/model.jl")
@assert double(4) == 8
@assert occursin(r"double\s+function\s+1 methods", slurp("variables.txt"))
spit("eval.jl", "\n\n\nerror(\"boom\")\n")
MepUI.eval_file(joinpath(dir, "eval.jl"), "/proj/model.jl")
# An expression's value is displayed (checked by the caller); a trailing ; hides it.
spit("eval.jl", "double(21)\n")
MepUI.eval_file(joinpath(dir, "eval.jl"), "/proj/model.jl")
spit("eval.jl", "double(1000);\n")
MepUI.eval_file(joinpath(dir, "eval.jl"), "/proj/model.jl")
# run: a whole file, @__FILE__ naming the source.
spit("run.jl", "module M\ngreet(who) = \"hi \" * who\nend\nconst where_from = @__FILE__\n")
MepUI.run(joinpath(dir, "run.jl"), "/proj/run_me.jl")
@assert M.greet("x") == "hi x" && where_from == "/proj/run_me.jl"
# Help: the docstring and methods, a qualified name, a module, an unknown name.
MepUI.help("double")
h = slurp("help.txt")
@assert occursin("Twice `x`.", h) && occursin("1 methods", h) h
MepUI.help("Base.sum")
@assert occursin("sum(itr", slurp("help.txt"))
MepUI.help("M")
@assert occursin("# Exports", slurp("help.txt"))
MepUI.help("nope.nothing")
@assert occursin("not a name in this session", slurp("help.txt"))
# Inspect.
MepUI.view("x_from_session")
@assert occursin("Int64", slurp("inspect.txt"))
MepUI.view([1, 2, 3])
@assert occursin("3-element Vector{Int64}", slurp("inspect.txt"))
# Modules: what this session loaded.
using Mmap
MepUI.refresh()
@assert occursin("Mmap", slurp("modules.txt")) slurp("modules.txt")
println("console side passed")
'''


def init_file(directory):
    init = Path(directory) / 'mep_init.jl'
    init.write_text('const __mep_dir = "' + directory + '"\n' + INIT)
    return init


class JuliaUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-juliaui-') as directory:
            init = init_file(directory)
            # A file, not -e: -e runs as one top-level block, so it can't see what it defines itself.
            console = Path(directory) / 'console.jl'
            console.write_text(CONSOLE)
            p = subprocess.run([JULIA_EXE, '--startup-file=no', '--history-file=no', '-L', str(init), str(console)],
                               text=True, capture_output=True, timeout=300,
                               env={'MEP_TEST_DIR': directory, 'PATH': '/usr/bin:/bin', 'HOME': directory})
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('boom', p.stderr)
            self.assertIn('/proj/model.jl:4', p.stderr)
            self.assertIn('\n42\n', p.stdout)
            self.assertNotIn('2000', p.stdout)

    def test_interactive_console(self):
        # The real REPL under a pty: the prompt function refreshes the panels.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-juliaui-') as directory:
            init = init_file(directory)
            p = subprocess.run([script, '-qfec', f'{JULIA_EXE} --startup-file=no --history-file=no -i -L {init}', '/dev/null'],
                               input='typed_value = 99\nprintln(typed_value + 1)\nexit()\n',
                               text=True, capture_output=True, timeout=300,
                               # As mep's terminal does: TERM unset (dumb) gives the basic REPL, refreshed by a timer instead.
                               env={'HOME': directory, 'PATH': '/usr/bin:/bin', 'TERM': 'xterm-256color'})
            self.assertIn('100', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            self.assertRegex((Path(directory) / 'variables.txt').read_text(), r'typed_value\s+Int64\s+99')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--julia', default=shutil.which('julia'))
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.julia:
        print('lua or julia not installed: skipping the Julia UI check')
        sys.exit(77)
    LUA_EXE, JULIA_EXE = args.lua, args.julia
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
