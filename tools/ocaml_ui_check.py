#!/usr/bin/env python3
"""Check the OCaml language UI mode (src/language_ui_ocaml.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the phrase at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (utop or ocaml,
    a REPL: override), and the panel poll;
  - the console side: the init script the mode writes, loaded by a real
    `ocaml -init`, fed phrases on stdin the way the editor sends them
    (#use of a file with a line directive, MepUI.help / MepUI.view),
    checking the panel files -- help reads a datamunge-style .mli's odoc
    comments through mep-ocaml-lsp --doc when a built one is found (--lsp,
    build*/mep-ocaml-lsp, or PATH);
  - the interactive console under a pty (script(1)): utop when installed
    (--utop or PATH), else ocaml, stays open and refreshes the panels.
Needs lua and ocaml on PATH. GUI coverage is manual.
"""
import argparse
import os
import pty
import select
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_ocaml.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]
INIT = CHUNK.split('[==[\n', 1)[1].split(']==]', 1)[0]
ENV = CHUNK.split('[==[\n', 2)[2].split(']==]', 1)[0]

DATAMUNGE_MLI = '''(** Data munging over native frames. *)

(** Summary statistics. *)
module Stats : sig
  val mean : float array -> float
  (** [mean xs] is the arithmetic mean of [xs].
      @raise Invalid_argument if [xs] is empty. *)
end

val version : string
(** The library version. *)
'''

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
function mep.filename() return '/proj/main.ml' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'ocaml' end
local help_sym = 'Datamunge.Stats.mean'
function mep.help_symbol_at_cursor() return help_sym, 1, #help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.ml == 'ocaml' and mep.language_ui_extensions.mli == 'ocaml')
assert(mep.language_ui_modes.ocaml and mep.language_ui_eval_handlers.ocaml and providers.ocaml)
assert(mep.ocaml_ui_quote('a "b"\\c') == '"a \\"b\\"\\\\c"', mep.ocaml_ui_quote('a "b"\\c'))

-- Phrase at the cursor.
source = {
  'open Datamunge',                  -- 1
  '',                                -- 2
  '(** Doubles x. *)',               -- 3
  'let double x =',                  -- 4
  '  if x > 0',                      -- 5
  '  then 2 * x',                    -- 6
  '  else 0',                        -- 7
  '',                                -- 8
  'type t =',                        -- 9
  '| A',                             -- 10
  '| B of int',                      -- 11
  'let rec even n = n = 0 || odd (n - 1)', -- 12
  'and odd n = n <> 0 && even (n - 1)',    -- 13
  '(* a comment',                    -- 14
  'let not_code = 1',                -- 15
  '*)',                              -- 16
  'let s = "multi',                  -- 17
  'let inside = string"',            -- 18
  'module M = struct',               -- 19
  '  let x = 1',                     -- 20
  'end',                             -- 21
  'let total =',                     -- 22
  '1 + 2',                           -- 23
  'let () = print_int (double 2);;', -- 24
}
local function block(r) return mep.ocaml_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, s .. ',' .. e)
s, e = block(4) assert(s == 4 and e == 7, s .. ',' .. e)
s, e = block(6) assert(s == 4 and e == 7, s .. ',' .. e)
s, e = block(8) assert(s == 4 and e == 7, s .. ',' .. e)
s, e = block(11) assert(s == 9 and e == 11, s .. ',' .. e)
s, e = block(13) assert(s == 12 and e == 13, s .. ',' .. e)
s, e = block(15) assert(s == 12 and e == 13, s .. ',' .. e)
s, e = block(18) assert(s == 17 and e == 18, s .. ',' .. e)
s, e = block(20) assert(s == 19 and e == 21, s .. ',' .. e)
s, e = block(21) assert(s == 19 and e == 21, s .. ',' .. e)
s, e = block(23) assert(s == 22 and e == 23, s .. ',' .. e)
s, e = block(24) assert(s == 24 and e == 24, s .. ',' .. e)

-- Opening: argv (ocaml without utop on PATH), the init script's prefix.
local saved_path = os.getenv
os.getenv = function(k) if k == 'PATH' then return '/nonexistent' end return saved_path(k) end
mep.ocaml_ui_open()
assert(mep.started[1] == 'ocaml' and mep.started[2] == '-init'
  and mep.started[3] == '/tmp/session_ocamlui/mep_init.ml', table.concat(mep.started, ' '))
local init = files['/tmp/session_ocamlui/mep_init.ml']
assert(init:find('^module MepUI_config = struct\n  let dir = "/tmp/session_ocamlui"\n  let lsp = "/opt/mep/bin/mep%-ocaml%-lsp"\n'
  .. '  let context = "/proj/main.ml"\nend;;\nmodule MepUI = struct'), init)
assert(init:find('#use "/tmp/session_ocamlui/mep_env.ml";;', 1, true))
assert(not files['/tmp/session_ocamlui/mep_env.ml']:find('UTop'))
assert(sections.Variables and sections.Modules and sections.Inspect and sections.Help)

-- utop: chosen when on PATH; its hook goes into mep_env.ml.
mep.opt.ocaml_ui_cmd = {'utop'}
mep.ocaml_ui_open()
assert(mep.started[1] == 'utop' and mep.started[2] == '-init')
assert(files['/tmp/session_ocamlui/mep_env.ml']:find('UTop.at_new_command MepUI.refresh', 1, true))
mep.opt.ocaml_ui_cmd = nil
os.getenv = saved_path

-- <leader>re: the phrase at the cursor, after a line directive, ;; added.
cursor_row = 5
mep.language_ui_eval_handlers.ocaml()
assert(sent[#sent] == '#use "/tmp/session_ocamlui/eval.ml";;\n', sent[#sent])
local text = files['/tmp/session_ocamlui/eval.ml']
assert(text == '# 4 "/proj/main.ml"\nlet double x =\n  if x > 0\n  then 2 * x\n  else 0\n;;\n', text)
cursor_row = 24
mep.language_ui_eval_handlers.ocaml()
assert(files['/tmp/session_ocamlui/eval.ml'] == '# 24 "/proj/main.ml"\nlet () = print_int (double 2);;\n')

-- <leader>rr: the whole buffer under the file's name.
local mode_handle = mep.ocaml_ui_open()
mode_handle.run_source('/proj/main.ml')
assert(sent[#sent] == '#use "/tmp/session_ocamlui/run.ml";;\n', sent[#sent])
assert(files['/tmp/session_ocamlui/run.ml']:find('^# 1 "/proj/main.ml"\nopen Datamunge\n'))

-- gh: help into the Help tab, plus the LSP hover from an OCaml buffer.
assert(providers.ocaml() == true)
assert(sent[#sent] == 'MepUI.help "Datamunge.Stats.mean";;\n', sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files.
files['/tmp/session_ocamlui/variables.txt'] =
  'x                      val     int = 42\nt                      type\n'
files['/tmp/session_ocamlui/modules.txt'] =
  'M                      module\n                       directory /lib/ocaml\n'
files['/tmp/session_ocamlui/help.txt'] = 'mean xs\n'
mep.revealed = nil
mep.ocaml_ui_poll()
assert(mep.revealed == 101)
assert(mep.ocaml_ui_info().help == 'mean xs\n')
local vars = sections.Variables[1].widgets
assert(vars[1].text:find('^x ') and vars[1].on_click and not vars[2].on_click)
vars[1].on_click()
assert(sent[#sent] == 'MepUI.view "x";;\n', sent[#sent])
local mods = sections.Modules
assert(mods[1].widgets[1].text == 'M' and mods[2].widgets[1].text == '/lib/ocaml', mods[2].widgets[1].text)
mods[1].widgets[1].on_click()
assert(sent[#sent] == 'MepUI.help "M";;\n', sent[#sent])

-- A REPL: override replaces only the toplevel.
mep.language_ui_repl_override = 'dune utop'
mep.ocaml_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-init' and mep.started[6]:match('mep_init.ml$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.ocaml()
assert(#sent == before and notes[#notes]:find('No OCaml console'))
print('editor side passed')
'''


def find_lsp(explicit):
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else ''
    for p in sorted(ROOT.glob('build*/**/mep-ocaml-lsp')):
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return shutil.which('mep-ocaml-lsp') or ''


def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def session(directory, lsp, context='', utop=False):
    """Writes mep_init.ml and mep_env.ml the way the mode does."""
    d = Path(directory)
    env = d / 'mep_env.ml'
    env.write_text(ENV + ('let () = UTop.at_new_command MepUI.refresh;;\n' if utop else ''))
    init = d / 'mep_init.ml'
    init.write_text(f'module MepUI_config = struct\n  let dir = {q(directory)}\n  let lsp = {q(lsp)}\n'
                    f'  let context = {q(context)}\nend;;\n' + INIT + f'#use {q(str(env))};;\n'
                    + 'MepUI.refresh ();;\nprint_endline "mep language UI";;\n')
    for name in ('variables', 'modules', 'inspect', 'help'):
        (d / f'{name}.txt').write_text('')
    return init


def fixture(directory):
    lib = Path(directory) / 'proj'
    lib.mkdir()
    (lib / 'datamunge.mli').write_text(DATAMUNGE_MLI)
    (lib / 'main.ml').write_text('open Datamunge\nlet m = Stats.mean [| 1.; 2. |]\n')
    return lib


class OcamlUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-ocamlui-') as directory:
            lib = fixture(directory)
            init = session(directory, LSP_EXE or '/nonexistent/mep-ocaml-lsp', str(lib / 'main.ml'))
            d = Path(directory)
            (d / 'eval.ml').write_text('# 4 "/proj/model.ml"\nlet answer = 6 * 7\ntype shape = Circle of float\n;;\n')
            (d / 'bad.ml').write_text('# 12 "/proj/model.ml"\nlet broken = 1 + "x"\n;;\n')
            (d / 'run.ml').write_text('# 1 "/proj/run.ml"\nmodule Geometry = struct\n  let area r = 3.14 *. r *. r\nend\n')
            phrases = '\n'.join([
                f'#use {q(str(d / "eval.ml"))};;',
                f'#use {q(str(d / "bad.ml"))};;',
                f'#use {q(str(d / "run.ml"))};;',
                'let words = ["a"; "b"];;',
                'MepUI.view "words";;',
                'MepUI.help "List.map";;',
                f'#directory {q(str(lib))};;',
                'MepUI.help "Datamunge.Stats.mean";;',
                'MepUI.help "answer";;',
                'MepUI.help "nope_nothing_here";;',
                'MepUI.view "Geometry";;',
                '"done";;',
            ]) + '\n'
            # Each MepUI.help overwrites help.txt; capture them in order by
            # copying after each one.
            copy = lambda n: f'let () = Sys.rename (Filename.concat MepUI.dir "help.txt") (Filename.concat MepUI.dir "help{n}.txt");;'
            lines = phrases.splitlines()
            out, n = [], 0
            for line in lines:
                out.append(line)
                if line.startswith('MepUI.help'):
                    n += 1
                    out.append(copy(n))
            p = subprocess.run([OCAML_EXE, '-init', str(init)], input='\n'.join(out) + '\n', text=True,
                               capture_output=True, timeout=120,
                               env={'PATH': os.environ.get('PATH', ''), 'HOME': directory,
                                    'OCAMLLIB': os.environ.get('OCAMLLIB', '')} if os.environ.get('OCAMLLIB')
                               else {'PATH': os.environ.get('PATH', ''), 'HOME': directory})
            output = p.stdout + p.stderr
            self.assertIn('mep language UI', output)
            self.assertIn('"done"', output)
            # The type error names the source file and line.
            self.assertIn('File "/proj/model.ml", line 12', output)
            variables = (d / 'variables.txt').read_text()
            self.assertRegex(variables, r'answer\s+val\s+int = 42')
            self.assertRegex(variables, r'words\s+val\s+string list = \["a"; "b"\]')
            self.assertRegex(variables, r'(?m)^shape\s+type$')
            self.assertNotIn('MepUI', variables)
            self.assertNotRegex(variables, r'(?m)^(int|list|option)\s')  # predefined types excluded
            modules = (d / 'modules.txt').read_text()
            self.assertRegex(modules, r'(?m)^Geometry\s+module$')
            self.assertNotIn('MepUI', modules)
            self.assertIn(f'directory {lib}', modules)
            inspect = (d / 'inspect.txt').read_text()
            self.assertIn('module Geometry', inspect)
            self.assertIn('val area : float -> float', inspect)
            help1, help2, help3, help4 = [(d / f'help{i}.txt').read_text() for i in range(1, 5)]
            self.assertIn("val List.map : ('a -> 'b) -> 'a list -> 'b list", help1)  # the session's view
            self.assertIn('## In this session', help1)
            self.assertIn('val answer : int', help3)
            self.assertIn('no documentation found', help4)
            if LSP_EXE:
                self.assertIn('applies function `f`', help1)  # odoc from list.mli
                self.assertIn('arithmetic mean', help2)
                self.assertIn('Invalid_argument', help2)
            else:
                self.assertIn('no documentation found', help2)

    def test_view_of_a_value(self):
        with tempfile.TemporaryDirectory(prefix='mep-ocamlui-') as directory:
            init = session(directory, '/nonexistent')
            typed = 'let xs = List.init 3 (fun i -> i * i);;\nMepUI.view "xs";;\nMepUI.view "missing";;\n'
            p = subprocess.run([OCAML_EXE, '-init', str(init)], input=typed, text=True, capture_output=True,
                               timeout=60, env={'PATH': os.environ.get('PATH', ''), 'HOME': directory})
            self.assertIn('mep language UI', p.stdout + p.stderr)
            # The last view wins; check the message for a missing name.
            self.assertIn('missing: not a value or module of this session', (Path(directory) / 'inspect.txt').read_text())
            self.assertRegex((Path(directory) / 'variables.txt').read_text(), r'xs\s+val\s+int list = \[0; 1; 4\]')

    def test_interactive_console(self):
        # The real toplevel (utop when installed) under a pty, typed one
        # line at a time once it is up (utop drops typeahead).
        with tempfile.TemporaryDirectory(prefix='mep-ocamlui-') as directory:
            init = session(directory, LSP_EXE or '/nonexistent', utop=bool(UTOP_EXE))
            (Path(directory) / 'eval.ml').write_text('# 3 "/proj/x.ml"\nlet sent_value = 6 * 7\n;;\n')
            exe = UTOP_EXE or OCAML_EXE
            pid, fd = pty.fork()
            if pid == 0:
                os.environ.update({'HOME': directory, 'TERM': 'xterm-256color'})
                os.execv(exe, [exe, '-init', str(init)])
            out = bytearray()

            def pump(until, seconds):
                end = time.time() + seconds
                while time.time() < end and until not in out:
                    if select.select([fd], [], [], 0.1)[0]:
                        try:
                            out.extend(os.read(fd, 65536))
                        except OSError:
                            return
            try:
                pump(b'mep language UI', 30)
                pump(b'\0', 2 if UTOP_EXE else 0.3)  # utop draws its prompt after the banner
                for line, expect in [('let typed_value = 99;;', b'= 99'), ('print_int (typed_value + 1);;', b'100'),
                                     (f'#use "{directory}/eval.ml";;', b'= 42'), ('"done";;', b'"done"')]:
                    os.write(fd, (line + '\r').encode())
                    pump(expect, 30)
                    if UTOP_EXE:
                        pump(b'\0', 1)  # and redraws it after each result
                os.write(fd, b'#quit;;\r')
                pump(b'\0', 2)
            finally:
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass
                os.waitpid(pid, 0)
            text = out.decode(errors='replace')
            self.assertIn('mep language UI', text, text[-2000:])
            self.assertIn('100', text)
            variables = (Path(directory) / 'variables.txt').read_text()
            # Panels refresh before the next phrase is read.
            self.assertRegex(variables, r'typed_value\s+val\s+int = 99', text[-2000:])
            self.assertRegex(variables, r'sent_value\s+val\s+int = 42')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--ocaml', default=shutil.which('ocaml'))
    parser.add_argument('--utop', default=shutil.which('utop') or '')
    parser.add_argument('--lsp', default='')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.ocaml:
        print('lua or ocaml not installed: skipping the OCaml UI check')
        sys.exit(77)
    LUA_EXE, OCAML_EXE, UTOP_EXE = args.lua, args.ocaml, args.utop
    LSP_EXE = find_lsp(args.lsp)
    print('mep-ocaml-lsp:', LSP_EXE or '(not found: help checks without odoc docs)')
    print('interactive console:', UTOP_EXE or OCAML_EXE)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
