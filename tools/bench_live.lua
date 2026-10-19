-- The live-editor tier of the benchmark suite (plans/PERFORMANCE_PLAN.md):
-- what a buffer costs *inside a running mep*, with every built-in hook,
-- the highlighter and the renderer in play. The headless tiers
-- (mep-bench and friends) cannot see any of that.
--
-- Driven by tools/bench_live.py, which starts a throwaway mep per fixture,
-- sets BENCH_LIVE and `:source`s this file:
--
--   BENCH_LIVE = {
--     fixture = {name = 'cpp_60k', path = '/abs/cpp_60k.cpp'},
--     out = '/abs/results.jsonl',   -- one JSON object per line
--     done = '/abs/done',           -- written last, so the driver knows
--     quick = false,
--   }
--
-- Frames are timed with mep.frame_stats(), which reports the CPU work of
-- the last frame (hooks + input + draw, without the vsync wait), so the
-- numbers do not depend on the display's refresh rate or on idle
-- throttling. Frame scenarios report the mean work per frame (with
-- median/p95/max, and the mean hooks/draw split, alongside). Everything runs from frame hooks as a state machine,
-- because the frame scenarios need real frames between their steps.

local cfg = BENCH_LIVE
local fx = cfg.fixture
local out = assert(io.open(cfg.out, 'w'))
local quick = cfg.quick

local function emit(scenario, n, ms, extra)
  local line = string.format('{"fixture":"%s","scenario":"%s","n":%d,"ms":%.4f', fx.name, scenario, n, ms)
  for k, v in pairs(extra or {}) do line = line .. string.format(',"%s":%.4f', k, v) end
  out:write(line .. '}\n')
  out:flush()
end

local function sorted(t)
  local c = {}
  for i, v in ipairs(t) do c[i] = v end
  table.sort(c)
  return c
end

local function median(t)
  local s = sorted(t)
  return s[math.floor(#s / 2) + 1] or 0
end

local function percentile(t, p)
  local s = sorted(t)
  return s[math.max(1, math.min(#s, math.ceil(#s * p)))] or 0
end

-- Median wall time of `fn` over `reps` calls, in ms (one warm-up first),
-- and the p95 as a table for emit's `extra`.
local function time_fn(fn, reps)
  fn()
  local ms = {}
  for _ = 1, reps do
    -- A full collection outside the timed region: these calls make a lot
    -- of garbage, and a collection landing inside one rep or another made
    -- identical runs differ by 20-80%.
    collectgarbage('collect')
    local t = mep.clock()
    fn()
    ms[#ms + 1] = (mep.clock() - t) * 1000
  end
  return median(ms), {p95 = percentile(ms, 0.95), min = percentile(ms, 0)}
end

-- Times `count` frames while `step(i)` runs once per frame. Each sample is
-- the *previous* frame's stats (the hook runs at the start of a frame).
local function frame_scenario(count, step, on_done)
  local samples = {work = {}, hooks = {}, draw = {}}
  local i = 0
  local last_frame = nil
  local started = mep.clock()
  mep.on_frame(function()
    local s = mep.frame_stats()
    if last_frame and s.frames ~= last_frame then
      samples.work[#samples.work + 1] = s.work
      samples.hooks[#samples.hooks + 1] = s.hooks
      samples.draw[#samples.draw + 1] = s.draw
    end
    last_frame = s.frames
    i = i + 1
    if i > count or (#samples.work >= 10 and mep.clock() - started > 20) then
      on_done(samples)
      return true
    end
    step(i)
  end)
end

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

local function mean(t)
  local sum = 0
  for _, v in ipairs(t) do sum = sum + v end
  return #t > 0 and sum / #t or 0
end

-- The headline is the *mean* CPU work per frame: hooks fire on throttles
-- (every few frames), so a median either misses them entirely or not
-- depending on the frame rate -- the mean charges their cost fairly.
local function report_frames(scenario, n, s)
  emit(scenario, n, mean(s.work), {
    median = median(s.work),
    p95 = percentile(s.work, 0.95),
    max = percentile(s.work, 1.0),
    hooks = mean(s.hooks),
    draw = mean(s.draw),
    frames = #s.work,
  })
end

-- A row in the middle of the buffer with some text on it, for edits.
local function edit_row()
  local n = mep.line_count()
  for r = math.floor(n / 2), n do
    if #mep.get_line(r) > 10 then return r end
  end
  return 1
end

local frames = quick and 40 or 120

-- Profile mode (tools/bench_live.py --profile FIXTURE:SCENARIO): open the
-- fixture, then repeat one scenario's per-frame step for cfg.seconds while
-- the driver samples the process, instead of running the timed scenarios.
if cfg.loop then
  local steps = {
    idle = function() end,
    scroll = function(i) mep.set_cursor(math.min(mep.line_count(), (i * 20) % mep.line_count() + 1), 1) end,
    cursor_down = function(i) mep.set_cursor(math.min(mep.line_count(), i % mep.line_count() + 1), 1) end,
    typing = function() mep.insert_text('x') end,
    highlight = function() mep.syntax_highlight() end,
  }
  local step = assert(steps[cfg.loop], 'unknown profile scenario ' .. tostring(cfg.loop))
  mep.cmd('e ' .. fx.path)
  if cfg.loop == 'typing' then
    local n = mep.line_count()
    for r = math.floor(n / 2), n do
      if #mep.get_line(r) > 10 then mep.set_cursor(r, 1) break end
    end
  end
  settle(1.5, function()
    local ready = io.open(cfg.done .. '.ready', 'w')
    ready:write('ok\n')
    ready:close()
    local started = mep.clock()
    local i = 0
    if mep.hook_profile then mep.hook_profile(true) end
    mep.on_frame(function()
      i = i + 1
      step(i)
      if mep.clock() - started >= cfg.seconds then
        -- The frame hooks' own costs (where each was defined, total and worst).
        if mep.hook_stats then
          for k, s in ipairs(mep.hook_stats()) do
            if k > 25 then break end
            out:write(string.format('%9.1f ms total %7.2f ms max %6d calls  %s\n', s.total_ms, s.max_ms, s.calls, s.where))
          end
          mep.hook_profile(false)
        end
        out:close()
        local d = io.open(cfg.done, 'w')
        d:write('ok\n')
        d:close()
        return true
      end
    end)
  end)
  return
end

push(function()
  local t = mep.clock()
  mep.cmd('e ' .. fx.path)
  emit('open_cmd', mep.line_count(), (mep.clock() - t) * 1000)
  -- The open's first frames: the first highlight pass, LSP start, scans.
  frame_scenario(20, function() end, function(s)
    report_frames('frame_after_open', mep.line_count(), s)
    next_step()
  end)
end)

-- The hooks every edit pays for, called directly.
push(function()
  local n = mep.line_count()
  local reps = quick and 5 or 11
  emit('hook_syntax_highlight_unchanged', n, time_fn(function() mep.syntax_highlight() end, reps))
  emit('lua_get_all_lines', n, time_fn(function()
    local lines = {}
    for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end
  end, reps))
  local row = edit_row()
  local base = mep.get_line(row)
  local k = 0
  emit('hook_syntax_highlight_after_edit', n, time_fn(function()
    k = k + 1
    mep.set_line(row, base .. string.rep('y', k % 7 + 1))
    mep.syntax_highlight()
  end, reps))
  emit('set_line', n, time_fn(function()
    k = k + 1
    mep.set_line(row, base .. string.rep('y', k % 7 + 1))
  end, reps * 3))
  mep.set_line(row, base)
  next_step()
end)

-- Idle: nothing changes, so a frame is the draw plus every hook's "anything to do?".
push(function() mep.set_cursor(1, 1) settle(1.5, next_step) end)
push(function()
  frame_scenario(quick and 30 or 60, function() end, function(s)
    report_frames('frame_idle', mep.line_count(), s)
    next_step()
  end)
end)

-- Scrolling: the cursor moves down one screen-ish per frame (like holding Ctrl-D).
push(function() mep.set_cursor(1, 1) settle(1.0, next_step) end)
push(function()
  local n = mep.line_count()
  frame_scenario(frames, function(i) mep.set_cursor(math.min(n, i * 20 + 1), 1) end, function(s)
    report_frames('frame_scroll', n, s)
    next_step()
  end)
end)

-- Cursor down one row per frame (like holding j).
push(function() mep.set_cursor(1, 1) settle(1.0, next_step) end)
push(function()
  local n = mep.line_count()
  frame_scenario(frames, function(i) mep.set_cursor(math.min(n, i + 1), 1) end, function(s)
    report_frames('frame_cursor_down', n, s)
    next_step()
  end)
end)

-- Typing: one character inserted at the cursor per frame, mid-file -- every
-- buffer-changed hook (syntax, LSP, spell, folds...) at its own throttle.
push(function()
  local row = edit_row()
  mep.set_cursor(row, 1)
  settle(1.5, next_step)
end)
push(function()
  local n = mep.line_count()
  frame_scenario(frames, function() mep.insert_text('x') end, function(s)
    report_frames('frame_typing', n, s)
    next_step()
  end)
end)
-- The frames right after typing stops: the deferred hooks catching up.
push(function()
  frame_scenario(quick and 20 or 40, function() end, function(s)
    report_frames('frame_after_typing', mep.line_count(), s)
    next_step()
  end)
end)

next_step()
