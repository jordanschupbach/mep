// Document UI and embedded-source adapters for the shared per-tab UI toggle.
// Included by main.cpp only; loaded after the document and Run commands.
#pragma once

inline const char* BuiltinLanguageUiDocument() {
    return R"MEPLUA(
mep.language_ui_extensions.mepml = mep.language_ui_extensions.mepml or 'mepml'
local documents = {}
local panels, rendered

local function block_mode(lang)
  lang = (lang or ''):lower()
  local def = (mep.org_babel_langs or {})[lang]
  local ext = def and def.extension and def.extension:gsub('^%.', ''):lower() or lang
  local key = mep.language_ui_extensions[ext] or lang
  if mep.language_ui_modes[key] and key ~= 'mepml' then return key, ext end
end

-- Keep language_for_buffer file-based: Run on a document still compiles the
-- document, even when a console for an embedded language is open in its tab.
function mep.language_ui_context()
  local row, col = mep.cursor()
  local ctx = {buf=mep.current_buffer(), row=row, col=col, lang=mep.language_for_buffer()}
  local ft = (mep_lsp_filetype(mep.filename() or '') or ''):lower()
  local block
  if ft == 'mepml' then
    local full = mep.mepml_block_at(row)
    if full then
      block = {lang=full.lang, first=full.fence_row+1, last=full.fence_row, include=full.include}
      for _, b in ipairs(mep.mepml_code_blocks()) do
        if b.first == block.first then block.last = b.last; break end
      end
    end
  elseif ft == 'org' then
    local b = mep_org_src_block_at(row)
    if b then block = {lang=b.lang, first=b.start_row+1, last=b.end_row-1} end
  elseif ft == 'rmd' then
    local lines = mep.buffer_get_lines(ctx.buf)
    for _, b in ipairs(mep.rmd_chunks(lines)) do
      if row >= b.header and row <= b.close then
        block = {lang=b.engine, first=b.first, last=b.last}
        break
      end
    end
  end
  if block then
    local key, ext = block_mode(block.lang)
    if key then ctx.lang, ctx.ext, ctx.block = key, ext, block end
  end
  return ctx
end

local function same_lines(a, b)
  if not a or not b or #a ~= #b then return false end
  for i, line in ipairs(a) do if line ~= b[i] then return false end end
  return true
end

-- Existing language modules get an ordinary editable source buffer. That
-- keeps their parsers, cursor coordinates, Help and Run handlers identical
-- to a standalone file, and prevents document prose entering the console.
function mep.language_ui_open_block(mode, ctx)
  local block, source, path = ctx.block, nil, nil
  local original = mep.buffer_get_lines(ctx.buf)
  local body = {}
  for i = block.first, block.last do body[#body+1] = original[i] end
  local body_count = #body
  if #body == 0 then body = {''} end
  if block.include then
    mep.open(block.include)
    source = mep.current_buffer()
  else
    local name = mep_lsp_abspath(mep.buffer_filename(ctx.buf))
    local token = os.tmpname()
    os.remove(token)
    path = (name:match('^(.*)/') or '.') .. '/.mep-ui-' .. token:match('[^/\\]+$') .. '.' .. ctx.ext
    local f, err = io.open(path, 'w')
    if not f then mep.notify('Cannot prepare code block: ' .. tostring(err), 'error') return end
    f:write(table.concat(body, '\n') .. '\n'); f:close()
    mep.open(path)
    source = mep.current_buffer()
    mep.buffer_set_footer(source, 'Code block: :w saves back to the document; <leader>uu returns to it')
  end
  mep.set_cursor(math.max(1, math.min(#body, ctx.row - block.first + 1)), ctx.col)
  local handle = mode.open()
  if not handle then
    mep.buffer_switch(ctx.buf)
    if path then mep.buffer_delete(source, true); os.remove(path) end
    return
  end
  local function commit(save)
    if not path then return true end
    local lines = mep.buffer_get_lines(source)
    if not lines then return true end
    mep.language_ui_write_file(path, table.concat(lines, '\n') .. '\n')
    if not same_lines(lines, body) then
      -- Refuse concurrent edits rather than overwriting the document. Keep
      -- the layout and source buffer alive so both versions can be recovered.
      local current = mep.buffer_get_lines(ctx.buf)
      if not same_lines(current, original) then
        local prefix, suffix = 0, 0
        while prefix < math.min(#current, #original) and current[prefix+1] == original[prefix+1] do prefix = prefix + 1 end
        while suffix < math.min(#current, #original) - prefix and current[#current-suffix] == original[#original-suffix] do suffix = suffix + 1 end
        if #original - suffix < block.first - 1 then
          block.first = block.first + #current - #original
        elseif prefix < block.first + body_count then
          mep.notify('The document block changed in another buffer; resolve those edits before closing its UI. The block buffer has been kept open.', 'error')
          return false
        end
      end
      local pane, buf = mep.current_pane_id(), mep.current_buffer()
      mep.buffer_switch(ctx.buf)
      mep.replace_lines(block.first, block.first + body_count, lines)
      if save then mep.buffer_save(ctx.buf) end
      original, body = mep.buffer_get_lines(ctx.buf), lines
      body_count = #lines
      mep.pane_focus(pane); mep.buffer_switch(buf)
    elseif save then
      mep.buffer_save(ctx.buf)
    end
    return true
  end
  if path then mep.buffer_set_on_write(source, function() commit(true) end) end
  local close = handle.close
  handle.close = function()
    if not commit(false) then return false end
    local row, col = mep.cursor()
    if mep.current_buffer() ~= source then row, col = ctx.row - block.first + 1, ctx.col end
    close()
    mep.buffer_switch(ctx.buf)
    mep.set_cursor(block.first + math.max(0, row - 1), col)
    if path then mep.buffer_delete(source, true); os.remove(path) end
  end
  return handle
end

local function jump(st, row, col)
  if mep.pane_focus(st.source_pane) then
    mep.buffer_switch(st.source_buf)
    mep.set_cursor(row, col or 1)
  end
end

local function render(st)
  rendered = st
  local outline, blocks, problems = {}, {}, {}
  for i, h in ipairs(mep.mepml_outline()) do
    outline[#outline+1] = {id='h'..i, text=string.rep('  ', math.max(0, h.level-1)) .. h.title,
      on_click=function() jump(st, h.row) end}
  end
  for i, b in ipairs(mep.mepml_code_blocks()) do
    blocks[#blocks+1] = {id='b'..i, text=(b.lang ~= '' and b.lang or 'code') .. ' · line ' .. b.first,
      on_click=function() jump(st, b.first) end}
  end
  for i, d in ipairs(mep.mepml_diagnostics()) do
    problems[#problems+1] = {id='d'..i, text=d.row .. ':' .. d.col .. ' [' .. d.severity .. '] ' .. d.message,
      wrap=true, on_click=function() jump(st, d.row, d.col) end}
  end
  for _, pair in ipairs({{st.outline, outline, 'No headings'}, {st.blocks, blocks, 'No code blocks'},
      {st.problems, problems, 'No problems'}}) do
    if #pair[2] == 0 then pair[2][1] = {id='empty', text=pair[3]} end
    mep.sidebar_set_sections(pair[1], {{id='items', title='', widgets=pair[2]}})
  end
end

-- Capture the destination before export starts. A late PDF completion must
-- never recreate a layout that was closed, or steal focus from another tab.
function mep.mepml_ui_output_target()
  local tid = mep.current_tab_id()
  local st = documents[tid]
  if not st or st.source_buf ~= mep.current_buffer() then return end
  st.show_output = function(path)
    if documents[tid] ~= st then return end
    if mep.current_tab_id() ~= tid then st.pending_output = path; return end
    st.pending_output = nil
    local focus = mep.current_pane_id()
    if not mep.pane_focus(st.preview_pane) then return end
    if st.output == path then
      if path:match('%.html?$') then mep.html_reload(path)
      elseif path:match('%.pdf$') then mep.pdf_reload(path)
      elseif path:match('%.docx$') or path:match('%.odt$') or path:match('%.rtf$') then mep.office_reload(path)
      else mep.cmd('e!') end
    else
      mep.open(path)
      st.output = path
    end
    mep.pane_focus(focus)
  end
  return st.show_output
end

function mep.mepml_ui_open()
  local tid = mep.current_tab_id()
  local st = {source_buf=mep.current_buffer()}
  mep.cmd('vsplit')
  st.source_pane = mep.current_pane_id()
  mep.nav_pane('right')
  mep.pane_set_share(mep.opt.mepml_ui_preview_share or 0.5)
  st.info_pane = mep.current_pane_id()
  mep.cmd('split')
  st.preview_pane = mep.current_pane_id()
  local placeholder = mep.buffer_new()
  mep.buffer_set_lines(placeholder, {'Compiling document…', 'Use <leader>rr in the source to refresh the preview.'})
  mep.buffer_switch(placeholder)
  mep.pane_focus(st.info_pane)
  mep.pane_set_share(mep.opt.mepml_ui_info_share or 0.3)
  panels = panels or {outline=mep.sidebar_create('Outline', 'right', 44),
    blocks=mep.sidebar_create('Code Blocks', 'right', 44), problems=mep.sidebar_create('Problems', 'right', 44)}
  st.outline, st.blocks, st.problems = panels.outline, panels.blocks, panels.problems
  mep.language_ui_open_tabs(st.info_pane, {st.outline, st.blocks, st.problems})
  documents[tid] = st
  mep.pane_focus(st.source_pane)
  render(st)
  st.epoch = mep.buffer_change_epoch()
  mep.run_button_run_mepml()
  mep.pane_focus(st.source_pane)
  mep.notify('MEPML UI: compiled preview, Outline / Code Blocks / Problems; <leader>rr refreshes; <leader>uu in a code block opens its language UI')
  return {close=function()
    documents[tid] = nil
    for _, pane in ipairs({st.preview_pane, st.info_pane}) do
      if mep.pane_focus(pane) then mep.cmd('close') end
    end
    mep.pane_focus(st.source_pane)
    mep.buffer_delete(placeholder, true)
  end}
end
mep.language_ui_modes.mepml = {open=mep.mepml_ui_open}
mep.on_frame(function()
  local st = documents[mep.current_tab_id()]
  if not st then return end
  if st.pending_output then st.show_output(st.pending_output) end
  local now, epoch = mep.now(), mep.buffer_change_epoch()
  if (epoch == st.epoch and rendered == st) or now < (st.next_refresh or 0) then return end
  local focus = mep.current_pane_id()
  if not mep.pane_focus(st.source_pane) then return end
  if mep.current_buffer() ~= st.source_buf or not mep.mepml_parse_ready() then mep.pane_focus(focus); return end
  st.epoch, st.next_refresh = epoch, now + (mep.opt.language_ui_poll_interval or 1)
  render(st)
  mep.pane_focus(focus)
end)
)MEPLUA";
}
