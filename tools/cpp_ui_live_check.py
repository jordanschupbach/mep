#!/usr/bin/env python3
"""Drive the real mep C++ UI over its agent socket, on a private X11 display.

Tests incremental submission, live object panels, value inspection, embedded PNG
figures, full-file Run, restart, and teardown. --source can exercise an external
example without adding a dependency on that example to mep or crepl.
"""
import argparse
import base64
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
    parser.add_argument('--crepl', default=shutil.which('crepl'))
    parser.add_argument('--source')
    parser.add_argument('--object', default='values')
    parser.add_argument('--preview', default='[3] 3')
    parser.add_argument('--keep', action='store_true')
    args = parser.parse_args()
    if not args.crepl:
        parser.error('crepl must be on PATH, or pass --crepl')
    home = tempfile.mkdtemp(prefix='mepcpp-')
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
        if args.source:
            source = Path(args.source).resolve()
        else:
            png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j2iYAAAAASUVORK5CYII=')
            image = ','.join(map(str, png))
            source = Path(home) / 'example.cpp'
            source.write_text('#include <vector>\n#include <fstream>\n#include <string>\n'
                'struct Figure { void save(std::string path) { unsigned char bytes[]={' + image + '}; '
                'std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<char*>(bytes), sizeof(bytes)); } '
                'void show() { throw 42; } };\n'
                'int main(void) {\nint counter = 40;\ncounter += 2;\n'
                'std::vector<int> values{\n1, 2, 3\n};\nFigure figure;\nfigure.show();\nreturn 0;\n}\n')
        editor = Mep(args.build_dir, home, ':' + number, [str(source)], cwd=str(source.parent))
        editor.cmd('lua mep.opt.cpp_ui_cmd={' + json.dumps(str(Path(args.crepl).resolve())) + '}; mep.opt.language_ui_poll_interval=0.05')
        editor.cmd('lua mep.language_ui_toggle()')

        def info():
            value = editor.lua_value('(function() local i=mep.cpp_ui_info(); if not i then return "null" end return mep_ai_json_encode(i) end)()')
            if value and value.startswith("ERR:"): raise RuntimeError(value)
            return json.loads(value) if value else None

        def wait(predicate, label, timeout=45):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                value = info()
                if predicate(value):
                    return value
                editor.pump(2)
            raise AssertionError(label + '\nstate=' + str(info()) + '\nlog=' + editor.log_tail())

        state = wait(lambda s: bool(s and s.get('directory')), 'C++ UI did not open')
        directory = Path(state['directory'])
        if not args.source:
            # Includes and definitions, then a statement selected INSIDE its multiline initializer.
            rows = [1,2,3,4,6,7,9,11,12]
            for number, row in enumerate(rows, 1):
                editor.cmd(f'lua mep.set_cursor({row},1); mep.cpp_ui_eval_block()')
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    history = directory / 'history.txt'
                    if history.exists() and len(history.read_text().splitlines()) >= number:
                        break
                    editor.pump(2)
                else:
                    raise AssertionError('increment did not finish at row ' + str(row) + '\n' + editor.log_tail())
            state = wait(lambda s: s and 'counter\tint\t42' in s.get('objects', '') and s.get('figures') == 1,
                         'incremental objects/figure did not refresh')
            print('PASS: source fragments execute once; multiline cursor selection and live objects update')
        else:
            editor.cmd('lua mep.run_button_run()')
            state = wait(lambda s: s and args.object + '\t' in s.get('objects', '') and s.get('figures', 0) >= 1,
                         'external example did not run into the C++ session', 90)
        editor.cmd('lua mep.cpp_ui_inspect(' + json.dumps(args.object) + ')')
        state = wait(lambda s: s and args.preview in s.get('preview', '') and args.object in s.get('inspect', ''),
                     'value preview / compiler member inspection did not refresh')
        figures = list((Path(state['directory']) / 'figures').glob('fig_*.png'))
        assert figures and figures[0].read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
        images = editor.lua_value('(function() local n=0 for _,b in ipairs(mep.pane_buffers()) do if mep.image_get_theme(b) ~= nil then n=n+1 end end return n end)()')
        assert int(images) >= 1, 'No image buffer was visible in a pane'
        print('PASS: object inspection and PNG gallery display')
        old_directory = state['directory']
        editor.cmd('MepCppRestart')
        wait(lambda s: s and s['directory'] != old_directory and not s.get('objects'), 'restart did not clear the live session')
        editor.cmd('lua mep.run_button_run()')
        wait(lambda s: s and args.object + '\t' in s.get('objects', '') and s.get('figures', 0) >= 1,
             'full-file Run did not use the new live session', 90)
        print('PASS: restart and full-file Run preserve UI integration')
        editor.cmd('lua mep.language_ui_toggle()')
        assert info() is None
        print('PASS: mode teardown closes the session and panels')
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
