#!/usr/bin/env python3
"""Check h/l across UTF-8 characters in the real editor on a private display."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile

from fold_live_check import Mep


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build/native')
    args = parser.parse_args()
    editor = server = None
    with tempfile.TemporaryDirectory(prefix='mep-utf8-') as home:
        read_fd, write_fd = os.pipe()
        try:
            server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                       '1400x1000x24', '-nolisten', 'tcp'], pass_fds=(write_fd,),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError('Xvfb did not start')
            display = ':' + os.read(read_fd, 64).decode().strip()
            source = Path(home) / 'motion.apl'
            lines = ['NL←⎕UCS 10', '⍝∇⍞⍬∆⊂⍕', 'aéλ🙂z', '', 'ascii']
            source.write_text('\n'.join(lines) + '\n')
            editor = Mep(args.build_dir, home, display, ['--no-session', str(source)], cwd=home)

            def col():
                return int(editor.lua_value('(select(2, mep.cursor()))'))

            # The API uses one-based byte columns; motions count characters.
            for row, line in enumerate(lines, 1):
                positions = [1]
                for ch in line[:-1]:
                    positions.append(positions[-1] + len(ch.encode('utf-8')))
                editor.cmd('lua mep.set_cursor(%d, 1)' % row)
                for expected in positions[1:]:
                    editor.key('l')
                    assert col() == expected, (row, expected, col())
                editor.key('l')
                assert col() == positions[-1]
                for expected in reversed(positions[:-1]):
                    editor.key('h')
                    assert col() == expected, (row, expected, col())
                editor.key('h')
                assert col() == 1

            editor.cmd('lua mep.set_cursor(2, 1)')
            editor.key('3'); editor.key('l')
            assert col() == 10
            editor.key('2'); editor.key('h')
            assert col() == 4
            editor.key('v'); editor.key('l')
            assert col() == 7
            editor.key('h')
            assert col() == 4
            editor.key('Escape')

            # A delete motion removes complete symbols, preserving UTF-8.
            editor.cmd('lua mep.set_cursor(2, 1)')
            editor.key('d'); editor.key('2'); editor.key('l')
            assert editor.line(2) == lines[1][2:]
            editor.key('u')
            assert editor.line(2) == lines[1]
            editor.cmd('lua mep.set_cursor(2, 7)')
            editor.key('d'); editor.key('h')
            assert editor.line(2) == lines[1][0] + lines[1][2:]
            editor.key('u')
            assert editor.line(2) == lines[1]
            print('PASS: UTF-8 horizontal motions, counts, Visual mode and deletes')
        finally:
            os.close(read_fd)
            if editor:
                editor.quit()
            if server:
                server.terminate()
                server.wait()


if __name__ == '__main__':
    main()
