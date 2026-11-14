#!/usr/bin/env python3
"""Check the Java language UI mode (src/language_ui_java.h) without a window.

Three parts:
  - the editor side: the mode's Lua chunk loaded against a stubbed `mep`
    table in a real `lua` -- the console argv (the CMD: header's class path,
    a REPL: override), what <leader>re / <leader>rr / gh type into the
    console, the /env restart when the header's class path changes, and the
    panel poll;
  - the console side: the launcher and the console program the mode writes,
    run with a real JDK and driven the way the editor drives it -- typed
    snippets, the Variables/Methods/Types files, errors and exceptions,
    /evalat and /eval with the source file's line numbers, the class path
    (header, $MEP_JAVA_CLASSPATH, /env restart and replay), /run by source
    launch, of a CMD: header and of a snippet file, /help from a class's
    Javadoc on the source path (mep-java-lsp --doc) and from the JDK;
  - the interactive console under a pty (script(1)): jshell's prompts, a
    method typed over several lines.
Needs lua, java and javac on PATH (`nix shell nixpkgs#jdk` when the shell has
none); --lsp names a built mep-java-lsp for the source-path Javadoc (skipped
without one). GUI coverage is manual.
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
HEADER = (ROOT / 'src/language_ui_java.h').read_text()
# The chunk is a concatenation of raw string pieces (MSVC caps each literal's length).
CHUNK = ''.join(m.group(2) for m in re.finditer(r'R"(MEPLUA|MEPJAVA)\((.*?)\)\1"', HEADER, re.S))
CONSOLE = CHUNK.split('local console_source = [===[\n', 1)[1].split(']===]', 1)[0]
LAUNCHER = CHUNK.split('local launcher = [==[\n', 1)[1].split(']==]', 1)[0]

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
function mep.terminal_info() return {exited = false} end
function mep.terminal_write(_, text) sent[#sent + 1] = text end
function mep.notify(msg) notes[#notes + 1] = msg end
function mep.cmd(c) cmds[#cmds + 1] = c end
function mep.command() end
function mep.language_ui_session_dir(s) return '/tmp/session_' .. s end
function mep.language_ui_write_file(path, text) files[path] = text end
function mep.language_ui_read_file(path) return files[path] end
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
function mep.filename() return '/proj/Main.java' end
function mep_lsp_abspath(p) return p end
function mep.get_line(i) return source[i] end
function mep.buffer_get_lines() return source end
local cursor_row = 1
function mep.cursor() return cursor_row, 1 end
local selection, range = '', nil
function mep.visual_selection() return selection end
function mep.visual_range() return range end
local help_sym = 'frame.filter'
function mep.help_symbol_at_cursor() return help_sym end
"""

CHECKS = r'''
assert(mep.language_ui_extensions.java == 'java' and mep.language_ui_extensions.jsh == 'java')
assert(mep.language_ui_modes.java and mep.language_ui_eval_handlers.java and providers.java)

-- The header's class path and source path.
local cmd, cp, sp = mep.java_ui_header({'// CMD: java -cp build:lib/dm.jar -sourcepath src Main.java'})
assert(cmd == 'java -cp build:lib/dm.jar -sourcepath src Main.java' and cp == 'build:lib/dm.jar' and sp == 'src', cp)
cmd, cp = mep.java_ui_header({'/* CMD: java --class-path=out Main.java */'})
assert(cp == 'out', cp)
cmd, cp = mep.java_ui_header({'class A {}'})
assert(cmd == nil and cp == '')

-- Opening: the launcher under sh, the program written into the session, the header's class path.
source = {'// CMD: java -cp build Main.java', 'public class Main {', '  public static void main(String[] a) {',
  '    System.out.println(1);', '  }', '}'}
local handle = mep.java_ui_open()
local a = mep.started
assert(a[1] == 'sh' and a[2] == '-c' and a[3]:find('MepJavaConsole', 1, true), a[1])
assert(a[5] == '/tmp/session_javaui/MepJavaConsole.java' and a[6] == '/tmp/session_javaui' and a[7] == '1' and a[8] == 'java')
local argv = table.concat(a, ' ', 9)
assert(argv == '--dir /tmp/session_javaui --class-path build --sourcepath  --lsp /opt/mep/bin/mep-java-lsp', argv)
assert(files['/tmp/session_javaui/MepJavaConsole.java']:find('public class MepJavaConsole', 1, true))
assert(sections.Variables and sections.History and sections.Help)
assert(handle.prefer_session_run == true and handle.run_source)

-- <leader>re: a snapshot of the buffer and the cursor's row.
cursor_row = 4
mep.language_ui_eval_handlers.java()
assert(sent[#sent] == '/evalat 4 /tmp/session_javaui/source_1.java /proj/Main.java\n', sent[#sent])
assert(files['/tmp/session_javaui/source_1.java'] == table.concat(source, '\n') .. '\n')

-- A Visual line selection: padded to its source lines.
selection, range = '    System.out.println(1);', {kind = 'line', start_row = 4, end_row = 4}
mep.language_ui_eval_handlers.java()
assert(sent[#sent] == '/eval /tmp/session_javaui/eval.java /proj/Main.java\n', sent[#sent])
assert(files['/tmp/session_javaui/eval.java'] == '\n\n\n    System.out.println(1);\n')
assert(cmds[#cmds] == 'normal!')
selection, range = '', nil

-- The header's class path changed: the console restarts its engine with it, then the code goes.
source[1] = '// CMD: java -cp build:extra.jar Main.java'
local before = #sent
mep.language_ui_eval_handlers.java()
assert(sent[before + 1] == '/env --class-path build:extra.jar\n', sent[before + 1])
assert(sent[before + 2]:find('^/evalat 4 '))
mep.language_ui_eval_handlers.java()
assert(not sent[#sent - 1]:find('^/env'), 'no restart while the class path stays')

-- <leader>rr: saved first (the CMD: line's command reads the file), then /run of the snapshot.
handle.run_source('/proj/Main.java')
assert(cmds[#cmds] == 'write' and sent[#sent]:find('^/run /tmp/session_javaui/source_%d+%.java /proj/Main.java\n$'), sent[#sent])
source[1] = '// no header'
before = #sent
handle.run_source('/proj/Main.java')
assert(sent[before + 1] == '/env --class-path ""\n', sent[before + 1])
assert(sent[before + 2]:find('^/run '))

-- gh: /help with the snapshot (for its imports and package root), Help revealed.
assert(providers.java() == true)
assert(sent[#sent]:find('^/help frame%.filter /tmp/session_javaui/source_%d+%.java\n$'), sent[#sent])
assert(mep.revealed == 101)

-- The poll: Variables sections from /vars, /methods, /types; history entries; Help revealed.
files['/tmp/session_javaui/vars.txt'] = 'x\tint\t5\nframe\tDataFrame\tDataFrame[3 rows]\n'
files['/tmp/session_javaui/methods.txt'] = 'twice\tint twice(int)\n'
files['/tmp/session_javaui/types.txt'] = 'Point\trecord Point\n'
files['/tmp/session_javaui/history.txt'] = 'int x = 5;\30/run Main.java'
files['/tmp/session_javaui/help.txt'] = 'filter docs\n'
mep.revealed = nil
mep.java_ui_poll()
local secs = sections.Variables
assert(secs[1].title == 'Variables (/vars)' and secs[1].widgets[1].text == 'int x = 5', secs[1].widgets[1].text)
assert(secs[1].widgets[2].text == 'DataFrame frame = DataFrame[3 rows]')
assert(secs[2].widgets[1].text == 'int twice(int)' and secs[3].widgets[1].text == 'record Point')
assert(mep.revealed == 101 and sections.Help == 'filter docs\n')
secs[1].widgets[2].on_click()
assert(sent[#sent] == '/help frame\n', sent[#sent])
local hist = sections.History[1].widgets
assert(hist[1].text == '/run Main.java' and hist[2].text == 'int x = 5;')
hist[1].on_click()
assert(sent[#sent] == '/run Main.java\n')
hist[2].on_click()
assert(sent[#sent] == '/eval /tmp/session_javaui/eval.java\n' and files['/tmp/session_javaui/eval.java'] == 'int x = 5;\n')
assert(mep.java_ui_info().directory == '/tmp/session_javaui')

-- A REPL: override names the java command.
mep.language_ui_repl_override = 'nix shell nixpkgs#jdk --command java'
mep.java_ui_open()
assert(mep.started[8] == 'nix shell nixpkgs#jdk --command java', mep.started[8])
mep.language_ui_repl_override = nil

-- No console: a warning, nothing sent.
tab = 2
local count = #sent
mep.language_ui_eval_handlers.java()
assert(#sent == count and notes[#notes]:find('No Java console'))
assert(providers.java() == false)
print('editor side passed')
'''

FRAME = '''\
package org.datamunge;

/**
 * A table of named columns.
 *
 * <p>Built by {@link #of(int)}.
 */
public class DataFrame {
    private final int rows;

    private DataFrame(int rows) { this.rows = rows; }

    /**
     * Makes a frame of {@code n} rows.
     *
     * @param n the number of rows
     * @return the frame
     */
    public static DataFrame of(int n) { return new DataFrame(n); }

    /**
     * Keeps the rows above a threshold.
     *
     * @param threshold rows at or below it are dropped
     * @return a smaller frame
     */
    public DataFrame filter(double threshold) { return new DataFrame(rows / 2); }

    /** The number of rows. */
    public int rows() { return rows; }

    @Override
    public String toString() { return "DataFrame[" + rows + " rows]"; }
}
'''

MAIN = '''\
// CMD: java -cp build Main.java
import org.datamunge.DataFrame;
import java.util.List;

public class Main {
    /** Twice x. */
    static int twice(int x) {
        return 2 * x;
    }

    public static void main(String[] args) {
        DataFrame f = DataFrame.of(10);
        int total = 0;
        for (int i = 0; i < 3; i++) {
            total += twice(i);
        }
        System.out.println("rows " + f.filter(0.5).rows() + " total " + total);
    }
}
'''


class JavaUi(unittest.TestCase):
    def test_editor_side(self):
        p = subprocess.run([LUA_EXE, '-'], input=STUBS + CHUNK + CHECKS, text=True, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('editor side passed', p.stdout)

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mep-javaui-')
        cls.dir = Path(cls.tmp.name)
        src = cls.dir / 'src/org/datamunge'
        src.mkdir(parents=True)
        (src / 'DataFrame.java').write_text(FRAME)
        subprocess.run([JAVAC_EXE, '-d', str(cls.dir / 'build'), str(src / 'DataFrame.java')], check=True)
        (cls.dir / 'Main.java').write_text(MAIN)
        cls.cache = cls.dir / 'cache'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def console(self, inputs, args=(), env=None, name='session'):
        """The console through the mode's own launcher, as the editor starts it."""
        session = self.dir / name
        if session.exists():
            shutil.rmtree(session)
        session.mkdir()
        (session / 'MepJavaConsole.java').write_text(CONSOLE)
        argv = ['sh', '-c', LAUNCHER, 'mep-java-ui', str(session / 'MepJavaConsole.java'), str(session), '0', 'java',
                '--dir', str(session), '--lsp', LSP_EXE or '', *args]
        full_env = dict(os.environ, XDG_CACHE_HOME=str(self.cache), **(env or {}))
        full_env.pop('CLASSPATH', None)
        full_env.pop('MEP_JAVA_CLASSPATH', None)
        full_env.pop('MEP_JAVA_SOURCEPATH', None)
        full_env.update(env or {})
        p = subprocess.run(argv, input=''.join(line + '\n' for line in inputs), text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=300, cwd=self.dir, env=full_env)
        self.assertEqual(p.returncode, 0, p.stdout)
        return p.stdout, session

    def test_console_snippets_panels_errors(self):
        out, session = self.console([
            'int x = 5;',
            'x * 2',
            'int twice(int a) {',
            '    return 2 * a;',
            '}',
            'twice(x)',
            'record Point(int x, int y) {}',
            'var p = new Point(1, 2);',
            'String s = "hi";',
            'undefinedName + 1',
            'int[] arr = new int[2];',
            'arr[5]',
            '/vars',
            '/methods',
            '/types',
            '/drop s',
        ])
        self.assertIn('x ==> 5\n', out)
        self.assertIn('$2 ==> 10\n', out)
        self.assertIn('|  created method twice(int)\n', out)
        self.assertIn('|  created record Point\n', out)
        self.assertIn('p ==> Point[x=1, y=2]\n', out)
        self.assertIn('|  Error:\n|  cannot find symbol\n|    symbol:   variable undefinedName\n|  undefinedName + 1\n|  ^', out)
        self.assertIn('|  Exception java.lang.ArrayIndexOutOfBoundsException: Index 5 out of bounds for length 2', out)
        self.assertIn('|    int x = 5\n', out)
        self.assertIn('|    int twice(int)\n', out)
        self.assertIn('|    record Point\n', out)
        self.assertIn('|  dropped s\n', out)
        variables = (session / 'vars.txt').read_text()
        self.assertIn('x\tint\t5\n', variables)
        self.assertIn('p\tPoint\tPoint[x=1, y=2]\n', variables)
        self.assertNotIn('\ts\t', variables)
        self.assertEqual((session / 'methods.txt').read_text(), 'twice\tint twice(int)\n')
        self.assertEqual((session / 'types.txt').read_text(), 'Point\trecord Point\n')
        self.assertIn('import java.util.*;', (session / 'imports.txt').read_text())
        history = (session / 'history.txt').read_text().split('\x1e')
        self.assertIn('int twice(int a) {\n    return 2 * a;\n}', history)

    def test_console_source_lines_class_path_run(self):
        main = str(self.dir / 'Main.java')
        padded = self.dir / 'eval.java'
        padded.write_text('\n' * 11 + '        DataFrame g = DataFrame.of(4);\n        int bad = g.nope();\n')
        out, session = self.console([
            '/evalat 7 %s %s' % (main, main),     # the method with its Javadoc, imports brought along
            '/evalat 12 %s %s' % (main, main),    # DataFrame f = DataFrame.of(10); -- on the class path
            '/evalat 13 %s %s' % (main, main),
            '/evalat 14 %s %s' % (main, main),    # the whole for loop
            'total',
            '/eval %s %s' % (padded, main),       # an error at the file's own line
            '/env --class-path ""',               # restarted without it: the replay drops f
            '/env --class-path build',            # and with it again: everything replays
            'f.filter(0.5).rows()',
            '/run %s %s' % (main, main),          # the CMD: header's command
        ], args=['--class-path', 'build'])
        self.assertIn('# Main.java:6-9\n|  created method twice(int)\n', out)
        self.assertIn('# Main.java:12\nf ==> DataFrame[10 rows]\n', out)
        self.assertIn('# Main.java:14-16\n', out)
        self.assertIn('total ==> 6\n', out)
        self.assertRegex(out, r'Main\.java:13:19: error: cannot find symbol\n    symbol:   method nope\(\)')
        self.assertIn('g ==> DataFrame[4 rows]', out)
        self.assertRegex(out, r'\|  Restarted with class path ; replayed \d+ of \d+ snippets')
        self.assertRegex(out, r'\|  Restarted with class path .*/build; replayed (\d+) of \1 snippets')
        self.assertIn('==> 5\n', out)
        self.assertIn('$ java -cp build Main.java\nrows 5 total 6\n', out)
        self.assertEqual((session / 'classpath.txt').read_text(), str(self.dir / 'build') + '\n')

    def test_console_run_and_open(self):
        nohead = self.dir / 'NoHeader.java'
        nohead.write_text(MAIN.replace('// CMD: java -cp build Main.java\n', '').replace('class Main', 'class NoHeader'))
        snippets = self.dir / 'setup.jsh'
        snippets.write_text('int base = 40;\nint answer() { return base + 2; }\n')
        broken = self.dir / 'Broken.java'
        broken.write_text('public class Broken {\n  public static void main(String[] a) {\n    int x = ;\n  }\n}\n')
        out, _ = self.console([
            '/run %s /proj/NoHeader.java' % nohead,   # source launch, with $MEP_JAVA_CLASSPATH
            '/run %s /proj/setup.jsh' % snippets,     # a snippet file: into the session
            'answer()',
            '/run %s /proj/Broken.java' % broken,
        ], env={'MEP_JAVA_CLASSPATH': str(self.dir / 'build')})
        self.assertIn('$ java --class-path %s NoHeader.java\nrows 5 total 6\n' % (self.dir / 'build'), out)
        self.assertIn('# /open setup.jsh\n', out)
        self.assertIn('==> 42\n', out)
        self.assertIn('Broken.java:3: error:', out)
        self.assertIn('[exit status 1]', out)

    def test_console_help(self):
        main = str(self.dir / 'Main.java')
        out, session = self.console([
            '/evalat 12 %s %s' % (main, main),
            '/help f.filter %s' % main,
        ], args=['--class-path', 'build', '--sourcepath', 'src'])
        page = (session / 'help.txt').read_text()
        if LSP_EXE:
            self.assertIn('```java\npublic DataFrame filter(double threshold)\n```', page)
            self.assertIn('Keeps the rows above a threshold.', page)
            self.assertIn('**Parameters:**\n- `threshold` -- rows at or below it are dropped', page)
            self.assertIn('DataFrame.java:27', page)
        out2, session2 = self.console(['/help DataFrame', '/help String.valueOf', '/help nothingLikeThis'],
                                      args=['--sourcepath', 'src'], name='session2')
        self.assertIn('# String.valueOf: in the Help tab', out2)
        page = (session2 / 'help.txt').read_text()
        self.assertIn('nothingLikeThis: no documentation found', page)
        _, session3 = self.console(['/help String.valueOf'], name='session3')
        page = (session3 / 'help.txt').read_text()
        if 'no documentation found' not in page:  # src.zip: the JDK's Javadoc
            self.assertIn('String.valueOf(int i)', page)
            self.assertIn('Returns the string representation of the `int` argument.', page)
        if LSP_EXE:
            _, session4 = self.console(['/help DataFrame'], args=['--sourcepath', 'src'], name='session4')
            self.assertIn('A table of named columns.\n\nBuilt by `of(int)`.', (session4 / 'help.txt').read_text())
            # $MEP_JAVA_SOURCEPATH reaches the server too.
            _, session5 = self.console(['/help DataFrame.of'], env={'MEP_JAVA_SOURCEPATH': str(self.dir / 'src')},
                                       name='session5')
            self.assertIn('Makes a frame of `n` rows.', (session5 / 'help.txt').read_text())

    def test_interactive_console(self):
        script = shutil.which('script')
        if not script:
            self.skipTest('script(1) not installed')
        session = self.dir / 'pty'
        session.mkdir(exist_ok=True)
        (session / 'MepJavaConsole.java').write_text(CONSOLE)
        cmd = "sh -c '%s' mep-java-ui %s %s 0 java --dir %s" % (
            LAUNCHER.replace("'", "'\\''"), session / 'MepJavaConsole.java', session, session)
        p = subprocess.run([script, '-qfec', cmd, '/dev/null'],
                           input='int thrice(int x) {\nreturn 3 * x;\n}\nthrice(7)\n/exit\n',
                           text=True, capture_output=True, timeout=300,
                           env=dict(os.environ, HOME=str(self.dir), XDG_CACHE_HOME=str(self.cache), TERM='dumb'))
        self.assertIn('jshell> ', p.stdout)
        self.assertIn('   ...> ', p.stdout)
        self.assertRegex(p.stdout, r'\$\d+ ==> 21')
        self.assertIn('thrice\tint thrice(int)', (session / 'methods.txt').read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua'))
    parser.add_argument('--javac', default=shutil.which('javac'))
    lsp_default = next((str(p) for p in [ROOT / 'build/native/mep-java-lsp', ROOT / 'build/native-dev/mep-java-lsp']
                        if p.exists()), shutil.which('mep-java-lsp'))
    parser.add_argument('--lsp', default=lsp_default)
    args, remaining = parser.parse_known_args()
    if not args.lua or not args.javac or not shutil.which('java'):
        print('lua, java or javac not installed: skipping the Java UI check')
        sys.exit(77)
    LUA_EXE, JAVAC_EXE = args.lua, args.javac
    LSP_EXE = os.path.abspath(args.lsp) if args.lsp else None
    if LSP_EXE and subprocess.run([LSP_EXE, '--doc', 'x'], capture_output=True).returncode > 1:
        print('%s has no --doc (an older build): skipping the source-path Javadoc checks' % LSP_EXE)
        LSP_EXE = None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
