"""Check the embedded project path picker without opening a window.

Usage: python tools/project_path_check.py [build/win/mep.exe]
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
start = source.index('    "function mep.projects_add_path(initial)\\n"')
end = source.index('    "function mep.projects()\\n"', start)
picker = "".join(json.loads(line.strip()) for line in source[start:end].splitlines())
harness = r'''
local picker_source = PICKER_SOURCE
-- Exercise native filesystem bindings before replacing the UI and filesystem.
local native = mep
local items, submit, added
local ui = {
  picker_open = function(_, _, cb) submit = cb end,
  picker_set_items = function(v) items = v end,
  picker_set_selected = function() end, picker_set_hint = function() end,
  picker_set_query = function() end, picker_set_preview = function() end,
  picker_preview_file = function() end, project_list = function() return {} end,
  project_add = function(p) added = p; return true end, projects = function() end,
  notify = function() end,
}
mep = setmetatable(ui, {__index = native})
assert(load(picker_source))()
local cwd = native.getcwd()
mep.projects_add_path(cwd)
assert(items[1].display:sub(1, 6) == '+ Add ', items[1].display)
submit('add')
assert(added == cwd:gsub(string.char(92), '/'):gsub('/+$', ''), added)
if package.config:sub(1, 1) == string.char(92) then
  mep.projects_add_path(cwd:sub(1, 3))
  assert(items[1].display:sub(1, 6) == '+ Add ', items[1].display)
end

local function check(windows)
  package.config = windows and string.char(92) or '/'
  local cwd = windows and 'C:/work' or '/work'
  local home = windows and 'C:/Users/test' or '/home/test'
  local dirs = {[cwd] = {{name = 'Demo', is_dir = true}, {name = 'file', is_dir = false}},
                [cwd .. '/Demo'] = {}, [home] = {},
                ['C:/'] = {{name = 'work', is_dir = true}},
                ['/'] = {{name = 'work', is_dir = true}},
                ['C:/Users'] = {{name = 'test', is_dir = true}},
                ['/home'] = {{name = 'test', is_dir = true}}, ['//server/share'] = {}}
  local function norm(p) return windows and p:gsub(string.char(92), '/') or p end
  local function lookup(p)
    p = norm(p):gsub('/+$', '')
    if p == '' then p = '/' end
    if p == 'C:' then p = 'C:/' end
    for k, v in pairs(dirs) do
      if (windows and k:lower() == p:lower()) or k == p then return v end
    end
  end
  local items, submit, change, key, query, added
  mep = {
    getcwd = function() return windows and 'C:\\work' or cwd end,
    list_dir = function(p) return lookup(p) or {} end,
    fs_exists = function(p) return lookup(p) ~= nil end,
    project_list = function() return {windows and 'C:\\work\\Demo' or cwd .. '/Demo'} end,
    project_readme_path = function() end,
    picker_set_preview = function() end,
    picker_set_selected = function() end,
    picker_set_hint = function() end,
    picker_set_items = function(v) items = v end,
    picker_set_query = function(v) query = v end,
    picker_open = function(_, _, cb, on_change, on_key) submit, change, key = cb, on_change, on_key end,
    project_add = function(p) added = p; return true end,
    notify = function() end,
    projects = function() end,
  }
  os.getenv = function(k)
    if k == (windows and 'USERPROFILE' or 'HOME') then return home end
  end
  assert(load(picker_source))()
  local function open(p) mep.projects_add_path(p) end
  local function display(prefix) assert(items[1].display:sub(1, #prefix) == prefix, items[1].display) end
  open(nil); display('+ Add ')
  open(windows and 'C:\\work\\demo' or '/work/Demo'); display('= already')
  open(windows and 'C:/work/Demo/' or '/work/Demo/'); display('= already')
  open('Demo'); display('= already')
  open('file'); display('! not a folder')
  open('missing'); display('! not a folder')
  open('~/'); display('+ Add '); submit('add'); assert(added == home, added)
  open(windows and 'C:\\work\\De' or '/work/De')
  assert(#items == 2, 'missing completion'); key('<Tab>'); assert(query == cwd .. '/Demo/', query)
  open(windows and 'C:\\work\\Demo\\' or '/work/Demo/')
  key('w'); assert(query == cwd .. '/', query)
  key('u'); assert(query == '', query)
  if windows then
    open('C:\\'); display('+ Add '); submit('add'); assert(added == 'C:/', added)
    open('\\\\server\\share\\'); display('+ Add '); submit('add'); assert(added == '//server/share', added)
    open('Z:\\'); display('! not a folder')
  end
end
check(true)
check(false)
print('Project path picker checks passed (Windows and Unix)')
'''
harness = harness.replace("PICKER_SOURCE", "[====[" + picker + "]====]")
with tempfile.TemporaryDirectory(prefix="mep-project-path-") as tmp:
    script = Path(tmp) / "check.lua"
    script.write_text(harness, encoding="utf-8")
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "build/win/mep.exe", "--run-lua", str(script)], check=True)
