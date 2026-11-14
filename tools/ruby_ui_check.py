#!/usr/bin/env python3
"""Check the Ruby language UI mode (src/language_ui_ruby.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the block at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (including a
    REPL: override), and the panel poll;
  - the console side: the init script the mode writes, loaded into a real
    `ruby`, then MepUI.eval_file / run / help / view driven the way the
    editor drives them, checking the panel files they write -- help reads
    the datamunge fixture's YARD docs through mep-ruby-lsp --doc when a
    built mep-ruby-lsp is found (--lsp, build*/mep-ruby-lsp, or PATH);
  - the interactive console under a pty (script(1)): `irb -r mep_init.rb`
    stays open, runs what is typed, and its evaluate hook refreshes the
    panels.
Needs lua, ruby and irb on PATH (no gems). GUI coverage is manual.
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
HEADER = (ROOT / 'src/language_ui_ruby.h').read_text()
CHUNK = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]
INIT = CHUNK.split('[==[\n', 1)[1].split(']==]', 1)[0]
FIXTURE = ROOT / 'test/fixtures/ruby/datamunge'

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
function mep.filename() return '/proj/model.rb' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'ruby' end
local help_sym = 'frame.filter'
function mep.help_symbol_at_cursor() return help_sym, 1, #help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.rb == 'ruby')
assert(mep.language_ui_modes.ruby and mep.language_ui_eval_handlers.ruby and providers.ruby)
assert(mep.ruby_ui_quote("it's a\\b") == "'it\\'s a\\\\b'", mep.ruby_ui_quote("it's a\\b"))

-- Statement at the cursor.
source = {
  "require 'datamunge'",          -- 1
  '',                             -- 2
  '# Doubles x.',                 -- 3
  'def double(x)',                -- 4
  '  if x > 0',                   -- 5
  '    2 * x',                    -- 6
  '  else',                       -- 7
  '    0',                        -- 8
  '  end',                        -- 9
  'end',                          -- 10
  '',                             -- 11
  'v = [1,',                      -- 12
  '2]',                           -- 13
  'text = <<~TXT',                -- 14
  'not a statement',              -- 15
  'TXT',                          -- 16
  'total = 1 +',                  -- 17
  '2',                            -- 18
  'class Point',                  -- 19
  '  attr_reader :x',             -- 20
  'end',                          -- 21
  '=begin',                       -- 22
  'docs',                         -- 23
  '=end',                         -- 24
  '[1, 2].each do |i|',           -- 25
  '  puts i',                     -- 26
  'end',                          -- 27
  'frame = Datamunge::Frame.new(3)', -- 28
  'frame',                        -- 29
  '  .filter("x")',               -- 30
}
local function block(r) return mep.ruby_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, s .. ',' .. e)
s, e = block(4) assert(s == 4 and e == 10, s .. ',' .. e)
s, e = block(7) assert(s == 4 and e == 10, s .. ',' .. e)
s, e = block(10) assert(s == 4 and e == 10, s .. ',' .. e)
s, e = block(13) assert(s == 12 and e == 13, s .. ',' .. e)
s, e = block(15) assert(s == 14 and e == 16, s .. ',' .. e)
s, e = block(16) assert(s == 14 and e == 16, s .. ',' .. e)
s, e = block(18) assert(s == 17 and e == 18, s .. ',' .. e)
s, e = block(20) assert(s == 19 and e == 21, s .. ',' .. e)
s, e = block(26) assert(s == 25 and e == 27, s .. ',' .. e)
s, e = block(30) assert(s == 29 and e == 30, s .. ',' .. e)

-- Opening: argv, panels, the init script's prefix.
mep.ruby_ui_open()
assert(mep.started[1] == 'irb' and mep.started[2] == '-r'
  and mep.started[3] == '/tmp/session_rubyui/mep_init.rb', table.concat(mep.started, ' '))
assert(files['/tmp/session_rubyui/mep_init.rb']:find(
  "^%$mep_dir = '/tmp/session_rubyui'\n%$mep_ruby_lsp = '/opt/mep/bin/mep%-ruby%-lsp'\nrequire 'pp'"))
assert(sections.Variables and sections.Features and sections.Inspect and sections.Help)

-- <leader>re: the def at the cursor, padded to its lines.
cursor_row = 6
mep.language_ui_eval_handlers.ruby()
assert(sent[#sent] == "MepUI.eval_file('/tmp/session_rubyui/eval.rb', '/proj/model.rb')\n", sent[#sent])
local text = files['/tmp/session_rubyui/eval.rb']
assert(text:sub(1, 3) == '\n\n\n' and text:find('def double%(x%)') and select(2, text:gsub('\n', '')) == 10, text)

-- <leader>rr: the whole buffer through MepUI.run under the file's name.
local mode_handle = mep.ruby_ui_open()
mode_handle.run_source('/proj/model.rb')
assert(sent[#sent] == "MepUI.run('/tmp/session_rubyui/run.rb', '/proj/model.rb')\n", sent[#sent])
assert(files['/tmp/session_rubyui/run.rb']:find("^require 'datamunge'\n"))

-- gh: help into the Help tab, plus the LSP hover from a .rb buffer.
assert(providers.ruby() == true)
assert(sent[#sent] == "MepUI.help('frame.filter')\n", sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files; Features rows open the file.
files['/tmp/session_rubyui/variables.txt'] = 'x                      Integer      42\n'
files['/tmp/session_rubyui/features.txt'] =
  'datamunge              feature      /proj/lib/datamunge.rb\n                       load path    /proj/lib\n'
files['/tmp/session_rubyui/help.txt'] = 'double(x)\n'
mep.revealed = nil
mep.ruby_ui_poll()
assert(mep.revealed == 101)
assert(mep.ruby_ui_info().help == 'double(x)\n')
local feats = sections.Features
assert(feats[1].widgets[1].text:find('datamunge.rb') and feats[2].widgets[1].text == '/proj/lib', feats[2].widgets[1].text)
feats[1].widgets[1].on_click()
assert(cmds[#cmds] == 'edit /proj/lib/datamunge.rb', cmds[#cmds])
local vars = sections.Variables[1].widgets
assert(vars[1].text:find('^x ') and vars[1].on_click)
vars[1].on_click()
assert(sent[#sent] == "MepUI.view('x')\n", sent[#sent])

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'bundle exec irb'
mep.ruby_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5] == '-r' and mep.started[6]:match('mep_init.rb$'))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.ruby()
assert(#sent == before and notes[#notes]:find('No Ruby console'))
print('editor side passed')
'''

CONSOLE = r'''
dir = ENV.fetch('MEP_TEST_DIR')
fixture = ENV.fetch('MEP_FIXTURE')
slurp = ->(name) { File.read(File.join(dir, name)) }
spit = ->(name, s) { File.write(File.join(dir, name), s) }
check = ->(ok, what) { raise "check failed at line #{caller_locations(1, 1)[0].lineno}: #{what}" unless ok }
check.(defined?(MepUI), 'MepUI defined')
# Panels: what the session defines (no irb here: the top-level binding).
MepUI.eval_file(File.join(dir, 'eval.rb').tap { |p| File.write(p, "x_from_session = 7\n") }, '/proj/model.rb')
check.(slurp.('variables.txt') =~ /x_from_session\s+Integer\s+7/, slurp.('variables.txt'))
# A block sent padded to its source lines: errors name the source file and line.
spit.('eval.rb', "\n\n# Twice x.\ndef double(x) = 2 * x\n")
MepUI.eval_file(File.join(dir, 'eval.rb'), '/proj/model.rb')
check.(double(4) == 8, 'double defined')
check.(slurp.('variables.txt') =~ /double\s+method\s+double\(x\)/, slurp.('variables.txt'))
spit.('eval.rb', "\n\n\nraise 'boom'\n")
begin
  MepUI.eval_file(File.join(dir, 'eval.rb'), '/proj/model.rb')
  check.(false, 'raised')
rescue RuntimeError => e
  check.(e.message == 'boom' && e.backtrace.first.start_with?('/proj/model.rb:4'), e.backtrace.first)
end
# The value comes back for irb to display.
spit.('eval.rb', "double(21)\n")
check.(MepUI.eval_file(File.join(dir, 'eval.rb'), '/proj/model.rb') == 42, 'value returned')
# run: a whole file, __FILE__ naming the source, require_relative from its directory.
spit.('run.rb', "module M\n  def self.greet(who) = \"hi \#{who}\"\nend\n$where_from = __FILE__\n")
MepUI.run(File.join(dir, 'run.rb'), '/proj/run_me.rb')
check.(M.greet('x') == 'hi x' && $where_from == '/proj/run_me.rb', 'run')
spit.('run.rb', "$LOAD_PATH.unshift(File.join(__dir__, 'lib'))\nrequire 'datamunge'\nframe = Datamunge::Frame.new(3, 'f')\n")
MepUI.run(File.join(dir, 'run.rb'), File.join(fixture, 'main.rb'))
check.(slurp.('features.txt').include?('datamunge.rb'), slurp.('features.txt'))
check.(slurp.('features.txt').include?('load path'), 'load path listed')
check.(slurp.('variables.txt') =~ /^Datamunge\s+module/, slurp.('variables.txt'))
# Help: through the live receiver's class, a class, a module function, a builtin, an unknown name.
MepUI.help('frame.filter')
h = slurp.('help.txt')
check.(h.include?('owner: Datamunge::Frame') && h.include?('filter(column, value = ..., **options)'), h)
check.(h.include?("defined at: #{fixture}/lib/datamunge.rb:"), h)
if ENV['MEP_EXPECT_DOCS'] == '1'
  check.(h.include?('Keeps the rows whose') && h.include?('**Options**'), h)
  MepUI.help('Datamunge::Frame')
  check.(slurp.('help.txt').include?('class Datamunge::Frame') && slurp.('help.txt').include?('ancestors:'), slurp.('help.txt'))
  MepUI.help('Datamunge.mean')
  check.(slurp.('help.txt').include?('arithmetic mean'), slurp.('help.txt'))
  MepUI.help('frame.column')
  check.(!slurp.('help.txt').include?('@overload') && slurp.('help.txt').include?('Datamunge::Frame#column'), slurp.('help.txt'))
end
MepUI.help('Datamunge::Frame')
check.(slurp.('help.txt').include?('instance methods:'), slurp.('help.txt'))
MepUI.help('[1].map')
check.(slurp.('help.txt').length.positive?, 'literal receiver')
MepUI.help('nope_nothing')
check.(slurp.('help.txt').include?('no documentation found'), slurp.('help.txt'))
# Inspect.
MepUI.view('x_from_session')
check.(slurp.('inspect.txt').include?('(Integer)'), slurp.('inspect.txt'))
MepUI.view('frame')
check.(slurp.('inspect.txt').include?('ancestors: Datamunge::Frame, Enumerable, Datamunge::NativeObject'), slurp.('inspect.txt'))
MepUI.view([1, 2, 3])
check.(slurp.('inspect.txt').include?('(Array)'), slurp.('inspect.txt'))
puts 'console side passed'
'''


def find_lsp(explicit):
    if explicit:
        return explicit if os.access(explicit, os.X_OK) else ''
    for p in sorted(ROOT.glob('build*/**/mep-ruby-lsp')):
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    return shutil.which('mep-ruby-lsp') or ''


def init_file(directory, lsp=''):
    init = Path(directory) / 'mep_init.rb'
    init.write_text(f"$mep_dir = '{directory}'\n$mep_ruby_lsp = '{lsp}'\n" + INIT)
    return init


class RubyUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-rubyui-') as directory:
            init = init_file(directory, LSP_EXE)
            console = Path(directory) / 'console.rb'
            console.write_text(CONSOLE)
            env = {'MEP_TEST_DIR': directory, 'MEP_FIXTURE': str(FIXTURE), 'PATH': os.environ.get('PATH', ''),
                   'HOME': directory, 'MEP_EXPECT_DOCS': '1' if LSP_EXE else '0'}
            p = subprocess.run([RUBY_EXE, '-r', str(init), str(console)], text=True, capture_output=True,
                               timeout=120, env=env)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('console side passed', p.stdout)
            self.assertIn('mep language UI', p.stdout)

    def test_interactive_console(self):
        # The real irb under a pty: the evaluate hook refreshes the panels.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-rubyui-') as directory:
            init = init_file(directory, LSP_EXE)
            (Path(directory) / 'eval.rb').write_text('\n\nsent_value = 6 * 7\n')
            typed = ("typed_value = 99\nputs typed_value + 1\n"
                     f"MepUI.eval_file('{directory}/eval.rb', '/proj/x.rb')\nexit\n")
            p = subprocess.run([script, '-qfec', f'{IRB_EXE} --nomultiline --nocolorize -r {init}', '/dev/null'],
                               input=typed, text=True, capture_output=True, timeout=120,
                               env={'HOME': directory, 'PATH': os.environ.get('PATH', ''), 'TERM': 'xterm-256color',
                                    'IRBRC': '/dev/null'})
            self.assertIn('100', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            self.assertIn('42', p.stdout)
            variables = (Path(directory) / 'variables.txt').read_text()
            self.assertRegex(variables, r'typed_value\s+Integer\s+99')
            self.assertRegex(variables, r'sent_value\s+Integer\s+42')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--ruby', default=shutil.which('ruby'))
    parser.add_argument('--irb', default=shutil.which('irb'))
    parser.add_argument('--lsp', default='')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.ruby or not args.irb:
        print('lua, ruby or irb not installed: skipping the Ruby UI check')
        sys.exit(77)
    LUA_EXE, RUBY_EXE, IRB_EXE = args.lua, args.ruby, args.irb
    LSP_EXE = find_lsp(args.lsp)
    print('mep-ruby-lsp:', LSP_EXE or '(not found: help checks without YARD docs)')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
