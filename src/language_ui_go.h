// Go language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .go
// buffer), kBuiltinLanguageUi's Go consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Fortran mode's
// shape. Included by main.cpp only; BuiltinLanguageUiGo() is the Lua chunk
// (help/go-mode.org is the user's page).
//
// Go has no REPL, so the console is run output plus docs: a Python program
// (console_source below, written into the session directory and started as
// `python3 mep_go_console.py ...`; mep.opt.go_ui_cmd or a `// REPL:` comment
// replaces the python3) that runs the file and shows godoc:
//   - :run (<leader>rr, the Run button, <leader>re): the file's `CMD:`
//     header (main.cpp's mep_run_button_cmd_override) typed in mep's
//     directory, else `go run .` in the file's directory for a package main
//     of several files, `go run FILE` for one, `go build .` for a library.
//     :MepGoRunOnSave (mep.opt.go_ui_run_on_save) re-runs it whenever a .go
//     file of the package is saved. Other typed lines run as shell commands
//     in the package's directory (go test ./..., go vet).
//   - Help: :doc NAME (gh) -- `mep-go-lsp --doc`, which reads doc comments
//     of the package, its imports by name (alias or package clause), the
//     main module, replace directives to local directories, vendor/, the
//     module cache and GOROOT (go_lsp_pkgdoc.h); `go doc NAME` when the
//     server finds nothing.
//   - Exports: the package's exported identifiers (`mep-go-lsp --exports`;
//     every top-level name for a package main) and its imports; clicking a
//     name shows its doc, clicking an import lists that package's exports.
//   - History: runs and commands; clicking one runs it again.
#pragma once
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiGo() {
    static const std::string chunk = std::string(R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.go_ui_cmd = mep.opt.go_ui_cmd or {'python3'}
-- Re-run the file whenever a .go file of its package is saved (:MepGoRunOnSave toggles it).
mep.opt.go_ui_run_on_save = mep.opt.go_ui_run_on_save or false
mep.opt.go_ui_info_share = mep.opt.go_ui_info_share or 0.34
mep.opt.go_ui_bottom_share = mep.opt.go_ui_bottom_share or 0.4
mep.opt.go_ui_bottom_info_share = mep.opt.go_ui_bottom_info_share or 0.5
mep.language_ui_extensions.go = mep.language_ui_extensions.go or 'go'

-- The console (see the header comment), written into each session directory.
local console_source = [===[
)MEPLUA") + R"MEPPY(#!/usr/bin/env python3
# mep's Go console: Go has no REPL, so this is run output plus docs. :run runs
# a file or its package (`go run`, or the file's `CMD:` header), :doc NAME
# writes godoc to the Help tab (mep-go-lsp --doc reads doc comments from the
# workspace, replace dirs, vendor/, the module cache and GOROOT; `go doc` is
# the fallback), :exports lists a package's exported identifiers for the
# Exports tab. Any other line runs as a shell command in the package's
# directory (go test ./..., go vet). The panels are files in the session
# directory that mep's Go UI mode re-reads (language_ui_go.h).
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys

HELP = '''Go console -- run output and docs (Go has no REPL).
  :run FILE         run the file's CMD: header, else `go run` of its package (or the file)
  :test [ARGS]      go test in the package's directory
  :doc NAME [FILE]  godoc of NAME in the Help tab (pkg.Name, pkg.Type.Method, Name, an import path)
  :godoc NAME       `go doc NAME` itself
  :exports [PKG]    the package's exported identifiers in the Exports tab (a directory or import path)
  :cd DIR           the directory commands run in (the source file's package by default)
  :quit             leave
  anything else     a shell command, run in that directory (go vet, go test ./..., ls)
'''


def esc(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')


class Console:
    def __init__(self, args):
        self.args = args
        self.dir = os.path.abspath(args.dir)
        self.cwd = os.path.abspath(args.src_dir or os.getcwd())
        self.lsp = args.lsp
        self.history = []
        self.go = shutil.which('go')

    def write_file(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path + '.tmp', 'w') as f:
            f.write(text)
        os.replace(path + '.tmp', path)

    def record(self, text):
        self.history.append(text)
        self.history = self.history[-200:]
        self.write_file('history.txt', ''.join(esc(h) + '\n' for h in self.history))

    # -- running -------------------------------------------------------------------
    def shell(self, cmd, cwd):
        print('$ ' + cmd)
        sys.stdout.flush()
        try:
            p = subprocess.run(['sh', '-c', cmd], cwd=cwd)
        except KeyboardInterrupt:
            print('\n[interrupted]')
            return 130
        if p.returncode != 0:
            print('[exit status %d]' % p.returncode)
        sys.stdout.flush()
        return p.returncode

    def run(self, path):
        """The file's CMD: header (typed as the Run button would, in mep's directory),
        else `go run .` for a package main of several files, else `go run FILE`."""
        self.record(':run ' + path)
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                lines = f.read().split('\n')
        except OSError as e:
            print('cannot read %s: %s' % (path, e))
            return
        cmd = find_cmd_header(lines)
        if cmd:
            self.shell(cmd, os.getcwd())
            return
        pkg_dir = os.path.dirname(os.path.abspath(path))
        pkg = package_name(lines)
        if not self.go:
            print('go is not on PATH: cannot run %s' % os.path.basename(path))
            return
        if pkg != 'main':
            # Not a program: build it (its errors show), and say how to test it.
            self.shell('go build .', pkg_dir)
            print('# package %s is not a program: :test runs its tests' % (pkg or '?'))
            return
        siblings = [n for n in os.listdir(pkg_dir) if n.endswith('.go') and not n.endswith('_test.go')
                    and n != os.path.basename(path)
                    and package_name(read_head(os.path.join(pkg_dir, n))) == 'main']
        target = '.' if siblings else shlex.quote(os.path.basename(path))
        self.shell('go run ' + target, pkg_dir)

    # -- docs ----------------------------------------------------------------------
    def lsp_doc(self, name, cwd, file):
        if not self.lsp:
            return None
        argv = [self.lsp, '--doc', name, '--dir', cwd]
        if file:
            argv += ['--file', file]
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            self.lsp = ''
            return None
        if p.returncode == 0 and p.stdout.strip():
            return p.stdout.strip()
        if p.returncode > 1:
            self.lsp = ''  # not a --doc capable build: stop asking
        return None

    def go_doc(self, name, cwd):
        if not self.go:
            return None
        try:
            p = subprocess.run([self.go, 'doc', name], capture_output=True, text=True, timeout=60, cwd=cwd)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if p.returncode == 0 and p.stdout.strip():
            return '```go\n' + p.stdout.rstrip() + '\n```\n\n(go doc %s)' % name
        return None

    def doc(self, name, file=None, force_go=False):
        cwd = os.path.dirname(os.path.abspath(file)) if file else self.cwd
        name = name.strip().strip('.()')
        text = None
        if not force_go:
            text = self.lsp_doc(name, cwd, file)
        if text is None:
            text = self.go_doc(name, cwd)
        if text is None:
            text = ('%s: no documentation found -- not in this package, its imports (workspace, replace '
                    'directories, vendor/, the module cache) or the standard library%s.\n'
                    % (name, '' if self.go else '; go is not on PATH, so `go doc` was not asked'))
        self.write_file('help.txt', text + ('\n' if not text.endswith('\n') else ''))
        print('# %s: in the Help tab' % name)

    def exports(self, pkg=None):
        """The Exports tab: the package's exported identifiers (all of them for a
        package main), and the imports of the source package."""
        target = pkg or self.cwd
        rows = []
        title = target
        if self.lsp:
            argv = [self.lsp, '--exports', target, '--dir', self.cwd]
            if not pkg and dir_package(self.cwd) == 'main':
                argv.append('--all')  # a program exports nothing: list its top-level names
            try:
                p = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                if p.returncode == 0:
                    rows = [l for l in p.stdout.split('\n') if l]
            except (OSError, subprocess.TimeoutExpired):
                pass
        if not rows and self.go:
            # go doc -short: the exported names, one line each.
            try:
                p = subprocess.run([self.go, 'doc', '-short', pkg or '.'], capture_output=True, text=True,
                                   timeout=60, cwd=self.cwd)
                for l in p.stdout.split('\n'):
                    l = l.strip()
                    m = re.match(r'^(func|type|const|var)\s+(?:\([^)]*\)\s*)?(\w+)', l)
                    if m:
                        rows.append('%s\t%s\t%s\t' % (m.group(1), m.group(2), l))
            except (OSError, subprocess.TimeoutExpired):
                pass
        imports = []
        if not pkg:
            for n in sorted(os.listdir(self.cwd)) if os.path.isdir(self.cwd) else []:
                if n.endswith('.go') and not n.endswith('_test.go'):
                    for path in file_imports(read_text(os.path.join(self.cwd, n))):
                        if path not in imports:
                            imports.append(path)
        self.write_file('exports.txt', '@package\t%s\n' % title + ''.join(r + '\n' for r in rows)
                        + ''.join('@import\t%s\n' % i for i in imports))
        print('# %s: %d exported identifiers in the Exports tab' % (os.path.basename(title) or title, len(rows)))


def read_text(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def read_head(path):
    return read_text(path).split('\n')[:200]


def package_name(lines):
    in_comment = False
    for l in lines:
        s = l.strip()
        if in_comment:
            if '*/' in s:
                in_comment = False
                s = s.split('*/', 1)[1].strip()
            else:
                continue
        if s.startswith('/*'):
            in_comment = '*/' not in s
            continue
        m = re.match(r'^package\s+(\w+)', s)
        if m:
            return m.group(1)
    return None


def dir_package(d):
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if n.endswith('.go') and not n.endswith('_test.go'):
            name = package_name(read_head(os.path.join(d, n)))
            if name:
                return name
    return None


def file_imports(text):
    out = []
    for m in re.finditer(r'^import\s*\(([^)]*)\)', text, re.M | re.S):
        out += re.findall(r'"([^"]+)"', m.group(1))
    out += re.findall(r'^import\s+(?:\w+\s+|\.\s+|_\s+)?"([^"]+)"', text, re.M)
    return out


def find_cmd_header(lines):
    """The Run button's `CMD:` comment (main.cpp's mep_run_button_cmd_override), or None."""
    for line in lines:
        m = re.match(r'^\s*[^\w\s]+\s*[Cc][Mm][Dd]:\s*(.*?)\s*$', line)
        if m:
            cmd = re.sub(r'\s*\*/$', '', m.group(1))
            cmd = re.sub(r'\s*-->$', '', cmd)
            if cmd:
                return cmd
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--src-dir', default='')
    ap.add_argument('--lsp', default='')
    args = ap.parse_args()
    c = Console(args)
    for n in ('help.txt', 'exports.txt', 'history.txt'):
        c.write_file(n, '')
    try:
        import readline  # noqa: F401  line editing and history at the prompt
    except ImportError:
        pass
    ver = 'go not found on PATH'
    if c.go:
        p = subprocess.run([c.go, 'version'], capture_output=True, text=True)
        ver = p.stdout.strip() or ver
    print('%s -- mep Go console: <leader>rr runs the file or package, gh / :doc NAME shows godoc, '
          ':help lists commands; other lines run as shell commands.' % ver)
    c.exports()
    sys.stdout.flush()
    interactive = sys.stdin.isatty()
    while True:
        try:
            line = input('go> ' if interactive else '')
        except EOFError:
            break
        except KeyboardInterrupt:
            print()
            continue
        s = line.strip()
        try:
            if not s:
                continue
            if s.startswith(':'):
                cmd, _, rest = s[1:].partition(' ')
                rest = rest.strip()
                if cmd in ('quit', 'q', 'exit'):
                    break
                elif cmd == 'help':
                    sys.stdout.write(HELP)
                elif cmd == 'run':
                    path, _, name = rest.partition(' ')
                    c.run(path)
                elif cmd == 'test':
                    c.record(':test ' + rest)
                    c.shell('go test' + (' ' + rest if rest else ''), c.cwd)
                elif cmd == 'doc':
                    name, _, file = rest.partition(' ')
                    c.doc(name, file or None)
                elif cmd == 'godoc':
                    c.doc(rest, None, force_go=True)
                elif cmd == 'exports':
                    c.exports(rest or None)
                elif cmd == 'cd':
                    d = os.path.abspath(os.path.join(c.cwd, rest)) if rest else c.cwd
                    if os.path.isdir(d):
                        c.cwd = d
                        c.exports()
                    print('# commands run in ' + c.cwd)
                else:
)MEPPY" + R"MEPPY(                    print('unknown command :%s (:help lists them)' % cmd)
                continue
            c.record(s)
            c.shell(s, c.cwd)
        except KeyboardInterrupt:
            print('\n# interrupted')
        except Exception as e:  # keep the console alive
            print('mep console error: %s' % e)
        sys.stdout.flush()


if __name__ == '__main__':
    main()
)MEPPY" + R"MEPLUA(]===]

-- tab id -> session state; the sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state, sidebars = {}, nil

local function decode(text)
  return (text:gsub('\\([nrt\\])', function(c) return ({n = '\n', r = '\r', t = '\t', ['\\'] = '\\'})[c] end))
end

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    exports = mep.sidebar_create('Exports', 'right', 50),
    history = mep.sidebar_create('History', 'right', 50),
    help = mep.sidebar_create('Help', 'right', 50),
  }
end

local function send(st, text)
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local info = mep.terminal_info and mep.terminal_info(st.console_buf)
  if info and info.exited then
    mep.notify('Go console exited; :MepGoRestart starts a fresh one', 'warn')
    return false
  end
  mep.terminal_write(st.console_buf, text .. '\n')
  return true
end

local function show_help(st, text)
  send(st, text)
  if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local title, rows, imports = nil, {}, {}
  for line in ((st and st.exports_text) or ''):gmatch('[^\n]+') do
    local tag, value = line:match('^@(%w+)\t(.*)$')
    if tag == 'package' then
      title = value
    elseif tag == 'import' then
      imports[#imports + 1] = {id = 'i' .. value, text = value, tooltip = 'its exports and package doc',
        on_click = function()
          send(st, ':exports ' .. value)
          show_help(st, ':doc ' .. value)
        end}
    else
      local kind, name, sig, where = line:match('^([^\t]*)\t([^\t]*)\t([^\t]*)\t?(.*)$')
      if kind then
        local indent = (kind == 'method' or kind == 'field') and '  ' or ''
        rows[#rows + 1] = {id = 'e' .. kind .. name, text = indent .. sig, tooltip = where ~= '' and where or nil,
          on_click = function()
            local local_pkg = title and title:sub(1, 1) == '/'
            if local_pkg then show_help(st, ':doc ' .. name .. ' ' .. st.src_abs)
            else show_help(st, ':doc ' .. (title or '') .. '.' .. name) end
          end}
      end
    end
  end
  local sections = {}
  local local_pkg = title == nil or title:sub(1, 1) == '/'
  if not local_pkg then
    table.insert(rows, 1, {id = 'back', text = '<- back to this package', on_click = function() send(st, ':exports') end})
  end
  if #rows == 0 then rows[1] = {id = 'empty', text = title and '(no exported identifiers)' or '(the package\'s exported identifiers appear here)'} end
  local label = title and (local_pkg and (title:match('[^/]+$') or title) or title) or ''
  sections[1] = {id = 'exports', title = 'Package ' .. label, collapsed = false, widgets = rows}
  if #imports > 0 then sections[2] = {id = 'imports', title = 'Imports', collapsed = false, widgets = imports} end
  mep.sidebar_set_sections(sidebars.exports, sections)
  local widgets, history = {}, {}
  for line in ((st and st.history_text) or ''):gmatch('[^\n]+') do history[#history + 1] = decode(line) end
  for i = #history, math.max(1, #history - 199), -1 do
    local cmd = history[i]
    widgets[#widgets + 1] = {id = 'h' .. i, text = cmd, wrap = true, on_click = function() send(st, cmd) end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(runs and commands appear here; clicking one runs it again)'} end
  mep.sidebar_set_sections(sidebars.history, {{id = 'history', title = '', collapsed = false, widgets = widgets}})
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(gh over a name, or :doc NAME in the console: godoc from the workspace, the module\'s dependencies or the standard library)')
end

-- Saves the source and runs it: its CMD: header, else `go run` of the package.
local function run(st)
  mep.cmd('write')
  st.save_epoch = mep.buffer_save_epoch and mep.buffer_save_epoch() or st.save_epoch
  send(st, ':run ' .. st.src_abs)
end

function mep.go_ui_open()
  local fname = mep.filename()
  local src_abs = mep_lsp_abspath(fname)
  local dir = mep.language_ui_session_dir('goui')
  local paths = {exports = dir .. '/exports.txt', history = dir .. '/history.txt', help = dir .. '/help.txt'}
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local script = dir .. '/mep_go_console.py'
  mep.language_ui_write_file(script, console_source)
  local src_dir = src_abs:match('^(.*)/[^/]*$') or '.'
  local argv = mep.language_ui_cmd(mep.opt.go_ui_cmd)
  for _, a in ipairs({script, '--dir', dir, '--src-dir', src_dir,
      '--lsp', mep.bundled_tool and mep.bundled_tool('mep-go-lsp') or 'mep-go-lsp'}) do
    argv[#argv + 1] = a
  end
  local src_buf = mep.current_buffer()
  local layout = mep.language_ui_layout({
    info_share = mep.opt.go_ui_info_share, bottom_share = mep.opt.go_ui_bottom_share,
    bottom_info_share = mep.opt.go_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'go') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, src_dir = src_dir, argv = argv,
    run_on_save = mep.opt.go_ui_run_on_save == true,
    save_epoch = mep.buffer_save_epoch and mep.buffer_save_epoch() or 0,
    exports_text = '', history_text = '', help_text = '',
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.exports, sidebars.history})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Go language UI: run output below (Go has no REPL); Exports/History top-right, Help bottom-right; <leader>rr runs the file or package, gh shows godoc'
    .. (st.run_on_save and '; re-runs on save' or ''))
  return {
    run_source = function() run(st) end,
    prefer_session_run = true,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Go language UI mode closed')
    end,
  }
end
mep.language_ui_modes.go = {open = mep.go_ui_open}

-- <leader>re: Go has no REPL, so it runs the file or package too.
function mep.go_ui_eval()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Go console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  run(st)
end
mep.language_ui_eval_handlers.go = mep.go_ui_eval

-- gh: godoc for the name under the cursor (pkg.Name, Type.Method) -- the
-- server's reading of the workspace and dependencies, else `go doc`.
function mep.go_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_%.]')
  if sym then sym = sym:gsub('^[%.]+', ''):gsub('[%.]+$', '') end
  if not sym or sym == '' or not sym:match('^[%a_]') then mep.notify('gh: no Go name under the cursor', 'warn') return true end
  local from_source = mep.current_buffer() == st.src_buf
  send(st, ':doc ' .. sym .. (from_source and (' ' .. st.src_abs) or ''))
  if from_source then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('go', mep.go_ui_help_at_cursor)

-- :MepGoRunOnSave toggles re-running the file whenever a .go file of its
-- package is saved (mep.opt.go_ui_run_on_save sets the default).
function mep.go_ui_toggle_run_on_save()
  local st = state[mep.current_tab_id()]
  if not st then mep.notify('No Go language UI in this tab', 'warn') return end
  st.run_on_save = not st.run_on_save
  mep.notify('Go: re-run on save ' .. (st.run_on_save and 'on' or 'off'))
end
mep.command('MepGoRunOnSave', mep.go_ui_toggle_run_on_save)

mep.command('MepGoRestart', function()
  if not state[mep.current_tab_id()] then return end
  mep.language_ui_toggle(); mep.language_ui_toggle()
end)

-- This tab's session as the panels see it (nil when no Go mode is open).
function mep.go_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, run_on_save = st.run_on_save,
    exports = st.exports_text, history = st.history_text, help = st.help_text}
end

-- A save of one of the package's .go files: the Exports tab refreshed, and
-- the program re-run when run-on-save is on.
local function on_save(st)
  local f = mep.filename and mep.filename() or ''
  local abs = f ~= '' and mep_lsp_abspath(f) or ''
  if not abs:match('%.go$') or (abs:match('^(.*)/[^/]*$') or '.') ~= st.src_dir then return end
  if st.run_on_save then send(st, ':run ' .. st.src_abs) else send(st, ':exports') end
end

-- Poll loop: re-reads the panel files on a timer, only while a Go mode is
-- open on the active tab; also notices saves.
function mep.go_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  if mep.buffer_save_epoch then
    local epoch = mep.buffer_save_epoch()
    if epoch ~= st.save_epoch then
      st.save_epoch = epoch
      on_save(st)
    end
  end
  local changed, help_changed = false, false
  for _, key in ipairs({'exports', 'history', 'help'}) do
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[key .. '_text'] then
      st[key .. '_text'] = text
      changed = true
      if key == 'help' then help_changed = true end
    end
  end
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.go_ui_poll(st)
  end)
end
)MEPLUA";
    return chunk.c_str();
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
