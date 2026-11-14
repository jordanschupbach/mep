#!/usr/bin/env python3
"""Check the Tcl language UI mode (src/language_ui_tcl.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the command at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (including a
    REPL: override), and the panel poll;
  - the console side: the init script the mode writes, loaded into a real
    `tclsh` (without its prompt loop), then ::mep::eval_file / run / help /
    view driven the way the editor drives them, checking the panel files
    they write -- help reads a package's proc doc comments through
    mep-tcl-lsp --doc when a built one is found (--lsp, build*/mep-tcl-lsp,
    or PATH);
  - the interactive console under a pty (script(1)): `tclsh mep_init.tcl`
    runs its read-eval-print loop, which refreshes the panels before each
    prompt.
Needs lua and tclsh (8.6 or 9) on PATH; skips (exit 77) when either is
missing. GUI coverage is manual.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_tcl.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]
INIT = CHUNK.split('[==[\n', 1)[1].split(']==]', 1)[0]

STUBS = r"""
local files, sections, sent, notes, cmds = {}, {}, {}, {}, {}
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
function mep.cmd(c) cmds[#cmds + 1] = c end
function mep.bundled_tool(name) return '/opt/mep/bin/' .. name end
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
function mep.filename() return '/proj/model.tcl' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'tcl' end
local help_sym = 'datamunge::mean'
function mep.help_symbol_at_cursor() return help_sym, 1, #help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.tcl == 'tcl' and mep.language_ui_extensions.tm == 'tcl')
assert(mep.language_ui_modes.tcl and mep.language_ui_eval_handlers.tcl and providers.tcl)
assert(mep.tcl_ui_quote('/a b/c') == '{/a b/c}', mep.tcl_ui_quote('/a b/c'))
assert(mep.tcl_ui_quote('a{b') == 'a\\{b', mep.tcl_ui_quote('a{b'))

-- Command at the cursor.
source = {
  'package require datamunge',      -- 1
  '',                               -- 2
  '# Doubles x.',                   -- 3
  'proc double {x} {',              -- 4
  '    if {$x > 0} {',              -- 5
  '        return [expr {2 * $x}]', -- 6
  '    }',                          -- 7
  '    return 0',                   -- 8
  '}',                              -- 9
  'set long [list a \\',            -- 10
  '    b c]',                       -- 11
  'set msg "two',                   -- 12
  'lines { here"',                  -- 13
  '# a comment {',                  -- 14
  'namespace eval ns {',            -- 15
  '    variable v 1',               -- 16
  '}',                              -- 17
  'puts [double 4]; puts done',     -- 18
  'set nested [lindex [list',       -- 19
  '    1 2] 0]',                    -- 20
}
local function block(r) return mep.tcl_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, s .. ',' .. e)
assert(block(2) == nil and block(3) == nil and block(14) == nil)
s, e = block(4) assert(s == 4 and e == 9, s .. ',' .. e)
s, e = block(7) assert(s == 4 and e == 9, s .. ',' .. e)
s, e = block(11) assert(s == 10 and e == 11, s .. ',' .. e)
s, e = block(13) assert(s == 12 and e == 13, s .. ',' .. e)
s, e = block(16) assert(s == 15 and e == 17, s .. ',' .. e)
s, e = block(18) assert(s == 18 and e == 18, s .. ',' .. e)
s, e = block(20) assert(s == 19 and e == 20, s .. ',' .. e)

-- Opening: argv, panels, the init script's start call.
mep.tcl_ui_open()
assert(mep.started[1] == 'tclsh' and mep.started[2] == '/tmp/session_tclui/mep_init.tcl', table.concat(mep.started, ' '))
local init = files['/tmp/session_tclui/mep_init.tcl']
assert(init:find('^namespace eval ::mep'))
assert(init:find('\n::mep::start {/tmp/session_tclui} {/opt/mep/bin/mep%-tcl%-lsp} {{/proj}}\n$'), init:sub(-120))
assert(sections.Variables and sections.Procs and sections.Packages and sections.Inspect and sections.Help)

-- <leader>re: the proc at the cursor, padded to its lines.
cursor_row = 6
mep.language_ui_eval_handlers.tcl()
assert(sent[#sent] == '::mep::eval_file {/tmp/session_tclui/eval.tcl} {/proj/model.tcl}\n', sent[#sent])
local text = files['/tmp/session_tclui/eval.tcl']
assert(text:sub(1, 3) == '\n\n\n' and text:find('proc double') and select(2, text:gsub('\n', '')) == 9, text)

-- <leader>rr: the whole buffer through ::mep::run under the file's name.
local mode_handle = mep.tcl_ui_open()
mode_handle.run_source('/proj/model.tcl')
assert(sent[#sent] == '::mep::run {/tmp/session_tclui/run.tcl} {/proj/model.tcl}\n', sent[#sent])
assert(files['/tmp/session_tclui/run.tcl']:find('^package require datamunge\n'))

-- gh: help into the Help tab, plus the LSP hover from a .tcl buffer.
assert(providers.tcl() == true)
assert(sent[#sent] == '::mep::help {datamunge::mean}\n', sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files; rows inspect / show help.
files['/tmp/session_tclui/variables.txt'] = 'x                      scalar     42\n'
files['/tmp/session_tclui/procs.txt'] = 'double                 proc       {x}\n'
files['/tmp/session_tclui/packages.txt'] = 'package datamunge 1.0\npath /usr/lib/tcl8.6\n'
files['/tmp/session_tclui/help.txt'] = 'proc ::double {x}\n'
mep.revealed = nil
mep.tcl_ui_poll()
assert(mep.revealed == 101)
assert(mep.tcl_ui_info().help == 'proc ::double {x}\n')
local pk = sections.Packages
assert(pk[1].widgets[1].text == 'datamunge 1.0' and pk[2].widgets[1].text == '/usr/lib/tcl8.6')
sections.Variables[1].widgets[1].on_click()
assert(sent[#sent] == '::mep::view {x}\n', sent[#sent])
sections.Procs[1].widgets[1].on_click()
assert(sent[#sent] == '::mep::help {double}\n', sent[#sent])

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop --command tclsh'
mep.tcl_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5]:match('mep_init.tcl$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.tcl()
assert(#sent == before and notes[#notes]:find('No Tcl console'))
print('editor side passed')
'''

CONSOLE = r'''
set dir $env(MEP_TEST_DIR)
proc slurp {name} {set f [open [file join $::dir $name]]; set t [read $f]; close $f; return $t}
proc spit {name text} {set f [open [file join $::dir $name] w]; puts -nonewline $f $text; close $f}
proc check {cond what} {if {![uplevel 1 [list expr $cond]]} {error "check failed: $what"}}
check [llength [info commands ::mep::help]] {::mep::help defined}
check {[file join $dir proj] in $auto_path} {source directory on auto_path}
# A block sent padded to its source lines.
spit eval.tcl "\n\nset x_from_session 7\n"
check {[::mep::eval_file [file join $dir eval.tcl] /proj/model.tcl] == 7} {value returned}
check [regexp {x_from_session\s+scalar\s+7} [slurp variables.txt]] [slurp variables.txt]
# Errors count the source's lines; [info script] names the source during the evaluation.
spit eval.tcl "\n\n\nerror boom\n"
set rc [catch {::mep::eval_file [file join $dir eval.tcl] /proj/model.tcl} msg opts]
check {$rc == 1 && $msg eq "boom"} $msg
check {[dict get $opts -errorline] == 4 || [string match {*line 4*} $::errorInfo]} $::errorInfo
spit eval.tcl "\n# Twice x.\nproc twice {x} {\n    expr {2 * \$x}\n}\nset where_from \[info script\]\n"
::mep::eval_file [file join $dir eval.tcl] /proj/model.tcl
check {[twice 4] == 8 && $where_from eq "/proj/model.tcl" && [info script] ne "/proj/model.tcl"} $where_from
check [regexp {twice\s+proc\s+\{x\}} [slurp procs.txt]] [slurp procs.txt]
# run: a whole file; package require through auto_path records the package's files.
spit run.tcl "lappend auto_path [file join $dir pkgs]\npackage require datamunge\nset m \[datamunge::mean {1 2 3}\]\narray set arr {a 1 b 2}\n"
::mep::run [file join $dir run.tcl] [file join $dir proj main.tcl]
check {$m == 2.0} $m
check [string match {*package datamunge 1.0*} [slurp packages.txt]] [slurp packages.txt]
check [regexp {datamunge::mean\s+proc\s+\{values w\}} [slurp procs.txt]] [slurp procs.txt]
check [regexp {arr\s+array\s+2 elements} [slurp variables.txt]] [slurp variables.txt]
check {![string match *auto_index* [slurp variables.txt]] && ![string match *tclPkg* [slurp procs.txt]]} {library noise left out}
# Help: a package proc (doc comment through mep-tcl-lsp), a session proc, a built-in, a namespace, unknown.
check {[::mep::help datamunge::mean] eq "# datamunge::mean: in the Help tab"} {help result}
set h [slurp help.txt]
check [string match "*proc ::datamunge::mean \{values \{w \{\}\}\}*" $h] $h
check [string match {*## Runtime*} $h] $h
if {$env(MEP_EXPECT_DOCS)} {
    check [string match {*Arithmetic mean of VALUES.*} $h] $h
    check [string match {*datamunge.tcl:*} $h] $h
    ::mep::help twice
    check [string match {*Twice x.*} [slurp help.txt]] [slurp help.txt]
    ::mep::help lsearch
    check [string match {*searches the elements*} [slurp help.txt]] [slurp help.txt]
    ::mep::help {string map}
    check [string match {*charMap*} [slurp help.txt]] [slurp help.txt]
}
::mep::help datamunge
check [string match {*proc mean*} [slurp help.txt]] [slurp help.txt]
::mep::help nope_nothing
check [string match {*no documentation found*} [slurp help.txt]] [slurp help.txt]
# Inspect.
::mep::view x_from_session
check [string match {*(scalar, 1 chars)*} [slurp inspect.txt]] [slurp inspect.txt]
set d {k1 v1 k2 v2}
::mep::view d
check [string match {*as a dict (2 keys)*k2 = v2*} [slurp inspect.txt]] [slurp inspect.txt]
::mep::view arr
check [string match {*(array, 2 elements)*b = 2*} [slurp inspect.txt]] [slurp inspect.txt]
puts "console side passed"
'''


def find_lsp(explicit):
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else ''
    # The most recently built one (an older build may predate --doc).
    found = [p for p in ROOT.glob('build*/**/mep-tcl-lsp') if p.is_file() and os.access(p, os.X_OK)]
    if found:
        return str(max(found, key=lambda p: p.stat().st_mtime))
    return shutil.which('mep-tcl-lsp') or ''


def make_session(directory, lsp='', repl=True):
    d = Path(directory)
    (d / 'proj').mkdir(exist_ok=True)
    pkg = d / 'pkgs' / 'datamunge'
    pkg.mkdir(parents=True, exist_ok=True)
    (pkg / 'datamunge.tcl').write_text(
        'package provide datamunge 1.0\nnamespace eval datamunge {\n    # Arithmetic mean of VALUES.\n    #\n'
        '    # Returns a double.\n    proc mean {values {w {}}} {\n'
        '        return [expr {[tcl::mathop::+ {*}$values] / double([llength $values])}]\n    }\n}\n')
    (pkg / 'pkgIndex.tcl').write_text('package ifneeded datamunge 1.0 [list source [file join $dir datamunge.tcl]]\n')
    init = d / 'mep_init.tcl'
    init.write_text(INIT + f"\n::mep::start {{{d}}} {{{lsp}}} {{{{{d / 'proj'}}}}}{'' if repl else ' 0'}\n")
    return init


class TclUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-tclui-') as directory:
            init = make_session(directory, LSP_EXE, repl=False)
            console = Path(directory) / 'console.tcl'
            console.write_text(f'source {{{init}}}\n' + CONSOLE)
            env = dict(os.environ, MEP_TEST_DIR=directory, HOME=directory, MEP_EXPECT_DOCS='1' if LSP_EXE else '0')
            env.pop('TCLLIBPATH', None)
            env.pop('MEP_TCL_PATH', None)
            p = subprocess.run([TCLSH_EXE, str(console)], text=True, capture_output=True, timeout=120, env=env)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('mep language UI', p.stdout)

    def test_interactive_console(self):
        # The real tclsh under a pty, in the init script's prompt loop.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-tclui-') as directory:
            init = make_session(directory, LSP_EXE)
            (Path(directory) / 'eval.tcl').write_text('\n\nset sent_value [expr {6 * 7}]\n')
            typed = (f"set typed_value 99\nputs [expr {{$typed_value + 1}}]\nproc f {{a}} {{\n  return $a\n}}\n"
                     f"f multi\n::mep::eval_file {{{directory}/eval.tcl}} /proj/x.tcl\nerror oops\nexit\n")
            p = subprocess.run([script, '-qfec', f'{TCLSH_EXE} {init}', '/dev/null'], input=typed, text=True,
                               capture_output=True, timeout=60,
                               env={'HOME': directory, 'PATH': os.environ.get('PATH', ''), 'TERM': 'dumb'})
            self.assertIn('100', p.stdout)
            self.assertIn('multi', p.stdout)
            self.assertIn('42', p.stdout)
            self.assertIn('oops', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            variables = (Path(directory) / 'variables.txt').read_text()
            self.assertRegex(variables, r'typed_value\s+scalar\s+99')
            self.assertRegex(variables, r'sent_value\s+scalar\s+42')
            self.assertRegex((Path(directory) / 'procs.txt').read_text(), r'f\s+proc\s+\{a\}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--tclsh', default=shutil.which('tclsh'))
    parser.add_argument('--lsp', default='')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.tclsh:
        print('lua or tclsh not installed: skipping the Tcl UI check')
        sys.exit(77)
    LUA_EXE, TCLSH_EXE = args.lua, args.tclsh
    LSP_EXE = find_lsp(args.lsp)
    print('mep-tcl-lsp:', LSP_EXE or '(not found: help checks without doc comments)')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
