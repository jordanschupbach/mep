#!/usr/bin/env python3
"""Drive the real mep Lua UI over its agent socket, on a private X11 display.

Opens the mode on a .lua file, runs statements with <leader>re's handler
(a multi-line function and top-level locals included), runs the whole file
through the Run path (which requires a module found only through LUA_PATH,
so the console must inherit mep's environment), looks a function up with
gh, and tears the mode down.
"""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time

from fold_live_check import Mep


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build/native')
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    if not args.lua:
        parser.error('lua must be on PATH, or pass --lua')
    home = tempfile.mkdtemp(prefix='mepluaui-')
    editor = None
    server = None
    read_fd, write_fd = os.pipe()
    try:
        server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                   '1600x1200x24', '-nolisten', 'tcp'], pass_fds=(write_fd,),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(write_fd)
        write_fd = None
        if not select.select([read_fd], [], [], 10)[0]:
            raise RuntimeError('Xvfb did not start')
        number = os.read(read_fd, 64).decode().strip()
        if not number.isdigit():
            raise RuntimeError('Xvfb could not allocate a display')
        libdir = Path(home) / 'lualib'
        libdir.mkdir()
        (libdir / 'mepuitestmod.lua').write_text(
            'local M = {}\n--- Adds two numbers.\nfunction M.add(a, b)\n  return a + b\nend\nreturn M\n')
        os.environ['LUA_PATH'] = str(libdir) + '/?.lua;;'
        source = Path(home) / 'example.lua'
        source.write_text('local base = 40\n'
                          '\n'
                          '--- Doubles x.\n'
                          'local function double(x)\n'
                          '  local y = x * 2\n'
                          '  return y\n'
                          'end\n'
                          '\n'
                          'local total = double(base / 2) +\n'
                          '  2\n'
                          'local mod = require("mepuitestmod")\n'
                          'sum = mod.add(total, 1)\n')
        editor = Mep(args.build_dir, home, ':' + number, [str(source)], cwd=str(source.parent))
        editor.cmd('lua mep.opt.lua_ui_cmd={' + json.dumps(str(Path(args.lua).resolve())) +
                   '}; mep.opt.language_ui_poll_interval=0.05')
        editor.cmd('lua mep.language_ui_toggle()')

        def info():
            value = editor.lua_value('(function() local i=mep.lua_ui_info(); if not i then return "null" end return mep_ai_json_encode(i) end)()')
            if value and value.startswith('ERR:'):
                raise RuntimeError(value)
            return json.loads(value) if value else None

        def wait(predicate, label, timeout=30):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                value = info()
                if value is not None and predicate(value):
                    return value
                editor.pump(2)
            raise AssertionError(label + '\nstate=' + str(info()) + '\nlog=' + editor.log_tail())

        state = wait(lambda s: True, 'Lua UI did not open')
        directory = Path(state['directory'])
        deadline = time.monotonic() + 15
        while not (directory / 'modules.txt').read_text() and time.monotonic() < deadline:
            editor.pump(2)
        assert (directory / 'modules.txt').read_text(), 'the console never reached its first prompt'
        print('PASS: mode opens, the console runs its init script')

        for row in (1, 5, 9):   # a local, inside a function (sends the whole function), a two-line statement
            editor.cmd(f'lua mep.set_cursor({row},1); mep.language_ui_eval()')
        state = wait(lambda s: 'base ' in s['globals'] and 'double ' in s['globals'] and 'total ' in s['globals'],
                     'statements sent with <leader>re did not define base/double/total')
        assert 'total                  number    42' in state['globals'], state['globals']
        print('PASS: <leader>re runs whole statements as one chunk; top-level locals persist')

        editor.cmd('lua mep.run_button_run()')
        state = wait(lambda s: 'sum ' in s['globals'] and 'mepuitestmod' in s['modules'],
                     'Run did not run the file in the console (or LUA_PATH was not inherited)')
        assert 'sum                    number    43' in state['globals'], state['globals']
        print('PASS: Run sources the file into the console; require() sees mep\'s LUA_PATH')

        editor.cmd('lua mep.set_cursor(4, 18); mep.help_at_cursor()')
        state = wait(lambda s: 'double(x)' in s['help'] and '--- Doubles x.' in s['help'],
                     'gh did not show the function\'s signature and doc comment')
        editor.cmd('lua mep.set_cursor(12, 12); mep.help_at_cursor()')
        state = wait(lambda s: 'mod.add(a, b)' in s['help'] and '--- Adds two numbers.' in s['help'],
                     'gh on a module function did not show its documentation')
        print('PASS: gh shows signature, doc comment and source in the Help tab')

        editor.cmd('lua mep.language_ui_toggle()')
        assert info() is None
        print('PASS: mode teardown')
    finally:
        if editor:
            editor.quit()
        if server:
            server.terminate()
            server.wait(timeout=5)
        os.close(read_fd)
        if write_fd is not None:
            os.close(write_fd)
        if args.keep:
            print('Kept isolated editor home:', home)
        else:
            shutil.rmtree(home, ignore_errors=True)


if __name__ == '__main__':
    main()
