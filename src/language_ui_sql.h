// SQL language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .sql
// buffer), kBuiltinLanguageUi's SQL consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Bash and Clojure
// modes. Included by main.cpp only; BuiltinLanguageUiSql() is the Lua chunk
// (help/sql-mode.org is the user's page). SQLite dialect.
//
// The console is the sqlite3 shell (under rlwrap when present). sqlite3 has
// no prompt hook, so -- unlike the other modes, whose consoles rewrite panel
// files after each prompt -- the panels read the session's database *file*
// with separate read-only `sqlite3 -readonly -json` jobs (mep.job_start),
// refreshed after each statement sent from mep and whenever the file (or its
// -wal / -journal) changes. That is why a file without a `-- database: PATH`
// line gets a scratch database file in the session directory rather than
// :memory:.
//
// Results never re-run a statement: a statement sent from the editor runs
// once, in the console, through a small `.read` script that captures its
// rows with `.mode json` + `.once FILE` (and its timing / rows changed from
// `.timer` / `.changes`, which land in the same file), each statement read
// from its own file named like the source and padded to its own line, so
// sqlite3's error messages point at the right place.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiSql() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.sql_ui_sqlite3 = mep.opt.sql_ui_sqlite3 or 'sqlite3'
mep.opt.sql_ui_rlwrap = mep.opt.sql_ui_rlwrap ~= false
mep.opt.sql_ui_mode = mep.opt.sql_ui_mode or 'box'
mep.opt.sql_ui_timer = mep.opt.sql_ui_timer == true
mep.opt.sql_ui_init = mep.opt.sql_ui_init or {}
mep.opt.sql_ui_row_limit = mep.opt.sql_ui_row_limit or 1000
mep.opt.sql_ui_preview_limit = mep.opt.sql_ui_preview_limit or 200
mep.opt.sql_ui_max_col_width = mep.opt.sql_ui_max_col_width or 40
mep.opt.sql_ui_null = mep.opt.sql_ui_null or 'NULL'
mep.opt.sql_ui_row_counts = mep.opt.sql_ui_row_counts ~= false
mep.opt.sql_ui_info_share = mep.opt.sql_ui_info_share or 0.38
mep.opt.sql_ui_bottom_share = mep.opt.sql_ui_bottom_share or 0.35
mep.opt.sql_ui_bottom_info_share = mep.opt.sql_ui_bottom_info_share or 0.6
mep.language_ui_extensions.sql = mep.language_ui_extensions.sql or 'sql'

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil
local hl_ns = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    schema = mep.sidebar_create('Schema', 'right', 44),
    history = mep.sidebar_create('History', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

local function current() return state[mep.current_tab_id()] end

-- A dot-command argument, single-quoted (the shell takes those literally).
local function dotq(s) return "'" .. tostring(s) .. "'" end
-- A string as an SQL literal / an SQL identifier.
local function sqlstr(s) return "'" .. tostring(s):gsub("'", "''") .. "'" end
local function sqlid(s) return '"' .. tostring(s):gsub('"', '""') .. '"' end

local function basename(p) return (p or ''):match('([^/]+)$') or p end
local function dirname(p) return (p or ''):match('^(.*)/[^/]*$') or '.' end

-- ---- which database --------------------------------------------------------
-- `-- database: PATH` among the leading comment lines names the database
-- (the convention mep-sql-lsp reads too): relative to the file's directory,
-- `~` expanded. ':memory:' is passed through.
local function resolve_db_path(path, base_dir)
  path = path:gsub('^%s+', ''):gsub('%s+$', '')
  path = path:match('^"(.*)"$') or path:match("^'(.*)'$") or path
  if path == '' then return nil end
  if path == ':memory:' then return path end
  if path:sub(1, 2) == '~/' or path == '~' then
    path = (os.getenv('HOME') or '') .. path:sub(2)
  elseif path:sub(1, 1) ~= '/' then
    path = base_dir .. '/' .. path
  end
  -- tidy ./ and dir/../ so the panels show a readable path
  path = path:gsub('/%./', '/')
  local n
  repeat path, n = path:gsub('/[^/]+/%.%./', '/', 1) until n == 0
  return path
end

function mep.sql_ui_database_directive(lines, file)
  for _, line in ipairs(lines or {}) do
    if line:match('^%s*$') then
      -- blank lines between the leading comments are fine
    elseif line:match('^%s*%-%-') then
      local p = line:match('^%s*%-%-%s*[Dd][Aa][Tt][Aa][Bb][Aa][Ss][Ee]%s*:%s*(.-)%s*$')
      if p and p ~= '' then return resolve_db_path(p, dirname(file)) end
    else
      break
    end
  end
  return nil
end

-- ---- statements ------------------------------------------------------------
-- Splits SQL text the way the sqlite3 shell does: sqlite3_complete()'s state
-- machine (complete.c) -- a ';' ends a statement except inside a string,
-- quoted identifier or comment, or inside CREATE TRIGGER ... BEGIN ... END --
-- plus the shell's own rule that a line starting with '.' while no SQL is
-- pending is a dot-command (and '#' such a line a comment). Returns items
-- {kind = 'sql'|'dot', s, e (byte offsets), incomplete}.
local TK_SEMI, TK_WS, TK_OTHER, TK_EXPLAIN, TK_CREATE, TK_TEMP, TK_TRIGGER, TK_END = 1, 2, 3, 4, 5, 6, 7, 8
local trans = {
  [0] = {1, 0, 2, 3, 4, 2, 2, 2},
  [1] = {1, 1, 2, 3, 4, 2, 2, 2},
  [2] = {1, 2, 2, 2, 2, 2, 2, 2},
  [3] = {1, 3, 3, 2, 4, 2, 2, 2},
  [4] = {1, 4, 2, 2, 2, 4, 5, 2},
  [5] = {6, 5, 5, 5, 5, 5, 5, 5},
  [6] = {6, 6, 5, 5, 5, 5, 5, 7},
  [7] = {1, 7, 5, 5, 5, 5, 5, 5},
}
local keywords = {explain = TK_EXPLAIN, create = TK_CREATE, temp = TK_TEMP, temporary = TK_TEMP,
  trigger = TK_TRIGGER, ['end'] = TK_END}

function mep.sql_ui_split(text)
  local items = {}
  local n = #text
  local i = 1
  local st = 0
  local cur, last_end = nil, nil
  local function line_start(k) return k == 1 or text:sub(k - 1, k - 1) == '\n' end
  local function eol(k) return (text:find('\n', k, true) or (n + 1)) end
  while i <= n do
    local c = text:sub(i, i)
    local tok, j = nil, nil
    if not cur and (c == '.' or c == '#') and line_start(i) then
      local e = eol(i)
      if c == '.' then items[#items + 1] = {kind = 'dot', s = i, e = e - 1} end
      i = e
    elseif c:match('%s') then
      tok, j = TK_WS, i + 1
    elseif c == '-' and text:sub(i + 1, i + 1) == '-' then
      tok, j = TK_WS, eol(i)
    elseif c == '/' and text:sub(i + 1, i + 1) == '*' then
      local e = text:find('*/', i + 2, true)
      tok, j = TK_WS, e and (e + 2) or (n + 1)
    elseif c == "'" or c == '"' or c == '`' then
      j = i + 1
      while true do
        local k = text:find(c, j, true)
        if not k then j = n + 1 break end
        if text:sub(k + 1, k + 1) == c then j = k + 2 else j = k + 1 break end
      end
      tok = TK_OTHER
    elseif c == '[' then
      local k = text:find(']', i + 1, true)
      tok, j = TK_OTHER, k and (k + 1) or (n + 1)
    elseif c == ';' then
      tok, j = TK_SEMI, i + 1
    elseif c:match('[%a_\128-\255]') then
      local word = text:match('^[%w_$\128-\255]+', i)
      tok, j = keywords[word:lower()] or TK_OTHER, i + #word
    else
      tok, j = TK_OTHER, i + 1
    end
    if tok then
      if tok ~= TK_WS then
        if not cur and tok ~= TK_SEMI then cur = i end
        last_end = j - 1
      end
      st = trans[st][tok]
      if tok == TK_SEMI and st == 1 then
        if cur then items[#items + 1] = {kind = 'sql', s = cur, e = i} end
        cur = nil
      end
      i = j
    end
  end
  if cur then items[#items + 1] = {kind = 'sql', s = cur, e = last_end, incomplete = true} end
  return items
end

-- byte offset -> 1-based row, col
local function line_index(text)
  local starts = {1}
  for p in text:gmatch('()\n') do starts[#starts + 1] = p + 1 end
  return function(off)
    local lo, hi = 1, #starts
    while lo < hi do
      local mid = (lo + hi + 1) // 2
      if starts[mid] <= off then lo = mid else hi = mid - 1 end
    end
    return lo, off - starts[lo] + 1
  end
end

-- Items of `text` with their text and positions; `row0`/`col0` place the
-- text's first byte in the source.
local function statements(text, row0, col0)
  local pos = line_index(text)
  local out = {}
  for _, it in ipairs(mep.sql_ui_split(text)) do
    local r, c = pos(it.s)
    local er = pos(it.e)
    out[#out + 1] = {kind = it.kind, text = text:sub(it.s, it.e), incomplete = it.incomplete,
      row = r + row0 - 1, col = (r == 1) and (c + col0 - 1) or c, end_row = er + row0 - 1, s = it.s, e = it.e}
  end
  return out
end

-- The statement (or dot-command) at the cursor: the one around it, else the
-- nearest one above, else the first one below.
local function statement_at_cursor()
  local lines = {}
  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) or '' end
  local text = table.concat(lines, '\n')
  local row, col = mep.cursor()
  local items = statements(text, 1, 1)
  if #items == 0 then return nil end
  local off = 0
  for i = 1, row - 1 do off = off + #lines[i] + 1 end
  off = off + math.min(col, #(lines[row] or '') + 1)
  local before = nil
  for _, it in ipairs(items) do
    local line_end = (text:find('\n', it.e, true) or (#text + 1)) - 1
    if off >= it.s and off <= math.max(it.e, line_end) then return it end
    if it.s <= off then before = it end
  end
  return before or items[1]
end

-- ---- JSON rows (sqlite3's .mode json: one row object per line) -------------
local function jstr(s, i)
  local out = {}
  local j = i + 1
  while true do
    local k = s:find('[\\"]', j)
    if not k then return nil end
    out[#out + 1] = s:sub(j, k - 1)
    if s:sub(k, k) == '"' then return table.concat(out), k + 1 end
    local e = s:sub(k + 1, k + 1)
    if e == 'u' then
      local cp = tonumber(s:sub(k + 2, k + 5), 16)
      j = k + 6
      if cp and cp >= 0xD800 and cp <= 0xDBFF and s:sub(j, j + 1) == '\\u' then
        local lo = tonumber(s:sub(j + 2, j + 5), 16)
        if lo and lo >= 0xDC00 and lo <= 0xDFFF then
          cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
          j = j + 6
        end
      end
      out[#out + 1] = (cp and cp < 0x110000) and utf8.char(cp) or '?'
    else
      out[#out + 1] = ({b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'})[e] or e
      j = k + 2
    end
  end
end

-- {cols = {...}, vals = {...}, kinds = {'s'|'n'|'null'}} for one row line,
-- column order and duplicate names kept.
function mep.sql_ui_json_row(line)
  local i = line:find('{', 1, true)
  if not i then return nil end
  local row = {cols = {}, vals = {}, kinds = {}}
  i = i + 1
  while true do
    i = line:find('[^%s,]', i)
    if not i then return nil end
    local c = line:sub(i, i)
    if c == '}' then break end
    if c ~= '"' then return nil end
    local key
    key, i = jstr(line, i)
    if not key then return nil end
    i = line:find(':', i, true)
    if not i then return nil end
    i = line:find('%S', i + 1)
    if not i then return nil end
    c = line:sub(i, i)
    local v, kind
    if c == '"' then
      v, i = jstr(line, i)
      if not v then return nil end
      kind = 's'
    elseif line:sub(i, i + 3) == 'null' then
      kind, i = 'null', i + 4
    else
      v = line:match('^[^,}%s]+', i)
      if not v then return nil end
      kind, i = 'n', i + #v
    end
    local k = #row.cols + 1
    row.cols[k], row.vals[k], row.kinds[k] = key, v, kind
  end
  return row
end

-- ---- grids -----------------------------------------------------------------
local function dlen(s) return utf8.len(s) or #s end
local function clip(s, w)
  s = s:gsub('\r\n', '\n'):gsub('\n', '\\n'):gsub('\t', ' ')
  if dlen(s) <= w then return s end
  if not utf8.len(s) then return s:sub(1, w - 1) .. '\u{2026}' end
  return s:sub(1, (utf8.offset(s, w) or (#s + 1)) - 1) .. '\u{2026}'
end
local function pad(s, w, right)
  local fill = string.rep(' ', w - dlen(s))
  return right and (fill .. s) or (s .. fill)
end

-- Grid lines for a result set (`rows` from mep.sql_ui_json_row): columns
-- split by a bar, a rule under the header, numbers right-aligned. Only the
-- plain rule and bar: the text panes' font has no box corners.
function mep.sql_ui_grid(rows)
  if #rows == 0 then return {} end
  local maxw = math.max(4, mep.opt.sql_ui_max_col_width or 40)
  local cols = {}
  local w, cells = {}, {}
  for c, name in ipairs(rows[1].cols) do
    cols[c] = clip(name, maxw)
    w[c] = dlen(cols[c])
  end
  for r, row in ipairs(rows) do
    cells[r] = {}
    for c = 1, #cols do
      local kind = row.kinds[c]
      local t = kind == 'null' and mep.opt.sql_ui_null or clip(tostring(row.vals[c] or ''), maxw)
      cells[r][c] = t
      if dlen(t) > w[c] then w[c] = dlen(t) end
    end
  end
  local bar = ' \u{2502} '
  local hdr = {}
  for c = 1, #cols do hdr[c] = pad(cols[c], w[c]) end
  local out = {(table.concat(hdr, bar):gsub('%s+$', ''))}
  local width = #cols * 3 - 3
  for c = 1, #cols do width = width + w[c] end
  out[2] = string.rep('\u{2500}', width)
  for r, row in ipairs(rows) do
    local parts = {}
    for c = 1, #cols do parts[c] = pad(cells[r][c], w[c], row.kinds[c] == 'n') end
    out[#out + 1] = (table.concat(parts, bar):gsub('%s+$', ''))
  end
  return out
end

local function fmt_time(secs)
  if not secs then return nil end
  if secs < 1 then return string.format('%.1f ms', secs * 1000) end
  return string.format('%.2f s', secs)
end
local function plural(n, one, many) return n .. ' ' .. (n == 1 and one or many) end
local function one_line(s, max)
  s = s:gsub('%-%-[^\n]*', ''):gsub('%s+', ' '):gsub('^ ', ''):gsub(' $', '')
  if max and dlen(s) > max then s = clip(s, max) end
  return s
end

-- ---- panels ----------------------------------------------------------------
local function set_results(st, lines, title_rows)
  if not st.results_buf or not mep.buffer_valid(st.results_buf) then return end
  if #lines == 0 then lines = {''} end
  mep.buffer_set_lines(st.results_buf, lines)
  hl_ns = hl_ns or mep.ns_create('mep_sql_ui')
  mep.buffer_ns_clear(st.results_buf, hl_ns)
  for i, line in ipairs(lines) do
    if line:sub(1, 2) == '--' then
      mep.buffer_deco_add(st.results_buf, hl_ns, {row = i, whole_line = true, hl_group = 'Comment'})
    end
  end
  if title_rows then
    for _, r in ipairs(title_rows) do
      mep.buffer_deco_add(st.results_buf, hl_ns, {row = r, col_start = 1, col_end = #lines[r] + 1, bold = true})
    end
  end
  mep.reveal_buffer(st.results_buf, true)
end

local function results_placeholder(st)
  set_results(st, {
    '-- Results',
    '--',
    '-- <leader>re in the source runs the statement at the cursor (or the',
    '-- Visual selection) in the console; what it returns shows here.',
    '-- Click a table in Schema to preview its rows here.',
  })
end

local function render_history()
  local sb = sidebars
  if not sb then return end
  local st = current()
  local widgets = {}
  for i = (st and #st.history or 0), 1, -1 do
    local h = st.history[i]
    widgets[#widgets + 1] = {id = 'h' .. i, text = one_line(h.text), wrap = true, wrap_indent = 0,
      tooltip = 'run again', on_click = function()
        if h.raw then mep.terminal_write(st.console_buf, h.raw) else mep.sql_ui_send(st, h.items, h.label, h.base) end
      end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing sent yet -- <leader>re runs the statement at the cursor)'} end
  mep.sidebar_set_sections(sb.history, {{id = 'history', title = '', collapsed = false, widgets = widgets}})
end

local function insert_name(st, name)
  if mep.pane_focus(st.editor_pane) then mep.insert_text(name) end
end

local function render_schema()
  local sb = sidebars
  if not sb then return end
  local st = current()
  if not st then return end
  local sections = {}
  local db_label = st.db
  local home = os.getenv('HOME')
  local src_dir = dirname(st.source_path) .. '/'
  if st.scratch then
    db_label = 'scratch database (this session only)'
  elseif db_label:sub(1, #src_dir) == src_dir then
    db_label = db_label:sub(#src_dir + 1)
  elseif home and home ~= '' and db_label:sub(1, #home + 1) == home .. '/' then
    db_label = '~' .. db_label:sub(#home + 1)
  end
  local head = {{id = 'db', text = 'db  ' .. db_label, tooltip = st.db}}
  if st.schema_note then head[#head + 1] = {id = 'note', text = st.schema_note, wrap = true, wrap_indent = 0} end
  sections[1] = {id = 'db', title = '', collapsed = false, widgets = head}
  local sc = st.schema
  if sc then
    local function objects(kind, title, with_cols)
      local widgets = {}
      for _, o in ipairs(sc.list) do
        if o.type == kind then
          local text = o.name
          if kind == 'table' and o.count then text = text .. '  (' .. plural(o.count, 'row', 'rows') .. ')' end
          if (kind == 'index' or kind == 'trigger') and o.tbl ~= o.name then
            text = text .. '  on ' .. o.tbl
            if kind == 'index' and #o.cols > 0 then
              local names = {}
              for _, c in ipairs(o.cols) do names[#names + 1] = c.name end
              text = text .. '(' .. table.concat(names, ', ') .. ')'
            end
          end
          widgets[#widgets + 1] = {id = kind .. ':' .. o.name, text = text,
            tooltip = with_cols and ('preview ' .. o.name .. ' in Results') or ('help ' .. o.name),
            on_click = function()
              if with_cols then mep.sql_ui_preview(st, o.name) else mep.sql_ui_help(o.name) end
            end}
          if with_cols then
            for _, c in ipairs(o.cols) do
              local flags = {}
              if c.pk and c.pk > 0 then flags[#flags + 1] = 'PK' end
              if c.notnull then flags[#flags + 1] = 'NOT NULL' end
              widgets[#widgets + 1] = {id = kind .. ':' .. o.name .. '.' .. c.name,
                text = '    ' .. c.name .. (c.type ~= '' and ('  ' .. c.type) or '') ..
                  (#flags > 0 and ('  ' .. table.concat(flags, ' ')) or ''),
                tooltip = 'insert ' .. c.name .. ' at the cursor',
                on_click = function() insert_name(st, c.name) end}
            end
          end
        end
      end
      if #widgets > 0 then
        sections[#sections + 1] = {id = kind, title = title .. ' (' .. sc.counts[kind] .. ')', collapsed = false, widgets = widgets}
      end
    end
    objects('table', 'Tables', true)
    objects('view', 'Views', true)
    objects('index', 'Indexes', false)
    objects('trigger', 'Triggers', false)
    if #sections == 1 then
      head[#head + 1] = {id = 'empty', text = '(no tables yet -- CREATE TABLE in the source, then <leader>re)', wrap = true}
    end
  end
  mep.sidebar_set_sections(sb.schema, sections)
end

-- Like mep.language_ui_render_textbox, except that indented lines (column
-- lists, CREATE statements) are left unwrapped so their layout survives.
local function render_help()
  local st = current()
  if not sidebars then return end
  local widgets = {}
  for line in (((st and st.help_text) or ''):gsub('\r', '') .. '\n'):gmatch('([^\n]*)\n') do
    local indented = line:match('^%s') ~= nil
    widgets[#widgets + 1] = {id = tostring(#widgets + 1), text = line == '' and ' ' or line,
      wrap = not indented, wrap_indent = 0}
  end
  while #widgets > 0 and widgets[#widgets].text == ' ' do widgets[#widgets] = nil end
  if #widgets == 0 then
    widgets[1] = {id = 'empty', text = '(no help viewed yet -- gh over a table, column, function or keyword)', wrap = true}
  end
  mep.sidebar_set_sections(sidebars.help, {{id = 'content', title = '', collapsed = false, widgets = widgets}})
end

local function render_all()
  render_schema()
  render_history()
  render_help()
end

-- ---- read-only side jobs ----------------------------------------------------
-- The panels never ask the console: they open the same database file in a
-- separate read-only sqlite3 (`-readonly -json`), so nothing typed into the
-- console is disturbed and nothing is executed twice.
local function db_readable(st)
  return st.db ~= ':memory:' and mep.fs_exists(st.db)
end

local function ro_query(st, sql, cb)
  local rows, err = {}, {}
  mep.job_start({st.sqlite3, '-readonly', '-json', '-cmd', '.timeout 2000', st.db, sql}, {
    on_stdout = function(line)
      local r = mep.sql_ui_json_row(line)
      if r then rows[#rows + 1] = r end
    end,
    on_stderr = function(line) err[#err + 1] = line end,
    on_exit = function(code)
      cb(code == 0 and rows or nil, table.concat(err, '\n'), code)
    end,
  })
end

local function named(row)
  local t = {}
  for i, c in ipairs(row.cols) do t[c] = row.kinds[i] ~= 'null' and row.vals[i] or nil end
  return t
end

local SCHEMA_SQL = [[
SELECT 'o' AS k, type AS t, name AS n, tbl_name AS tb, NULL AS c, NULL AS ct, NULL AS nn, NULL AS pk
  FROM sqlite_schema WHERE name NOT LIKE 'sqlite\_%' ESCAPE '\'
UNION ALL
SELECT 'c', m.type, m.name, m.tbl_name, p.name, p.type, p."notnull", p.pk
  FROM sqlite_schema m, pragma_table_info(m.name) p
  WHERE m.type IN (@TYPES@) AND m.name NOT LIKE 'sqlite\_%' ESCAPE '\'
UNION ALL
SELECT 'i', m.type, m.name, m.tbl_name, p.name, NULL, NULL, p.seqno
  FROM sqlite_schema m, pragma_index_info(m.name) p
  WHERE m.type = 'index' AND m.name NOT LIKE 'sqlite\_%' ESCAPE '\'
]]

local function schema_from_rows(rows)
  local sc = {list = {}, by_name = {}, counts = {table = 0, view = 0, index = 0, trigger = 0}}
  for _, r in ipairs(rows) do
    local t = named(r)
    if t.k == 'o' then
      local o = {type = t.t, name = t.n, tbl = t.tb, cols = {}}
      sc.list[#sc.list + 1] = o
      sc.by_name[t.n:lower()] = o
      sc.counts[t.t] = (sc.counts[t.t] or 0) + 1
    end
  end
  for _, r in ipairs(rows) do
    local t = named(r)
    local o = t.k ~= 'o' and sc.by_name[(t.n or ''):lower()]
    if o then
      o.cols[#o.cols + 1] = {name = t.c or '', type = t.ct or '', notnull = t.nn == '1', pk = tonumber(t.pk)}
    end
  end
  table.sort(sc.list, function(a, b) return a.name:lower() < b.name:lower() end)
  return sc
end

local function refresh_counts(st, sc)
  if not mep.opt.sql_ui_row_counts then return end
  local parts = {}
  for _, o in ipairs(sc.list) do
    if o.type == 'table' and #parts < 200 then
      parts[#parts + 1] = 'SELECT ' .. sqlstr(o.name) .. ' AS n, count(*) AS c FROM ' .. sqlid(o.name)
    end
  end
  if #parts == 0 then return end
  ro_query(st, table.concat(parts, ' UNION ALL '), function(rows)
    if not rows or st.schema ~= sc then return end
    for _, r in ipairs(rows) do
      local t = named(r)
      local o = sc.by_name[(t.n or ''):lower()]
      if o then o.count = tonumber(t.c) end
    end
    if current() == st then render_schema() end
  end)
end

function mep.sql_ui_refresh_schema(st)
  st = st or current()
  if not st then return end
  if st.schema_busy then st.schema_again = true return end
  if st.db == ':memory:' then
    st.schema_note = '(an in-memory database lives only inside the console, so the panels cannot read it; use a file to see its schema)'
    st.schema = nil
    if current() == st then render_schema() end
    return
  end
  if not mep.fs_exists(st.db) then
    st.schema_note = '(new database: the file is created by the first statement that writes to it)'
    st.schema = {list = {}, by_name = {}, counts = {table = 0, view = 0, index = 0, trigger = 0}}
    if current() == st then render_schema() end
    return
  end
  st.schema_busy = true
  local function done(rows, err)
    st.schema_busy = false
    if state[st.tid] ~= st then return end
    if rows then
      st.schema = schema_from_rows(rows)
      st.schema_note = nil
      refresh_counts(st, st.schema)
    else
      st.schema_note = 'schema: ' .. (err ~= '' and err or 'sqlite3 failed')
    end
    if current() == st then render_schema() end
    if st.schema_again then
      st.schema_again = false
      mep.sql_ui_refresh_schema(st)
    end
  end
  ro_query(st, (SCHEMA_SQL:gsub('@TYPES@', "'table','view'")), function(rows, err)
    -- A view over a missing table makes pragma_table_info fail for the
    -- whole query: list the views without their columns then.
    if rows then return done(rows, err) end
    ro_query(st, (SCHEMA_SQL:gsub('@TYPES@', "'table'")), function(rows2, err2)
      if rows2 then
        done(rows2, err2)
        st.schema_note = 'a view does not compile: ' .. (err:match('[^\n]+') or err)
        if current() == st then render_schema() end
      else
        done(nil, err)
      end
    end)
  end)
end

-- What changed on disk: the database and its -wal / -journal.
local function db_signature(st)
  if st.db == ':memory:' then return '' end
  local parts = {}
  for _, suffix in ipairs({'', '-wal', '-journal'}) do parts[#parts + 1] = tostring(mep.fs_mtime(st.db .. suffix)) end
  return table.concat(parts, ':')
end

function mep.sql_ui_preview(st, name)
  st = st or current()
  if not st then return end
  if not db_readable(st) then mep.notify('Nothing to preview: the database has no file yet', 'warn') return end
  local limit = mep.opt.sql_ui_preview_limit
  set_results(st, {'-- ' .. name .. ': loading...'})
  ro_query(st, 'SELECT * FROM ' .. sqlid(name) .. ' LIMIT ' .. math.floor(limit), function(rows, err)
    if state[st.tid] ~= st then return end
    local o = st.schema and st.schema.by_name[name:lower()]
    local lines = {'-- ' .. name}
    if not rows then
      lines[2] = '-- ' .. (err ~= '' and err:gsub('\n', ' ') or 'sqlite3 failed')
      set_results(st, lines, {1})
      return
    end
    local total = o and o.count
    local info = (total and plural(total, 'row', 'rows') or plural(#rows, 'row', 'rows'))
    if #rows >= limit and (not total or total > #rows) then info = info .. ', first ' .. limit .. ' shown' end
    lines[2] = '-- ' .. info .. ' (read from the database file: uncommitted changes in the console are not seen)'
    if #rows == 0 then
      lines[3] = '(no rows)'
    else
      for _, l in ipairs(mep.sql_ui_grid(rows)) do lines[#lines + 1] = l end
    end
    set_results(st, lines, {1})
  end)
end

-- ---- sending to the console -------------------------------------------------
-- A statement sent from the editor runs once, in the console, with its rows
-- captured for Results: the console gets one `.read` of a small script that
-- switches to `.mode json`, points `.once` at a file for each statement, and
-- `.read`s each statement from its own file -- named like the source and
-- padded to the statement's own line and column, so an error reads "near
-- line 12 of orders.sql". `.timer`/`.changes` lines land in the same file
-- and give the timing and the rows changed. A dot-command runs as itself.
local function fs_mkdirs(path)
  local acc = ''
  for part in path:gmatch('[^/]+') do
    acc = acc .. '/' .. part
    if not mep.fs_exists(acc) then mep.fs_mkdir(acc) end
  end
end

local function cleanup_send(send)
  if not send then return end
  for i = #send.files, 1, -1 do os.remove(send.files[i]) end
end

-- `base`: the file name the statements are read from (the buffer they were
-- sent from), which sqlite3 names in its error messages.
function mep.sql_ui_send(st, items, label, base)
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No SQL console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  if #items == 0 then mep.notify('Nothing to run', 'warn') return end
  st.seq = st.seq + 1
  local dir = st.session_dir .. '/send/' .. st.seq
  fs_mkdirs(dir)
  local send = {seq = st.seq, dir = dir, files = {dir}, stmts = {}, label = label, started = mep.now()}
  local function file(rel, text)
    local p = dir .. '/' .. rel
    if text then mep.language_ui_write_file(p, text) end
    send.files[#send.files + 1] = p
    return p
  end
  if not base or base == '' then base = st.source_name or 'statement.sql' end
  local w = {}
  local sql_count, mode, timer = 0, st.mode, st.timer
  for _, it in ipairs(items) do
    if it.kind == 'dot' then
      local line = it.text:gsub('%s+$', '')
      local m = line:match('^%.mode%s+(%S+)')
      if m then mode = m end
      local t = line:match('^%.timer%s+(%S+)')
      if t then timer = (t == 'on' or t == '1') end
      w[#w + 1] = line
    else
      sql_count = sql_count + 1
      local k = sql_count
      local sub = dir .. '/s' .. k
      mep.fs_mkdir(sub)
      local body = string.rep('\n', math.max(0, it.row - 1)) .. string.rep(' ', math.max(0, it.col - 1)) ..
        it.text .. (it.incomplete and '\n;' or '') .. '\n'
      local path = sub .. '/' .. base
      mep.language_ui_write_file(path, body)
      send.files[#send.files + 1] = sub
      send.files[#send.files + 1] = path
      local out = file('o' .. k .. '.txt')
      local before = file('b' .. k .. '.txt')
      send.stmts[k] = {text = it.text, out = out, before = before}
      -- `changes:` after a no-op statement gives the total before this one
      w[#w + 1] = '.changes on'
      w[#w + 1] = '.once ' .. dotq(before)
      w[#w + 1] = 'SELECT 0 WHERE 0;'
      w[#w + 1] = '.timer on'
      w[#w + 1] = '.mode json'
      w[#w + 1] = '.once ' .. dotq(out)
      w[#w + 1] = '.read ' .. dotq(path)
      w[#w + 1] = '.mode ' .. mode
      w[#w + 1] = '.changes off'
      w[#w + 1] = '.timer ' .. (timer and 'on' or 'off')
    end
  end
  st.mode, st.timer = mode, timer
  send.done = file('done.txt')
  w[#w + 1] = '.once ' .. dotq(send.done)
  w[#w + 1] = '.print ok'
  local script = file('run.sql', table.concat(w, '\n') .. '\n')
  -- an older send still running (or interrupted) is superseded
  if st.pending then cleanup_send(st.pending) end
  st.pending = send
  if sql_count > 0 then
    set_results(st, {'-- running ' .. (label or one_line(send.stmts[1].text, 70)) .. ' ...',
      '-- (Ctrl-C in the console interrupts it)'})
  end
  local text = {}
  for _, it in ipairs(items) do text[#text + 1] = it.text end
  local entry = {text = table.concat(text, '\n'), items = items, label = label, base = base}
  local last = st.history[#st.history]
  if not last or last.text ~= entry.text then
    st.history[#st.history + 1] = entry
    if #st.history > 200 then table.remove(st.history, 1) end
  end
  render_history()
  mep.terminal_write(st.console_buf, '.read ' .. dotq(script) .. '\n')
end

-- One statement's capture file -> {rows, count, text, secs, changed, failed}
local function read_capture(path, base_total, limit)
  local f = io.open(path, 'r')
  if not f then return {missing = true} end
  local res = {rows = {}, count = 0, text = {}}
  for line in f:lines() do
    local c1 = line:sub(1, 1)
    if c1 == '{' or line:sub(1, 2) == '[{' then
      res.count = res.count + 1
      if res.count <= limit then
        local r = mep.sql_ui_json_row(line)
        if r then res.rows[#res.rows + 1] = r end
      end
    elseif line:match('^Run Time: real ') then
      res.secs = tonumber(line:match('^Run Time: real ([%d%.]+)'))
    elseif line:match('^changes: ') then
      res.total = tonumber(line:match('total_changes:%s*(%d+)'))
    elseif line ~= '' and line ~= ']' then
      res.text[#res.text + 1] = line
    end
  end
  f:close()
  res.failed = res.total == nil
  if res.total and base_total then res.changed = res.total - base_total end
  return res
end

local function finish_send(st, send)
  local limit = mep.opt.sql_ui_row_limit
  local lines, titles = {}, {}
  for _, s in ipairs(send.stmts) do
    local res = read_capture(s.out, read_capture(s.before, nil, 0).total, limit)
    if #lines > 0 then lines[#lines + 1] = '' end
    lines[#lines + 1] = '-- ' .. one_line(s.text, 100)
    titles[#titles + 1] = #lines
    local info
    if res.missing then
      info = 'not run'
    elseif res.failed then
      info = 'error -- the message is in the console'
    elseif res.count > 0 then
      info = plural(res.count, 'row', 'rows')
      if res.count > #res.rows then info = info .. ', first ' .. #res.rows .. ' shown (mep.opt.sql_ui_row_limit)' end
    elseif res.changed and res.changed > 0 then
      info = plural(res.changed, 'row changed', 'rows changed')
    else
      info = 'done, no rows'
    end
    if res.secs and not res.missing then info = info .. ' \u{00b7} ' .. fmt_time(res.secs) end
    lines[#lines + 1] = '-- ' .. info
    for _, l in ipairs(res.text) do lines[#lines + 1] = l end
    if #res.rows > 0 then
      for _, l in ipairs(mep.sql_ui_grid(res.rows)) do lines[#lines + 1] = l end
    end
  end
  if #send.stmts > 0 then set_results(st, lines, titles) end
  cleanup_send(send)
  mep.sql_ui_refresh_schema(st)
  st.sig = db_signature(st)
end

-- ---- help ------------------------------------------------------------------
local function describe_tool()
  if mep.opt.sql_ui_describe then return mep.opt.sql_ui_describe end
  local p = mep.bundled_tool('mep-sql-lsp')
  if p:sub(1, 1) == '/' then return p end
  if mep_org_babel_has_exe('mep-sql-lsp') then return 'mep-sql-lsp' end
  return nil
end

local function show_help(st, text)
  st.help_text = text
  if current() == st then
    render_help()
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  end
end

local function help_object(st, o)
  local lines = {o.type .. ' ' .. o.name}
  if o.type ~= 'table' and o.type ~= 'view' and o.tbl and o.tbl ~= o.name then lines[1] = lines[1] .. ' on ' .. o.tbl end
  if o.count then lines[#lines + 1] = plural(o.count, 'row', 'rows') end
  if (o.type == 'table' or o.type == 'view') and #o.cols > 0 then
    lines[#lines + 1] = ''
    lines[#lines + 1] = 'Columns:'
    for _, c in ipairs(o.cols) do
      local flags = {}
      if c.pk and c.pk > 0 then flags[#flags + 1] = 'PRIMARY KEY' end
      if c.notnull then flags[#flags + 1] = 'NOT NULL' end
      lines[#lines + 1] = '  ' .. c.name .. (c.type ~= '' and ('  ' .. c.type) or '') ..
        (#flags > 0 and ('  ' .. table.concat(flags, ' ')) or '')
    end
  end
  show_help(st, table.concat(lines, '\n'))
  ro_query(st, 'SELECT type AS t, name AS n, sql AS s FROM sqlite_schema WHERE name = ' .. sqlstr(o.name) ..
    " OR (tbl_name = " .. sqlstr(o.name) .. " AND type IN ('index', 'trigger') AND sql IS NOT NULL)", function(rows)
    if not rows or #rows == 0 then return end
    local more = {'', 'Definition:'}
    local related = {}
    for _, r in ipairs(rows) do
      local t = named(r)
      if t.n == o.name then
        for l in ((t.s or '(built in)') .. '\n'):gmatch('([^\n]*)\n') do more[#more + 1] = '  ' .. l end
      else
        related[#related + 1] = t
      end
    end
    for _, t in ipairs(related) do
      more[#more + 1] = ''
      more[#more + 1] = t.t .. ' ' .. t.n .. ':'
      for l in ((t.s or '') .. '\n'):gmatch('([^\n]*)\n') do more[#more + 1] = '  ' .. l end
    end
    show_help(st, table.concat(lines, '\n') .. '\n' .. table.concat(more, '\n'))
  end)
end

-- gh / :MepSqlUiHelp: a table, view, index or trigger -> its definition from
-- the database; a column -> the tables that have it; anything else (a
-- function, keyword, PRAGMA) -> `mep-sql-lsp --describe NAME`.
function mep.sql_ui_help(name)
  local st = current()
  if not st then return false end
  name = name:gsub('^["`%[]', ''):gsub('["`%]]$', '')
  local sc = st.schema
  local o = sc and sc.by_name[name:lower()]
  if o then help_object(st, o) return true end
  if sc then
    local hits = {}
    for _, t in ipairs(sc.list) do
      for _, c in ipairs(t.cols) do
        if (t.type == 'table' or t.type == 'view') and c.name:lower() == name:lower() then
          local flags = {}
          if c.pk and c.pk > 0 then flags[#flags + 1] = 'PRIMARY KEY' end
          if c.notnull then flags[#flags + 1] = 'NOT NULL' end
          hits[#hits + 1] = '  ' .. t.name .. '.' .. c.name .. (c.type ~= '' and ('  ' .. c.type) or '') ..
            (#flags > 0 and ('  ' .. table.concat(flags, ' ')) or '')
        end
      end
    end
    if #hits > 0 then
      show_help(st, 'column ' .. name .. '\n\n' .. table.concat(hits, '\n'))
      return true
    end
  end
  local tool = describe_tool()
  if not tool then
    show_help(st, 'Nothing named ' .. name .. ' in the database.\n\n(SQL keywords and functions are described by mep-sql-lsp, which was not found beside mep or on PATH.)')
    return true
  end
  local out = {}
  mep.job_start({tool, '--describe', name}, {
    on_stdout = function(line) out[#out + 1] = line end,
    on_exit = function(code)
      if state[st.tid] ~= st then return end
      if code == 0 and #out > 0 then
        show_help(st, table.concat(out, '\n'))
      else
        show_help(st, 'Nothing named ' .. name .. ' in the database, and no SQL documentation for it.')
      end
    end,
  })
  return true
end

function mep.sql_ui_help_at_cursor()
  local st = current()
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_$\128-\255]')
  if not sym or sym == '' then mep.notify('gh: no SQL name under the cursor', 'warn') return true end
  mep.sql_ui_help(sym)
  if st.help_buf then mep.jump_to_buffer(st.help_buf) end
  return true
end
mep.help_register_provider('sql', mep.sql_ui_help_at_cursor)
mep.command('MepSqlUiHelp', function(args)
  if not current() then mep.notify('No SQL language UI in this tab -- <leader>uu in a .sql file opens one', 'warn') return end
  if args and args:match('%S') then mep.sql_ui_help(args:match('^%s*(.-)%s*$')) end
end)

-- ---- <leader>re --------------------------------------------------------------
function mep.sql_ui_eval()
  local st = current()
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No SQL console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  local sel = mep.visual_selection()
  local items
  if sel ~= '' then
    local r = mep.visual_range()
    items = statements(sel, r and r.start_row or 1, (r and r.kind ~= 'line') and r.start_col or 1)
    mep.enter_normal()
    if #items == 0 then mep.notify('No statement in the selection', 'warn') return end
  else
    local it = statement_at_cursor()
    if not it then mep.notify('No statement at the cursor', 'warn') return end
    items = {it}
  end
  mep.sql_ui_send(st, items, nil, basename(mep.buffer_filename(mep.current_buffer())))
end
mep.language_ui_eval_handlers.sql = mep.sql_ui_eval

-- :MepSqlUiDatabase [PATH]: show, or switch, the session's database (the
-- console `.open`s it; relative to the source file's directory).
mep.command('MepSqlUiDatabase', function(args)
  local st = current()
  if not st then mep.notify('No SQL language UI in this tab -- <leader>uu in a .sql file opens one', 'warn') return end
  local p = args and args:match('^%s*(.-)%s*$') or ''
  if p == '' then mep.notify('SQL UI database: ' .. st.db) return end
  local path = resolve_db_path(p, dirname(st.source_path))
  if not path then return end
  if path ~= ':memory:' and not mep.fs_exists(dirname(path)) then
    mep.notify('No directory ' .. dirname(path), 'error')
    return
  end
  mep.terminal_write(st.console_buf, '.open ' .. dotq(path) .. '\n')
  st.db, st.scratch, st.sig = path, false, nil
  st.schema = nil
  mep.sql_ui_refresh_schema(st)
  mep.notify('SQL UI: now on ' .. path)
end)

-- ---- open / close ------------------------------------------------------------
function mep.sql_ui_open()
  local sqlite3 = mep.opt.sql_ui_sqlite3
  if not mep_org_babel_has_exe(sqlite3) then
    mep.notify('SQL UI: ' .. sqlite3 .. " not found on PATH -- install SQLite's command-line shell (e.g. `nix profile install nixpkgs#sqlite`) or set mep.opt.sql_ui_sqlite3", 'error')
    return nil
  end
  local source_buf = mep.current_buffer()
  local source_path = mep_lsp_abspath(mep.buffer_filename(source_buf))
  local session_dir = mep.language_ui_session_dir('sqlui')
  local db = mep.sql_ui_database_directive(mep.buffer_get_lines(source_buf), source_path)
  local scratch = false
  if not db then db, scratch = session_dir .. '/scratch.db', true end
  if db ~= ':memory:' and not mep.fs_exists(dirname(db)) then
    mep.notify('SQL UI: no directory ' .. dirname(db) .. ' for the database named in the file', 'error')
    return nil
  end
  local argv = {}
  if mep.opt.sql_ui_rlwrap and mep_org_babel_has_exe('rlwrap') then argv[1] = 'rlwrap' end
  argv[#argv + 1] = sqlite3
  local init = {'.headers on', '.mode ' .. mep.opt.sql_ui_mode, '.nullvalue ' .. mep.opt.sql_ui_null}
  if mep.opt.sql_ui_timer then init[#init + 1] = '.timer on' end
  for _, c in ipairs(mep.opt.sql_ui_init) do init[#init + 1] = c end
  init[#init + 1] = '.print ' .. dotq('-- mep: database ' .. db .. (scratch and ' (a scratch file for this session)' or '') ..
    '; <leader>re runs the statement at the cursor, <leader>rr the file')
  for _, c in ipairs(init) do argv[#argv + 1] = '-cmd'; argv[#argv + 1] = c end
  argv[#argv + 1] = db
  local layout = mep.language_ui_layout({
    info_share = mep.opt.sql_ui_info_share, bottom_share = mep.opt.sql_ui_bottom_share,
    bottom_info_share = mep.opt.sql_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'sqlite3') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local sb = sidebars
  local st = {
    tid = tid, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    session_dir = session_dir, source_path = source_path, source_name = basename(source_path),
    sqlite3 = sqlite3, db = db, scratch = scratch, mode = mep.opt.sql_ui_mode, timer = mep.opt.sql_ui_timer,
    seq = 0, history = {}, help_text = '', schema = nil, sig = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sb.schema, sb.history})
  -- bottom right: Results (a plain buffer, so a wide grid scrolls
  -- sideways) and Help
  mep.pane_focus(layout.bottom_pane)
  mep.pane_open(session_dir .. '/Results')
  st.results_buf = mep.current_buffer()
  mep.buffer_set_hide_line_numbers(st.results_buf, true)
  mep.buffer_set_wrap(st.results_buf, false)
  mep.sidebar_open_pane(sb.help)
  st.help_buf = mep.current_buffer()
  mep.pane_prev_buffer()
  mep.pane_prev_buffer()
  mep.pane_close_buffer()
  results_placeholder(st)
  mep.pane_focus(layout.editor_pane)
  mep.sql_ui_refresh_schema(st)
  st.sig = db_signature(st)
  mep.notify('SQL language UI (' .. (scratch and 'scratch database' or basename(db)) ..
    '): sqlite3 below; Schema/History top-right, Results/Help bottom-right; <leader>re runs the statement at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- writes
  -- the buffer and `.read`s it in the console, whose output it is.
  local function run_source(fname)
    mep.cmd('write')
    local path = mep_lsp_abspath(fname)
    -- a `.mode` the file sets is the console's mode afterwards
    for _, line in ipairs(mep.buffer_get_lines(mep.current_buffer()) or {}) do
      local m = line:match('^%.mode%s+(%S+)')
      if m then st.mode = m end
    end
    local raw = '.read ' .. dotq(path) .. '\n'
    st.history[#st.history + 1] = {text = '.read ' .. path, raw = raw}
    render_history()
    mep.terminal_write(st.console_buf, raw)
    st.sig = nil
  end
  return {
    run_source = run_source,
    close = function()
      if st.pending then cleanup_send(st.pending) end
      os.remove(session_dir .. '/send')
      -- Deleting a terminal buffer leaves its program running, so ask
      -- sqlite3 to go: Ctrl-U drops a half-typed line, .quit exits, and
      -- Ctrl-D (EOF) covers an unfinished multi-line statement.
      if mep.is_terminal_buffer(st.console_buf) then mep.terminal_write(st.console_buf, '\21.quit\n\4') end
      state[tid] = nil
      mep.language_ui_teardown(layout)
      if st.results_buf and mep.buffer_valid(st.results_buf) then mep.buffer_delete(st.results_buf, true) end
      mep.notify('SQL language UI mode closed')
    end,
  }
end
mep.language_ui_modes.sql = {open = mep.sql_ui_open}
function mep.sql_ui_state() return current() end

-- Poll: a finished send (its done file) fills Results; the database file
-- changing (a statement typed in the console, another process) refreshes
-- Schema.
do
  local last_poll, last_pending = 0, 0
  mep.on_frame(function()
    local st = current()
    if not st then return end
    local now = mep.now()
    local send = st.pending
    if send and now - last_pending >= 0.1 then
      last_pending = now
      if mep.fs_exists(send.done) then
        st.pending = nil
        finish_send(st, send)
      end
    end
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    local sig = db_signature(st)
    if sig ~= st.sig then
      st.sig = sig
      mep.sql_ui_refresh_schema(st)
    end
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
