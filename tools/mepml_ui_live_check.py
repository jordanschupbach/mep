#!/usr/bin/env python3
"""Exercise document previews and embedded language UI in a real editor.

Uses a private X11 display and HOME. No changes to the user's session or files.
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
    parser.add_argument('--crepl', default=shutil.which('crepl'))
    args = parser.parse_args()
    editor = server = None
    with tempfile.TemporaryDirectory(prefix='mepmlui-') as home:
        read_fd, write_fd = os.pipe()
        try:
            server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                       '1600x1200x24', '-nolisten', 'tcp'], pass_fds=(write_fd,),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError('Xvfb did not start')
            display = ':' + os.read(read_fd, 64).decode().strip()
            source = Path(home) / 'document with spaces.mepml'
            source.write_text('//? Title: UI test\n\n# Introduction\nProse.\n\n```cpp\nint counter = 40;\ncounter += 2;\n```\n\n# Next\n')
            editor = Mep(args.build_dir, home, display, [str(source)], cwd=home)

            def lua(code):
                editor.cmd('lua ' + code)

            def value(expr):
                result = editor.lua_value(expr)
                if result and result.startswith('ERR:') and result != 'ERR: false':
                    raise AssertionError(result + '\n' + editor.log_tail())
                return result

            def check(expr):
                assert value(expr) == 'true', expr + '\n' + editor.log_tail()

            def wait(expr, timeout=40):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if value(expr) == 'true':
                        return
                    editor.pump(3)
                raise AssertionError(expr + '\n' + editor.log_tail())

            lua('docbuf=mep.current_buffer(); mep.set_cursor(4,1); mep.language_ui_toggle()')
            check("mep.language_ui_active[mep.current_tab_id()].lang == 'mepml'")
            check('#mep.pane_buffers() == 3 and mep.current_buffer() == docbuf')
            output = Path(home) / 'build/document with spaces.html'
            wait('(function() local f=io.open(' + json.dumps(str(output)) + ',"r"); if f then f:close(); return true end return false end)()')
            check('(function() for _,b in ipairs(mep.pane_buffers()) do if mep.buffer_filename(b):match("%.html$") then return true end end return false end)()')

            # Refresh uses the same pane and keeps the source focused.
            lua('mep.replace_lines(4,5,{"Updated prose."}); mep.run_button_run()')
            check('#mep.pane_buffers() == 3 and mep.current_buffer() == docbuf')
            assert 'Updated prose.' in output.read_text()
            # A completion arriving in another tab is deferred, and one
            # arriving after close must not recreate the preview layout.
            lua('late_output=mep.mepml_ui_output_target(); mep.tab_new(docbuf); late_output(' + json.dumps(str(output)) + ')')
            check('#mep.pane_buffers() == 1')
            lua('mep.cmd("tabprevious")')
            check('#mep.pane_buffers() == 3 and mep.current_buffer() == docbuf')
            lua('mep.language_ui_toggle()')
            check('#mep.pane_buffers() == 1 and mep.current_buffer() == docbuf')
            lua('late_output(' + json.dumps(str(output)) + ')')
            check('#mep.pane_buffers() == 1')

            # A different configured export format goes through the same Run path.
            lua('mep.replace_lines(1,1,{"//? Export: txt"}); mep.set_cursor(5,1); mep.language_ui_toggle()')
            check('(function() for _,b in ipairs(mep.pane_buffers()) do if mep.buffer_filename(b):match("%.txt$") then return true end end return false end)()')
            lua('mep.language_ui_toggle(); mep.replace_lines(1,2,{})')

            # Open a language layout directly from the document layout.
            if args.crepl:
                lua('mep.opt.cpp_ui_cmd={' + json.dumps(str(Path(args.crepl).resolve())) + '}')
            lua('mep.set_cursor(4,1); mep.language_ui_toggle(); mep.set_cursor(7,1); mep.language_ui_toggle()')
            check("mep.language_ui_active[mep.current_tab_id()].lang == 'c'")
            check('mep.current_buffer() ~= docbuf and #mep.pane_buffers() == 4')
            check('mep.line_count() == 2 and mep.get_line(1) == "int counter = 40;"')
            lua('blockbuf=mep.current_buffer(); blockpath=mep.filename()')
            if args.crepl:
                lua('mep.language_ui_eval(); mep.set_cursor(2,1); mep.language_ui_eval()')
                wait('(function() local i=mep.cpp_ui_info(); return i and i.objects:find("counter\tint\t42",1,true) ~= nil end)()')

            # Saving updates the original file; closing retains unsaved edits.
            lua('mep.replace_lines(1,2,{"int counter = 41;"}); mep.cmd("write")')
            assert 'int counter = 41;' in source.read_text()
            lua('mep.replace_lines(1,2,{"int counter = 43;"}); mep.language_ui_toggle()')
            check('mep.current_buffer() == docbuf and #mep.pane_buffers() == 1')
            check('mep.get_line(7) == "int counter = 43;" and mep.buffer_modified(docbuf)')
            assert not list(Path(home).glob('.mep-ui-*'))
            editor.key('u')
            check('mep.get_line(7) == "int counter = 41;"')

            # Edits before the block move its destination; concurrent changes
            # to the block itself are refused and leave the source available.
            lua('mep.set_cursor(7,1); mep.language_ui_toggle(); blockbuf=mep.current_buffer(); '
                'mep.replace_lines(1,2,{"int counter = 44;"}); '
                'mep.buffer_switch(docbuf); mep.replace_lines(1,1,{"New preface."}); '
                'mep.buffer_switch(blockbuf); mep.language_ui_toggle()')
            check('mep.current_buffer() == docbuf and mep.get_line(8) == "int counter = 44;"')
            lua('mep.set_cursor(8,1); mep.language_ui_toggle(); blockbuf=mep.current_buffer(); '
                'mep.replace_lines(1,2,{"int counter = 45;"}); '
                'mep.buffer_switch(docbuf); mep.replace_lines(8,9,{"int counter = 46;"}); '
                'mep.buffer_switch(blockbuf); mep.language_ui_toggle()')
            check('mep.language_ui_active[mep.current_tab_id()] ~= nil and mep.current_buffer() == blockbuf')
            check('mep.buffer_get_lines(docbuf)[8] == "int counter = 46;"')
            lua('mep.buffer_switch(docbuf)')
            editor.key('u')
            lua('mep.buffer_switch(blockbuf); mep.language_ui_toggle()')
            check('mep.current_buffer() == docbuf and mep.get_line(8) == "int counter = 45;"')
            assert not list(Path(home).glob('.mep-ui-*'))

            for filename, content, row in [
                ('empty.mepml', '# Empty\n\n```cpp\n```\n', 3),
                ('example.org', '* Org\n#+begin_src cpp\nint value = 1;\n#+end_src\n', 3),
                ('example.Rmd', '# Rmd\n```{cpp}\nint value = 1;\n```\n', 3),
            ]:
                example = Path(home) / filename
                example.write_text(content)
                lua('mep.open(' + json.dumps(str(example)) + '); parentbuf=mep.current_buffer(); '
                    f'mep.set_cursor({row},1); mep.language_ui_toggle()')
                check('mep.language_ui_active[mep.current_tab_id()].lang == "c" and mep.current_buffer() ~= parentbuf')
                lua('mep.language_ui_toggle()')
                check('mep.current_buffer() == parentbuf and #mep.pane_buffers() == 1')
                assert example.read_text() == content
            assert not list(Path(home).glob('.mep-ui-*'))
            print('MEPML layout, preview refresh, tab handling, embedded C++, save/undo, conflicts, Org/Rmd and teardown passed')
        finally:
            os.close(read_fd)
            if editor:
                editor.kill()
            if server:
                server.terminate()
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
