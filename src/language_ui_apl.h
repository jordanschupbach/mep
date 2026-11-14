// APL language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in an
// .apl/.apls/.aplf/.aplo/.apln/.aplc/.apli/.dyalog buffer), built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the other modes.
// Included by main.cpp only; BuiltinLanguageUiApl() is the Lua chunk
// (help/apl-mode.org is the user's page).
//
// The console is GNU APL's own interactive session -- `apl --silent
// --noCONT --noColor` (mep.opt.apl_ui_cmd; a `⍝ REPL:` comment replaces it
// via mep.language_ui_cmd, e.g. `⍝ REPL: dyalog`) -- below the source. When
// `apl` isn't on PATH but `dyalog` is, Dyalog is the console.
//
// Sending code never pastes a multi-line block into the prompt (GNU APL's
// ∇-editor and its one-line lambdas would see it line by line): <leader>rr
// writes the buffer (unsaved edits too) to run.apl in the session directory
// and types `)COPY run.apl` -- GNU APL runs a script file given to )COPY
// when it starts with `#!`, so the copy gets one, and loses any `)OFF`
// line. <leader>re sends the statement at the cursor: a one-line statement
// is typed as it is (its value prints as at the prompt); a tradfn (∇ ... ∇),
// a multi-line dfn or a control structure goes through eval.apl the same
// way. With Dyalog as the console, files are fixed with `2 ⎕FIX 'file://'`.
//
// The right column:
//   - Help (top): gh's documentation for the glyph, ⎕name, control word or
//     function under the cursor, from `mep-apl-lsp --doc` -- a glyph's
//     monadic and dyadic meanings, a user function's header and its ⍝ doc
//     lines (for a Rosetta-generated binding such as datamunge's, the C++
//     documentation), looked up in the buffer, the files under its
//     directory and MEP_APL_PATH.
//   - Glyphs (top, tabbed with Help): the language bar. Every primitive and
//     operator with its names and backtick key; clicking one inserts it at
//     the cursor in the source.
//   - Functions (bottom): the namespaces, functions, operators and
//     variables the file defines (`mep-apl-lsp --outline`), refreshed on
//     save; clicking one jumps to it.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiApl() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.apl_ui_cmd = mep.opt.apl_ui_cmd or {'apl', '--silent', '--noCONT', '--noColor'}
mep.opt.apl_ui_info_share = mep.opt.apl_ui_info_share or 0.34
mep.opt.apl_ui_bottom_share = mep.opt.apl_ui_bottom_share or 0.35
mep.opt.apl_ui_bottom_info_share = mep.opt.apl_ui_bottom_info_share or 0.4
for _, e in ipairs({'apl', 'apls', 'aplf', 'aplo', 'apln', 'aplc', 'apli', 'dyalog'}) do
  mep.language_ui_extensions[e] = mep.language_ui_extensions[e] or 'apl'
end

local state, sidebars = {}, nil
local shq = mep.language_ui_shq
local function active(st) return state[mep.current_tab_id()] == st end
local function lsp_tool() return (mep.bundled_tool and mep.bundled_tool('mep-apl-lsp')) or 'mep-apl-lsp' end

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    help = mep.sidebar_create('Help', 'right', 60),
    glyphs = mep.sidebar_create('Glyphs', 'right', 60),
    functions = mep.sidebar_create('Functions', 'right', 60),
  }
end

-- A line's code without strings and its ⍝ comment (for counting braces and words).
local function code_of(line)
  local out, i, n = {}, 1, #line
  while i <= n do
    local c = line:sub(i, i)
    if c == "'" or c == '"' then
      local j = i + 1
      while j <= n do
        local d = line:sub(j, j)
        if c == '"' and d == '\\' then j = j + 1
        elseif d == c then
          if c == "'" and line:sub(j + 1, j + 1) == "'" then j = j + 1 else break end
        end
        j = j + 1
      end
      out[#out + 1] = ' '
      i = j + 1
    elseif line:sub(i, i + 2) == '⍝' then
      break
    else
      out[#out + 1] = c
      i = i + 1
    end
  end
  return table.concat(out)
end

local openers = {IF = true, FOR = true, WHILE = true, REPEAT = true, SELECT = true, TRAP = true, WITH = true,
  HOLD = true, SECTION = true, PROPERTY = true, DISPOSABLE = true, NAMESPACE = true, CLASS = true, INTERFACE = true}

-- How a line changes the nesting: (brace delta, control delta, toggles ∇).
local function line_depths(line, brace_depth)
  local code = code_of(line)
  local braces = 0
  for ch in code:gmatch('[{}]') do braces = braces + (ch == '{' and 1 or -1) end
  local ctl = 0
  if brace_depth == 0 then
    for stmt in (code .. '⋄'):gmatch('(.-)⋄') do
      local w = stmt:match('^%s*:(%a+)')
      if w then
        w = w:upper()
        if openers[w] then ctl = ctl + 1
        elseif w == 'END' or w == 'UNTIL' or (w:sub(1, 3) == 'END' and #w > 3) then ctl = ctl - 1 end
      end
    end
  end
  local del = brace_depth == 0 and code:match('^%s*∇') ~= nil
  return braces, ctl, del
end

-- The top-level statement around `row` (1-based): {first, last}, a whole
-- tradfn, multi-line dfn or control structure; nil on a blank or comment line.
function mep.apl_ui_block_at(lines, row)
  local first, braces, ctl, in_fn = nil, 0, 0, false
  for i, line in ipairs(lines) do
    local blank = code_of(line):match('^%s*$') ~= nil
    if not first and not blank then first = i end
    local b, c, del = line_depths(line, braces)
    if del then in_fn = not in_fn end
    braces = math.max(0, braces + b)
    ctl = math.max(0, ctl + c)
    if first and braces == 0 and ctl == 0 and not in_fn then
      if row >= first and row <= i then return first, i end
      first = nil
    elseif not first and row == i then
      return nil
    end
    if i > row and not first then return nil end
  end
  if first and row >= first then return first, #lines end
  return nil
end

-- The text GNU APL's )COPY runs as a script: a #! line first, no )OFF.
function mep.apl_ui_script(lines)
  local out = {}
  if not (lines[1] or ''):match('^#!') then out[1] = '#!/usr/bin/env apl --script' end
  for _, l in ipairs(lines) do
    if not l:match('^%s*%)[Oo][Ff][Ff]%s*$') then out[#out + 1] = l end
  end
  return table.concat(out, '\n') .. '\n'
end

-- The glyph, ⎕name, :Keyword or (dotted) name at byte `col` (1-based) of `line`.
function mep.apl_ui_symbol_at(line, col)
  if col < 1 or col > #line then return nil end
  -- Back up to the start of a UTF-8 sequence.
  while col > 1 and line:byte(col) >= 0x80 and line:byte(col) < 0xC0 do col = col - 1 end
  local b = line:byte(col)
  local function cp_at(i)
    local c = line:byte(i)
    if not c then return nil, i end
    local n = c < 0x80 and 1 or (c >= 0xF0 and 4 or (c >= 0xE0 and 3 or 2))
    return line:sub(i, i + n - 1), i + n
  end
  local function namech(cp) return cp and (cp:match('^[%w_.]$') or cp == '∆' or cp == '⍙' or cp == '¯') end
  local cp = cp_at(col)
  if namech(cp) or cp == '⎕' or cp == ':' then
    local s = col
    while s > 1 do
      local p = s - 1
      while p > 1 and line:byte(p) >= 0x80 and line:byte(p) < 0xC0 do p = p - 1 end
      local q = cp_at(p)
      if namech(q) or q == '⎕' then s = p else break end
    end
    if s > 1 and line:sub(s - 1, s - 1) == ':' then s = s - 1 end
    local e = col
    local _, nx = cp_at(e)
    while nx <= #line and namech((cp_at(nx))) do e = nx; _, nx = cp_at(e) end
    local word = line:sub(s, nx - 1):gsub('^%.+', ''):gsub('%.+$', '')
    if word == '' or word == ':' then return nil end
    return word
  end
  if b and b >= 0x80 or (cp and cp:match('^[%+%-%*%%|!?~<=>,/\\@&]$')) then return cp end
  return nil
end

local function render_help()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local text = st and st.help_text
  if text then
    local out = {}
    for line in (text .. '\n'):gmatch('([^\n]*)\n') do
      if not line:match('^%s*```') then out[#out + 1] = line end
    end
    text = table.concat(out, '\n')
  end
  mep.language_ui_render_textbox(sidebars.help, text,
    '(gh shows the documentation of the glyph, ⎕name or function under the cursor)')
end

local glyph_sections = {
  {id = 'function', title = 'Functions'},
  {id = 'operator', title = 'Operators'},
  {id = 'syntax', title = 'Syntax and constants'},
}

local function render_glyphs()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local sections = {}
  for _, sec in ipairs(glyph_sections) do
    local widgets = {}
    for i, g in ipairs((st and st.glyphs) or {}) do
      local kind = (g.kind == 'constant') and 'syntax' or g.kind
      if kind == sec.id then
        local key = g.key ~= '' and ('`' .. g.key) or '  '
        local name = (g.names:match('^(%S+)') or '')
        widgets[#widgets + 1] = {id = 'g' .. i, text = g.glyph .. '  ' .. key .. '  ' .. name .. '  ' .. g.forms,
          wrap = true, wrap_indent = 8, tooltip = g.names, on_click = function() mep.apl_ui_insert(g.glyph) end}
      end
    end
    if #widgets > 0 then sections[#sections + 1] = {id = sec.id, title = sec.title, collapsed = false, widgets = widgets} end
  end
  if #sections == 0 then
    sections[1] = {id = 'none', title = '', collapsed = false, widgets = {{id = 'empty', text = '(loading the glyphs...)'}}}
  end
  mep.sidebar_set_sections(sidebars.glyphs, sections)
end

local function render_functions()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for i, d in ipairs((st and st.functions) or {}) do
    local name = d.name:match('([^.]+)$') or d.name
    local text = string.rep('  ', d.depth) .. name
    if d.kind ~= 'namespace' and d.kind ~= 'class' and d.header ~= '' then text = text .. '  ' .. d.header end
    widgets[#widgets + 1] = {id = 'f' .. i, text = text, wrap = true, wrap_indent = 2 * d.depth + 4,
      hl = (d.kind == 'namespace' or d.kind == 'class') and 'Title' or (d.kind == 'variable' and 'Comment' or nil),
      tooltip = d.kind .. ' ' .. d.name, on_click = function()
        if not mep.pane_focus_buffer(st.src_buf) then mep.jump_to_buffer(st.src_buf) end
        mep.set_cursor(d.line, 1)
      end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no functions, operators or namespaces in this file)'} end
  mep.sidebar_set_sections(sidebars.functions, {{id = 'functions', title = '', collapsed = false, widgets = widgets}})
end

local function render_all()
  render_help()
  render_glyphs()
  render_functions()
end

-- Parses `mep-apl-lsp --outline` / `--glyphs` output.
function mep.apl_ui_parse_outline(lines)
  local out = {}
  for _, l in ipairs(lines) do
    local line, depth, kind, name, header = l:match('^(%d+)\t(%d+)\t([^\t]*)\t([^\t]*)\t(.*)$')
    if line then out[#out + 1] = {line = tonumber(line), depth = tonumber(depth), kind = kind, name = name, header = header} end
  end
  return out
end
function mep.apl_ui_parse_glyphs(lines)
  local out = {}
  for _, l in ipairs(lines) do
    local glyph, kind, key, names, forms = l:match('^([^\t]+)\t([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$')
    if glyph then out[#out + 1] = {glyph = glyph, kind = kind, key = key, names = names, forms = forms} end
  end
  return out
end

-- Runs mep-apl-lsp with `args`, then calls done(lines).
local function lsp_job(args, done)
  local argv = {lsp_tool()}
  for _, a in ipairs(args) do argv[#argv + 1] = a end
  local out = {}
  mep.job_start(argv, {
    on_stdout = function(line) out[#out + 1] = line end,
    on_exit = function(code) done(out, code) end,
  })
end

local function refresh_functions(st)
  local path = st.session_dir .. '/outline.' .. (st.ext or 'apl')
  mep.language_ui_write_file(path, table.concat(mep.buffer_get_lines(st.src_buf) or {}, '\n') .. '\n')
  lsp_job({'--outline', path}, function(lines)
    st.functions = mep.apl_ui_parse_outline(lines)
    if active(st) then render_functions() end
  end)
end

-- Inserts a glyph at the cursor in the source (the language bar's click).
function mep.apl_ui_insert(glyph)
  local st = state[mep.current_tab_id()]
  if st then
    if not mep.pane_focus_buffer(st.src_buf) then mep.pane_focus(st.editor_pane) end
  end
  mep.insert_text(glyph)
end

-- Types `text` into the console (a fresh console gets it once it's up).
local function send(st, text)
  if not st.console_buf or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('APL: the console is not running', 'warn')
    return
  end
  mep.terminal_write(st.console_buf, text .. '\n')
end

-- Makes the console run the lines (a whole file or a block) as a script.
local function send_script(st, path, lines)
  if st.dialect == 'dyalog' then
    mep.language_ui_write_file(path, table.concat(lines, '\n') .. '\n')
    send(st, "2 ⎕FIX 'file://" .. path:gsub("'", "''") .. "'")
  else
    mep.language_ui_write_file(path, mep.apl_ui_script(lines))
    send(st, ')COPY ' .. path)
  end
end

-- <leader>re: the statement at the cursor (or the visual selection).
function mep.apl_ui_eval_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st then return end
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local lines = {}
    for l in (sel .. '\n'):gmatch('([^\n]*)\n') do lines[#lines + 1] = l end
    if #lines == 1 then send(st, (lines[1]:gsub('^%s+', ''))) else send_script(st, st.eval_path, lines) end
    return
  end
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  local row = mep.cursor()
  local first, last = mep.apl_ui_block_at(lines, row)
  if not first then mep.notify('APL: no statement at the cursor', 'warn') return end
  if first == last and not lines[first]:match('^%s*∇') then
    send(st, (code_of(lines[first]):gsub('^%s+', ''):gsub('%s+$', '')))
  else
    local block = {}
    for i = first, last do block[#block + 1] = lines[i] end
    send_script(st, st.eval_path, block)
  end
end
mep.language_ui_eval_handlers.apl = mep.apl_ui_eval_at_cursor

-- gh: the documentation of the symbol under the cursor in the Help tab.
function mep.apl_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st then return false end
  local row, col = mep.cursor()
  local sym = mep.apl_ui_symbol_at(mep.get_line(row) or '', col)
  if not sym then mep.notify('gh: no APL name or glyph under the cursor', 'warn') return true end
  local args = {'--doc', '--root', st.src_dir, sym}
  if mep.language_for_buffer() == 'apl' then
    local copy = st.session_dir .. '/help_src.' .. (st.ext or 'apl')
    mep.language_ui_write_file(copy, table.concat(mep.buffer_get_lines(mep.current_buffer()) or {}, '\n') .. '\n')
    args[#args + 1] = copy
  end
  lsp_job(args, function(lines, code)
    st.help_text = (code == 0 and #lines > 0) and table.concat(lines, '\n') or ('No documentation for ' .. sym .. '.')
    if not active(st) then return end
    render_help()
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  end)
  return true
end
mep.help_register_provider('apl', mep.apl_ui_help_at_cursor)

local function on_path(exe) return os.execute('command -v ' .. exe .. ' >/dev/null 2>&1') == true end

-- The console's argv: a REPL: override, else GNU APL, else Dyalog.
function mep.apl_ui_console_argv()
  local default = mep.opt.apl_ui_cmd
  if not mep.language_ui_repl_override and default[1] == 'apl' and not on_path('apl') and on_path('dyalog') then
    default = {'dyalog'}
  end
  return mep.language_ui_cmd(default)
end

function mep.apl_ui_open()
  local fname = mep.filename()
  local src_abs = mep_lsp_abspath(fname)
  local session_dir = mep.language_ui_session_dir('aplui')
  local argv = mep.apl_ui_console_argv()
  local dialect = (table.concat(argv, ' '):match('dyalog') and 'dyalog') or 'gnu'
  local st = {
    src_buf = mep.current_buffer(), src_abs = src_abs, src_dir = src_abs:match('^(.*)/[^/]*$') or '.',
    ext = (fname:match('%.([%w]+)$') or 'apl'):lower(), session_dir = session_dir, dialect = dialect,
    run_path = session_dir .. '/run.apl', eval_path = session_dir .. '/eval.apl',
    glyphs = {}, functions = {}, help_text = nil, last_save_epoch = mep.buffer_save_epoch(),
  }
  local layout = mep.language_ui_layout({
    info_share = mep.opt.apl_ui_info_share, bottom_share = mep.opt.apl_ui_bottom_share,
    bottom_info_share = mep.opt.apl_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, dialect == 'dyalog' and 'dyalog' or 'apl') end,
  })
  st.editor_pane, st.console_buf = layout.editor_pane, layout.console_buf
  local tid = mep.current_tab_id()
  ensure_sidebars()
  state[tid] = st
  render_all()
  local bufs = mep.language_ui_open_tabs(layout.top_pane, {sidebars.help, sidebars.glyphs})
  st.help_buf = bufs[sidebars.help]
  mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.functions})
  mep.pane_focus(layout.editor_pane)
  lsp_job({'--glyphs'}, function(lines)
    st.glyphs = mep.apl_ui_parse_glyphs(lines)
    if active(st) then render_glyphs() end
  end)
  refresh_functions(st)
  mep.notify('APL language UI: ' .. (dialect == 'dyalog' and 'Dyalog' or 'GNU APL') ..
    ' below; Help/Glyphs top-right, Functions bottom-right; <leader>rr runs the file, <leader>re the statement, gh shows docs')
  return {
    run_source = function()
      send_script(st, st.run_path, mep.buffer_get_lines(st.src_buf) or {})
    end,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('APL language UI mode closed')
    end,
  }
end
mep.language_ui_modes.apl = {open = mep.apl_ui_open}

-- This tab's session as the panels see it (nil when no APL mode is open).
function mep.apl_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.session_dir, dialect = st.dialect, help = st.help_text, glyphs = #st.glyphs,
    functions = st.functions}
end

-- Frame hook: refresh the Functions panel after a save.
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < (mep.opt.language_ui_poll_interval or 1) then return end
    last_poll = now
    local epoch = mep.buffer_save_epoch()
    if epoch ~= st.last_save_epoch then
      st.last_save_epoch = epoch
      refresh_functions(st)
    end
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
