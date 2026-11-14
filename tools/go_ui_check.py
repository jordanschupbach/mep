#!/usr/bin/env python3
"""Check the Go language UI mode (src/language_ui_go.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the console argv (a REPL: override), what
    <leader>rr / <leader>re / gh type into the console, re-run on save, and
    the Exports/History/Help poll;
  - the console side: the console program the mode writes, run under a real
    python3 with go, offline (GOFLAGS=-mod=mod, GOPROXY=off, a temporary
    GOCACHE and GOPATH), on a module whose dependency is a local `replace`
    directory and another in the module cache -- :run of a package main, of
    a single file, of a CMD: header and of a library; :doc from the
    dependency's doc comments (mep-go-lsp --doc) and from the standard
    library (`go doc`); :exports; a shell command;
  - the interactive console under a pty (script(1)): its prompt.
Needs lua and python3; the console parts need go on PATH (skipped without
it); --lsp names a built mep-go-lsp for the workspace docs and exports
(skipped without one). GUI coverage is manual.
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
HEADER = (ROOT / 'src/language_ui_go.h').read_text()
# The chunk is a concatenation of raw string pieces (MSVC caps each literal's length).
CHUNK = ''.join(m.group(2) for m in re.finditer(r'R"(MEPLUA|MEPPY)\((.*?)\)\1"', HEADER, re.S))
CONSOLE = CHUNK.split('local console_source = [===[\n', 1)[1].split(']===]', 1)[0]

STUBS = r"""
local files, sections, sent, notes, cmds = {}, {}, {}, {}, {}
local buffer, tab, epoch = 1, 1, 0
local fname = '/proj/cmd/app/main.go'
mep = {opt = {language_ui_poll_interval = 1}, language_ui_extensions = {}, language_ui_modes = {},
  language_ui_eval_handlers = {}}
local providers, commands = {}, {}
function mep.help_register_provider(name, fn) providers[name] = fn end
function mep.current_tab_id() return tab end
function mep.current_buffer() return buffer end
function mep.is_terminal_buffer(b) return b == 2 end
function mep.terminal_info() return {exited = false} end
function mep.terminal_write(_, text) sent[#sent + 1] = text end
function mep.notify(msg) notes[#notes + 1] = msg end
function mep.cmd(c) cmds[#cmds + 1] = c if c == 'write' then epoch = epoch + 1 end end
function mep.command(name, fn) commands[name] = fn end
function mep.buffer_save_epoch() return epoch end
function mep.language_ui_session_dir(s) return '/tmp/session_' .. s end
function mep.language_ui_write_file(path, text) files[path] = text end
function mep.language_ui_read_file(path) return files[path] end
function mep.language_ui_cmd(default)
  if mep.language_ui_repl_override then return {'sh', '-c', mep.language_ui_repl_override .. ' "$@"', 'mep-repl'} end
  local a = {} for _, x in ipairs(default) do a[#a + 1] = x end return a
end
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
function mep.filename() return fname end
function mep_lsp_abspath(p) return p end
local help_sym = 'dm.NewFrame'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.go == 'go')
assert(mep.language_ui_modes.go and mep.language_ui_eval_handlers.go and providers.go)

-- Opening: the written script under python3, the file's directory, the server.
local handle = mep.go_ui_open()
local argv = table.concat(mep.started, ' ')
assert(argv == 'python3 /tmp/session_goui/mep_go_console.py --dir /tmp/session_goui --src-dir /proj/cmd/app '
  .. '--lsp /opt/mep/bin/mep-go-lsp', argv)
assert(files['/tmp/session_goui/mep_go_console.py']:find('^#!/usr/bin/env python3\n'))
assert(sections.Exports and sections.History and sections.Help)
assert(handle.prefer_session_run == true and handle.run_source)

-- <leader>rr and <leader>re: saved, then :run of the file (its own save does not re-run it).
handle.run_source('/proj/cmd/app/main.go')
assert(cmds[#cmds] == 'write' and sent[#sent] == ':run /proj/cmd/app/main.go\n', sent[#sent])
local count = #sent
mep.go_ui_poll()
assert(#sent == count, 'the run\'s own save is not a save to react to')
mep.language_ui_eval_handlers.go()
assert(sent[#sent] == ':run /proj/cmd/app/main.go\n')

-- A save of a .go file of the package: the Exports refreshed; with run-on-save, a run.
epoch = epoch + 1
mep.go_ui_poll()
assert(sent[#sent] == ':exports\n', sent[#sent])
commands.MepGoRunOnSave()
assert(mep.go_ui_info().run_on_save == true and notes[#notes]:find('on'))
epoch = epoch + 1
mep.go_ui_poll()
assert(sent[#sent] == ':run /proj/cmd/app/main.go\n', sent[#sent])
-- A save elsewhere: nothing.
fname = '/elsewhere/notes.txt'
count = #sent
epoch = epoch + 1
mep.go_ui_poll()
assert(#sent == count)
fname = '/proj/cmd/app/main.go'

-- gh: :doc with the file, Help revealed.
assert(providers.go() == true)
assert(sent[#sent] == ':doc dm.NewFrame /proj/cmd/app/main.go\n', sent[#sent])
assert(mep.revealed == 101)

-- The poll: the Exports table, imports, history, help.
files['/tmp/session_goui/exports.txt'] = '@package\t/proj/cmd/app\n'
  .. 'type\tFrame\ttype Frame struct { ... }\t/proj/cmd/app/frame.go:3\n'
  .. 'method\tFrame.Rows\tfunc (f *Frame) Rows() int\t/proj/cmd/app/frame.go:9\n'
  .. '@import\texample.com/datamunge\n'
files['/tmp/session_goui/history.txt'] = ':run /proj/cmd/app/main.go\ngo test ./...\n'
files['/tmp/session_goui/help.txt'] = 'NewFrame docs\n'
mep.revealed = nil
mep.go_ui_poll()
local secs = sections.Exports
assert(secs[1].title == 'Package app', secs[1].title)
assert(secs[1].widgets[1].text == 'type Frame struct { ... }' and secs[1].widgets[2].text == '  func (f *Frame) Rows() int')
assert(secs[2].title == 'Imports' and secs[2].widgets[1].text == 'example.com/datamunge')
assert(mep.revealed == 101 and sections.Help == 'NewFrame docs\n')
secs[1].widgets[2].on_click()
assert(sent[#sent] == ':doc Frame.Rows /proj/cmd/app/main.go\n', sent[#sent])
secs[2].widgets[1].on_click()
assert(sent[#sent - 1] == ':exports example.com/datamunge\n' and sent[#sent] == ':doc example.com/datamunge\n')
-- An imported package's exports: its names are asked for by import path, and a way back.
files['/tmp/session_goui/exports.txt'] = '@package\texample.com/datamunge\nfunc\tNewFrame\tfunc NewFrame(n int) *Frame\t\n'
mep.go_ui_poll()
secs = sections.Exports
assert(secs[1].title == 'Package example.com/datamunge' and secs[1].widgets[1].text == '<- back to this package')
secs[1].widgets[2].on_click()
assert(sent[#sent] == ':doc example.com/datamunge.NewFrame\n', sent[#sent])
secs[1].widgets[1].on_click()
assert(sent[#sent] == ':exports\n')
local hist = sections.History[1].widgets
assert(hist[1].text == 'go test ./...' and hist[2].text == ':run /proj/cmd/app/main.go')
hist[1].on_click()
assert(sent[#sent] == 'go test ./...\n')

-- A REPL: override replaces only the python3.
mep.language_ui_repl_override = 'nix shell nixpkgs#python3 --command python3'
mep.go_ui_open()
assert(mep.started[1] == 'sh' and mep.started[5]:match('mep_go_console.py$'), table.concat(mep.started, ' '))
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
count = #sent
mep.language_ui_eval_handlers.go()
assert(#sent == count and notes[#notes]:find('No Go console'))
assert(providers.go() == false)
print('editor side passed')
'''

GO_MOD = '''\
module example.com/app

go 1.21

require (
\texample.com/datamunge v0.0.0
\texample.com/Ext v1.2.0
)

replace example.com/datamunge => ../datamunge
'''

MAIN = '''\
package main

import (
\t"fmt"

\tdm "example.com/datamunge"
\t"example.com/Ext/sub"
)

func main() {
\tf := dm.NewFrame(3)
\tfmt.Println("rows", f.Rows(), "answer", sub.Answer, "twice", twice(21))
}
'''

HELPER = '''\
package main

// twice doubles x.
func twice(x int) int { return 2 * x }
'''

FRAME = '''\
// Package datamunge wraps the datamunge C++ library.
package datamunge

// Frame is a table of columns.
type Frame struct {
\trows int
}

// NewFrame makes a frame of n rows.
func NewFrame(n int) *Frame { return &Frame{rows: n} }

// Rows returns the number of rows.
func (f *Frame) Rows() int { return f.rows }
'''

SUB = '''\
// Package sub is a dependency from the module cache.
package sub

// Answer is the answer.
const Answer = 42
'''


class GoUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    def project(self, root):
        root = Path(root)
        app = root / 'app'
        app.mkdir()
        (app / 'go.mod').write_text(GO_MOD)
        (app / 'main.go').write_text(MAIN)
        (app / 'helper.go').write_text(HELPER)
        dm = root / 'datamunge'
        dm.mkdir()
        (dm / 'go.mod').write_text('module example.com/datamunge\n\ngo 1.21\n')
        (dm / 'frame.go').write_text(FRAME)
        # example.com/Ext in the module cache, as `go mod download` leaves it (offline: GOFLAGS=-mod=mod
        # with the cache's .info/.mod/.zip and the extracted tree, GONOSUMDB/GOFLAGS so no checksum is fetched).
        cache = root / 'gopath/pkg/mod'
        ext = cache / 'example.com/!ext@v1.2.0'
        (ext / 'sub').mkdir(parents=True)
        (ext / 'go.mod').write_text('module example.com/Ext\n\ngo 1.21\n')
        (ext / 'sub/sub.go').write_text(SUB)
        dl = cache / 'cache/download/example.com/!ext/@v'
        dl.mkdir(parents=True)
        (dl / 'v1.2.0.mod').write_text('module example.com/Ext\n\ngo 1.21\n')
        (dl / 'v1.2.0.info').write_text('{"Version":"v1.2.0"}\n')
        import zipfile
        with zipfile.ZipFile(dl / 'v1.2.0.zip', 'w') as z:
            z.writestr('example.com/!ext@v1.2.0/go.mod', 'module example.com/Ext\n\ngo 1.21\n')
            z.writestr('example.com/!ext@v1.2.0/sub/sub.go', SUB)
        (dl / 'list').write_text('v1.2.0\n')
        return app

    def env(self, root):
        root = Path(root)
        return dict(os.environ, GOFLAGS='-mod=mod', GOPROXY='off', GOSUMDB='off', GONOSUMCHECK='1',
                    GONOSUMDB='*', GOPRIVATE='*', GOTOOLCHAIN='local', GOCACHE=str(root / 'gocache'),
                    GOPATH=str(root / 'gopath'), GOMODCACHE=str(root / 'gopath/pkg/mod'), HOME=str(root))

    def console(self, root, cwd, inputs, src_dir):
        session = Path(root) / 'session'
        session.mkdir(exist_ok=True)
        script = session / 'mep_go_console.py'
        script.write_text(CONSOLE)
        args = [PYTHON_EXE, str(script), '--dir', str(session), '--src-dir', str(src_dir)]
        if LSP_EXE:
            args += ['--lsp', LSP_EXE]
        p = subprocess.run(args, input=''.join(line + '\n' for line in inputs), text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=600, cwd=cwd, env=self.env(root))
        self.assertEqual(p.returncode, 0, p.stdout)
        return p.stdout, session

    def test_console_run(self):
        if not GO_EXE:
            self.skipTest('go not installed')
        with tempfile.TemporaryDirectory(prefix='mep-goui-') as root:
            app = self.project(root)
            (app / 'solo').mkdir()
            (app / 'solo/one.go').write_text('package main\n\nimport "fmt"\n\nfunc main() { fmt.Println("solo", 7*6) }\n')
            (Path(root) / 'withcmd').mkdir()
            (Path(root) / 'withcmd/x.go').write_text('// CMD: echo built by cmd $((40 + 2))\npackage main\n\nfunc main() {}\n')
            (Path(root) / 'broken').mkdir()
            (Path(root) / 'broken/go.mod').write_text('module example.com/broken\n\ngo 1.21\n')
            (Path(root) / 'broken/b.go').write_text('package main\n\nfunc main() {\n\tundefined()\n}\n')
            out, session = self.console(root, root, [
                ':run %s' % (app / 'main.go'),             # go run . (main.go + helper.go)
                ':run %s' % (app / 'solo/one.go'),         # go run one.go
                ':run %s' % (Path(root) / 'withcmd/x.go'),  # the CMD: header, in the console's directory
                ':run %s' % (Path(root) / 'broken/b.go'),
                ':run %s' % (Path(root) / 'datamunge/frame.go'),  # a library: built
                'echo shell $((6 * 7))',
            ], src_dir=app)
            self.assertRegex(out, r'\$ go run \.\n(go: downloading .*\n)*rows 3 answer 42 twice 42\n')
            self.assertIn('$ go run one.go\nsolo 42\n', out)
            self.assertIn('$ echo built by cmd $((40 + 2))\nbuilt by cmd 42\n', out)
            self.assertIn('undefined: undefined', out)
            self.assertIn('[exit status 1]', out)
            self.assertIn('# package datamunge is not a program: :test runs its tests', out)
            self.assertIn('$ echo shell $((6 * 7))\nshell 42\n', out)
            history = (session / 'history.txt').read_text()
            self.assertIn(':run %s\n' % (app / 'main.go'), history)
            self.assertIn('echo shell $((6 * 7))\n', history)

    def test_console_docs_and_exports(self):
        with tempfile.TemporaryDirectory(prefix='mep-goui-') as root:
            app = self.project(root)
            main = str(app / 'main.go')
            pages = {}
            exports = None
            for name in ['dm.NewFrame', 'dm.Frame.Rows', 'sub.Answer', 'twice', 'strings.ToUpper', 'nothingLikeThis']:
                out, session = self.console(root, root, [':doc %s %s' % (name, main)], src_dir=app)
                self.assertIn('# %s: in the Help tab' % name, out)
                pages[name] = (session / 'help.txt').read_text()
                if exports is None:
                    exports = (session / 'exports.txt').read_text()
            self.assertIn('nothingLikeThis: no documentation found', pages['nothingLikeThis'])
            if LSP_EXE:
                self.assertIn('```go\nfunc NewFrame(n int) *Frame\n```', pages['dm.NewFrame'])
                self.assertIn('NewFrame makes a frame of n rows.', pages['dm.NewFrame'])
                self.assertIn('Rows returns the number of rows.', pages['dm.Frame.Rows'])
                self.assertIn('Answer is the answer.', pages['sub.Answer'])
                self.assertIn('twice doubles x.', pages['twice'])
                # A package main exports nothing: its top-level names, and its imports.
                self.assertTrue(exports.startswith('@package\t%s\n' % app), exports)
                self.assertIn('func\ttwice\tfunc twice(x int) int\t', exports)
                self.assertIn('@import\texample.com/datamunge\n', exports)
                out, session = self.console(root, root, [':exports example.com/datamunge'], src_dir=app)
                table = (session / 'exports.txt').read_text()
                self.assertIn('@package\texample.com/datamunge\n', table)
                self.assertIn('func\tNewFrame\tfunc NewFrame(n int) *Frame\t', table)
                self.assertIn('method\tFrame.Rows\tfunc (f *Frame) Rows() int\t', table)
            if GO_EXE:
                self.assertIn('ToUpper', pages['strings.ToUpper'])
                out, session = self.console(root, root, [':godoc strings.ToLower'], src_dir=app)
                self.assertIn('(go doc strings.ToLower)', (session / 'help.txt').read_text())

    def test_interactive_console(self):
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        with tempfile.TemporaryDirectory(prefix='mep-goui-') as root:
            session = Path(root) / 'session'
            session.mkdir()
            (session / 'mep_go_console.py').write_text(CONSOLE)
            cmd = '%s %s --dir %s --src-dir %s' % (PYTHON_EXE, session / 'mep_go_console.py', session, root)
            p = subprocess.run([script, '-qfec', cmd, '/dev/null'], input='echo hi $((1 + 1))\n:quit\n',
                               text=True, capture_output=True, timeout=120, env=dict(self.env(root), TERM='dumb'))
            self.assertIn('go> ', p.stdout)
            self.assertIn('hi 2', p.stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--python', default=sys.executable)
    parser.add_argument('--go', default=shutil.which('go'))
    lsp_default = next((str(p) for p in [ROOT / 'build/native/mep-go-lsp', ROOT / 'build/native-dev/mep-go-lsp']
                        if p.exists()), shutil.which('mep-go-lsp'))
    parser.add_argument('--lsp', default=lsp_default)
    args, remaining = parser.parse_known_args()
    if not args.lua:
        print('lua not installed: skipping the Go UI check')
        sys.exit(77)
    LUA_EXE, PYTHON_EXE, GO_EXE = args.lua, args.python, args.go
    if not GO_EXE:
        print('go not installed: the console checks that run go are skipped')
    LSP_EXE = os.path.abspath(args.lsp) if args.lsp else None
    if LSP_EXE and subprocess.run([LSP_EXE, '--doc', 'x'], capture_output=True).returncode > 1:
        print('%s has no --doc (an older build): skipping the workspace doc checks' % LSP_EXE)
        LSP_EXE = None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
