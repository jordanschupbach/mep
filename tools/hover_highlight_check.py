#!/usr/bin/env python3
"""Check hover content and highlighting with mep's real bundled grammars.

Runs without a window. Usage: python3 tools/hover_highlight_check.py [--mep PATH]
"""
import argparse
import ast
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def builtin(name):
    source = (ROOT / 'src/main.cpp').read_text().split('const char *' + name + ' =', 1)[1]
    parts = []
    for line in source.splitlines():
        match = re.fullmatch(r'\s*("(?:[^"\\]|\\.)*")(;?)\s*', line)
        if match:
            parts.append(ast.literal_eval(match[1]))
            if match[2]:
                break
    return ''.join(parts)


CHECKS = r'''
local function color(spans, row, col)
  local result
  for _, s in ipairs(spans) do
    if s.row == row and col >= s.col_start and col < s.col_end then result = s.hl end
  end
  return result
end
local function plain(text, ft)
  local raw, spans = mep.lsp_hover_content({kind='plaintext', value=text}, ft or 'py')
  assert(raw == text, 'hover text changed')
  return spans
end

local spans = plain('def greet(name: str, count=2) -> str\n\nReturns True if this is for you.')
assert(color(spans, 1, 1) == 'Purple')
assert(color(spans, 1, 5) == 'Blue')
assert(color(spans, 1, 11) == 'Red')
assert(color(spans, 1, 17) == 'Orange')
assert(color(spans, 1, 28) == 'Cyan')
assert(color(spans, 3, 9) == nil, 'prose was parsed as Python')
spans = plain('for  keyword\n\nRepeatedly run a block.')
assert(color(spans, 1, 1) == 'Purple')
spans = plain('def numpy.array(value, dtype=None)\n\nCreate an array.')
assert(color(spans, 1, 5) == 'Blue', 'qualified interpreter signature')
assert(color(spans, 1, 30) == 'Cyan')
spans = plain('async def fetch(\n    url: str,\n    timeout=10\n) -> str\n\nFetch the URL.')
assert(color(spans, 1, 11) == 'Blue')
assert(color(spans, 2, 5) == 'Red')
assert(color(spans, 3, 13) == 'Cyan')
assert(color(spans, 6, 1) == nil)

spans = plain('run()\n\nExamples\n--------\n  >>> if True:\n  ...     print("hello")\nFalse\n\nUse `None` for the default.')
assert(color(spans, 3, 1) == 'PickerTitle')
assert(color(spans, 5, 3) == 'Comment')
assert(color(spans, 5, 7) == 'Purple')
assert(color(spans, 5, 10) == 'Cyan')
assert(color(spans, 6, 11) == 'Blue')
assert(color(spans, 6, 18) == 'Green')
assert(color(spans, 7, 1) == nil, 'doctest output was parsed as code')
assert(color(spans, 9, 6) == 'Green')

local md = '**Bold** and `literal`\n\n```python\nif True:\n    print("yes")\n```'
local raw
raw, spans = mep.lsp_hover_content({kind='markdown', value=md}, 'py')
assert(raw == md)
assert(color(spans, 1, 3) == 'Yellow')
assert(color(spans, 1, 15) == 'Green')
assert(color(spans, 4, 1) == 'Purple')
assert(color(spans, 4, 4) == 'Cyan')
assert(color(spans, 5, 5) == 'Blue')
assert(color(spans, 5, 12) == 'Green')
raw, spans = mep.lsp_hover_content({{language='python', value='print(42)'}, 'Description'}, 'py')
assert(raw == '```python\nprint(42)\n```\n\nDescription')
assert(color(spans, 2, 1) == 'Blue')
assert(color(spans, 2, 7) == 'Cyan')
raw, spans = mep.lsp_hover_content({kind='markdown', value='~~~unknown\nanything\n~~~~'}, 'py')
assert(color(spans, 2, 1) == 'Normal', 'unknown grammar must fall back to plain text')
assert(#plain('An ordinary plain text hover.', 'cpp') == 0)
assert(#plain('') == 0)
-- All these shapes are valid LSP hover contents, including a single MarkedString.
raw, spans = mep.lsp_hover_content({language='python', value='print(42)'}, 'py')
assert(color(spans, 2, 1) == 'Blue')
raw, spans = mep.lsp_hover_content('**Bold**', 'py')
assert(color(spans, 1, 3) == 'Yellow')

-- Drive the real hover callback, forwarding its spans to the native binding.
local native_show = mep.hover_show
local shown, requests = nil, 0
function mep.cursor() return 1, 1 end
function mep.filename() return 'example.py' end
function mep.lsp_client_for() return 1 end
function mep_lsp_filetype(path) return path:match('%.([^%.]+)$') end
function mep_lsp_uri(path) return 'file://' .. path end
function mep_lsp_position() return {line=0, character=0} end
function mep_lsp_result(msg) return msg.result end
function mep.hover_show(title, text, sticky, colors)
  shown = {text=text, spans=colors}
  native_show(title, text, sticky, colors)
end
function mep.lsp_request(_, method, params, callback)
  requests = requests + 1
  assert(method == 'textDocument/hover')
  callback({result={contents={kind='plaintext', value='def example(x=42)\n\nDescription.'}}})
end
mep.lsp_hover()
assert(mep.hover_is_open())
assert(color(shown.spans, 1, 1) == 'Purple')
mep.hover_close()
function mep.filename() return 'example.org' end
function mep_polyglot_context_at_cursor()
  return {client=1, uri='file:///tmp/shadow.py', position={line=0, character=0}}
end
mep.lsp_hover()
assert(color(shown.spans, 1, 1) == 'Purple', 'embedded Python must use its own filetype')
local focused = false
function mep.hover_focus_enter() focused = true end
mep.lsp_hover()
mep.lsp_hover()
assert(focused and requests == 3, 'repeated K must still focus the popup')
native_show('Plain', 'No spans')
assert(mep.hover_is_open())
mep.hover_close()
print('Hover highlighting checks passed')
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mep', default=str(ROOT / 'build/native/mep'))
    args = parser.parse_args()
    syntax = builtin('kBuiltinSyntax')
    mapping = 'mep.ts_capture_hl = {' + syntax.split('mep.ts_capture_hl = {', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
    header = (ROOT / 'src/hover_highlight_lua.h').read_text()
    helper = header.split('R"LUA(', 1)[1].split(')LUA"', 1)[0]
    lsp = builtin('kBuiltinLsp')
    hover = 'function mep.lsp_hover()' + lsp.split('function mep.lsp_hover()', 1)[1].split('function mep.lsp_goto_definition()', 1)[0]
    with tempfile.TemporaryDirectory(prefix='mep-hover-') as directory:
        script = Path(directory) / 'check.lua'
        script.write_text(mapping + helper + hover + CHECKS)
        subprocess.run([args.mep, '--run-lua', str(script)], check=True, cwd=ROOT, timeout=30)


if __name__ == '__main__':
    main()
