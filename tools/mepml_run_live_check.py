#!/usr/bin/env python3
"""Check elapsed-time notifications from actual MEPML jobs and embedded programs."""
import argparse
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile
import time

from fold_live_check import Mep


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build/native')
    args = parser.parse_args()
    editor = server = None
    read_fd, write_fd = os.pipe()
    with tempfile.TemporaryDirectory(prefix='meptime-') as home:
        try:
            server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                       '1200x900x24', '-nolisten', 'tcp'], pass_fds=(write_fd,),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError('Xvfb did not start')
            display = ':' + os.read(read_fd, 64).decode().strip()
            initial = Path(home) / 'initial.mepml'
            initial.write_text('# Timing checks\n')
            editor = Mep(args.build_dir, home, display, [str(initial)], cwd=home)

            def completions():
                found = []
                for event in editor.rpc.events:
                    if event.get('method') != 'event.notification':
                        continue
                    notice = event['params']
                    match = re.search(r' (?:in|after) (\d+\.\d+) s', notice['message'])
                    if match and 'block at line' in notice['message']:
                        found.append((notice, float(match[1])))
                return found

            def run_case(index, header, code, expected, minimum, stop=False, page_stays=False):
                source = Path(home) / f'case{index}.mepml'
                source.write_text(f'```{header}\n{code}\n```\n')
                # Open through the normal command path, preserving spaces in paths.
                editor.cmd('edit ' + str(source))
                editor.rpc.events.clear()
                editor.cmd('lua mep.set_cursor(2,1); mep.mepml_execute()')
                if stop:
                    assert not completions(), 'An interactive block was reported finished at launch'
                    editor.cmd('lua mep.set_cursor(2,1); mep.mepml_stop()')
                deadline = time.monotonic() + 15
                while not completions() and time.monotonic() < deadline:
                    editor.pump(2)
                found = completions()
                assert len(found) == 1, str(found) + '\n' + editor.log_tail()
                notice, seconds = found[0]
                assert expected in notice['message'], notice
                assert minimum <= seconds < 12, (seconds, notice)
                if expected == 'failed':
                    assert notice['level'] == 'warn', notice
                if page_stays:
                    # Server exit reports immediately; later closing its retained
                    # page must not produce a second runtime notification.
                    editor.cmd('lua mep.set_cursor(2,1); mep.mepml_stop()')
                editor.pump(8)
                assert len(completions()) == 1, completions()
                print(notice['message'], flush=True)

            run_case(1, 'bash', 'sleep 0.25; printf "batch result"', 'Done running', 0.24)
            run_case(2, 'bash', 'sleep 0.15; exit 3', 'failed', 0.14)
            run_case(3, 'mep-lua', 'os.execute("sleep 0.2"); return "synchronous result"', 'Done running', 0.19)
            run_case(4, 'exec', 'sleep 0.3; printf "terminal result"', 'finished', 0.29)
            run_case(5, '{bash, results=exec}', 'sleep 0.3; printf "prepared result"', 'finished', 0.29)
            run_case(6, 'exec', 'sleep 30', 'stopped', 0.05, stop=True)
            run_case(7, 'exec-gui', 'sleep 0.25; printf "gui result"', 'finished', 0.24)
            run_case(8, '{bash, results=web}', "printf 'http://127.0.0.1:9/\\n'; sleep 0.25", 'finished', 0.24, page_stays=True)
            print('MEPML live timing: batch, errors, synchronous Lua, terminal, prepared program, stop, GUI and web server passed')
        finally:
            os.close(read_fd)
            if editor:
                editor.kill()
            if server:
                server.terminate()
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
