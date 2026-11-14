#!/usr/bin/env python3
"""Check the APL language UI mode (src/language_ui_apl.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the statement at the cursor, the script text
    )COPY runs, the symbol gh looks up, what <leader>re / <leader>rr / gh
    send, the console argv (including a REPL: override), the Glyphs and
    Functions panels and the language bar's click;
  - the console side: a real GNU APL (`apl`, skipped cleanly when it isn't
    installed) driven headless the way the editor types into it: the run.apl
    and eval.apl files the mode writes, )COPY'd into a session, defining a
    tradfn, a multi-line dfn and a namespace-free script, then calling them;
  - the Help tab's source: `mep-apl-lsp --doc`, `--outline` and `--glyphs`
    on a datamunge-like generated namespace (a build of mep-apl-lsp, else
    PATH; skipped when there is none).
Needs lua on PATH. GUI coverage is manual.
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
HEADER = (ROOT / 'src/language_ui_apl.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]

STUBS = r"""
local files, sections, sent, notes, jobs, inserted = {}, {}, {}, {}, {}, {}
local buffer, tab = 1, 1
local source = {}
mep = {opt = {language_ui_poll_interval = 1}, language_ui_extensions = {}, language_ui_modes = {},
  language_ui_eval_handlers = {}}
local providers = {}
function mep.help_register_provider(name, fn) providers[name] = fn end
function mep.bundled_tool(name) return '/opt/mep/bin/' .. name end
function mep.current_tab_id() return tab end
function mep.current_buffer() return buffer end
function mep.is_terminal_buffer(b) return b == 2 end
function mep.terminal_write(_, text) sent[#sent + 1] = text end
function mep.notify(msg) notes[#notes + 1] = msg end
function mep.language_ui_session_dir(s) return '/tmp/session_' .. s end
function mep.language_ui_write_file(path, text) files[path] = text end
function mep.language_ui_read_file(path) return files[path] end
function mep.language_ui_shq(s) return "'" .. s .. "'" end
function mep.language_ui_cmd(default)
  if mep.language_ui_repl_override then return {'sh', '-c', mep.language_ui_repl_override .. ' "$@"', 'mep-repl'} end
  local a = {} for _, x in ipairs(default) do a[#a + 1] = x end return a
end
function mep.language_ui_layout(opts) opts.console() buffer = 2 return {editor_pane = 10, top_pane = 20, bottom_pane = 30, console_buf = 2} end
function mep.terminal_here_argv(argv, title) mep.started = argv mep.started_title = title end
function mep.sidebar_create(name) return name end
function mep.sidebar_set_sections(id, value) sections[id] = value end
function mep.language_ui_open_tabs(_, ids) local b = {} for i, id in ipairs(ids) do b[id] = 100 + i end return b end
function mep.language_ui_render_textbox(id, value, empty) sections[id] = value or empty end
function mep.language_ui_teardown() end
function mep.pane_focus(p) mep.focused = p return true end
function mep.pane_focus_buffer(b) mep.focused_buf = b return true end
function mep.reveal_buffer(b) mep.revealed = b end
function mep.jump_to_buffer(b) mep.jumped = b end
function mep.set_cursor(r, c) mep.cursor_set = {r, c} end
function mep.insert_text(t) inserted[#inserted + 1] = t end
function mep.on_frame(fn) mep.frame_fn = fn end
local now = 0
function mep.now() return now end
local epoch = 1
function mep.buffer_save_epoch() return epoch end
function mep.filename() return '/proj/stats.apl' end
function mep_lsp_abspath(p) return p end
function mep.buffer_get_lines() return source end
function mep.get_line(i) return source[i] end
local cursor_row, cursor_col = 1, 1
function mep.cursor() return cursor_row, cursor_col end
local selection = ''
function mep.visual_selection() return selection end
function mep.language_for_buffer() return 'apl' end
-- mep.job_start: records the argv; the test feeds the output.
function mep.job_start(argv, opts) jobs[#jobs + 1] = {argv = argv, opts = opts} end
local function finish(job, lines, code)
  for _, l in ipairs(lines) do job.opts.on_stdout(l) end
  job.opts.on_exit(code or 0)
end
-- `command -v apl` is true, dyalog false, unless a test says otherwise.
local path_has = {apl = true}
os.execute = function(cmd)
  local exe = cmd:match('command %-v (%S+)')
  return exe and path_has[exe] or false
end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.apl == 'apl' and mep.language_ui_extensions.apls == 'apl'
  and mep.language_ui_extensions.dyalog == 'apl' and mep.language_ui_extensions.apln == 'apl')
assert(mep.language_ui_modes.apl and mep.language_ui_eval_handlers.apl and providers.apl)

-- The statement at the cursor.
source = {
  '⍝ stats.apl',                     -- 1
  '',                                -- 2
  '∇ R←Mean X',                      -- 3
  '⍝ The mean of X.',                -- 4
  '  R←(+/X)÷≢X',                    -- 5
  '∇',                               -- 6
  'sd←{',                            -- 7
  '    m←Mean ⍵ ⍝ a } in a comment',  -- 8
  "    s←'}'",                       -- 9
  '    ((+/(⍵-m)*2)÷≢⍵)*0.5',        -- 10
  '}',                               -- 11
  ':If 1',                           -- 12
  '  ⎕←sd 1 2 3',                    -- 13
  ':EndIf',                          -- 14
  'Mean 1 2 3 ⋄ x←2',                -- 15
  ':if 0 ⋄ 1 ⋄ :endif',              -- 16
  'y←3',                             -- 17
}
local function block(r) return mep.apl_ui_block_at(source, r) end
local s, e = block(1) assert(s == nil, tostring(s))
s, e = block(3) assert(s == 3 and e == 6, s .. ',' .. e)
s, e = block(5) assert(s == 3 and e == 6, s .. ',' .. e)
s, e = block(6) assert(s == 3 and e == 6, s .. ',' .. e)
s, e = block(9) assert(s == 7 and e == 11, s .. ',' .. e)
s, e = block(13) assert(s == 12 and e == 14, s .. ',' .. e)
s, e = block(15) assert(s == 15 and e == 15, s .. ',' .. e)
s, e = block(16) assert(s == 16 and e == 16, s .. ',' .. e)
s, e = block(17) assert(s == 17 and e == 17, s .. ',' .. e)

-- The script )COPY runs: a #! line first, no )OFF.
assert(mep.apl_ui_script({'x←1', ')OFF'}) == '#!/usr/bin/env apl --script\nx←1\n', mep.apl_ui_script({'x←1', ')OFF'}))
assert(mep.apl_ui_script({'#!/usr/bin/apl --script', 'x'}) == '#!/usr/bin/apl --script\nx\n')

-- The symbol gh looks up: glyphs, ⎕names, :Keywords, dotted names.
local line = 'r←⎕IO+datamunge.Mean ⍳10 ⋄ :If'
local function at(needle, off) return mep.apl_ui_symbol_at(line, (line:find(needle, 1, true)) + (off or 0)) end
assert(at('⍳') == '⍳', tostring(at('⍳')))
assert(at('⍳', 1) == '⍳')  -- a continuation byte backs up to the glyph
assert(at('⎕IO', 3) == '⎕IO', tostring(at('⎕IO', 3)))
assert(at('Mean') == 'datamunge.Mean', tostring(at('Mean')))
assert(at('+') == '+')
assert(at(':If', 1) == ':If', tostring(at(':If', 1)))
assert(mep.apl_ui_symbol_at(line, 100) == nil)

-- Opening: GNU APL in the console, the glyph and outline jobs.
mep.apl_ui_open()
assert(mep.started[1] == 'apl' and mep.started[2] == '--silent' and mep.started_title == 'apl', table.concat(mep.started, ' '))
assert(sections.Help and sections.Glyphs and sections.Functions)
assert(jobs[1].argv[1] == '/opt/mep/bin/mep-apl-lsp' and jobs[1].argv[2] == '--glyphs')
assert(jobs[2].argv[2] == '--outline' and jobs[2].argv[3] == '/tmp/session_aplui/outline.apl')
assert(files['/tmp/session_aplui/outline.apl']:find('∇ R←Mean X', 1, true))
finish(jobs[1], {'⍴\tfunction\tr\trho shape reshape\tShape / Reshape', '¨\toperator\t1\teach\tEach / Each',
  '⍬\tconstant\t}\tzilde empty\tZilde'})
local gl = sections.Glyphs
assert(#gl == 3 and gl[1].title == 'Functions' and gl[2].title == 'Operators' and gl[3].title == 'Syntax and constants')
assert(gl[1].widgets[1].text == '⍴  `r  rho  Shape / Reshape', gl[1].widgets[1].text)
gl[1].widgets[1].on_click()
assert(inserted[#inserted] == '⍴' and mep.focused_buf == 1)
finish(jobs[2], {'1\t0\tnamespace\tdm\t:Namespace dm', '3\t1\ttradfn\tdm.Mean\tR←Mean X', '7\t1\tdfn\tdm.sd\tsd←{'})
local fw = sections.Functions[1].widgets
assert(#fw == 3 and fw[2].text == '  Mean  R←Mean X' and fw[1].hl == 'Title', fw[2].text)
fw[2].on_click()
assert(mep.cursor_set[1] == 3)

-- <leader>re: a one-line statement is typed; a tradfn, dfn or control structure goes through eval.apl.
cursor_row = 15
mep.language_ui_eval_handlers.apl()
assert(sent[#sent] == 'Mean 1 2 3 ⋄ x←2\n', sent[#sent])
cursor_row = 5
mep.language_ui_eval_handlers.apl()
assert(sent[#sent] == ')COPY /tmp/session_aplui/eval.apl\n', sent[#sent])
assert(files['/tmp/session_aplui/eval.apl'] == '#!/usr/bin/env apl --script\n∇ R←Mean X\n⍝ The mean of X.\n  R←(+/X)÷≢X\n∇\n',
  files['/tmp/session_aplui/eval.apl'])
cursor_row = 8
mep.language_ui_eval_handlers.apl()
assert(files['/tmp/session_aplui/eval.apl']:find('^#![^\n]*\nsd←{\n'))
-- A line with a comment: the comment isn't sent.
source[18] = 'z←1 ⍝ note'
cursor_row = 18
mep.language_ui_eval_handlers.apl()
assert(sent[#sent] == 'z←1\n', sent[#sent])
-- A selection of several lines.
selection = 'a←1\nb←2'
mep.language_ui_eval_handlers.apl()
assert(sent[#sent] == ')COPY /tmp/session_aplui/eval.apl\n' and files['/tmp/session_aplui/eval.apl']:find('\na←1\nb←2\n'))
selection = ''

-- <leader>rr: the buffer as run.apl.
local handle = mep.apl_ui_open()
handle.run_source('/proj/stats.apl')
assert(sent[#sent] == ')COPY /tmp/session_aplui/run.apl\n', sent[#sent])
assert(files['/tmp/session_aplui/run.apl']:find('^#![^\n]*\n⍝ stats.apl\n'))

-- gh: mep-apl-lsp --doc with the buffer's directory and a copy of the buffer.
cursor_row, cursor_col = 13, 9   -- the 'sd' in ⎕←sd
local before = #jobs
assert(providers.apl() == true)
local j = jobs[#jobs]
assert(#jobs == before + 1 and j.argv[2] == '--doc' and j.argv[3] == '--root' and j.argv[4] == '/proj' and j.argv[5] == 'sd',
  table.concat(j.argv, ' '))
assert(j.argv[6] == '/tmp/session_aplui/help_src.apl')
finish(j, {'```apl', 'sd←{', '```', '*dfn*', '', 'Standard deviation.'})
assert(sections.Help:find('Standard deviation.', 1, true) and not sections.Help:find('```', 1, true))
assert(mep.revealed == 101)
cursor_row, cursor_col = 2, 1
assert(providers.apl() == true and notes[#notes]:find('no APL name'))

-- The Functions panel refreshes after a save.
local njobs = #jobs
epoch = 2
now = 10
mep.frame_fn()
assert(#jobs == njobs + 1 and jobs[#jobs].argv[2] == '--outline')

-- Dyalog: a REPL: override naming dyalog fixes files with 2 ⎕FIX.
mep.language_ui_repl_override = 'dyalog -b'
local h2 = mep.apl_ui_open()
assert(mep.started[1] == 'sh' and mep.started_title == 'dyalog')
h2.run_source('/proj/stats.apl')
assert(sent[#sent] == "2 ⎕FIX 'file:///tmp/session_aplui/run.apl'\n", sent[#sent])
assert(not files['/tmp/session_aplui/run.apl']:find('^#!'))
mep.language_ui_repl_override = nil
-- No apl on PATH but dyalog: Dyalog is the console.
path_has = {dyalog = true}
mep.apl_ui_open()
assert(mep.started[1] == 'dyalog', mep.started[1])
path_has = {apl = true}

-- No console: a warning, nothing sent.
tab = 2
mep.apl_ui_open()
mep.is_terminal_buffer = function() return false end
local count = #sent
cursor_row = 17
mep.language_ui_eval_handlers.apl()
assert(#sent == count and notes[#notes]:find('console is not running'))
print('editor side passed')
'''

LUA_SCRIPT_TEXT = r'''
-- Writes run.apl / eval.apl the way the mode does, for the console check.
local out = arg[1]
local function write(name, lines)
  local f = assert(io.open(out .. '/' .. name, 'w'))
  f:write(mep.apl_ui_script(lines))
  f:close()
end
write('run.apl', {
  '⍝ A GNU APL script, as the Run button and <leader>rr send it.',
  '∇ R←Mean X',
  '⍝ The mean of X.',
  '  R←(+/X)÷≢X',
  '∇',
  'sd←{((+/(⍵-Mean ⍵)*2)÷≢⍵)*0.5}',
  'total←+/⍳10',
  ')OFF',
})
write('eval.apl', {'∇ Z←A Plus B', '  Z←A+B', '∇'})
print('scripts written')
'''

BINDING = ''':Namespace datamunge
⍝ Bindings to the datamunge C++ library.

    ∇ R←A Add B
    ⍝ Adds two arrays elementwise.
    ⍝
    ⍝ - `A`: left array
      R←A+B
    ∇

    ⍝ The mean of a vector.
    Mean←{(+/⍵)÷≢⍵}
:EndNamespace
'''


class AplUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True, capture_output=True, timeout=20)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        if not APL_EXE:
            self.skipTest('GNU APL (apl) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-aplui-') as d:
            script = Path(d) / 'write.lua'
            script.write_text(STUBS + CHUNK + LUA_SCRIPT_TEXT)
            p = subprocess.run([LUA_EXE, str(script), d], text=True, capture_output=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertNotIn(')OFF', (Path(d) / 'run.apl').read_text())
            typed = [f')COPY {d}/run.apl', 'Mean 1 2 3 4', 'sd 2 4 4 4 5 5 7 9', 'total',
                     f')COPY {d}/eval.apl', '2 Plus 40', ')FNS', ')OFF']
            p = subprocess.run([APL_EXE, '--silent', '--noCONT', '--noColor', '--rawCIN'],
                               input=''.join(t + '\n' for t in typed), text=True, capture_output=True, timeout=60,
                               cwd=d, env={'HOME': d, 'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'TERM': 'dumb'})
            out = p.stdout + p.stderr
            self.assertIn('2.5', out)       # Mean 1 2 3 4
            self.assertIn('2', [l.strip() for l in out.splitlines()])  # sd 2 4 4 4 5 5 7 9: exactly 2
            self.assertIn('55', out)        # total
            self.assertIn('42', out)        # 2 Plus 40
            self.assertRegex(out, r'Mean\s+Plus|Mean.*Plus')  # )FNS
            self.assertNotIn('VALUE ERROR', out)

    def test_help_cli(self):
        if not APL_LSP:
            self.skipTest('no mep-apl-lsp build')
        with tempfile.TemporaryDirectory(prefix='mep-aplui-') as d:
            (Path(d) / 'datamunge.apln').write_text(BINDING)
            run = lambda *a: subprocess.run([APL_LSP, *a], capture_output=True, text=True, timeout=30)
            p = run('--doc', '--root', d, 'Add')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('R←A Add B', p.stdout)
            self.assertIn('Adds two arrays elementwise.', p.stdout)
            p = run('--doc', '--root', d, 'datamunge.Mean')
            self.assertIn('The mean of a vector.', p.stdout)
            p = run('--doc', 'rho')
            self.assertIn('Reshape', p.stdout)
            p = run('--doc', '⎕NA')
            self.assertIn('Name association', p.stdout)
            self.assertNotEqual(run('--doc', '--root', d, 'Nope').returncode, 0)
            p = run('--outline', str(Path(d) / 'datamunge.apln'))
            self.assertEqual(p.stdout.splitlines()[1].split('\t')[:4], ['4', '1', 'tradfn', 'datamunge.Add'])
            p = run('--glyphs')
            self.assertIn('⍴\tfunction\tr\t', p.stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--apl', default=shutil.which('apl'))
    parser.add_argument('--apl-lsp', default=None, help='mep-apl-lsp (default: the newest build, else PATH)')
    args, remaining = parser.parse_known_args()
    if not args.lua:
        print('lua not installed: skipping the APL UI check')
        sys.exit(77)
    LUA_EXE, APL_EXE = args.lua, args.apl
    APL_LSP = args.apl_lsp
    if not APL_LSP:
        builds = sorted(ROOT.glob('build/*/mep-apl-lsp'), key=lambda p: p.stat().st_mtime, reverse=True)
        APL_LSP = str(builds[0]) if builds else (shutil.which('mep-apl-lsp') or '')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
