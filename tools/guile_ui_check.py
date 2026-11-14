#!/usr/bin/env python3
"""Check the Guile Scheme language UI mode (src/language_ui_guile.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the top-level form at the cursor, what
    <leader>re / <leader>rr / gh type into the console, the console argv
    (including a REPL: override), and the panel poll;
  - the console side: the init script the mode writes, loaded into a real
    `guile`, then mep-eval-file / mep-run / mep-help / mep-view driven the
    way the editor drives them, checking the panel files they write -- help
    adds the docstring mep-scheme-lsp --doc reads from a module's source
    when a built one is found (--lsp, build*/mep-scheme-lsp, or PATH);
  - the interactive REPL under a pty (script(1)): `guile -q -l mep_init.scm`,
    whose before-read-hook refreshes the panels.
Needs lua and guile on PATH; skips (exit 77) when either is missing.
GUI coverage is manual.
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
HEADER = (ROOT / 'src/language_ui_guile.h').read_text()
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
function mep.filename() return '/proj/model.scm' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'guile' end
local help_sym = 'mean'
function mep.help_symbol_at_cursor() return help_sym, 1, #help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.scm == 'guile')
assert(mep.language_ui_modes.guile and mep.language_ui_eval_handlers.guile and providers.guile)
assert(mep.guile_ui_quote('a"b\\c') == '"a\\"b\\\\c"', mep.guile_ui_quote('a"b\\c'))

-- Top-level form at the cursor.
source = {
  '(use-modules (datamunge stats))', -- 1
  '',                                -- 2
  ';; Doubles x.',                   -- 3
  '(define (twice x)',               -- 4
  '  "Doubles X (a paren: ( )."',    -- 5
  '  (* 2 x))',                      -- 6
  '#| block (',                      -- 7
  '   comment |#',                   -- 8
  '(define c #\\()',                 -- 9
  'some-atom',                       -- 10
  "'(1 2",                           -- 11
  '  3)',                            -- 12
  '(a) (b',                          -- 13
  ' c)',                             -- 14
}
local function block(r) return mep.guile_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, tostring(s) .. ',' .. tostring(e))
assert(block(2) == nil and block(3) == nil and block(7) == nil)
s, e = block(5) assert(s == 4 and e == 6, s .. ',' .. e)
s, e = block(9) assert(s == 9 and e == 9, s .. ',' .. e)
s, e = block(10) assert(s == 10 and e == 10, s .. ',' .. e)
s, e = block(12) assert(s == 11 and e == 12, s .. ',' .. e)
s, e = block(13) assert(s == 13 and e == 14, s .. ',' .. e)

-- Opening: argv, panels, the init script's start call.
mep.guile_ui_open()
assert(mep.started[1] == 'guile' and mep.started[2] == '-q' and mep.started[3] == '-l'
  and mep.started[4] == '/tmp/session_guileui/mep_init.scm', table.concat(mep.started, ' '))
local init = files['/tmp/session_guileui/mep_init.scm']
assert(init:find('^%(use%-modules'))
assert(init:find('\n%(mep%-ui%-start "/tmp/session_guileui" "/opt/mep/bin/mep%-scheme%-lsp" \'%("/proj"%)%)\n$'), init:sub(-120))
assert(sections.Module and sections.Modules and sections.Inspect and sections.Help)

-- <leader>re: the define at the cursor, padded to its lines.
cursor_row = 5
mep.language_ui_eval_handlers.guile()
assert(sent[#sent] == '(mep-eval-file "/tmp/session_guileui/eval.scm" "/proj/model.scm")\n', sent[#sent])
local text = files['/tmp/session_guileui/eval.scm']
assert(text:sub(1, 3) == '\n\n\n' and text:find('%(define %(twice x%)') and select(2, text:gsub('\n', '')) == 6, text)

-- <leader>rr: the whole buffer through mep-run under the file's name.
local mode_handle = mep.guile_ui_open()
mode_handle.run_source('/proj/model.scm')
assert(sent[#sent] == '(mep-run "/tmp/session_guileui/run.scm" "/proj/model.scm")\n', sent[#sent])
assert(files['/tmp/session_guileui/run.scm']:find('^%(use%-modules'))

-- gh: help into the Help tab, plus the LSP hover from a .scm buffer.
assert(providers.guile() == true)
assert(sent[#sent] == '(mep-help "mean")\n', sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files.
files['/tmp/session_guileui/module.txt'] = 'x                      variable   42\n'
files['/tmp/session_guileui/modules.txt'] = 'current (guile-user)\nuses (datamunge stats)\npath /proj\n'
files['/tmp/session_guileui/help.txt'] = '(mean values)\n'
mep.revealed = nil
mep.guile_ui_poll()
assert(mep.revealed == 101)
assert(mep.guile_ui_info().help == '(mean values)\n')
local ms = sections.Modules
assert(ms[1].title == 'Used by (guile-user)' and ms[1].widgets[1].text == '(datamunge stats)' and ms[2].widgets[1].text == '/proj')
sections.Module[1].widgets[1].on_click()
assert(sent[#sent] == '(mep-view "x")\n', sent[#sent])

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop --command guile'
mep.guile_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-q' and mep.started[7]:match('mep_init.scm$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.guile()
assert(#sent == before and notes[#notes]:find('No Guile console'))
print('editor side passed')
'''

CONSOLE = r'''
(use-modules (ice-9 rdelim))
(define dir (getenv "MEP_TEST_DIR"))
(define (path n) (string-append dir "/" n))
(define (slurp n) (call-with-input-file (path n) (lambda (p) (let ((s (read-delimited "" p))) (if (eof-object? s) "" s)))))
(define (spit n s) (call-with-output-file (path n) (lambda (p) (display s p))))
(define (check ok what) (unless ok (error "check failed:" what)))
(define (has text part) (string-contains text part))
(check (defined? 'mep-help) "mep-help defined")
(check (member (path "proj") %load-path) "source directory on %load-path")
;; A form sent padded to its source lines: the value comes back; errors name the source line.
(spit "eval.scm" "\n\n(define x-from-session 7)\nx-from-session\n")
(check (equal? (mep-eval-file (path "eval.scm") "/proj/model.scm") 7) "value returned")
(check (has (slurp "module.txt") "x-from-session") (slurp "module.txt"))
(spit "eval.scm" "\n\n\n(error \"boom\")\n")
(define err-line
  (catch #t (lambda () (mep-eval-file (path "eval.scm") "/proj/model.scm") #f)
    (lambda (key . args) key)))
(check err-line "raised")
(spit "eval.scm" "\n(define (twice x)\n  \"Doubles X.\"\n  (* 2 x))\n")
(mep-eval-file (path "eval.scm") "/proj/model.scm")
(check (= (twice 4) 8) "twice defined")
(check (has (slurp "module.txt") "(twice x)") (slurp "module.txt"))
;; mep-run: a whole file; the module it uses comes from %load-path.
(spit "run.scm" "(use-modules (datamunge stats))\n(define m (mean '(1 2 3)))\n")
(mep-run (path "run.scm") (path "proj/main.scm"))
(check (= m 2) "run")
(check (has (slurp "modules.txt") "uses (datamunge stats)") (slurp "modules.txt"))
(check (not (has (slurp "modules.txt") "pretty-print")) "init's own modules left out")
;; Help: live docstring and owner, a Guile built-in, unknown.
(mep-help 'mean)
(define h (slurp "help.txt"))
(check (and (has h "procedure in module (datamunge stats)") (has h "Arithmetic mean of VALUES.")) h)
(when (equal? (getenv "MEP_EXPECT_DOCS") "1")
  (check (has h "stats.scm:") h)
  ;; A procedure without a live docstring (as a C gsubr can be): the source's.
  (mep-help "nodoc")
  (check (has (slurp "help.txt") "Docstring only in the source.") (slurp "help.txt")))
(mep-help "twice")
(check (has (slurp "help.txt") "Doubles X.") (slurp "help.txt"))
(mep-help 'string-split)
(check (has (slurp "help.txt") "Split the string") (slurp "help.txt"))
(mep-help 'nope-nothing)
(check (has (slurp "help.txt") "no documentation found") (slurp "help.txt"))
;; Inspect.
(mep-view 'x-from-session)
(check (has (slurp "inspect.txt") "(number)") (slurp "inspect.txt"))
(define lst '(1 2 3))
(mep-view "lst")
(check (has (slurp "inspect.txt") "(list, 3 elements)") (slurp "inspect.txt"))
(display "console side passed\n")
'''


def find_lsp(explicit):
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else ''
    # The most recently built one (an older build may predate --doc).
    found = [p for p in ROOT.glob('build*/**/mep-scheme-lsp') if p.is_file() and os.access(p, os.X_OK)]
    if found:
        return str(max(found, key=lambda p: p.stat().st_mtime))
    return shutil.which('mep-scheme-lsp') or ''


def make_session(directory, lsp=''):
    d = Path(directory)
    (d / 'proj').mkdir(exist_ok=True)
    site = d / 'site' / 'datamunge'
    site.mkdir(parents=True, exist_ok=True)
    (site / 'stats.scm').write_text(
        '(define-module (datamunge stats)\n  #:export (mean nodoc))\n\n(define (mean values)\n'
        '  "Arithmetic mean of VALUES."\n  (/ (apply + values) (length values)))\n\n'
        ';; Its docstring is stripped at load time below, as a C procedure has none.\n'
        '(define (nodoc x)\n  "Docstring only in the source."\n  x)\n'
        "(set-procedure-property! nodoc 'documentation #f)\n")
    init = d / 'mep_init.scm'
    init.write_text(INIT + f'\n(mep-ui-start "{d}" "{lsp}" \'("{d / "proj"}"))\n')
    return init


def env_for(directory):
    env = dict(os.environ, HOME=directory, MEP_TEST_DIR=directory, GUILE_AUTO_COMPILE='0',
               GUILE_LOAD_PATH=str(Path(directory) / 'site'), MEP_EXPECT_DOCS='1' if LSP_EXE else '0')
    env.pop('MEP_GUILE_PATH', None)
    return env


class GuileUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-guileui-') as directory:
            init = make_session(directory, LSP_EXE)
            console = Path(directory) / 'console.scm'
            console.write_text(CONSOLE)
            p = subprocess.run([GUILE_EXE, '-q', '-l', str(init), str(console)], text=True, capture_output=True,
                               timeout=120, env=env_for(directory))
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('mep language UI', p.stdout)

    def test_interactive_console(self):
        # The real REPL under a pty: the before-read-hook refreshes the panels.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-guileui-') as directory:
            init = make_session(directory, LSP_EXE)
            (Path(directory) / 'eval.scm').write_text('\n\n(define sent-value (* 6 7))\nsent-value\n')
            typed = (f'(define typed-value 99)\n(+ typed-value 1)\n'
                     f'(mep-eval-file "{directory}/eval.scm" "/proj/x.scm")\n(display "next")\n,q\n')
            env = env_for(directory)
            env['TERM'] = 'dumb'
            p = subprocess.run([script, '-qfec', f'{GUILE_EXE} -q -l {init}', '/dev/null'], input=typed, text=True,
                               capture_output=True, timeout=60, env=env)
            self.assertIn('100', p.stdout)
            self.assertIn('42', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            module = (Path(directory) / 'module.txt').read_text()
            self.assertRegex(module, r'typed-value\s+variable\s+99')
            self.assertRegex(module, r'sent-value\s+variable\s+42')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--guile', default=shutil.which('guile'))
    parser.add_argument('--lsp', default='')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.guile:
        print('lua or guile not installed: skipping the Guile UI check')
        sys.exit(77)
    LUA_EXE, GUILE_EXE = args.lua, args.guile
    LSP_EXE = find_lsp(args.lsp)
    print('mep-scheme-lsp:', LSP_EXE or '(not found: help checks without source docstrings)')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
