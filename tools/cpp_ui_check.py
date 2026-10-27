#!/usr/bin/env python3
"""Check real crepl execution, structural C++ previews, and the mep Lua panel bridge.

No client library, window, network, or Python packages are required. Supply
--crepl to test an uninstalled checkout. GUI coverage is in cpp_ui_live_check.py.
"""
import argparse
import base64
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = (ROOT / 'src/language_ui_cpp.h').read_text()
RUNTIME = HEADER.split('R"MEPCPP(', 1)[1].split(')MEPCPP"', 1)[0]
LUA = HEADER.split('R"MEPLUA(', 1)[1].split(')MEPLUA"', 1)[0]

class CppUi(unittest.TestCase):
    def test_previews_and_nonblocking_figure_capture(self):
        with tempfile.TemporaryDirectory(prefix='mep-cpp-') as directory:
            root = Path(directory)
            (root / 'figures').mkdir()
            adapter = root / 'adapter.hpp'
            adapter.write_text('#define MEP_UI_DIRECTORY "' + directory + '"\n' + RUNTIME)
            png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j2iYAAAAASUVORK5CYII=')
            image = ','.join(map(str, png))
            source = ('#include <vector>\n'
                      'std::vector<int> values{1,2,3};\nmep_ui::view(values);\n'
                      'struct Frame { Frame head(int) const { return {}; } '
                      'std::string to_string() const { return "table preview"; } };\n'
                      'Frame frame;\nmep_ui::view(frame);\n'
                      'struct Figure { void save(std::string path) { unsigned char bytes[]={' + image + '}; '
                      'std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<char*>(bytes), sizeof(bytes)); } '
                      'void show() { throw std::runtime_error("native show must not run"); } };\n'
                      'Figure figure;\nfigure.show();\nfigure.show();\nint after_show = 42;\n:inspect figure\n')
            process = subprocess.run([CREPL, '--state-dir', directory, '--display-function', 'mep_ui::show',
                                      '-include', str(adapter)], input=source, text=True, capture_output=True, timeout=40)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual((root / 'preview.txt').read_text(), 'table preview')
            self.assertIn('after_show\tint\t42', (root / 'objects.tsv').read_text())
            self.assertIn('save : void', (root / 'inspect.txt').read_text())
            self.assertEqual(len(list((root / 'figures').glob('fig_*.png'))), 2)
            self.assertEqual((root / 'figures/fig_0001.png').read_bytes(), png)
            self.assertFalse(list((root / 'figures').glob('pending_*')))

    def test_lua_panel_bridge_and_source_buffer(self):
        if not LUA_EXE:
            self.skipTest('lua not installed')
        setup = r"""
local files, sections, sent, commands = {}, {}, {}, {}
local buffer, tab = 1, 1
mep = {opt={}, language_ui_eval_handlers={}}
function mep.current_tab_id() return tab end
function mep.current_buffer() return buffer end
function mep.is_terminal_buffer() return true end
function mep.terminal_info() return {exited=false} end
function mep.terminal_write(_, text) sent[#sent+1]=text end
function mep.notify() end
function mep.command(name, fn) commands[name]=fn end
function mep.fs_mkdir() end
function mep.language_ui_write_file(path, text) files[path]=text end
function mep.language_ui_read_file(path) return files[path] end
function mep.language_ui_shq(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
function mep.line_count() return 2 end
function mep.get_line(n) return n==1 and '#include <vector>' or 'int value=42;' end
function mep.buffer_get_lines(id) assert(id==1); return {'int value = 42;'} end
function mep.run_button_config_for() return {flags='-DUSER_FLAG=42'} end
function mep.terminal_here_argv(argv) mep.started=argv; buffer=2 end
function mep.sidebar_create(name) return name end
function mep.sidebar_set_sections(id, value) sections[id]=value end
function mep.sidebar_open_pane(id) buffer=id end
function mep.language_ui_render_textbox(id, value) sections[id]=value end
function mep.language_ui_gallery() return {figures={},poll=function() end} end
function mep.pane_focus() return true end
function mep.reveal_buffer() end
function mep.cursor() return 2 end
function mep.help_symbol_at_cursor() return 'value' end
"""
        checks = r"""
local st={ext='cpp',src_buf=1,src_dir='/project with spaces',src_abs='/project with spaces/file.cpp',session_dir='/tmp/session'}
mep.cpp_ui_start_console(st); st.term_buf=2
assert(mep.started[3]:find('crepl',1,true)); assert(mep.started[3]:find('-DUSER_FLAG=42',1,true))
mep.cpp_ui_setup(st,{editor_pane=10,top_pane=20,bottom_pane=30})
files['/tmp/session/objects.tsv']='value\tint\t42\n'
files['/tmp/session/inspect.txt']='value : int\n'
files['/tmp/session/history.txt']='int value=42;\\n\n'
mep.cpp_ui_poll(st)
assert(sections.Objects[1].widgets[1].text=='value : int = 42')
sections.Objects[1].widgets[1].on_click()
assert(sent[#sent-1]==':inspect value\n'); assert(sent[#sent]=='mep_ui::view(value);\n')
assert(not mep.cpp_ui_inspect('value); unsafe()'))
-- Run from a sidebar must still submit the source buffer, including unsaved edits.
mep.cpp_ui_run(st,'file.cpp')
assert(files['/tmp/session/source_1.cpp']=='int value = 42;\n')
assert(sent[#sent]==':load-auto /tmp/session/source_1.cpp\n')
mep.cpp_ui_run(st,'file.cpp'); assert(files['/tmp/session/source_2.cpp'])
assert(sent[#sent]==':load-auto /tmp/session/source_2.cpp\n')
assert(mep.cpp_ui_info().objects=='value\tint\t42\n')
mep.cpp_ui_close(st); assert(mep.cpp_ui_info()==nil)
print('Lua panel bridge passed')
"""
        code = setup + '\nlocal runtime="runtime"\n' + LUA + checks
        p = subprocess.run([LUA_EXE, '-'], input=code, text=True, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('Lua panel bridge passed', p.stdout)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--crepl', default=shutil.which('crepl'))
    parser.add_argument('--lua', default=shutil.which('lua'))
    args, remaining = parser.parse_known_args()
    if not args.crepl:
        print('crepl not installed: skipping C++ UI integration')
        sys.exit(77)
    CREPL, LUA_EXE = args.crepl, args.lua
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
