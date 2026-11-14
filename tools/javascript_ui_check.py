#!/usr/bin/env python3
"""Check the JavaScript language UI mode (src/language_ui_javascript.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the statement at the cursor, what <leader>re /
    <leader>rr / gh type into the console, the console argv (including a
    REPL: override), and the panel poll;
  - the console side: the init script the mode writes, run by a real `node`
    with commands piped in the way the editor types them (:eval, :run,
    :help, :inspect), checking the panel files it writes and the help it
    finds in a package's TypeScript declarations -- through
    `mep-js-lsp --dts-doc` when the given binary has it, and through the
    init script's own .d.ts reader;
  - the interactive REPL under a pty (script(1)): `node mep_init.js` stays
    open, runs what is typed, and refreshes the panels after each input.
Needs lua and node on PATH (no packages). GUI coverage is manual.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_javascript.h').read_text()
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
function mep.bundled_tool(name) return '/opt/mep/bin/' .. name end
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
function mep.filename() return '/proj/model.js' end
function mep_lsp_abspath(p) return p end
function mep.line_count() return #source end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
function mep.visual_selection() return '' end
function mep.language_for_buffer() return 'javascript' end
local help_sym = 'dm.ref'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.js == 'javascript' and mep.language_ui_extensions.mjs == 'javascript'
  and mep.language_ui_extensions.cjs == 'javascript')
assert(mep.language_ui_modes.javascript and mep.language_ui_eval_handlers.javascript and providers.javascript)
assert(mep.javascript_ui_quote('a"b\\\n') == '"a\\"b\\\\\\n"', mep.javascript_ui_quote('a"b\\\n'))

-- Statement at the cursor.
source = {
  "const dm = require('@datamunge/datamungejs');",  -- 1
  '',                                                -- 2
  '/**',                                             -- 3
  ' * Twice x.',                                     -- 4
  ' */',                                             -- 5
  'function double(x) {',                            -- 6
  '  if (x > 0) {',                                  -- 7
  '    return 2 * x;',                               -- 8
  '  }',                                             -- 9
  '  return 0;',                                     -- 10
  '}',                                               -- 11
  '',                                                -- 12
  'const v = [1,',                                   -- 13
  '2];',                                             -- 14
  'const total = 1 +',                               -- 15
  '2;',                                              -- 16
  '// comment',                                      -- 17
  'console.log(double(2));',                         -- 18
  'class P {',                                       -- 19
  '  constructor() { this.x = `a ${ {b: 1}.b }',     -- 20
  '}`; }',                                           -- 21
  '}',                                               -- 22
  'const chained = [1, 2]',                          -- 23
  '.map(x => x * 2)',                                -- 24
  '  .filter(Boolean);',                             -- 25
  "const re = /[}{]/g; const s = '{';",              -- 26
  'const after = 1;',                                -- 27
}
local function block(r) return mep.javascript_ui_block_at(source, r) end
local s, e = block(1) assert(s == 1 and e == 1, s .. ',' .. e)
s, e = block(4) assert(s == 6 and e == 11, s .. ',' .. e)
s, e = block(6) assert(s == 6 and e == 11, s .. ',' .. e)
s, e = block(8) assert(s == 6 and e == 11, s .. ',' .. e)
s, e = block(11) assert(s == 6 and e == 11, s .. ',' .. e)
s, e = block(14) assert(s == 13 and e == 14, s .. ',' .. e)
s, e = block(16) assert(s == 15 and e == 16, s .. ',' .. e)
s, e = block(18) assert(s == 18 and e == 18, s .. ',' .. e)
s, e = block(21) assert(s == 19 and e == 22, s .. ',' .. e)
s, e = block(24) assert(s == 23 and e == 25, s .. ',' .. e)
s, e = block(26) assert(s == 26 and e == 26, s .. ',' .. e)
s, e = block(27) assert(s == 27 and e == 27, s .. ',' .. e)

-- Opening: argv, init script, panels.
mep.javascript_ui_open()
assert(mep.started[1] == 'node' and mep.started[2] == '/tmp/session_jsui/mep_init.js' and #mep.started == 2,
  table.concat(mep.started, ' '))
local init = files['/tmp/session_jsui/mep_init.js']
assert(init:find('^const MEP_DIR = "/tmp/session_jsui";\nconst MEP_JS_LSP = "/opt/mep/bin/mep%-js%-lsp";\n\'use strict\';'), init:sub(1, 200))
assert(sections.Variables and sections.Modules and sections.History and sections.Inspect and sections.Help)

-- <leader>re: the function at the cursor, padded to its lines.
cursor_row = 8
mep.language_ui_eval_handlers.javascript()
assert(sent[#sent] == ':eval "/tmp/session_jsui/eval.js" "/proj/model.js"\n', sent[#sent])
local text = files['/tmp/session_jsui/eval.js']
assert(text:sub(1, 6) == '\n\n\n\n\nf' and text:find('function double%(x%)') and select(2, text:gsub('\n', '')) == 11, text)

-- <leader>rr: the whole buffer through :run under the file's name.
local mode_handle = mep.javascript_ui_open()
mode_handle.run_source('/proj/model.js')
assert(sent[#sent] == ':run "/tmp/session_jsui/run.js" "/proj/model.js"\n', sent[#sent])
assert(files['/tmp/session_jsui/run.js']:find("^const dm = require"))

-- gh: help into the Help tab (with the source file, for its require()s), plus the LSP hover.
assert(providers.javascript() == true)
assert(sent[#sent] == ':help "dm.ref" "/proj/model.js"\n', sent[#sent])
assert(mep.hovered and mep.revealed == 101)

-- The poll picks up the console's files; a Variables row inspects.
local st = {paths = {
  variables = '/tmp/session_jsui/variables.txt', modules = '/tmp/session_jsui/modules.txt',
  history = '/tmp/session_jsui/history.txt', inspect = '/tmp/session_jsui/inspect.txt',
  help = '/tmp/session_jsui/help.txt'}, help_buf = 7}
files['/tmp/session_jsui/variables.txt'] = 'x                      number     42\n'
files['/tmp/session_jsui/help.txt'] = 'double(x)\n'
mep.javascript_ui_poll(st)
assert(mep.revealed == 7)
mep.javascript_ui_open()
files['/tmp/session_jsui/variables.txt'] = 'y                      number     1\n'
mep.javascript_ui_poll()
local w = sections.Variables[1].widgets[1]
assert(w.text:find('^y '), w.text)
w.on_click()
assert(sent[#sent] == ':inspect y\n', sent[#sent])

-- A REPL: override replaces only the interpreter.
mep.language_ui_repl_override = 'nix develop .#javascript --command node'
mep.javascript_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5]:match('mep_init.js$'), table.concat(mep.started, ' '))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local before = #sent
mep.language_ui_eval_handlers.javascript()
assert(#sent == before and notes[#notes]:find('No Node.js console'))
print('editor side passed')
'''

FIX_DTS = '''/** Shape kinds. */
export declare enum Kind { circle = 0, square = 1 }

/** A circle with area pi r^2. */
export declare class Circle {
    /** @param r distance from the centre */
    constructor(r: number);
    /** Area of the circle. */
    area(): number;
}

/**
 * Adds two numbers.
 * @param a first term
 * @param b second term
 */
export declare function add(a: number, b?: number): number;
'''

FIX_JS = '''class Circle { constructor(r) { this.r = r; } area() { return Math.PI * this.r * this.r; } }
function add(a, b = 0) { return a + b; }
module.exports = { Circle, add, Kind: { circle: 0, square: 1 } };
'''

# A package reachable only through NODE_PATH, its .d.ts beside its main (as datamunge's is): no `types` field.
SCOPED_JS = 'module.exports = { mean(xs) { return xs.reduce((a, b) => a + b, 0) / xs.length; } };\n'
SCOPED_DTS = '''/**
 * Arithmetic mean of the values.
 * @param xs the sample
 */
export declare function mean(xs: number[]): number;
'''


def make_project(directory):
    d = Path(directory)
    pkg = d / 'proj/node_modules/fixpkg'
    (pkg / 'lib').mkdir(parents=True)
    (pkg / 'types').mkdir()
    (pkg / 'package.json').write_text(json.dumps({'name': 'fixpkg', 'version': '1.2.3', 'main': 'lib/main.js',
                                                  'types': 'types/fix.d.ts'}))
    (pkg / 'lib/main.js').write_text(FIX_JS)
    (pkg / 'types/fix.d.ts').write_text(FIX_DTS)
    scoped = d / 'nodepath/@dm/stats'
    scoped.mkdir(parents=True)
    (scoped / 'package.json').write_text(json.dumps({'name': '@dm/stats', 'version': '0.0.1', 'main': 'index.js'}))
    (scoped / 'index.js').write_text(SCOPED_JS)
    (scoped / 'index.d.ts').write_text(SCOPED_DTS)
    (d / 'proj/model.mjs').write_text("import { mean as avg } from '@dm/stats';\nimport * as fx from 'fixpkg';\n")
    return d / 'proj'


def init_file(directory, lsp):
    init = Path(directory) / 'mep_init.js'
    init.write_text('const MEP_DIR = ' + json.dumps(str(directory)) + ';\nconst MEP_JS_LSP = ' + json.dumps(lsp) + ';\n' + INIT)
    return init


def lsp_has_dts_doc(lsp):
    if not lsp:
        return False
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / 'a.d.ts'
        f.write_text('/** Doc. */\nexport declare function f(): void;\n')
        try:
            p = subprocess.run([lsp, '--dts-doc', str(f), 'f'], capture_output=True, text=True, timeout=30)
        except OSError:
            return False
        return p.returncode == 0 and 'Doc.' in p.stdout


class JavascriptUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True,
                           capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def run_console(self, directory, lines, lsp='/nonexistent/mep-js-lsp'):
        proj = Path(directory) / 'proj'
        init = init_file(directory, lsp)
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': directory, 'NODE_REPL_HISTORY': '',
               'NODE_PATH': str(Path(directory) / 'nodepath')}
        return subprocess.run([NODE_EXE, str(init)], input=''.join(l + '\n' for l in lines), text=True,
                              capture_output=True, timeout=120, cwd=proj, env=env)

    def test_console_side(self):
        with tempfile.TemporaryDirectory(prefix='mep-jsui-') as directory:
            make_project(directory)
            d = Path(directory)
            slurp = lambda name: (d / name).read_text()
            (d / 'eval1.js').write_text('\n\n\nfunction double(x) {\n  return 2 * x;\n}\n')
            (d / 'eval2.js').write_text('\n\n\n\n\n\nnull.boom;\n')
            (d / 'eval3.js').write_text('double(21)\n')
            (d / 'eval4.js').write_text('import { add } from "fixpkg";\nconst five = add(2, 3);\n')
            (d / 'run.js').write_text("const fix = require('fixpkg');\nconst c = new fix.Circle(2);\n"
                                      "const where_from = __filename;\n")
            q = json.dumps
            p = self.run_console(directory, [
                'const x_from_session = 7',
                'const x_from_session = 8',       # top-level const can be sent again
                f':eval {q(str(d / "eval1.js"))} "/proj/model.js"',
                f':eval {q(str(d / "eval2.js"))} "/proj/model.js"',
                f':eval {q(str(d / "eval3.js"))} "/proj/model.js"',
                f':eval {q(str(d / "eval4.js"))} {q(str(d / "proj/model.js"))}',
                f':run {q(str(d / "run.js"))} {q(str(d / "proj/run_me.js"))}',
                ':inspect fix',
            ])
            self.assertEqual(p.returncode, 0, p.stderr)
            out = p.stdout + p.stderr
            self.assertIn('mep language UI', out)
            self.assertNotIn('already been declared', out)
            self.assertIn("Cannot read properties of null (reading 'boom')", out)
            self.assertIn('/proj/model.js:7', out)
            self.assertIn('\n> 42\n', '\n' + out)
            variables = slurp('variables.txt')
            self.assertRegex(variables, r'x_from_session\s+number\s+8')
            self.assertRegex(variables, r'double\s+function\s+double\(x\)')
            self.assertRegex(variables, r'five\s+number\s+5')
            self.assertRegex(variables, r'c\s+Circle\s+Circle \{ r: 2 \}')
            self.assertIn('run_me.js', variables)  # where_from: __filename is the source's
            self.assertRegex(slurp('modules.txt'), r'fixpkg\s+1\.2\.3\s+\S*node_modules/fixpkg/lib/main\.js')
            history = slurp('history.txt')
            self.assertIn('const x_from_session = 7', history)
            self.assertIn('// model.js:4-6  function double(x) {', history)
            self.assertIn('Circle', slurp('inspect.txt'))

    def help_cases(self, lsp):
        with tempfile.TemporaryDirectory(prefix='mep-jsui-') as directory:
            make_project(directory)
            d = Path(directory)
            source = json.dumps(str(d / 'proj/model.mjs'))
            cases = [
                (':help fix.add', ['Adds two numbers.', '`a`', 'first term', 'add(a, b = 0)']),
                (':help c.area', ['Area of the circle.', 'area()']),
                (':help fix.Circle', ['A circle with area pi r^2.', 'new Circle(r: number)', 'distance from the centre']),
                (':help fix.Kind.square', ['Shape kinds.', '= 1']),
                (':help fix', ['Exports:', '`Circle`', '`add`']),
                (':help fixpkg', ['Exports:', '`Kind`']),
                (f':help "avg" {source}', ['Arithmetic mean of the values.', '`xs`']),
                (f':help "fx.add" {source}', ['Adds two numbers.']),
                (':help @dm/stats', ['Exports:', '`mean`']),
                (':help path.join', ['join(', 'https://nodejs.org/api/path.html']),
                (':help no.such.name', ['not a name in this session']),
            ]
            for command, expected in cases:
                with self.subTest(command=command, lsp=lsp):
                    p = self.run_console(directory, [
                        "const fix = require('fixpkg')", 'const c = new fix.Circle(2)', command], lsp=lsp)
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertIn(': in the Help tab', p.stdout)
                    text = (d / 'help.txt').read_text()
                    for e in expected:
                        self.assertIn(e, text)

    def test_help_from_declarations_own_reader(self):
        self.help_cases('/nonexistent/mep-js-lsp')

    def test_help_from_declarations_mep_js_lsp(self):
        if not lsp_has_dts_doc(JS_LSP):
            self.skipTest('no mep-js-lsp with --dts-doc (pass --js-lsp BUILD/mep-js-lsp)')
        self.help_cases(JS_LSP)

    def test_transform(self):
        with tempfile.TemporaryDirectory(prefix='mep-jsui-') as directory:
            init = init_file(directory, '')
            script = Path(directory) / 'transform.js'
            script.write_text('''
const repl = require('repl');
repl.start = () => ({eval() {}, setupHistory() {}, displayPrompt() {}, _domain: {prependListener() {}}});
require(process.argv[2]);
const t = __mep.transform;
const cases = [
  ['const a = 1;\\nlet b = 2', 'var a = 1;\\nvar b = 2'],
  ['class K extends Object { m() { return `${1}` } }\\nfoo()', 'var K = class K extends Object { m() { return `${1}` } };\\nfoo()'],
  ['import fs from "fs";', 'var {default: fs} = __mep.named(await __mep.import("fs"));'],
  ["import * as p from 'path'", 'var p = await __mep.import("path");'],
  ['import {a, b as c} from "./x.js"', 'var {a, b: c} = __mep.named(await __mep.import("./x.js"));'],
  ['import "side";', 'await __mep.import("side");'],
  ['export const z = 1\\nexport default function f() {}', 'var z = 1\\nfunction f() {}'],
  ['export default 3 + 4', 'var __default = 3 + 4'],
  ['for (let i = 0; i < 2; i++) { const t = i }', 'for (let i = 0; i < 2; i++) { const t = i }'],
  ['const re = /const x/g; const s = "let y" // const c', 'var re = /const x/g; var s = "let y" // const c'],
  ['import {\\n  a,\\n  b\\n} from "m";\\nx', 'var {a, b} = __mep.named(await __mep.import("m"));\\n\\n\\n\\nx'],
];
for (const [code, want] of cases) {
  const got = t(code);
  if (got !== want) { console.log('MISMATCH', JSON.stringify(code), JSON.stringify(got)); process.exitCode = 1; }
}
console.log('transform checked');
''')
            p = subprocess.run([NODE_EXE, str(script), str(init)], text=True, capture_output=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertIn('transform checked', p.stdout)

    def test_interactive_console(self):
        # The real REPL under a pty: what is typed runs, and the panels refresh after it.
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-jsui-') as directory:
            make_project(directory)
            init = init_file(directory, '')
            p = subprocess.run([script, '-qfec', f'{NODE_EXE} {init}', '/dev/null'],
                               input='const typed_value = 99\nconsole.log(typed_value + 1)\n:help typed_value\n.exit\n',
                               text=True, capture_output=True, timeout=120, cwd=Path(directory) / 'proj',
                               env={'HOME': directory, 'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                                    'TERM': 'xterm-256color', 'NODE_REPL_HISTORY': ''})
            self.assertIn('100', p.stdout)
            self.assertIn('mep language UI', p.stdout)
            self.assertRegex((Path(directory) / 'variables.txt').read_text(), r'typed_value\s+number\s+99')
            self.assertIn('typed_value = 99', (Path(directory) / 'help.txt').read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--node', default=shutil.which('node'))
    parser.add_argument('--js-lsp', default=None, help='a mep-js-lsp with --dts-doc (default: a build of it, else PATH)')
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.node:
        print('lua or node not installed: skipping the JavaScript UI check')
        sys.exit(77)
    LUA_EXE, NODE_EXE = args.lua, args.node
    JS_LSP = args.js_lsp
    if not JS_LSP:
        builds = sorted(ROOT.glob('build/*/mep-js-lsp'), key=lambda p: p.stat().st_mtime, reverse=True)
        candidates = [str(b) for b in builds] + [shutil.which('mep-js-lsp') or '']
        JS_LSP = next((c for c in candidates if lsp_has_dts_doc(c)), '')
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
