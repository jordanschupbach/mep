#pragma once

// Hover text stays byte-for-byte intact for focus navigation and yanking.
// Only signatures and explicit examples go through a programming grammar;
// parsing an entire docstring as Python would color ordinary prose as code.
inline constexpr const char *kBuiltinHoverHighlight = R"LUA(
function mep.lsp_hover_content(contents, filetype)
  local function marked(value)
    if type(value) == 'string' then return value end
    if type(value) ~= 'table' then return '' end
    if value.language then
      return '```' .. value.language .. '\n' .. (value.value or '') .. '\n```'
    end
    return value.value or ''
  end
  local markdown = type(contents) == 'string'
  local text
  if type(contents) == 'table' and contents.value == nil and contents.language == nil then
    local parts = {}
    for _, item in ipairs(contents) do parts[#parts + 1] = marked(item) end
    text, markdown = table.concat(parts, '\n\n'), true
  else
    text = marked(contents)
    if type(contents) == 'table' then
      markdown = contents.kind == 'markdown' or contents.language ~= nil
    end
  end

  local spans, lines = {}, {}
  for line in (text .. '\n'):gmatch('(.-)\n') do lines[#lines + 1] = line end
  local function add(row, first, last, hl)
    if last > first then
      spans[#spans + 1] = {row=row, col_start=first, col_end=last, hl=hl}
    end
  end
  local function highlight(lang, source, first, offsets)
    local aliases = mep_org_babel_lang_ts_ft or {python='py'}
    lang = aliases[lang:lower()] or lang
    for _, c in ipairs(mep.ts_captures(lang, source, 'hover:' .. lang) or {}) do
      local map = mep.ts_capture_hl or {}
      local hl = map[c.capture] or map[c.capture:match('^([^.]+)')]
      if hl then
        local offset = offsets and offsets[c.row] or 0
        add(first + c.row - 1, c.col_start + offset, c.col_end + offset, hl)
      end
    end
  end
  if markdown then highlight('md', text, 1) end
  local python = filetype == 'py' or filetype == 'python' or filetype == 'pyi'
  -- Plain Python hovers start with a keyword, declaration or signature,
  -- followed by a blank line and prose (including interpreter docstrings).
  if python and not markdown then
    local head = {}
    for _, line in ipairs(lines) do
      if line:match('^%s*$') then break end
      head[#head + 1] = line
    end
    local source = table.concat(head, '\n')
    if source:match('^async%s+def%s') or source:match('^def%s') or source:match('^class%s') then
      -- Hover headers omit the body (and often the colon). Complete the
      -- declaration for the parser without shifting any displayed bytes.
      -- Interpreter signatures can also have a qualified function name.
      source = source:gsub('^(async%s+def%s+)([%w_%.]+)', function(prefix, name)
        return prefix .. name:gsub('%.', '_')
      end):gsub('^(def%s+)([%w_%.]+)', function(prefix, name)
        return prefix .. name:gsub('%.', '_')
      end):gsub('^(class%s+)([%w_%.]+)', function(prefix, name)
        return prefix .. name:gsub('%.', '_')
      end)
      if not source:match(':%s*$') then source = source .. ':' end
      source = source .. '\n    pass'
    end
    highlight('py', source, 1)
  end

  local row = 1
  while row <= #lines do
    local line = lines[row]
    local fence, lang = line:match('^%s*(```+)%s*([^%s`]*)')
    if not fence then fence, lang = line:match('^%s*(~~~+)%s*([^%s~]*)') end
    if fence then
      local first, body = row + 1, {}
      row = first
      while row <= #lines do
        local closing = lines[row]:match('^%s*([`~]+)%s*$')
        if closing and #closing >= #fence and closing:sub(1, 1) == fence:sub(1, 1) then break end
        body[#body + 1] = lines[row]
        add(row, 1, #lines[row] + 1, 'Normal')
        row = row + 1
      end
      highlight(lang ~= '' and lang or filetype or '', table.concat(body, '\n'), first)
    elseif python and line:match('^%s*>>>') then
      local first, body, offsets = row, {}, {}
      while row <= #lines do
        local prefix, code = lines[row]:match('^(%s*>>> ?)(.*)$')
        if not prefix then prefix, code = lines[row]:match('^(%s*%.%.%. ?)(.*)$') end
        if not prefix then break end
        body[#body + 1], offsets[#offsets + 1] = code, #prefix
        add(row, 1, #prefix + 1, 'Comment')
        row = row + 1
      end
      highlight('py', table.concat(body, '\n'), first, offsets)
      row = row - 1
    elseif python and not markdown then
      -- NumPy/reST docstrings: section names and inline literals have
      -- useful visual structure even though the surrounding text is prose.
      if row < #lines and lines[row + 1]:match('^%s*[-=][-=-]+%s*$') then
        add(row, 1, #line + 1, 'PickerTitle')
      end
      local pos = 1
      while true do
        local a, b = line:find('`[^`]+`', pos)
        if not a then break end
        add(row, a, b + 1, 'Green')
        pos = b + 1
      end
    end
    row = row + 1
  end
  return text, spans
end
)LUA";
