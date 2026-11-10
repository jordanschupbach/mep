"""Check native project saving and picker reload in isolated Windows profiles.

Usage: python tools/project_persistence_check.py [build/win/mep.exe]
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
exe = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build/win/mep.exe").resolve()
source = (root / "src/main.cpp").read_text(encoding="utf-8")
start = source.index('    "function mep.projects()\\n"')
end = source.index('    "mep.command(', start)
# The embedded function is interleaved with C++ comments.
picker = "".join(json.loads(line.strip()) for line in source[start:end].splitlines()
                 if line.strip().startswith('"'))
with tempfile.TemporaryDirectory(prefix="mep-project-save-") as tmp:
    tmp = Path(tmp)
    project = tmp / "Project with spaces"
    project.mkdir()
    script = tmp / "check.lua"
    env = os.environ.copy()
    for key in ("HOME", "XDG_DATA_HOME", "USERPROFILE"):
        env.pop(key, None)
    def run(code, settings):
        script.write_text(code, encoding="utf-8")
        subprocess.run([str(exe), "--run-lua", str(script)], env=env | settings, check=True)
    quoted = json.dumps(str(project))
    check = "local p = " + quoted + "\nassert(#mep.project_list() == 1)\nassert(mep.project_list()[1] == p)\n"
    for key in ("USERPROFILE", "HOME", "XDG_DATA_HOME"):
        profile = tmp / key
        settings = {key: str(profile)}
        run("mep.project_add(" + quoted + ")\n" + check, settings)
        saved = profile / ("mep" if key == "XDG_DATA_HOME" else ".local/share/mep") / "projects.json"
        assert json.loads(saved.read_text())["projects"] == [str(project)]
        # A fresh process must see the saved project and put it in the picker.
        run(check + r'''
local native = mep
local items
mep = setmetatable({
  picker_open = function(_, rows) items = rows end,
  picker_set_hint = function() end,
  picker_set_preview = function() end,
  picker_preview_file = function() end,
}, {__index = native})
function mep_project_basename(p) return p:match('[^/]+$') end
''' + "\nassert(load([====[" + picker + "]====]))()\nmep.projects()\nassert(items[1].data == p)\n", settings)
    blocked = tmp / "blocked"
    blocked.write_text("not a directory")
    run("assert(mep.project_add(" + quoted + ") == false)\n"
        "assert(#mep.project_list() == 0)\n",
        {"USERPROFILE": str(blocked)})
print("Project persistence checks passed (USERPROFILE, HOME, XDG, restart, picker, write failure)")
