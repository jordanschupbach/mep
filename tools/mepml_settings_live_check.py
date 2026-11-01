#!/usr/bin/env python3
"""Click a real MEPML card's gear and edit its source through the settings UI."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile

from fold_live_check import Mep


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build/native')
    parser.add_argument('--screenshot')
    args = parser.parse_args()
    editor = server = None
    read_fd, write_fd = os.pipe()
    with tempfile.TemporaryDirectory(prefix='mepgear-') as home:
        try:
            server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                                       '1400x1100x24', '-nolisten', 'tcp'], pass_fds=(write_fd,),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            os.close(write_fd)
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError('Xvfb did not start')
            display = ':' + os.read(read_fd, 64).decode().strip()
            source = Path(home) / 'settings.mepml'
            original = '\n```{cpp, flags="-O2"}\nint main() { return 0; }\n```\n'
            source.write_text(original)
            editor = Mep(args.build_dir, home, display, [str(source)], cwd=home)
            editor.cmd('lua mep.set_cursor(1,1)')
            editor.rpc.call('ui.hints', {'scope': 'all'})
            editor.pump(3)
            targets = editor.rpc.call('ui.hint_targets')['targets']
            # The top code card has three adjacent controls at one y:
            # settings, play, minimize. Use their rendered hit regions,
            # rather than baking font- and window-dependent coordinates in.
            groups = {}
            for target in targets:
                if target['y'] > 100 and target['x'] > 400:
                    groups.setdefault(round(target['y']), []).append(target)
            candidates = [group for _, group in sorted(groups.items()) if len(group) == 3]
            assert candidates, targets
            gear = min(candidates[0], key=lambda target: target['x'])
            editor.rpc.call('ui.hint_cancel')
            editor.pump(3)
            font_size = float(editor.lua_value('mep.font_size()'))
            editor.rpc.events.clear()
            editor.rpc.call('ui.mouse_click', {'x': int(gear['x'] + font_size * 0.3),
                                               'y': int(gear['y'] + font_size * 0.4)})
            editor.pump(8)
            modes = [event['params']['mode'] for event in editor.rpc.events if event.get('method') == 'event.modeChanged']
            assert 'BLOCK-SETTINGS' in modes, (modes, editor.log_tail())
            if args.screenshot:
                assert editor.screenshot(args.screenshot)

            # The first row is eval: choose false from its dropdown.
            editor.key('Return')
            for character in 'false':
                editor.key(character)
            editor.key('Return')
            editor.key('Escape')
            assert editor.lua_value('tostring(mep.mepml_block_at(2).options.eval)') == 'false'
            editor.key('u')
            assert editor.lua_value('tostring(mep.mepml_block_at(2).options.eval)') == 'nil'

            # The keyboard command reaches the same popup; edit a text field.
            editor.cmd('lua mep.set_cursor(3,1)')
            editor.cmd('MepmlBlockSettings')
            editor.key('j', 2)  # results, then file
            editor.key('Return')
            for character in 'plot, v2.png':
                editor.key(character)
            editor.key('Return')
            editor.key('Escape')
            assert editor.lua_value('mep.mepml_block_at(2).options.file') == 'plot, v2.png'
            editor.cmd('write')
            saved = source.read_text()
            assert 'file="plot, v2.png"' in saved and 'flags="-O2"' in saved, saved
            assert 'int main() { return 0; }' in saved
            print('MEPML gear click, dropdown, command, text field, source update, save and undo passed')
        finally:
            os.close(read_fd)
            if editor:
                editor.kill()
            if server:
                server.terminate()
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
