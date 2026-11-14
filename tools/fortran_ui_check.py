#!/usr/bin/env python3
"""Check the Fortran language UI mode (src/language_ui_fortran.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the console argv (Setup's compiler and flags, a
    REPL: override), what <leader>re / <leader>rr / gh type into the console,
    and the panel poll;
  - the console side: the console program the mode writes, run under a real
    python3 with gfortran and driven the way the editor drives it -- typed
    inputs, :evalat of a program's statements, the Variables file, :help from
    a used module's FORD `!>` docs and for an intrinsic, compile and runtime
    errors, and :run of a program, of a CMD: header and of a module file;
  - the interactive console under a pty (script(1)): a construct typed over
    several lines continues until its `end`.
Needs lua, python3 and gfortran on PATH (`nix shell nixpkgs#gfortran` when
the shell has none); --lsp names a built mep-fortran-lsp for the intrinsic
help (skipped without one). GUI coverage is manual.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_fortran.h').read_text()
# The chunk is a concatenation of raw string pieces (MSVC caps each literal's length).
CHUNK = ''.join(m.group(2) for m in re.finditer(r'R"(MEPLUA|MEPPY)\((.*?)\)\1"', HEADER, re.S))
CONSOLE = CHUNK.split('local console_source = [===[\n', 1)[1].split(']===]', 1)[0]

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
function mep.terminal_info() return {exited = false} end
function mep.terminal_write(_, text) sent[#sent + 1] = text end
function mep.notify(msg) notes[#notes + 1] = msg end
function mep.cmd(c) cmds[#cmds + 1] = c end
function mep.command() end
function mep.language_ui_shq(s) return "'" .. s .. "'" end
function mep.language_ui_session_dir(s) return '/tmp/session_' .. s end
function mep.language_ui_write_file(path, text) files[path] = text end
function mep.language_ui_read_file(path) return files[path] end
function mep.language_ui_cmd(default)
  if mep.language_ui_repl_override then return {'sh', '-c', mep.language_ui_repl_override .. ' "$@"', 'mep-repl'} end
  local a = {} for _, x in ipairs(default) do a[#a + 1] = x end return a
end
function mep.run_button_config_for(ext) return {compiler = 'gfortran-15', flags = '-O1 -I mods'} end
function mep.bundled_tool(name) return '/opt/mep/bin/' .. name end
function mep.language_ui_layout(opts) opts.console() return {editor_pane = 10, top_pane = 20, bottom_pane = 30, console_buf = 2} end
function mep.terminal_here_argv(argv) mep.started = argv end
function mep.sidebar_create(name) return name end
function mep.sidebar_set_sections(id, value) sections[id] = value end
function mep.language_ui_open_tabs(_, ids) local b = {} for i, id in ipairs(ids) do b[id] = 100 + i end return b end
function mep.language_ui_render_textbox(id, value) sections[id] = value end
function mep.language_ui_teardown() end
function mep.pane_focus() return true end
function mep.reveal_buffer(b) mep.revealed = b end
function mep.jump_to_buffer(b) mep.jumped = b end
function mep.on_frame() end
function mep.now() return 0 end
function mep.filename() return '/proj/model.f90' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
local selection, range = '', nil
function mep.visual_selection() return selection end
function mep.visual_range() return range end
function mep.language_for_buffer() return 'fortran' end
local help_sym = 'distance'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r'''
for _, e in ipairs({'f90', 'f95', 'f03', 'f08', 'f', 'for'}) do assert(mep.language_ui_extensions[e] == 'fortran', e) end
assert(mep.language_ui_modes.fortran and mep.language_ui_eval_handlers.fortran and providers.fortran)

-- Opening: the console is the written script under python3, with Setup's compiler and flags.
source = {'program model', '  use geometry', '  print *, 1', 'end program model'}
local handle = mep.fortran_ui_open()
local a = mep.started
assert(a[1] == 'python3' and a[2] == '/tmp/session_fortranui/mep_fortran_console.py', table.concat(a, ' '))
local argv = table.concat(a, ' ')
assert(argv:find('--dir /tmp/session_fortranui', 1, true), argv)
assert(argv:find('--compiler gfortran-15', 1, true) and argv:find('--flags=-O1 -I mods', 1, true), argv)
assert(argv:find('--src-dir /proj', 1, true) and argv:find('--lsp /opt/mep/bin/mep-fortran-lsp', 1, true), argv)
assert(files['/tmp/session_fortranui/mep_fortran_console.py']:find('^#!/usr/bin/env python3\n'))
assert(sections.Variables and sections.Session and sections.History and sections.Help)
assert(handle.prefer_session_run == true and handle.run_source)

-- <leader>re: a snapshot of the buffer and the cursor's row; the console picks the piece.
buffer = 1
cursor_row = 3
mep.language_ui_eval_handlers.fortran()
assert(sent[#sent] == ':evalat 3 /tmp/session_fortranui/source_1.f90 /proj/model.f90\n', sent[#sent])
assert(files['/tmp/session_fortranui/source_1.f90'] == table.concat(source, '\n') .. '\n')

-- A Visual line selection: padded to its source lines.
selection, range = '  print *, 1', {kind = 'line', start_row = 3, end_row = 3}
mep.language_ui_eval_handlers.fortran()
assert(sent[#sent] == ':eval /tmp/session_fortranui/eval.f90 /proj/model.f90\n', sent[#sent])
assert(files['/tmp/session_fortranui/eval.f90'] == '\n\n  print *, 1\n', files['/tmp/session_fortranui/eval.f90'])
assert(cmds[#cmds] == 'normal!')
selection, range = '', nil

-- <leader>rr: the snapshot to :run; a CMD: header saves the file first (its command reads it).
local before = #cmds
handle.run_source('/proj/model.f90')
assert(sent[#sent] == ':run /tmp/session_fortranui/source_2.f90 /proj/model.f90\n', sent[#sent])
assert(#cmds == before)
source[#source + 1] = '! CMD: make run'
handle.run_source('/proj/model.f90')
assert(cmds[#cmds] == 'write' and sent[#sent]:find('^:run /tmp/session_fortranui/source_3.f90'))

-- gh: :help with the snapshot (for the file's own use statements), Help revealed.
assert(providers.fortran() == true)
assert(sent[#sent] == ':help distance /tmp/session_fortranui/source_4.f90\n', sent[#sent])
assert(mep.revealed == 101)

-- The poll: Variables rows decoded, Help revealed when it changes.
local st_paths = {variables = '/tmp/session_fortranui/variables.tsv', session = '/tmp/session_fortranui/session.txt',
  history = '/tmp/session_fortranui/history.txt', help = '/tmp/session_fortranui/help.txt'}
files[st_paths.variables] = 'n\tinteger\t3\nv(3)\treal\t[3] 1.0 2.0 3.0\n'
files[st_paths.history] = 'integer :: n = 3\\nprint *, n\n'
files[st_paths.help] = 'distance(a, b)\n'
mep.revealed = nil
mep.fortran_ui_poll()
local rows = sections.Variables[1].widgets
assert(rows[1].text == 'n : integer = 3' and rows[2].text == 'v(3) : real = [3] 1.0 2.0 3.0', rows[1].text)
assert(mep.revealed == 101)
local hist = sections.History[1].widgets
assert(hist[1].text == 'integer :: n = 3\nprint *, n', hist[1].text)
hist[1].on_click()
assert(sent[#sent] == ':begin\ninteger :: n = 3\nprint *, n\n:end\n', sent[#sent])
assert(sections.Help == 'distance(a, b)\n')
-- Clicking a Variables row asks for its help.
rows[1].on_click()
assert(sent[#sent] == ':help n\n', sent[#sent])
local info_state = mep.fortran_ui_info()
assert(info_state.directory == '/tmp/session_fortranui')

-- A REPL: override replaces only the python3.
mep.language_ui_repl_override = 'nix shell nixpkgs#gfortran nixpkgs#python3 --command python3'
mep.fortran_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5]:match('mep_fortran_console.py$'), table.concat(mep.started, ' '))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local count = #sent
mep.language_ui_eval_handlers.fortran()
assert(#sent == count and notes[#notes]:find('No Fortran console'))
assert(providers.fortran() == false)
print('editor side passed')
'''

GEOMETRY = '''\
!> Plane geometry helpers.
module geometry
  use constants, only: pi
  implicit none
  private
  public :: point, distance, circle_area

  !> A point in the plane.
  type :: point
    real :: x = 0.0  !! abscissa
    real :: y = 0.0  !! ordinate
  end type point

contains

  !> Euclidean distance between two points.
  !>
  !> Symmetric in its arguments.
  pure function distance(a, b) result(d)
    type(point), intent(in) :: a  !! first point
    type(point), intent(in) :: b  !! second point
    real :: d
    d = sqrt((a%x - b%x)**2 + (a%y - b%y)**2)
  end function distance

  !> Area of a circle of radius `r`.
  elemental real function circle_area(r)
    real, intent(in) :: r !! the radius
    circle_area = pi * r**2
  end function circle_area
end module geometry
'''

CONSTANTS = '''\
module constants
  implicit none
  !> The ratio of a circle's circumference to its diameter.
  real, parameter :: pi = 3.14159265
end module constants
'''

PROGRAM = '''\
program demo
  use geometry
  implicit none
  integer :: i, total
  total = 0
  do i = 1, 4
    if (i > 1) then
      total = total + &
        i
    end if
  end do
  print *, 'total', total
  print *, 'area', nint(circle_area(2.0))
contains
  !> Twice x.
  integer function twice(x)
    integer, intent(in) :: x
    twice = 2 * x
  end function twice
end program demo
'''

SHAPES = '''\
module shapes
  implicit none
contains
  !> The number of sides of a polygon called `name`.
  integer function sides(name)
    character(*), intent(in) :: name
    sides = merge(3, 4, name == 'triangle')
  end function sides
end module shapes
'''


class FortranUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def console(self, directory, inputs, extra_env=None):
        session = Path(directory) / 'session'
        session.mkdir(exist_ok=True)
        script = session / 'mep_fortran_console.py'
        script.write_text(CONSOLE)
        env = dict(os.environ, MEP_FORTRAN_PATH=str(Path(directory) / 'lib'), **(extra_env or {}))
        args = [PYTHON_EXE, str(script), '--dir', str(session), '--compiler', GFORTRAN_EXE,
                '--flags=-fcheck=bounds', '--src-dir', str(directory), '--timeout', '60']
        if LSP_EXE:
            args += ['--lsp', LSP_EXE]
        p = subprocess.run(args, input=''.join(line + '\n' for line in inputs), text=True,
                           capture_output=True, timeout=600, cwd=directory, env=env)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout, session

    def project(self, directory):
        lib = Path(directory) / 'lib'
        lib.mkdir()
        (lib / 'geometry.f90').write_text(GEOMETRY)
        (lib / 'constants.f90').write_text(CONSTANTS)
        (Path(directory) / 'demo.f90').write_text(PROGRAM)
        (Path(directory) / 'shapes.f90').write_text(SHAPES)

    def test_console_eval_variables_help_errors(self):
        with tempfile.TemporaryDirectory(prefix='mep-fortranui-') as directory:
            self.project(directory)
            demo = str(Path(directory) / 'demo.f90')
            out, session = self.console(directory, [
                'integer :: n = 3',
                'real, allocatable :: v(:)',
                'n * 14',                                  # a lone expression is printed
                'allocate(v(n))',
                'v = [2.0, 4.0, 6.0]',
                'print *, "after", n',                    # only the new output shows
                'use geometry',                            # compiled from lib/ with constants
                'type(point) :: p = point(3.0, 4.0)',
                'distance(p, point(0.0, 0.0))',
                'n = undefined_name + 1',                  # compile error: dropped
                'v(7) = 1.0',                              # runtime error (-fcheck=bounds): dropped
                'print *, "still", n',
                ':evalat 4 %s %s' % (demo, demo),          # the declaration line
                ':evalat 5 %s %s' % (demo, demo),          # total = 0
                ':evalat 8 %s %s' % (demo, demo),          # the whole do construct around line 8
                ':evalat 16 %s %s' % (demo, demo),         # a contained function, with its !> docs
                'twice(21)',
                ':help twice',
                ':vars',
            ])
            self.assertIn('          42\n', out)
            self.assertIn(' after           3\n', out)
            self.assertEqual(out.count(' after'), 1, out)
            self.assertIn('   5.00000000', out)
            self.assertRegex(out, r'input\d+:1:\d+: Error: Symbol .undefined_name. at \(1\) has no IMPLICIT type')
            self.assertIn("Index '7' of dimension 1 of array 'v' above upper bound of 3", out)
            self.assertIn('error: exit status 2: input not kept', out)
            self.assertIn(' still           3\n', out)
            self.assertEqual(out.count('          42\n'), 2, out)
            self.assertIn('# demo.f90:6-11', out)
            # Variables: declared names, types, values (an array's shape and elements).
            variables = (session / 'variables.tsv').read_text()
            self.assertIn('n\tinteger\t3\n', variables)
            self.assertRegex(variables, r'v\(:\)\treal, allocatable\t\[3\] 2\.0+ +4\.0+ +6\.0+')
            self.assertRegex(variables, r'p\ttype\(point\)\t3\.0+ +4\.0+')
            self.assertRegex(variables, r'total\tinteger\t9\n')  # the loop ran on the session's own total
            self.assertIn('use geometry', (session / 'session.txt').read_text())
            self.assertIn('integer function twice(x)', (session / 'session.txt').read_text())
            history = (session / 'history.txt').read_text()
            self.assertIn('integer :: n = 3\n', history)
            self.assertNotIn('undefined_name', history)
            # Help: the module's FORD docs, the arguments and where it is.
            help_text = (session / 'help.txt').read_text()
            self.assertIn('integer function twice(x)\n\nTwice x.', help_text)
            self.assertIn('demo.f90:16', help_text)

    def test_console_help_pages(self):
        with tempfile.TemporaryDirectory(prefix='mep-fortranui-') as directory:
            self.project(directory)
            pages = {}
            for name in ['distance', 'point', 'pi', 'geometry', 'nothing_like_it'] + (['sum'] if LSP_EXE else []):
                _, session = self.console(directory, ['use geometry', ':help ' + name])
                pages[name] = (session / 'help.txt').read_text()
            self.assertIn('pure function distance(a, b) result(d)\n\nEuclidean distance between two points.\n\n'
                          'Symmetric in its arguments.\n\nArguments:\n'
                          '  type(point), intent(in) :: a   ! first point\n', pages['distance'])
            self.assertIn('module geometry, ', pages['distance'])
            self.assertIn('geometry.f90:19', pages['distance'])
            self.assertIn('Components:\n  real :: x = 0.0   ! abscissa', pages['point'])
            self.assertIn("The ratio of a circle's circumference to its diameter.", pages['pi'])
            self.assertIn('module constants', pages['pi'])
            self.assertIn('Plane geometry helpers.\n\nContents:\n  type :: point', pages['geometry'])
            self.assertIn('no documentation found', pages['nothing_like_it'])
            if LSP_EXE:
                self.assertIn('SUM(ARRAY, DIM [, MASK])', pages['sum'])
                self.assertIn('(gfortran intrinsic)', pages['sum'])
            # gh from a source file: its own use statements count before the session has any.
            _, session = self.console(directory, [':help circle_area %s' % (Path(directory) / 'demo.f90')])
            self.assertIn('Area of a circle of radius `r`.', (session / 'help.txt').read_text())

    def test_console_run(self):
        with tempfile.TemporaryDirectory(prefix='mep-fortranui-') as directory:
            self.project(directory)
            (Path(directory) / 'cmd.f90').write_text('! CMD: echo built by cmd $((40 + 2))\nprogram c\nend program c\n')
            (Path(directory) / 'broken.f90').write_text('program b\n  call missing_thing(\nend program b\n')
            demo = str(Path(directory) / 'demo.f90')
            out, session = self.console(directory, [
                ':run %s %s' % (demo, demo),
                ':run %s /proj/cmd.f90' % (Path(directory) / 'cmd.f90'),
                ':run %s /proj/broken.f90' % (Path(directory) / 'broken.f90'),
                ':run %s /proj/shapes.f90' % (Path(directory) / 'shapes.f90'),
                'use shapes',
                "sides('triangle') + sides('square')",
            ])
            self.assertIn(' total           9\n', out)
            self.assertIn(' area          13\n', out)
            self.assertIn('built by cmd 42', out)
            self.assertIn('/proj/broken.f90:2', out)
            self.assertIn('error: build failed', out)
            self.assertIn('# loaded module shapes', out)
            self.assertIn('           7\n', out)

    def test_interactive_console(self):
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-fortranui-') as directory:
            session = Path(directory) / 'session'
            session.mkdir()
            (session / 'mep_fortran_console.py').write_text(CONSOLE)
            cmd = '%s %s --dir %s --compiler %s' % (PYTHON_EXE, session / 'mep_fortran_console.py', session, GFORTRAN_EXE)
            p = subprocess.run([script, '-qfec', cmd, '/dev/null'],
                               input='integer :: k\ndo k = 1, 3\nprint *, 10 * k\nend do\n:quit\n',
                               text=True, capture_output=True, timeout=300,
                               env=dict(os.environ, HOME=directory, TERM='dumb'))
            self.assertIn('f> ', p.stdout)
            self.assertIn('.. ', p.stdout)
            self.assertRegex(p.stdout, r'10\s*\r?\n\s+20\s*\r?\n\s+30')
            self.assertIn('k\tinteger\t4', (session / 'variables.tsv').read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--python', default=sys.executable)
    parser.add_argument('--gfortran', default=shutil.which('gfortran'))
    lsp_default = next((str(p) for p in [ROOT / 'build/native/mep-fortran-lsp', ROOT / 'build/native-dev/mep-fortran-lsp']
                        if p.exists()), shutil.which('mep-fortran-lsp'))
    parser.add_argument('--lsp', default=lsp_default)
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.gfortran:
        print('lua or gfortran not installed: skipping the Fortran UI check')
        sys.exit(77)
    LUA_EXE, PYTHON_EXE, GFORTRAN_EXE = args.lua, args.python, args.gfortran
    LSP_EXE = os.path.abspath(args.lsp) if args.lsp else None
    if LSP_EXE and subprocess.run([LSP_EXE, '--intrinsic', 'sum'], capture_output=True).returncode != 0:
        print('%s has no --intrinsic (an older build): skipping the intrinsic help check' % LSP_EXE)
        LSP_EXE = None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
