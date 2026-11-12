// COBOL language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a
// .cob/.cbl/.cobol/.cpy buffer), built on kBuiltinLanguageUiCommon
// (mep.language_ui_layout) like the C/C++ mode it is modelled on. Included
// by main.cpp only; BuiltinLanguageUiCobol() is the Lua chunk.
//
// COBOL has no REPL, so the console is a plain shell below the source
// (where the compiled program runs), and the right column holds:
//   - Build: what cobc said. `cobc -fsyntax-only` runs when the mode opens
//     and after every save (and on <leader>re), so syntax errors and
//     missing copybooks show up without a full build; <leader>rr's build
//     shows its command and messages here too. `file:line: kind: message`
//     rows are clickable.
//   - Help: gh's view of the name under the cursor -- the language
//     server's hover (mep-cobol-lsp: a copybook record's or field's `*>>`
//     doc block, the C++ documentation rosetta writes into the bindings it
//     generates) kept in a tab instead of a popup.
//   - Copybooks (bottom): each `COPY` the file makes, resolved the way
//     mep-cobol-lsp resolves it (the file's directory, $COBCPY, then `-I`
//     in the Run button's Setup flags), with the 01 records it declares
//     and the CALLs each one documents; clicking a record opens the
//     copybook there. A copybook is re-read only when its mtime changes.
// <leader>rr (run_source): writes the buffer, builds it with the Run
// button's own per-project compiler and flags (mep.run_button_config_for:
// `cobc -x` unless Setup says otherwise -- add `-I DIR -L DIR -lfoo` there
// for bindings) into the session directory, and on success runs the
// executable in the shell below.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiCobol() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.cobol_ui_info_share = mep.opt.cobol_ui_info_share or 0.38
mep.opt.cobol_ui_bottom_share = mep.opt.cobol_ui_bottom_share or 0.3
mep.opt.cobol_ui_bottom_info_share = mep.opt.cobol_ui_bottom_info_share or 0.45
-- Records listed per copybook in the Copybooks tab (datamunge.cpy has ~2800).
mep.opt.cobol_ui_copybook_rows = mep.opt.cobol_ui_copybook_rows or 400
for _, e in ipairs({'cob', 'cbl', 'cobol', 'cpy'}) do
  mep.language_ui_extensions[e] = mep.language_ui_extensions[e] or 'cobol'
end

local state, sidebars = {}, nil
local shq = mep.language_ui_shq
local function basename(path) return path:match('([^/]+)$') or path end
local function active(st) return state[mep.current_tab_id()] == st end

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    build = mep.sidebar_create('Build', 'right', 60),
    help = mep.sidebar_create('Help', 'right', 60),
    copybooks = mep.sidebar_create('Copybooks', 'right', 60),
  }
end

-- The compiler and flags: the Run button's Setup for this extension (a
-- copybook's .cpy has no Run entry of its own: .cob's).
local function config_for_ext(ext)
  local cfg = mep.run_button_config_for and (mep.run_button_config_for(ext) or mep.run_button_config_for('cob')) or nil
  return (cfg and cfg.compiler) or 'cobc -x', (cfg and cfg.flags) or ''
end
local function config(st) return config_for_ext(st.ext) end

-- The -I directories in a flags string, absolute (relative ones are the
-- project's: mep's working directory, where Setup's flags apply).
local function include_dirs(flags)
  local dirs = {}
  for dir in (' ' .. flags):gmatch('%s%-I%s*(%S+)') do
    dir = dir:gsub("^['\"]", ''):gsub("['\"]$", '')
    if dir:sub(1, 1) ~= '/' then dir = mep.getcwd() .. '/' .. dir end
    dirs[#dirs + 1] = dir
  end
  return dirs
end

-- mep-cobol-lsp's initializationOptions (the cobol_ls entry in
-- kBuiltinLsp calls this when the server starts): the Setup flags' -I
-- directories as `includeDirs`, so hover and completion see the same
-- copybooks cobc does.
function mep.cobol_lsp_init_options(fname)
  local ext = fname and mep_lsp_filetype(fname) or 'cob'
  local _, flags = config_for_ext((ext or 'cob'):lower())
  local dirs = include_dirs(flags)
  if #dirs == 0 then return nil end
  return {includeDirs = dirs}
end

-- Where COPY looks, in mep-cobol-lsp's order: the source's directory,
-- $COBCPY, then -I flags.
local function copy_dirs(st)
  local dirs = {st.src_dir}
  for dir in (os.getenv('COBCPY') or ''):gmatch('[^:]+') do dirs[#dirs + 1] = dir end
  local _, flags = config(st)
  for _, dir in ipairs(include_dirs(flags)) do dirs[#dirs + 1] = dir end
  return dirs
end

-- cobc reads fixed format unless told otherwise. A free-format file that
-- does not say so itself (no >>SOURCE / $SET SOURCEFORMAT directive) gets
-- -free -- the same guess mep-cobol-lsp and the highlighter make: code in
-- the sequence area (columns 1-6) that is not a sequence number. Setup's
-- own -free / -fixed / -fformat wins.
local function format_flag(st, flags)
  if (' ' .. flags):match('%s%-free') or (' ' .. flags):match('%s%-fixed') or flags:match('%-fformat') then return '' end
  for _, line in ipairs(mep.buffer_get_lines(st.src_buf) or {}) do
    local up = line:upper()
    if up:match('>>%s*SOURCE') or up:match('%$SET%s+SOURCEFORMAT') then return '' end
    local t = line:match('^%s*(.-)%s*$')
    if t ~= '' and t:sub(1, 2) ~= '*>' and t:sub(1, 2) ~= '>>' then
      local area = line:sub(1, 6):match('^%s*(.-)%s*$')
      if area ~= '' and not line:sub(1, 6):find('\t') and not area:match('^%d+$') then return ' -free' end
    end
  end
  return ''
end

local function readable(path)
  local f = io.open(path, 'r')
  if f then f:close() return true end
  return false
end

local function resolve_copy(st, name)
  local names = {name, name:upper(), name:lower()}
  local exts = {'', '.cpy', '.CPY', '.cbl', '.CBL', '.cob', '.COB'}
  for _, dir in ipairs(copy_dirs(st)) do
    for _, n in ipairs(names) do
      for _, ext in ipairs(exts) do
        local p = dir .. '/' .. n .. ext
        if readable(p) then return p end
      end
    end
  end
  return nil
end

-- A copybook's named 01 records: {name, line, calls} -- `calls` from the
-- `*>> CALL "x"` lines of the doc block above the record (several on
-- field, director, container and table records). Cached by mtime: a
-- generated copybook can be megabytes.
local records_cache = {}
local function copybook_records(path)
  local mtime = mep.fs_mtime and mep.fs_mtime(path) or nil
  local hit = records_cache[path]
  if hit and mtime and hit.mtime == mtime then return hit.records end
  local f = io.open(path, 'r')
  if not f then return {} end
  local out, calls, n = {}, {}, 0
  for line in f:lines() do
    n = n + 1
    local doc = line:match('^%s*%*>>%s?(.*)$') or line:match('^......%*>>%s?(.*)$')
    if doc then
      local c = doc:match('^%s*[Cc][Aa][Ll][Ll]%s*["\']([^"\']+)["\']')
      if c then calls[#calls + 1] = c end
    elseif not line:match('^%s*%*>') and not line:match('^......[*/]') and line:match('%S') then
      local name = line:match('^%s*0?1%s+([%w][%w_-]*)') or line:match('^%d%d%d%d%d%d%s+0?1%s+([%w][%w_-]*)')
      if name and name:upper() ~= 'FILLER' then out[#out + 1] = {name = name, line = n, calls = calls} end
      calls = {}
    end
  end
  f:close()
  records_cache[path] = {mtime = mtime, records = out}
  return out
end

local function scan_copybooks(st)
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  local books, seen = {}, {}
  for _, line in ipairs(lines) do
    local code = line:gsub('%*>.*$', '')
    for name in code:gmatch('%f[%w][Cc][Oo][Pp][Yy]%s+["\']?([%w_./-]+)') do
      name = name:gsub('%.$', '')
      if not seen[name] then
        seen[name] = true
        local path = resolve_copy(st, name)
        books[#books + 1] = {name = name, path = path, records = path and copybook_records(path) or {}}
      end
    end
  end
  st.copybooks = books
end

local function render_build()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for i, line in ipairs((st and st.build_lines) or {}) do
    local file, ln, kind = line:match('^([^:%s][^:]*):(%d+):%s*(%a+)')
    local hl = nil
    if kind == 'error' then hl = 'Error'
    elseif kind == 'warning' then hl = 'Warn'
    elseif line:sub(1, 2) == '$ ' then hl = 'Comment'
    elseif line:match('^%[.* ok') then hl = 'Add' end
    local shown = line
    if st and line:sub(1, #st.src_abs) == st.src_abs then shown = basename(st.src_abs) .. line:sub(#st.src_abs + 1) end
    local w = {id = 'b' .. i, text = shown, hl = hl, wrap = true, wrap_indent = 4}
    if file and ln then
      w.on_click = function()
        local abs = file:sub(1, 1) == '/' and file or (mep.getcwd() .. '/' .. file)
        if abs == st.src_abs then
          if not mep.pane_focus_buffer(st.src_buf) then mep.jump_to_buffer(st.src_buf) end
        else
          mep.pane_focus(st.editor_pane)
          mep.open(abs)
        end
        mep.set_cursor(tonumber(ln), 1)
      end
    end
    widgets[#widgets + 1] = w
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing yet -- saving checks the file with cobc, <leader>rr builds and runs it)'} end
  mep.sidebar_set_sections(sidebars.build, {{id = 'build', title = '', collapsed = false, widgets = widgets}})
end

local function render_help()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(gh shows the language server\'s docs for the name under the cursor: a copybook record, field or CALL "name")')
end

local function render_copybooks()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local sections = {}
  for i, b in ipairs((st and st.copybooks) or {}) do
    local widgets = {}
    if not b.path then
      widgets[1] = {id = 'missing', text = 'not found (file directory, $COBCPY, -I in Setup flags)', hl = 'Warn', wrap = true}
    else
      widgets[1] = {id = 'path', text = b.path, hl = 'Comment', wrap = true, on_click = function()
        mep.pane_focus(st.editor_pane)
        mep.open(b.path)
      end}
      local shown = math.min(#b.records, mep.opt.cobol_ui_copybook_rows)
      for j = 1, shown do
        local r = b.records[j]
        local text = r.name
        if #r.calls > 0 then
          text = text .. '  CALL "' .. r.calls[1] .. '"' .. (#r.calls > 1 and (' +' .. (#r.calls - 1)) or '')
        end
        widgets[#widgets + 1] = {id = 'r' .. j, text = text, wrap = true, wrap_indent = 2,
          tooltip = #r.calls > 1 and table.concat(r.calls, ', ') or nil, on_click = function()
          mep.pane_focus(st.editor_pane)
          mep.open(b.path)
          mep.set_cursor(r.line, 1)
        end}
      end
      if #b.records > shown then
        widgets[#widgets + 1] = {id = 'more', hl = 'Comment', wrap = true,
          text = '... ' .. (#b.records - shown) .. ' more (K on a name or CALL literal shows its docs; completion offers them all)'}
      end
    end
    sections[#sections + 1] = {id = 'c' .. i, title = 'COPY ' .. b.name .. (b.path and (' (' .. #b.records .. ')') or ''),
      collapsed = false, widgets = widgets}
  end
  if #sections == 0 then
    sections[1] = {id = 'none', title = '', collapsed = false, widgets = {{id = 'empty', text = '(this file COPYs nothing)'}}}
  end
  mep.sidebar_set_sections(sidebars.copybooks, sections)
end

local function render_all()
  render_build()
  render_help()
  render_copybooks()
end

-- cobc -fsyntax-only: the check that runs on open, on save and on <leader>re.
function mep.cobol_ui_check(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  if st.checking then st.check_pending = true return end
  st.checking = true
  local compiler, flags = config(st)
  local cobc = compiler:match('^%s*(%S+)') or 'cobc'
  local cmd = cobc .. ' -fsyntax-only' .. format_flag(st, flags) .. ' ' .. shq(st.src_abs) .. (flags ~= '' and (' ' .. flags) or '')
  local out = {}
  local function add(line) out[#out + 1] = line end
  mep.job_start({'sh', '-c', cmd}, {
    cwd = mep.getcwd(),  -- where Setup's relative -I flags point (the Run button's own cwd)
    on_stdout = add,
    on_stderr = add,
    on_exit = function(code)
      st.checking = false
      if not st.building then
        st.build_lines = {'$ ' .. cmd}
        for _, l in ipairs(out) do st.build_lines[#st.build_lines + 1] = l end
        st.build_lines[#st.build_lines + 1] = (code == 0) and '[syntax ok]' or ('[cobc: exit ' .. code .. ']')
        if active(st) then render_build() end
      end
      if st.check_pending then st.check_pending = false mep.cobol_ui_check(st) end
    end,
  })
end
mep.language_ui_eval_handlers.cobol = function() mep.cmd('write') mep.cobol_ui_check() end

local function ensure_terminal(st)
  local buf = st.term_buf
  if buf and mep.is_terminal_buffer(buf) then
    local info = mep.terminal_info(buf)
    if info and not info.exited then return buf, false end
  end
  local cur = mep.current_pane_id()
  if not mep.pane_focus(st.console_pane) then
    mep.pane_focus(st.editor_pane)
    mep.pane_split_bottom(nil, mep.opt.cobol_ui_bottom_share)
    st.console_pane = mep.current_pane_id()
  end
  mep.terminal_here()
  st.term_buf = mep.current_buffer()
  st.extra_term_bufs[#st.extra_term_bufs + 1] = st.term_buf
  mep.pane_focus(cur)
  return st.term_buf, true
end

function mep.cobol_ui_build_and_run(st, fname)
  if st.building then mep.notify('Build: already running, please wait...', 'warn') return end
  mep.cmd('write')
  st.last_save_epoch = mep.buffer_save_epoch()
  st.building = true
  local compiler, flags = config(st)
  local cmd = compiler .. format_flag(st, flags) .. ' ' .. shq(st.src_abs) .. (flags ~= '' and (' ' .. flags) or '') ..
    ' -o ' .. shq(st.prog_path)
  st.build_lines = {'$ ' .. cmd}
  render_build()
  mep.notify('Building ' .. basename(fname or st.src_abs) .. '...')
  local function add(line) st.build_lines[#st.build_lines + 1] = line end
  mep.job_start({'sh', '-c', cmd}, {
    cwd = mep.getcwd(),
    on_stdout = add,
    on_stderr = add,
    on_exit = function(code)
      st.building = false
      if code == 0 then
        add('[build ok -> ' .. st.prog_path .. ']')
        if active(st) then render_build() end
        local term, is_new = ensure_terminal(st)
        -- Run from the source's directory: programs open their data files relative to it.
        local run = 'cd ' .. shq(st.src_dir) .. ' && ' .. shq(st.prog_path)
        if is_new then st.run_at, st.run_cmd = mep.now() + 0.3, run
        else mep.terminal_write(term, run .. '\n') end
      else
        add('[build failed: exit ' .. code .. ']')
        if active(st) then
          render_build()
          if st.build_buf then mep.reveal_buffer(st.build_buf, true) end
        end
        mep.notify('Build failed (exit ' .. code .. ') -- see the Build tab', 'error')
      end
    end,
  })
end

-- An LSP hover's contents as plain rows (code fences dropped).
local function hover_text(contents)
  if contents == nil or contents == mep.json_null then return '' end
  local text
  if type(contents) == 'string' then text = contents
  elseif contents.value then text = tostring(contents.value)
  else
    local parts = {}
    for _, c in ipairs(contents) do
      local t = hover_text(c)
      if t ~= '' then parts[#parts + 1] = t end
    end
    text = table.concat(parts, '\n\n')
  end
  local out = {}
  for line in (text .. '\n'):gmatch('([^\n]*)\n') do
    if not line:match('^%s*```') then out[#out + 1] = line end
  end
  return (table.concat(out, '\n'):gsub('^%s+', ''):gsub('%s+$', ''))
end

-- gh: the language server's hover for the name under the cursor, in the Help tab.
function mep.cobol_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or mep.language_for_buffer() ~= 'cobol' then return false end
  local id = mep.lsp_client_for and mep.lsp_client_for()
  if not id then mep.notify('gh: no COBOL language server attached to this buffer', 'warn') return true end
  local sym = mep.help_symbol_at_cursor('[%w_-]') or '?'
  mep.lsp_request(id, 'textDocument/hover', {
    textDocument = {uri = mep_lsp_uri(mep.filename())},
    position = mep_lsp_position(),
  }, function(msg)
    local result = mep_lsp_result(msg)
    local text = result and hover_text(result.contents) or ''
    st.help_text = (text ~= '') and text or ('No documentation for ' .. sym .. '.')
    if not active(st) then return end
    render_help()
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  end)
  return true
end
mep.help_register_provider('cobol', mep.cobol_ui_help_at_cursor)

function mep.cobol_ui_open()
  local fname = mep.filename()
  local src_abs = mep_lsp_abspath(fname)
  local session_dir = mep.language_ui_session_dir('cobolui')
  local base = basename(src_abs):gsub('%.[^.]+$', '')
  local st = {
    ext = mep_lsp_filetype(fname), src_buf = mep.current_buffer(), src_abs = src_abs,
    src_dir = src_abs:match('^(.*)/[^/]*$') or '.',
    session_dir = session_dir, prog_path = session_dir .. '/' .. base,
    build_lines = {}, help_text = nil, copybooks = {},
    last_save_epoch = mep.buffer_save_epoch(), extra_term_bufs = {},
  }
  local layout = mep.language_ui_layout({
    info_share = mep.opt.cobol_ui_info_share, bottom_share = mep.opt.cobol_ui_bottom_share,
    bottom_info_share = mep.opt.cobol_ui_bottom_info_share,
    console = function() mep.terminal_here() end,
  })
  st.editor_pane, st.console_pane, st.term_buf = layout.editor_pane, layout.console_pane, layout.console_buf
  local tid = mep.current_tab_id()
  ensure_sidebars()
  state[tid] = st
  scan_copybooks(st)
  render_all()
  local bufs = mep.language_ui_open_tabs(layout.top_pane, {sidebars.build, sidebars.help})
  st.build_buf, st.help_buf = bufs[sidebars.build], bufs[sidebars.help]
  mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.copybooks})
  mep.pane_focus(layout.editor_pane)
  mep.cobol_ui_check(st)
  mep.notify('COBOL language UI: shell below, Build/Help tabbed top-right, Copybooks bottom-right; <leader>rr builds (cobc -x) and runs, <leader>re checks, gh shows docs in Help')
  return {
    run_source = function(f) mep.cobol_ui_build_and_run(st, f) end,
    close = function()
      state[tid] = nil
      layout.console_pane = st.console_pane
      mep.language_ui_teardown(layout, st.extra_term_bufs)
      mep.notify('COBOL language UI mode closed')
    end,
  }
end
mep.language_ui_modes.cobol = {open = mep.cobol_ui_open}

-- This tab's session as the panels see it (nil when no COBOL mode is open).
function mep.cobol_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.session_dir, program = st.prog_path, build = st.build_lines, help = st.help_text,
    copybooks = st.copybooks}
end

-- Frame hook: a deferred run into a freshly respawned shell, and a re-check
-- plus copybook rescan after a save.
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    if st.run_at and mep.now() >= st.run_at then
      mep.terminal_write(st.term_buf, st.run_cmd .. '\n')
      st.run_at, st.run_cmd = nil, nil
    end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    local epoch = mep.buffer_save_epoch()
    if epoch ~= st.last_save_epoch then
      st.last_save_epoch = epoch
      scan_copybooks(st)
      render_copybooks()
      if not st.building then mep.cobol_ui_check(st) end
    end
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
