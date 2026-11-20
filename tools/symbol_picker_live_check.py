#!/usr/bin/env python3
"""Exercise Unicode symbol insertion in a real editor."""

import argparse
from pathlib import Path
import tempfile

from fold_live_check import Mep, Xvfb


def type_query(editor, query):
    editor.rpc.call('ui.type_text', {'text': query})
    editor.pump(len(query) + 4)


def check(editor):
    editor.cmd(
        'lua local preview = mep.picker_set_preview; '
        'mep.picker_set_preview = function(text, spans, glyph) '
        'symbol_preview, symbol_glyph = text, glyph or ""; preview(text, spans, glyph) end'
    )
    editor.cmd('lua mep.set_cursor(1,2)')
    for key in ('space', 'i', 's'):
        editor.key(key)
    assert editor.lua_value('symbol_glyph') == 'α'
    editor.key('Down')
    assert editor.lua_value('symbol_glyph') == 'β'
    type_query(editor, 'lambda')
    assert editor.lua_value('symbol_glyph') == 'λ'
    assert 'U+03BB' in editor.lua_value('symbol_preview')
    editor.key('Return')
    assert editor.line(1) == 'aλb'

    editor.cmd('MepInsertSymbol')
    type_query(editor, 'U+2192')
    assert editor.lua_value('symbol_glyph') == '→'
    editor.key('Return')
    assert editor.line(1) == 'aλ→b'
    editor.key('u')
    assert editor.line(1) == 'aλb'

    editor.cmd('MepInsertSymbol')
    type_query(editor, 'zzzzzzz')
    assert editor.lua_value('symbol_glyph') == ''
    editor.key('Escape')
    assert editor.line(1) == 'aλb'
    editor.cmd('write')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build/native')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mep-symbol-') as tmp:
        doc = Path(tmp) / 'notes.txt'
        doc.write_text('ab\n')
        display = Xvfb()
        editor = None
        try:
            editor = Mep(args.build_dir, tmp, display.display, [str(doc)], cwd=tmp)
            check(editor)
            assert doc.read_text() == 'aλb\n'
        finally:
            if editor:
                editor.kill()
            display.close()
    print('Symbol picker navigation, aliases, code points, preview, insertion, undo and save passed')


if __name__ == '__main__':
    main()
