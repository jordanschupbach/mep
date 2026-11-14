#!/usr/bin/env python3
"""Check the Octave language UI mode (src/language_ui_octave.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the block at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (including a
    REPL: override), the CMD: line's --path directories, and the panel poll;
  - the console side: the init script the mode writes, run by a real
    `octave-cli`, then source / mep_run / mep_help / mep_view driven the way
    the editor drives them, checking the panel files they write -- help for
    a function Octave has no text for comes from the .m stubs on the path
    through mep-octave-lsp --doc when a built one is found (--lsp,
    build*/mep-octave-lsp, or PATH);
  - the interactive console under a pty (script(1)): the real prompt, whose
    input-event hook refreshes the panels.
Needs lua and octave-cli on PATH; skips (exit 77) when either is missing.
GUI coverage is manual.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_octave.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]
INIT = CHUNK.split('[==[\n', 1)[1].split(']==]', 1)[0]

STUBS = r"""
local files, sections, sent, notes, cmds, dirs = {}, {}, {}, {}, {}, {}
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
function mep.fs_mkdir(d) dirs[#dirs + 1] = d end
function mep.getcwd() return '/work' end
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
function mep.filename() return '/proj/model.m' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'octave' end
local help_sym = 'Dual_add'
function mep.help_symbol_at_cursor() return help_sym, 1, #help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.m == 'octave')
assert(mep.language_ui_modes.octave and mep.language_ui_eval_handlers.octave and providers.octave)
assert(mep.octave_ui_quote("it's") == "'it''s'", mep.octave_ui_quote("it's"))

-- Statement at the cursor.
source = {
  '% CMD: octave -qf --path "$(pwd)/build/dm" -p lib model.m', -- 1
  '1;',                              -- 2
  '% Doubles x.',                    -- 3
  'function y = double_it (x)',      -- 4
  '  if x > 0',                      -- 5
  '    y = 2 * x;',                  -- 6
  '  else',                          -- 7
  '    y = x(end);',                 -- 8
  '  end',                           -- 9
  'end',                             -- 10
  '',                                -- 11
  'v = [1, 2, ...',                  -- 12
  '     3];',                        -- 13
  'm = [1 2',                        -- 14
  '     3 4];',                      -- 15
  'for i = 1:3, disp (i), end',      -- 16
  '%{',                              -- 17
  'x = 1',                           -- 18
  '%}',                              -- 19
  'for k = 1:2',                     -- 20
  "  s = 'end';",                    -- 21
  '  t = "if";',                     -- 22
  'endfor',                          -- 23
  'q = v(end) + 1',                  -- 24
}
local function block(r) return mep.octave_ui_block_at(source, r) end
local s, e = block(2) assert(s == 2 and e == 2, s .. ',' .. e)
s, e = block(4) assert(s == 4 and e == 10, s .. ',' .. e)
s, e = block(8) assert(s == 4 and e == 10, s .. ',' .. e)
s, e = block(10) assert(s == 4 and e == 10, s .. ',' .. e)
assert(block(11) == nil)
s, e = block(13) assert(s == 12 and e == 13, s .. ',' .. e)
s, e = block(15) assert(s == 14 and e == 15, s .. ',' .. e)
s, e = block(16) assert(s == 16 and e == 16, s .. ',' .. e)
assert(block(18) == nil)
s, e = block(22) assert(s == 20 and e == 23, s .. ',' .. e)
s, e = block(24) assert(s == 24 and e == 24, s .. ',' .. e)

-- The CMD: line's --path / -p directories.
local cp = mep.octave_ui_cmd_paths(source, '/work')
assert(#cp == 2 and cp[1] == '/work/build/dm' and cp[2] == '/work/lib', table.concat(cp, ' '))
assert(#mep.octave_ui_cmd_paths({'% CMD: just TARGET=x run-octave'}, '/work') == 0)

-- Opening: argv, panels, the init script's settings.
mep.octave_ui_open()
assert(mep.started[1] == 'octave-cli' and mep.started[2] == '-q' and mep.started[3] == '--persist'
  and mep.started[4] == '--eval' and mep.started[5] == "source ('/tmp/session_octaveui/mep_init.m');",
  table.concat(mep.started, ' '))
local init = files['/tmp/session_octaveui/mep_init.m']
assert(init:find("^1;\n"))
assert(init:find("mep_ui_set %('dir', '/tmp/session_octaveui'%);\nmep_ui_set %('lsp', '/opt/mep/bin/mep%-octave%-lsp'%);"))
assert(init:find("mep_ui_set %('add_path', {'/proj', '/work/build/dm', '/work/lib'}%);\nmep_ui_start %(%);\n$"), init:sub(-200))
assert(sections.Workspace and sections.Path and sections.Inspect and sections.Help)
assert(dirs[1] == '/tmp/session_octaveui/eval' and dirs[2] == '/tmp/session_octaveui/run')

-- <leader>re: the function at the cursor, padded to its lines, named after the source.
cursor_row = 6
mep.language_ui_eval_handlers.octave()
assert(sent[#sent] == "source ('/tmp/session_octaveui/eval/model.m');\n", sent[#sent])
local text = files['/tmp/session_octaveui/eval/model.m']
assert(text:sub(1, 3) == '\n\n\n' and text:find('function y = double_it') and select(2, text:gsub('\n', '')) == 10, text)

-- <leader>rr: the whole buffer through mep_run, from the file's directory.
local mode_handle = mep.octave_ui_open()
mode_handle.run_source('/proj/model.m')
assert(sent[#sent] == "mep_run ('/proj/model.m', '/tmp/session_octaveui/run/model.m');\n", sent[#sent])
assert(files['/tmp/session_octaveui/run/model.m']:find("^%% CMD:"))

-- gh: help into the Help tab, plus the LSP hover from a .m buffer.
assert(providers.octave() == true)
assert(sent[#sent] == "mep_help ('Dual_add')\n", sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files.
files['/tmp/session_octaveui/variables.txt'] = 'x                      double       1x3        [1 2 3]\n'
files['/tmp/session_octaveui/path.txt'] = 'path    /work/lib\npackage statistics 1.6.0\n'
files['/tmp/session_octaveui/help.txt'] = 'Adds two duals.\n'
mep.revealed = nil
mep.octave_ui_poll()
assert(mep.revealed == 101)
assert(mep.octave_ui_info().help == 'Adds two duals.\n')
local p = sections.Path
assert(p[1].widgets[1].text == '/work/lib' and p[2].widgets[1].text == 'statistics 1.6.0')
local vars = sections.Workspace[1].widgets
assert(vars[1].text:find('^x ') and vars[1].on_click)
vars[1].on_click()
assert(sent[#sent] == "mep_view ('x')\n", sent[#sent])

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop --command octave-cli'
mep.octave_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-q' and mep.started[8]:match('mep_init.m'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.octave()
assert(#sent == before and notes[#notes]:find('No Octave console'))
print('editor side passed')
'''

CONSOLE = r'''
dir = getenv ('MEP_TEST_DIR');
slurp = @(name) fileread (fullfile (dir, name));
function spit (dir, name, s)
  fid = fopen (fullfile (dir, name), 'w'); fwrite (fid, s); fclose (fid);
end
function check (ok, what)
  if (! ok)
    error ('check failed: %s', what);
  end
end
check (exist ('mep_help') > 0, 'mep_help defined');
check (exist ('ans', 'var') == 0, 'init leaves no ans');
% The init script put the CMD:/source directories on the path.
check (any (strcmp (strsplit (path (), pathsep ()), fullfile (dir, 'lib'))), 'lib on the path');
% A block sent padded to its source lines: errors name the source line.
spit (dir, 'eval/model.m', "\n\nx_from_session = 7;\n");
source (fullfile (dir, 'eval/model.m'));
mep_ui_refresh ();
check (! isempty (regexp (slurp ('variables.txt'), 'x_from_session\s+double\s+1x1\s+7')), slurp ('variables.txt'));
spit (dir, 'eval/model.m', "\n\n\nerror ('boom');\n");
try
  source (fullfile (dir, 'eval/model.m'));
  check (false, 'raised');
catch err
  check (strcmp (err.message, 'boom') && err.stack(1).line == 4, sprintf ('%s line %d', err.message, err.stack(1).line));
end
% A function defined from a chunk stays defined.
spit (dir, 'eval/model.m', "\n% TWICE doubles x.\nfunction y = twice (x)\n  y = 2 * x;\nend\n");
source (fullfile (dir, 'eval/model.m'));
check (twice (4) == 8, 'twice defined');
% mep_run: the buffer copy, from the file's directory, in the base workspace.
mkdir (fullfile (dir, 'proj'));
spit (dir, 'proj/sibling.m', "function r = sibling ()\n  r = 5;\nend\n");
spit (dir, 'run/main.m', "1;\n% HELPER adds one.\nfunction r = helper (a)\n  r = a + 1;\nend\nfrom_run = helper (sibling ());\nwhere_run = pwd ();\n");
here = pwd ();
mep_run (fullfile (dir, 'proj/main.m'), fullfile (dir, 'run/main.m'));
check (from_run == 6, 'run');
check (strcmp (where_run, fullfile (dir, 'proj')) && strcmp (pwd (), here), where_run);
check (! isempty (strfind (slurp ('variables.txt'), 'from_run')), slurp ('variables.txt'));
% Help: Octave's own (a .m on the path, a built-in), a command-line function, an unknown name.
mep_help ('Dual_add');
h = slurp ('help.txt');
check (! isempty (strfind (h, 'Adds two duals.')) && ! isempty (strfind (h, 'which:')), h);
check (isempty (strfind (h, 'Additional help')), 'footer dropped');
mep_help ('disp');
check (! isempty (strfind (slurp ('help.txt'), 'Display the value')), slurp ('help.txt'));
mep_help ('helper');
check (! isempty (strfind (slurp ('help.txt'), 'HELPER adds one.')), slurp ('help.txt'));
% A function Octave has no help text for (as a .oct without a docstring):
% the .m stub in stubs/ carries it, read by mep-octave-lsp --doc.
eval ("function r = nodoc_fn (a)\n  r = a;\nend");
addpath (fullfile (dir, 'stubs'));
mep_help ('nodoc_fn');
if (strcmp (getenv ('MEP_EXPECT_DOCS'), '1'))
  check (! isempty (strfind (slurp ('help.txt'), 'Stub help for nodoc_fn.')), slurp ('help.txt'));
end
mep_help ('nope_nothing');
check (! isempty (strfind (slurp ('help.txt'), 'no documentation found')), slurp ('help.txt'));
% Inspect.
mep_view ('x_from_session');
check (! isempty (strfind (slurp ('inspect.txt'), '(double, 1x1)')), slurp ('inspect.txt'));
st.a = 1; st.b = 'two';
mep_view (st);
check (! isempty (strfind (slurp ('inspect.txt'), 'fields: a, b')) && strncmp (slurp ('inspect.txt'), 'st ', 3), slurp ('inspect.txt'));
% The Path panel lists what was added.
mep_ui_refresh ();
check (! isempty (strfind (slurp ('path.txt'), fullfile (dir, 'stubs'))), slurp ('path.txt'));
printf ('console side passed\n');
'''


def find_lsp(explicit):
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else ''
    # The most recently built one (an older build may predate --doc).
    found = [p for p in ROOT.glob('build*/**/mep-octave-lsp') if p.is_file() and os.access(p, os.X_OK)]
    if found:
        return str(max(found, key=lambda p: p.stat().st_mtime))
    return shutil.which('mep-octave-lsp') or ''


def oquote(s):
    return "'" + str(s).replace("'", "''") + "'"


def make_session(directory, lsp=''):
    d = Path(directory)
    for sub in ('eval', 'run', 'lib', 'stubs'):
        (d / sub).mkdir(exist_ok=True)
    (d / 'lib' / 'Dual_add.m').write_text(
        '## -*- texinfo -*-\n## @deftypefn {} {@var{r} =} Dual_add (@var{a}, @var{b})\n## Adds two duals.\n'
        '## @end deftypefn\nfunction r = Dual_add (a, b)\n  r = a + b;\nend\n')
    (d / 'stubs' / 'nodoc_fn.m').write_text('% Stub help for nodoc_fn.\nfunction r = nodoc_fn (a)\nend\n')
    init = d / 'mep_init.m'
    init.write_text(INIT + f"\nmep_ui_set ('dir', {oquote(d)});\nmep_ui_set ('lsp', {oquote(lsp)});\n"
                    f"mep_ui_set ('add_path', {{{oquote(d / 'lib')}}});\nmep_ui_start ();\n")
    return init


class OctaveUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-octaveui-') as directory:
            init = make_session(directory, LSP_EXE)
            console = Path(directory) / 'console.m'
            console.write_text(CONSOLE)
            env = dict(os.environ, MEP_TEST_DIR=directory, HOME=directory,
                       MEP_EXPECT_DOCS='1' if LSP_EXE else '0')
            env.pop('MEP_OCTAVE_PATH', None)
            p = subprocess.run([OCTAVE_EXE, '-q', '--norc', '--eval', f"source ({oquote(init)}); source ({oquote(console)});"],
                               text=True, capture_output=True, timeout=180, env=env, cwd=directory)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('mep language UI', p.stdout)

    def test_oct_function_stub_help(self):
        # A compiled .oct function without a docstring (Octave's `help` errors
        # "not documented"): its help text comes from a .m stub on the path,
        # as Rosetta generates for a C++ library's .oct functions.
        mkoctfile = shutil.which('mkoctfile')
        if not mkoctfile or not LSP_EXE:
            self.skipTest('mkoctfile or a built mep-octave-lsp missing')
        with tempfile.TemporaryDirectory(prefix='mep-octaveui-') as directory:
            init = make_session(directory, LSP_EXE)
            d = Path(directory)
            (d / 'oct').mkdir()
            (d / 'oct' / 'dm_ident.cc').write_text(
                '#include <octave/oct.h>\nDEFUN_DLD (dm_ident, args, , "")\n{\n  return octave_value (args(0));\n}\n')
            p = subprocess.run([mkoctfile, 'dm_ident.cc'], cwd=d / 'oct', capture_output=True, text=True, timeout=300)
            if p.returncode != 0:
                self.skipTest('mkoctfile failed: ' + p.stderr[-300:])
            (d / 'stubs' / 'dm_ident.m').write_text(
                '## -*- texinfo -*-\n## @deftypefn {} {@var{y} =} dm_ident (@var{x})\n'
                '## Returns @var{x} unchanged (C++ @code{datamunge::ident}).\n## @end deftypefn\n'
                'function y = dm_ident (x)\nend\n')
            script = (f"source ({oquote(init)}); addpath ({oquote(d / 'stubs')}); addpath ({oquote(d / 'oct')});"
                      " assert (dm_ident (3) == 3); mep_help ('dm_ident');")
            p = subprocess.run([OCTAVE_EXE, '-q', '--norc', '--eval', script], text=True, capture_output=True,
                               timeout=120, env=dict(os.environ, HOME=directory), cwd=directory)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            help_text = (d / 'help.txt').read_text()
            self.assertIn("Returns X unchanged (C++ 'datamunge::ident').", help_text)
            self.assertIn('dm_ident.oct', help_text)

    def test_interactive_console(self):
        # The real prompt under a pty: the input-event hook refreshes the panels.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-octaveui-') as directory:
            init = make_session(directory, LSP_EXE)
            (Path(directory) / 'eval' / 'x.m').write_text('\n\nsent_value = 6 * 7\n')
            cmd = f"{OCTAVE_EXE} -q --norc --persist --eval \"source ('{init}');\""
            proc = subprocess.Popen([script, '-qfec', cmd, '/dev/null'], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    env=dict(os.environ, HOME=directory, TERM='dumb', INPUTRC='/dev/null'))
            time.sleep(3)
            for line in ('typed_value = 99;', 'disp (typed_value + 1)', f"source ('{directory}/eval/x.m');"):
                proc.stdin.write(line + '\n')
                proc.stdin.flush()
                time.sleep(0.5)
            variables = Path(directory) / 'variables.txt'
            deadline = time.time() + 20
            while time.time() < deadline and 'sent_value' not in variables.read_text():
                time.sleep(0.3)
            text = variables.read_text()
            proc.stdin.write('exit\n')
            proc.stdin.flush()
            out, _ = proc.communicate(timeout=30)
            self.assertIn('100', out)
            self.assertIn('mep language UI', out)
            self.assertIn('42', out)
            self.assertRegex(text, r'typed_value\s+double\s+1x1\s+99')
            self.assertRegex(text, r'sent_value\s+double\s+1x1\s+42')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--octave', default=shutil.which('octave-cli'))
    parser.add_argument('--lsp', default='')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.octave:
        print('lua or octave-cli not installed: skipping the Octave UI check')
        sys.exit(77)
    LUA_EXE, OCTAVE_EXE = args.lua, args.octave
    LSP_EXE = find_lsp(args.lsp)
    print('mep-octave-lsp:', LSP_EXE or '(not found: help checks without the .m stub fallback)')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
