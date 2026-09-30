-- The editor half of the mepml benchmarks (plans/MEPML_PERFORMANCE_PLAN.md):
-- what a mepml buffer costs *inside mep* -- the per-edit and per-cursor-move
-- hooks, and whole frames while idle, scrolling and typing. The headless
-- half (parser, exporters, language server, grammar) is mep-mepml-bench.
--
-- Driven by tools/mepml_editor_bench.py, which starts a throwaway mep on
-- its own X display, sets MEPML_BENCH and `:source`s this file:
--
--   MEPML_BENCH = {
--     fixtures = { {name = 'synth_1000', path = '/abs/synth_1000.mepml'}, ... },
--     out = '/abs/results.jsonl',   -- one JSON object per line
--     done = '/abs/done',           -- written last, so the driver knows
--   }
--
-- Everything runs from one frame hook as a state machine, because the
-- frame scenarios need real frames between their steps.

local cfg = MEPML_BENCH
local out = assert(io.open(cfg.out, 'w'))

local function emit(fixture, scenario, n, ms, extra)
  local line = string.format('{"fixture":"%s","scenario":"%s","n":%d,"ms":%.4f', fixture, scenario, n, ms)
  for k, v in pairs(extra or {}) do line = line .. string.format(',"%s":%.4f', k, v) end
  out:write(line .. '}\n')
  out:flush()
end

local function median(t)
  table.sort(t)
  return t[math.floor(#t / 2) + 1]
end

local function percentile(t, p)
  table.sort(t)
  return t[math.max(1, math.min(#t, math.ceil(#t * p)))]
end

-- Median wall time of `fn` over `reps` calls, in ms.
local function time_fn(fn, reps)
  fn()  -- warm
  local ms = {}
  for _ = 1, reps do
    local t = mep.clock()
    fn()
    ms[#ms + 1] = (mep.clock() - t) * 1000
  end
  return median(ms)
end

-- A prose line near the middle of the buffer, for the typing scenario to
-- edit (typing into prose is what triggers every hook at once).
local function prose_row()
  local n = mep.line_count()
  for r = math.floor(n / 2), n do
    local l = mep.get_line(r)
    if l:match('^%a') and not l:match('^>') then return r end
  end
  return 1
end

-- Each hook the editor runs for a mepml buffer, called directly. Which of
-- them fire on an edit and which on a cursor-row change is in the plan.
local hooks = {
  {'hook_mepml_render', function() mep.mepml_render() end},
  {'hook_syntax_highlight', function() mep.syntax_highlight() end},
  {'hook_mepml_folds', function() mep.mepml_folds() end},
  {'hook_org_latex_scan', function() mep.org_latex_scan() end},
  {'hook_spell_highlight', function() if mep.spell_highlight then mep.spell_highlight() end end},
  {'lua_get_all_lines', function()
    local lines = {}
    for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end
  end},
  {'ts_captures_mepml', function()
    local lines = {}
    for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end
    mep.ts_captures('mepml', table.concat(lines, '\n'))
  end},
}

-- Frames: record `count` frame intervals while `step(i)` runs once per frame
-- -- or fewer, at least 10, once 15 s have gone by (a fixture whose frames
-- take seconds has made its point long before 120 of them).
local function frame_scenario(count, step, on_done)
  local last, i, ms = nil, 0, {}
  local started = mep.clock()
  mep.on_frame(function()
    local now = mep.clock()
    if last then ms[#ms + 1] = (now - last) * 1000 end
    last = now
    i = i + 1
    if i > count or (#ms >= 10 and now - started > 15) then
      on_done(ms)
      return true
    end
    step(i)
  end)
end

-- Frames with nothing timed, for `seconds`: lets hooks that the previous
-- step's edits left due (the edit hooks' throttles and frame budget defer
-- them by up to ~0.8 s) run before the next scenario starts measuring.
local function settle(seconds, on_done)
  local started = mep.clock()
  mep.on_frame(function()
    if mep.clock() - started >= seconds then
      on_done()
      return true
    end
  end)
end

local queue = {}
local function push(f) queue[#queue + 1] = f end
local function next_step()
  local f = table.remove(queue, 1)
  if f then f() else
    out:close()
    local d = io.open(cfg.done, 'w')
    d:write('ok\n')
    d:close()
  end
end

local function report_frames(fixture, scenario, n, ms)
  emit(fixture, scenario, n, median(ms), {p95 = percentile(ms, 0.95), max = percentile(ms, 1.0), frames = #ms})
end

for _, fx in ipairs(cfg.fixtures) do
  push(function()
    mep.cmd('e ' .. fx.path)
    -- Let the open settle (first scans, LSP start) for a few frames.
    frame_scenario(30, function() end, function() next_step() end)
  end)
  push(function()
    local n = mep.line_count()
    local reps = n > 8000 and 3 or 7
    -- On unchanged text: what a cursor move or a repeat pays (the parse
    -- and span caches hit).
    for _, h in ipairs(hooks) do emit(fx.name, h[1], n, time_fn(h[2], reps)) end
    -- Right after an edit (one prose line changed before every call): what
    -- typing pays, caches missing.
    local row = prose_row()
    local base = mep.get_line(row)
    for _, h in ipairs(hooks) do
      local k = 0
      local ms = {}
      for _ = 1, reps + 1 do
        k = k + 1
        mep.set_line(row, base .. string.rep('y', k))
        local t = mep.clock()
        h[2]()
        ms[#ms + 1] = (mep.clock() - t) * 1000
      end
      table.remove(ms, 1)  -- warm-up
      emit(fx.name, 'edit_' .. h[1], n, median(ms))
    end
    mep.set_line(row, base)
    next_step()
  end)
  -- Idle: nothing changes, so a frame is the draw plus every frame hook's
  -- own "anything to do?" check.
  push(function() mep.set_cursor(1, 1) settle(1.5, next_step) end)
  push(function()
    frame_scenario(90, function() end, function(ms)
      report_frames(fx.name, 'frame_idle', mep.line_count(), ms)
      next_step()
    end)
  end)
  -- Scrolling: the cursor moves one row per frame, which re-runs the
  -- cursor-row hooks (mepml_render among them) and scrolls the view.
  push(function() mep.set_cursor(1, 1) settle(1.5, next_step) end)
  push(function()
    local n = mep.line_count()
    frame_scenario(120, function(i) mep.set_cursor(math.min(n, i + 1), 1) end, function(ms)
      report_frames(fx.name, 'frame_cursor_down', n, ms)
      next_step()
    end)
  end)
  -- Typing: one character appended to a prose line per frame, the cursor
  -- on it -- the buffer-changed hooks at their own throttles.
  push(function() settle(1.5, next_step) end)
  push(function()
    local row = prose_row()
    local base = mep.get_line(row)
    mep.set_cursor(row, 1)
    local n = mep.line_count()
    frame_scenario(120, function(i)
      mep.set_line(row, base .. string.rep('x', i))
    end, function(ms)
      mep.set_line(row, base)
      report_frames(fx.name, 'frame_typing', n, ms)
      next_step()
    end)
  end)
end

next_step()
