// Lua language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .lua
// buffer), kBuiltinLanguageUi's Lua consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Bash and SQL
// modes. Included by main.cpp only; BuiltinLanguageUiLua() is the Lua chunk
// (help/lua-mode.org is the user's page).
//
// The console is the stock interactive interpreter -- `lua -i mep_init.lua`
// (mep.opt.lua_ui_cmd, default {'lua'}; a `-- REPL:` comment replaces it via
// mep.language_ui_cmd) -- so it keeps lua.c's own readline line editing,
// history and expression printing. It is spawned like every other console
// (mep.terminal_here_argv -> execvp), inheriting mep's whole environment:
// PATH, LUA_PATH/LUA_CPATH and LUA_INIT from the dev shell or direnv, so
// `require` resolves exactly as it would in a shell in that project.
//
// The init script (mep_lua_ui_init_template below) defines mep_eval,
// mep_run, mep_help and mep_view, and hooks the prompt: lua.c (5.4) prints
// _PROMPT through luaL_tolstring, so a table whose __tostring rewrites the
// panel files runs before every prompt -- the Bash mode's PROMPT_COMMAND,
// with no REPL of our own. (5.1-5.3 and LuaJIT read _PROMPT with
// lua_tostring, which ignores __tostring: there the panels refresh after
// each mep_* call only.) As in the other modes, nothing reads the console's
// output; the panels are files the poll loop re-reads on a timer.
//
// Sending code. Pasting lines into the stock REPL (mod1+Enter) runs them one
// line at a time -- an incomplete chunk continues at `>>` -- and every line
// is its own chunk, so a `local` from one line is gone on the next. So
// <leader>re and <leader>rr never paste code: they write it to a file in the
// session directory and type one `mep_eval(path, chunkname)` /
// `mep_run(...)` line, which loads the whole text as a single chunk named
// after the source file (padded so line numbers match it -- errors and
// debug.getinfo point at the real file and line). Top-level `local`
// declarations (at column 0, outside long strings) are rewritten to global
// assignments first (mep.opt.lua_ui_globalize_locals, on by default), so
// what a block or file defines stays reachable from the console and shows
// up in the Globals panel -- the R/Python "source into the session"
// behavior. The rewrite is line-preserving and leaves indented locals (in
// functions, do-blocks) alone.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiLua() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.lua_ui_cmd = mep.opt.lua_ui_cmd or {'lua'}
mep.opt.lua_ui_globalize_locals = mep.opt.lua_ui_globalize_locals ~= false
mep.opt.lua_ui_info_share = mep.opt.lua_ui_info_share or 0.3
mep.opt.lua_ui_bottom_share = mep.opt.lua_ui_bottom_share or 0.4
mep.opt.lua_ui_bottom_info_share = mep.opt.lua_ui_bottom_info_share or 0.4
mep.language_ui_extensions.lua = mep.language_ui_extensions.lua or 'lua'

-- The console's init script. Lua puts one line above this naming the
-- session directory (local __mep_dir = '...'). Written for 5.1-5.4 and
-- LuaJIT alike, since a REPL: comment may pick any of them.
local mep_lua_ui_init_template = [==[
local dir = __mep_dir
local load_chunk = loadstring or load

local function write(name, text)
  local f = io.open(dir .. '/' .. name, 'w')
  if f then f:write(text) f:close() end
end

-- What exists before the user types anything is the interpreter's (and
-- LUA_INIT's); the panels show what the session adds.
local base_globals, base_modules = {}, {}
for k in pairs(_G) do base_globals[k] = true end
for k in pairs(package.loaded) do base_modules[k] = true end
local own = {mep_eval = true, mep_run = true, mep_help = true, mep_view = true, _PROMPT = true, _PROMPT2 = true}

local function safe_tostring(v)
  local ok, s = pcall(tostring, v)
  return ok and tostring(s) or ('<' .. type(v) .. '>')
end

local function count_keys(t)
  local n = 0
  for _ in pairs(t) do
    n = n + 1
    if n >= 100000 then break end
  end
  return n
end

local function short(v, width)
  width = width or 60
  local t, s = type(v), nil
  if t == 'string' then
    s = string.format('%q', v)
  elseif t == 'table' then
    local mt = getmetatable(v)
    if type(mt) == 'table' and mt.__tostring then
      s = safe_tostring(v)
    else
      local n, len = count_keys(v), #v
      s = (len > 0 and len == n) and (len .. ' items') or (n .. ' keys')
    end
  elseif t == 'function' then
    local info = debug.getinfo(v, 'S')
    if info.what == 'C' then s = 'C function'
    else s = info.short_src .. ':' .. info.linedefined end
  else
    s = safe_tostring(v)
  end
  s = s:gsub('[\r\n]+', ' ')
  if #s > width then s = s:sub(1, width - 3) .. '...' end
  return s
end

local function sorted_keys(t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b)
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta == 'number' end
    if ta == 'number' or ta == 'string' then return a < b end
    return safe_tostring(a) < safe_tostring(b)
  end)
  return keys
end

local function refresh()
  local rows = {}
  for _, k in ipairs(sorted_keys(_G)) do
    if type(k) == 'string' and not base_globals[k] and not own[k] then
      local v = rawget(_G, k)
      rows[#rows + 1] = string.format('%-22s %-9s %s', k, type(v), short(v))
    end
  end
  write('globals.txt', table.concat(rows, '\n') .. (#rows > 0 and '\n' or ''))
  rows = {}
  local pre = {}
  for _, k in ipairs(sorted_keys(package.loaded)) do
    if type(k) == 'string' then
      if base_modules[k] then pre[#pre + 1] = k
      else rows[#rows + 1] = string.format('%-22s %s', k, short(package.loaded[k])) end
    end
  end
  if #pre > 0 then rows[#rows + 1] = '(preloaded: ' .. table.concat(pre, ' ') .. ')' end
  write('modules.txt', table.concat(rows, '\n') .. '\n')
end

local user_prompt = _PROMPT
_PROMPT = setmetatable({}, {__tostring = function()
  pcall(refresh)
  return user_prompt and safe_tostring(user_prompt) or '> '
end})

-- name -> value, through dots or a method colon, from the globals.
local function resolve(name)
  local v = _G
  for part in tostring(name):gmatch('[^.:]+') do
    if type(v) ~= 'table' and type(v) ~= 'userdata' then return false end
    local ok, nv = pcall(function() return v[part] end)
    if not ok or nv == nil then return false end
    v = nv
  end
  return true, v
end

local function signature(name, f)
  local params = {}
  local ok, info = pcall(debug.getinfo, f, 'u')
  if ok and info and info.nparams then
    for i = 1, info.nparams do
      local got, pname = pcall(debug.getlocal, f, i)
      params[#params + 1] = (got and pname) or ('arg' .. i)
    end
    if info.isvararg then params[#params + 1] = '...' end
  elseif ok then
    params[1] = '...'
  end
  return name .. '(' .. table.concat(params, ', ') .. ')'
end

local function read_lines(path)
  local f = io.open(path, 'r')
  if not f then return nil end
  local lines = {}
  for l in f:lines() do lines[#lines + 1] = l end
  f:close()
  return lines
end

local stdlib = {string = true, table = true, math = true, io = true, os = true, coroutine = true,
  utf8 = true, debug = true, package = true}
local function manual_link(name)
  local version = (_VERSION or ''):match('%d+%.%d+')
  if not version then return nil end
  local head = name:match('^[^.:]+')
  local std = (stdlib[head] and base_modules[head]) or (not name:find('[.:]') and base_globals[name])
  if not std then return nil end
  return 'https://www.lua.org/manual/' .. version .. '/manual.html#pdf-' .. name:gsub(':', '.')
end

local function describe_fields(out, t, title)
  local funcs, others = {}, {}
  for _, k in ipairs(sorted_keys(t)) do
    local ok, v = pcall(function() return t[k] end)
    if ok then
      if type(v) == 'function' then funcs[#funcs + 1] = signature(tostring(k), v)
      else others[#others + 1] = string.format('  %-20s %-9s %s', safe_tostring(k), type(v), short(v)) end
    end
    if #funcs + #others > 400 then break end
  end
  if #funcs > 0 then
    out[#out + 1] = ''
    out[#out + 1] = title .. ' functions:'
    for _, s in ipairs(funcs) do out[#out + 1] = '  ' .. s end
  end
  if #others > 0 then
    out[#out + 1] = ''
    out[#out + 1] = title .. ' fields:'
    for _, s in ipairs(others) do out[#out + 1] = s end
  end
end

local function help_text(name)
  local found, v = resolve(name)
  if not found then
    return name .. ': not a global in this console -- a local, or not loaded yet.\n' ..
      'K over it in the source shows what the language server knows.\n'
  end
  local out = {name .. '  (' .. type(v) .. ')'}
  if type(v) == 'function' then
    out[1] = signature(name, v)
    local info = debug.getinfo(v, 'S')
    if info.what == 'C' then
      out[#out + 1] = ''
      out[#out + 1] = 'A C function: there is no Lua source to show.'
      out[#out + 1] = 'K over it in the source shows its documentation from the language server.'
      local link = manual_link(name)
      if link then out[#out + 1] = 'Reference manual: ' .. link end
    elseif info.source:sub(1, 1) == '@' then
      local file = info.source:sub(2)
      out[#out + 1] = 'defined at ' .. file .. ':' .. info.linedefined
      local lines = read_lines(file)
      if lines and lines[info.linedefined] then
        local s = info.linedefined
        while s > 1 and lines[s - 1]:match('^%s*%-%-') do s = s - 1 end
        local e = math.min(info.lastlinedefined, info.linedefined + 80)
        out[#out + 1] = ''
        for i = s, e do out[#out + 1] = lines[i] end
        if e < info.lastlinedefined then out[#out + 1] = '...' end
      end
    else
      out[#out + 1] = 'defined in the console'
    end
  elseif type(v) == 'table' then
    out[#out + 1] = short(v)
    describe_fields(out, v, 'Its')
    local mt = getmetatable(v)
    if type(mt) == 'table' and type(rawget(mt, '__index')) == 'table' and rawget(mt, '__index') ~= v then
      describe_fields(out, rawget(mt, '__index'), 'Inherited (metatable __index)')
    end
  elseif type(v) == 'userdata' then
    out[#out + 1] = safe_tostring(v)
    local mt = getmetatable(v)
    if type(mt) == 'table' then
      if rawget(mt, '__name') then out[#out + 1] = 'type: ' .. tostring(rawget(mt, '__name')) end
      if type(rawget(mt, '__index')) == 'table' then describe_fields(out, rawget(mt, '__index'), 'Its') end
      describe_fields(out, mt, 'Metatable')
    end
  else
    out[#out + 1] = short(v, 400)
  end
  local link = type(v) ~= 'function' and manual_link(name)
  if link then out[#out + 1] = '' out[#out + 1] = 'Reference manual: ' .. link end
  return table.concat(out, '\n') .. '\n'
end

function mep_help(name)
  local ok, text = pcall(help_text, name)
  write('help.txt', ok and text or (tostring(name) .. ': ' .. tostring(text) .. '\n'))
  print('-- ' .. tostring(name) .. ': in the Help tab')
end

local function dump(v, out, indent, depth, seen)
  if type(v) ~= 'table' then return end
  if seen[v] then out[#out + 1] = indent .. '(seen above)' return end
  seen[v] = true
  for _, k in ipairs(sorted_keys(v)) do
    if #out > 1000 then out[#out + 1] = indent .. '...' return end
    local ok, x = pcall(function() return v[k] end)
    if ok then
      out[#out + 1] = string.format('%s[%s] %s', indent, type(k) == 'string' and k or safe_tostring(k),
        type(x) == 'table' and short(x) or (type(x) .. ' ' .. short(x, 200)))
      if type(x) == 'table' and depth > 1 then dump(x, out, indent .. '  ', depth - 1, seen) end
    end
  end
end

-- mep_view('name') (or any value): the value, key by key, in the Inspect tab.
function mep_view(x)
  local label, v = safe_tostring(x), x
  if type(x) == 'string' then
    local found, rv = resolve(x)
    if found then v = rv else label = string.format('%q', x) end
  end
  local out = {label .. '  (' .. type(v) .. ')', short(v, 400)}
  if type(v) == 'table' then out[#out + 1] = '' dump(v, out, '', 2, {}) end
  write('inspect.txt', table.concat(out, '\n') .. '\n')
end

-- An error's traceback down to the sent code: the frames from xpcall on
-- are this file's and the REPL's own.
local function traceback(e)
  local tb = debug.traceback(safe_tostring(e), 2)
  local kept = {}
  for line in (tb .. '\n'):gmatch('([^\n]*)\n') do
    if line:find('mep_init.lua', 1, true) or line:find("in function 'xpcall'", 1, true) then break end
    kept[#kept + 1] = line
  end
  return table.concat(kept, '\n')
end

local function report(ok, ...)
  if not ok then
    io.stderr:write(tostring((...)) .. '\n')
  elseif select('#', ...) > 0 then
    print(...)
  end
end

local function read_file(path)
  local f, err = io.open(path, 'rb')
  if not f then return nil, err end
  local s = f:read('*a')
  f:close()
  return s
end

-- One chunk, the way the REPL itself would take it: an expression's values
-- are printed, statements just run.
local function run_text(code, chunkname)
  local fn = load_chunk('return ' .. code, chunkname)
  local err
  if not fn then fn, err = load_chunk(code, chunkname) end
  if not fn then
    io.stderr:write(tostring(err) .. '\n')
  else
    report(xpcall(fn, traceback))
  end
  pcall(refresh)
end

-- <leader>re: a block of the source file, padded to its own lines.
function mep_eval(path, chunkname)
  local code, err = read_file(path)
  if not code then io.stderr:write(tostring(err) .. '\n') return end
  run_text(code, chunkname or ('@' .. path))
end

-- <leader>rr: a whole file in this session (dofile, keeping what it defines
-- when mep rewrote its top-level locals), its return values printed.
function mep_run(path, chunkname)
  local code, err = read_file(path)
  if not code then io.stderr:write(tostring(err) .. '\n') return end
  code = code:gsub('^#![^\n]*', '--')
  local fn
  fn, err = load_chunk(code, chunkname or ('@' .. path))
  if not fn then io.stderr:write(tostring(err) .. '\n') pcall(refresh) return end
  report(xpcall(fn, traceback))
  pcall(refresh)
end

pcall(refresh)
print(_VERSION .. ' -- mep language UI: <leader>re runs the block at the cursor, <leader>rr the file; mep_help(\'name\') / gh shows help, mep_view(\'name\') inspects.')
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    globals = mep.sidebar_create('Globals', 'right', 44),
    modules = mep.sidebar_create('Modules', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

local function luaquote(s)
  return string.format('%q', tostring(s))
end

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_globals()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.globals_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and ('inspect ' .. name) or nil,
      on_click = (name and st) and function() send(st, 'mep_view(' .. luaquote(name) .. ')') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no globals set in this session yet)'} end
  mep.sidebar_set_sections(sidebars.globals, {{id = 'globals', title = '', collapsed = false, widgets = widgets}})
end

local function render_modules()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.modules_text) or ''):gmatch('[^\n]+') do
    local name = not line:match('^%(') and line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, wrap = not name, wrap_indent = 0,
      tooltip = name and ('help ' .. name) or nil,
      on_click = (name and st) and function()
        send(st, 'mep_help(' .. luaquote(name) .. ')')
        if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
      end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no modules required yet)'} end
  mep.sidebar_set_sections(sidebars.modules, {{id = 'modules', title = '', collapsed = false, widgets = widgets}})
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_globals()
  render_modules()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Globals row, or mep_view('name') in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    "(no help viewed yet -- gh over a name, or mep_help('name') in the console)")
end

-- Which lines are inside a multi-line long string or comment ([[ ]], [==[ ]==],
-- --[[ ]]): never the start of a statement, never rewritten. Quotes are not
-- tracked, so a `[[` inside a quoted string can fool it.
local function long_lines(lines)
  local inside, level = {}, nil
  for i, l in ipairs(lines) do
    local pos = 1
    if level then
      inside[i] = true
      local _, e = l:find(']' .. level .. ']', 1, true)
      if e then pos = e + 1; level = nil end
    end
    while not level do
      local s, e, eq = l:find('%[(=*)%[', pos)
      if not s then break end
      local _, ce = l:find(']' .. eq .. ']', e + 1, true)
      if ce then pos = ce + 1 else level = eq end
    end
  end
  return inside
end

-- Top-level (column 0) `local` declarations rewritten to global assignments,
-- one line for one line, so what a sent block or file defines outlives the
-- chunk it ran in. `local function f` -> `function f`; `local a, b <const> =
-- x` -> `a, b = x`; a bare `local a` -> `a = nil`.
function mep.lua_ui_globalize(lines)
  local inside = long_lines(lines)
  local out = {}
  for i, l in ipairs(lines) do
    local r = l
    if not inside[i] then
      local fname = l:match('^local%s+function%s+([%a_][%w_]*)')
      if fname then
        r = l:gsub('^local%s+function', 'function', 1)
      else
        local names, rest = l:match('^local%s+([%a_][%w_%s,<>]-)%s*=%s*(.*)$')
        if names and rest:sub(1, 1) ~= '=' then
          r = names:gsub('%s*<%s*%a+%s*>', '') .. ' = ' .. rest
        else
          local bare, tail = l:match('^local%s+([%a_][%w_%s,<>]*)(.*)$')
          if bare and (tail == '' or tail:match('^;') or tail:match('^%-%-')) then
            bare = bare:gsub('%s*<%s*%a+%s*>', ''):gsub('[%s,]+$', '')
            r = bare .. ' = nil' .. (tail ~= '' and (' ' .. tail) or '')
          end
        end
      end
    end
    out[i] = r
  end
  return out
end

-- The top-level statement around `row`: from the nearest line at or above it
-- that starts a statement at column 0 (not a closing keyword, not a comment,
-- not continuing the line before, not inside a long string) to just before
-- the next one -- a whole function, if/for/while block, table constructor,
-- or a single line. Returns the first and last rows, or nil.
function mep.lua_ui_block_at(lines, row)
  local n = #lines
  local inside = long_lines(lines)
  local function closer(l)
    for _, w in ipairs({'end', 'else', 'elseif', 'until'}) do
      if l:sub(1, #w) == w and not l:sub(#w + 1, #w + 1):match('[%w_]') then return true end
    end
    return l:match('^[%)%]}]') ~= nil
  end
  local function continues(k)
    local p = (lines[k - 1] or ''):gsub('%s*%-%-[^%[]*$', ''):gsub('%s+$', '')
    if p:match('[,%(%[{=+%-*/%%%^<>~&|%.]$') then return true end
    for _, w in ipairs({'and', 'or', 'not', 'local', 'return'}) do
      if p:sub(-#w) == w and not p:sub(-#w - 1, -#w - 1):match('[%w_]') then return true end
    end
    return false
  end
  local function starts(k)
    local l = lines[k]
    return l ~= nil and not inside[k] and l:match('^[^%s]') ~= nil and not l:match('^%-%-')
      and not l:match('^#!') and not closer(l) and not continues(k)
  end
  local s = row
  while s > 1 and not starts(s) do s = s - 1 end
  if not starts(s) then return nil end
  local e = s
  while e < n and not starts(e + 1) do e = e + 1 end
  while e > s and not inside[e] and (lines[e]:match('^%s*$') or lines[e]:match('^%s*%-%-')) do e = e - 1 end
  return s, e
end

-- Writes `text` (the source's rows from `first`) to the session's eval file,
-- padded so its line numbers are the source's, and has the console run it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  if mep.opt.lua_ui_globalize_locals then chunk = mep.lua_ui_globalize(chunk) end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  st.eval_count = (st.eval_count or 0) + 1
  mep.language_ui_write_file(st.eval_path, text)
  local fname = mep.filename and mep.filename()
  local chunkname = (fname and fname ~= '') and ('@' .. mep_lsp_abspath(fname)) or '=eval'
  send(st, 'mep_eval(' .. luaquote(st.eval_path) .. ', ' .. luaquote(chunkname) .. ')')
end

-- <leader>re: the Visual selection if one is live (a mep.map('v', ...)
-- binding to this function), else the top-level statement at the cursor.
function mep.lua_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Lua console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  local lines = {}
  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) or '' end
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local r = mep.visual_range()
    mep.cmd('normal!')
    if r and r.kind == 'char' then
      local sel_lines = {}
      for l in (sel .. '\n'):gmatch('([^\n]*)\n') do sel_lines[#sel_lines + 1] = l end
      local padded = {}
      for i = 1, r.start_row - 1 do padded[i] = '' end
      for i, l in ipairs(sel_lines) do padded[r.start_row - 1 + i] = l end
      eval_text(st, padded, r.start_row, r.start_row + #sel_lines - 1)
    elseif r then
      eval_text(st, lines, r.start_row, r.end_row)
    end
    return
  end
  local s, e = mep.lua_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Lua statement at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.lua_ui_eval_selection = mep.lua_ui_eval_block
mep.language_ui_eval_handlers.lua = mep.lua_ui_eval_block

function mep.lua_ui_open()
  local session_dir = mep.language_ui_session_dir('luaui')
  local paths = {
    globals = session_dir .. '/globals.txt', modules = session_dir .. '/modules.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local init_path = session_dir .. '/mep_init.lua'
  mep.language_ui_write_file(init_path, 'local __mep_dir = ' .. luaquote(session_dir) .. '\n' .. mep_lua_ui_init_template)
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.lua_ui_cmd)
  argv[#argv + 1] = '-i'
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.lua_ui_info_share, bottom_share = mep.opt.lua_ui_bottom_share,
    bottom_info_share = mep.opt.lua_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'lua') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane, src_buf = src_buf,
    eval_path = session_dir .. '/eval.lua', run_path = session_dir .. '/run.lua',
    globals_text = '', modules_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.globals, sidebars.modules, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Lua language UI: console below; Globals/Modules/Inspect top-right, Help bottom-right; <leader>re runs the statement at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- the
  -- source buffer as it is now (unsaved edits too), its top-level locals
  -- made global, run in the console under the file's own name.
  local function run_source(fname)
    local abs = mep_lsp_abspath(fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, 'mep_run(' .. luaquote(abs) .. ')')
      return
    end
    if mep.opt.lua_ui_globalize_locals then lines = mep.lua_ui_globalize(lines) end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, 'mep_run(' .. luaquote(st.run_path) .. ', ' .. luaquote('@' .. abs) .. ')')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Lua language UI mode closed')
    end,
  }
end
mep.language_ui_modes.lua = {open = mep.lua_ui_open}

-- gh: the console's own view of the name under the cursor (a function's
-- signature, doc comment and source; a module's or object's fields) in the
-- Help tab. From a .lua buffer the language server's hover pops up as well:
-- for a C function -- a native module's, documented by LuaCATS stubs -- the
-- console has nothing but its parameter count, and the hover has the docs.
function mep.lua_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_.:]')
  if sym then sym = sym:gsub('^[.:]+', ''):gsub('[.:]+$', '') end
  if not sym or sym == '' then mep.notify('gh: no Lua name under the cursor', 'warn') return true end
  send(st, 'mep_help(' .. luaquote(sym) .. ')')
  if mep.language_for_buffer() == 'lua' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('lua', mep.lua_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Lua mode is open):
-- for scripts and tools/lua_ui_live_check.py.
function mep.lua_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, globals = st.globals_text, modules = st.modules_text,
    inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Lua mode is
-- open on the active tab.
function mep.lua_ui_poll(st)
  local changed, help_changed = false, false
  local function take(key, field)
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[field] then
      st[field] = text
      changed = true
      return true
    end
    return false
  end
  take('globals', 'globals_text')
  take('modules', 'modules_text')
  take('inspect', 'inspect_text')
  help_changed = take('help', 'help_text')
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.lua_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
