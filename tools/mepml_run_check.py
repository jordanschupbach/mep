#!/usr/bin/env python3
"""Check the real embedded MEPML runner with controlled asynchronous jobs/time."""
import argparse
import ast
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def runner_lua():
    source = (ROOT / 'src/main.cpp').read_text().split('const char *kBuiltinMepml =', 1)[1]
    parts = []
    for line in source.splitlines():
        match = re.fullmatch(r'\s*("(?:[^"\\]|\\.)*")(;?)\s*', line)
        if match:
            parts.append(ast.literal_eval(match[1]))
            if match[2]:
                break
    return ''.join(parts)


SETUP = r'''
local function noop() end
now, notices, blocks, jobs, splices = 100, {}, {}, {}, {}
mep = setmetatable({opt={}}, {__index=function() return noop end})
function mep.clock() return now end
function mep.now() return 0 end -- Deliberately frozen, as inside one frame.
function mep.notify(text, level) notices[#notices+1] = {text=text, level=level or 'info'} end
function mep.current_buffer() return 1 end
function mep.filename() return '/project/test.mepml' end
function mep.mepml_block_at(row) return blocks[row] end
function mep.mepml_splice_results(buf, fence, code, text)
  splices[#splices+1]={buf=buf, fence=fence, text=text}
  now = now + 0.025
  return not changed
end
function mep_org_babel_resolve_lang() return {compiled=true}, 'interpreter' end
function mep_org_babel_prepare_script(_, _, _, lines) return lines end
function mep_org_babel_spawn(_, _, _, _, callback) jobs[#jobs+1]=callback end
function mep_org_babel_prepare_exec(_, _, _, _, _, callback) prepare=callback end
function mep.mepml_terminal_start(...) launched=table.pack(...) return 1 end
function mep.mepml_gui_start(...) launched=table.pack(...) return 1 end
function mep.mepml_web_start(...) launched=table.pack(...) return 1 end
'''

CHECKS = r'''
local function reset(lang, options, code)
  now, notices, jobs, splices, launched, result, changed = 100, {}, {}, {}, nil, nil, false
  blocks = {[7]={lang=lang or 'bash', code=code or 'echo result', fence_row=7, options=options or {}, option_list={}}}
end
local function done(status) result=status end
local function near(value, expected) assert(math.abs(value-expected)<0.00001, tostring(value)) end
local function last() return notices[#notices] end

reset()
mep.mepml_run_block_at(7, done)
assert(not result and #splices==0 and #notices==1) -- Nothing finished at launch.
now=101.25; jobs[1](0, {'result'}, {})
assert(result.status=='ok' and #splices==1)
near(result.seconds,1.275)
assert(last().text=='Done running bash block at line 7 in 1.275 s')

reset('mep-lua', {}, 'now=now+0.25; return "synchronous result"')
mep.mepml_run_block_at(7, done)
assert(result.status=='ok' and splices[1].text=='synchronous result')
near(result.seconds,0.275) -- A per-frame or CPU clock must not be used.

reset('mep-lua', {}, 'error("broken")')
mep.mepml_run_block_at(7, done)
assert(result.status=='failed' and last().level=='warn' and last().text:find('failed after',1,true))
reset('mep-lua', {}, 'this is not Lua')
mep.mepml_run_block_at(7, done)
assert(result.status=='failed' and result.reason=='compile error')

reset()
mep.mepml_run_block_at(7, done)
now=100.75; jobs[1](3, {}, {'broken'}, 'compile')
assert(result.status=='failed' and result.output=='broken' and last().level=='warn')
assert(last().text:find('compile exited with 3',1,true))
near(result.seconds,0.775)
reset()
mep.mepml_run_block_at(7, done, {keep_failed=true})
now=100.75; jobs[1](1, {}, {'broken'})
assert(result.status=='failed' and #splices==0); near(result.seconds,0.75)

reset()
mep.mepml_run_block_at(7, done)
changed=true; now=101; jobs[1](0, {'result'}, {})
assert(result.status=='failed' and last().text:find('results discarded',1,true))
assert(not last().text:find('Done running',1,true))

reset('bash',{eval=false})
mep.mepml_run_block_at(7, done)
assert(result.status=='skipped' and result.seconds==nil and #jobs==0)
assert(not last().text:find('Done running',1,true))

reset()
local first, second
mep.mepml_run_block_at(7, function(s) first=s end)
blocks[20]={lang='bash',code='second',fence_row=20,options={},option_list={}}
now=101; mep.mepml_run_block_at(20, function(s) second=s end)
now=103; jobs[2](0, {'second'}, {})
near(second.seconds,2.025); assert(not first)
now=104; jobs[1](0, {'first'}, {})
near(first.seconds,4.025)

-- Interactive launch callbacks retain their existing chaining behavior;
-- native completion receives the original clock, including compilation.
for _, options in ipairs({{results='exec'},{results='exec-gui'},{results='web'}}) do
  reset('cpp',options)
  mep.mepml_run_block_at(7,done)
  assert(not launched and not result)
  now=102; prepare({'program'}, {})
  assert(launched and launched[options.results=='web' and 6 or 5]==100)
  for _, notice in ipairs(notices) do assert(not notice.text:find('Done running',1,true)) end
end
reset('cpp',{results='exec'})
mep.mepml_run_block_at(7,done)
now=102; prepare(nil,{'compiler error'},'compile')
assert(result.status=='failed' and last().text:find('2.025 s',1,true))
reset('cpp',{results='web'})
mep.mepml_run_block_at(7,done)
now=102; prepare(nil,{'compiler error'},'compile')
assert(result.status=='failed' and last().text:find('2.025 s',1,true))
reset('exec')
mep.mepml_run_block_at(7,done)
assert(launched[5]==100 and result.status=='ok' and #notices==0)
print('MEPML timing: asynchronous/synchronous success, errors, skipped blocks, concurrency and interactive handoff passed')
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lua', default=shutil.which('lua') or shutil.which('lua5.4'))
    args = parser.parse_args()
    if not args.lua:
        parser.error('Lua 5.4 is required')
    subprocess.run([args.lua, '-'], input=SETUP + runner_lua() + CHECKS,
                   text=True, check=True, timeout=15)


if __name__ == '__main__':
    main()
