// Octave language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .m
// buffer), kBuiltinLanguageUi's Octave consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Ruby mode's shape.
// Included by main.cpp only; BuiltinLanguageUiOctave() is the Lua chunk
// (help/octave-mode.org is the user's page).
//
// The console is the stock `octave-cli -q --persist --eval "source('mep_init.m')"`
// (mep.opt.octave_ui_cmd, default {'octave-cli'}; a `% REPL:` comment replaces
// it via mep.language_ui_cmd), so line editing, history, completion and value
// display are Octave's own. It inherits mep's environment (OCTAVE_PATH,
// MEP_OCTAVE_PATH). The init script adds to the load path: the source file's
// directory, $MEP_OCTAVE_PATH, and the --path/-p directories of an `octave`
// command in the file's `CMD:` line (the Run button's own header).
//
// The init script defines mep_* command-line functions and registers an
// input-event hook (add_input_event_hook: it runs while the prompt waits), so
// the panel files are rewritten (at most twice a second, only when they
// change) after each command. Nothing reads the console's output; the
// panels are files the poll loop re-reads on a timer.
//   - Workspace: `whos` of the base workspace -- name, class, size and a
//     one-line value. Clicking a row inspects it.
//   - Path: the directories added to the load path since the console
//     started, and the loaded packages.
//   - Inspect: mep_view(x) -- disp of the value, class, size, a struct's
//     fields, an object's methods.
//   - Help: mep_help('name') -- Octave's own `help name` (a .m file's comment
//     block, a built-in's docstring), with `which`; when Octave has no text
//     (a .oct function without a docstring, a command-line function in a
//     sourced script), `mep-octave-lsp --doc` reads the comment blocks of
//     the .m files on the session's path and of the scripts it ran -- for a
//     Rosetta-generated .oct library, the .m stubs carrying its help text.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, named after the source
// and padded so its line numbers are the source's, and type one `source`
// line (<leader>rr: mep_run, which also cds to the file's directory and
// adds it to the path, like Octave's `run`), in the base workspace --
// variables it assigns stay in the session, and errors name the source's
// lines. The block at the cursor is a top-level statement: a whole function,
// if/for/while/switch/try ... end, or a single (continued) line.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiOctave() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.octave_ui_cmd = mep.opt.octave_ui_cmd or {'octave-cli'}
mep.opt.octave_ui_info_share = mep.opt.octave_ui_info_share or 0.3
mep.opt.octave_ui_bottom_share = mep.opt.octave_ui_bottom_share or 0.4
mep.opt.octave_ui_bottom_info_share = mep.opt.octave_ui_bottom_info_share or 0.4
mep.language_ui_extensions.m = mep.language_ui_extensions.m or 'octave'

-- The console's init script. Lua appends the session's settings and the
-- start call below it (mep_ui_set(...) lines, then mep_ui_start()).
local mep_octave_ui_init_template = [==[
1;

function v = mep_ui_conf (key, val)
  persistent conf;
  if (isempty (conf))
    conf = struct ();
  end
  if (nargin > 1)
    conf.(key) = val;
  end
  if (isfield (conf, key))
    v = conf.(key);
  else
    v = [];
  end
end

% Sets a session setting (no output: a call at the prompt leaves no `ans`).
function mep_ui_set (key, val)
  mep_ui_conf (key, val);
end

% Atomically (write, then rename), so a reader never sees a half-written panel.
function mep_ui_write (name, text)
  p = fullfile (mep_ui_conf ('dir'), name);
  tmp = sprintf ('%s.%d.tmp', p, getpid ());
  fid = fopen (tmp, 'w');
  if (fid < 0)
    return;
  end
  fwrite (fid, text);
  fclose (fid);
  rename (tmp, p);
end

function s = mep_ui_short (v, width)
  if (nargin < 2)
    width = 60;
  end
  try
    if (ischar (v) && (isrow (v) || isempty (v)))
      s = ['"' v '"'];
    elseif ((isnumeric (v) || islogical (v)) && ndims (v) == 2 && numel (v) <= 12)
      s = mat2str (v, 6);
    elseif (isnumeric (v) || islogical (v))
      s = sprintf ('[%s %s]', mep_ui_size (v), class (v));
    elseif (iscell (v))
      s = sprintf ('{%s cell}', mep_ui_size (v));
    elseif (isstruct (v))
      f = fieldnames (v);
      s = sprintf ('struct: %s', strjoin (f(:)', ', '));
    elseif (is_function_handle (v))
      s = func2str (v);
    else
      s = sprintf ('<%s>', class (v));
    end
  catch
    s = sprintf ('<%s>', class (v));
  end
  s = regexprep (s, '[\r\n]+', ' ');
  if (numel (s) > width)
    s = [s(1:width-3) '...'];
  end
end

function s = mep_ui_size (v)
  s = strjoin (arrayfun (@num2str, size (v), 'UniformOutput', false), 'x');
end

function d = mep_ui_user_path ()
  d = strsplit (path (), pathsep ());
  base = mep_ui_conf ('base_path');
  home = OCTAVE_HOME ();
  keep = true (size (d));
  for i = 1:numel (d)
    keep(i) = ! isempty (d{i}) && ! any (strcmp (d{i}, base)) && ! strncmp (d{i}, home, numel (home));
  end
  d = d(keep);
end

function mep_ui_refresh ()
  persistent last_vars last_path;
  s = evalin ('base', 'whos');
  rows = {};
  for i = 1:numel (s)
    n = s(i).name;
    try
      v = evalin ('base', n);
      val = mep_ui_short (v);
    catch
      val = '';
    end
    rows{end+1} = sprintf ('%-22s %-12s %-10s %s', n, s(i).class, mep_ui_size (zeros (s(i).size)), val);
  end
  text = '';
  if (! isempty (rows))
    text = [strjoin(rows, "\n") "\n"];
  end
  if (! isequal (text, last_vars))
    mep_ui_write ('variables.txt', text);
    last_vars = text;
  end
  lines = {};
  for d = mep_ui_user_path ()
    lines{end+1} = sprintf ('path    %s', d{1});
  end
  try
    l = pkg ('list');
    for i = 1:numel (l)
      if (l{i}.loaded)
        lines{end+1} = sprintf ('package %s %s', l{i}.name, l{i}.version);
      end
    end
  catch
  end
  text = '';
  if (! isempty (lines))
    text = [strjoin(lines, "\n") "\n"];
  end
  if (! isequal (text, last_path))
    mep_ui_write ('path.txt', text);
    last_path = text;
  end
end

function mep_ui_tick ()
  persistent last;
  if (isempty (last))
    last = 0;
  end
  t = time ();
  if (t - last < 0.5)
    return;
  end
  last = t;
  try
    mep_ui_refresh ();
  catch
  end
end

function q = mep_ui_shq (s)
  q = ["'" strrep(s, "'", "'\\''") "'"];
end

% `name`'s help in mep's Help tab: Octave's own `help`, else (or for a .oct
% function without a docstring) the comment blocks mep-octave-lsp --doc reads.
function mep_help (name)
  name = strtrim (name);
  out = '';
  octave_text = '';
  try
    octave_text = evalc (sprintf ('help (''%s'')', strrep (name, '''', '''''')));
  catch err
    octave_text = '';
  end
  % Octave's footer (the same for every name) is noise in a panel.
  k = strfind (octave_text, 'Additional help for built-in functions');
  if (! isempty (k))
    octave_text = octave_text(1:k(1)-1);
  end
  octave_text = regexprep (octave_text, '\s+$', '');
  body = regexprep (octave_text, '^''[^\n]*'' is [^\n]*\n', '');
  if (! isempty (regexp (body, '^\s*undocumented function', 'once')))
    body = '';  % what Octave says for a function without help text
  end
  if (! isempty (strtrim (body)))
    out = [octave_text "\n"];
  else
    lsp = mep_ui_conf ('lsp');
    doc = '';
    if (! isempty (lsp))
      cmd = [mep_ui_shq(lsp) ' --doc ' mep_ui_shq(name)];
      for d = [mep_ui_user_path(), {pwd()}]
        cmd = [cmd ' --path ' mep_ui_shq(d{1})];
      end
      files = mep_ui_conf ('files');
      for i = 1:numel (files)
        cmd = [cmd ' --file ' mep_ui_shq(files{i})];
      end
      [status, doc] = system ([cmd ' 2>/dev/null']);
      if (status != 0)
        doc = '';
      end
    end
    header = regexp (octave_text, '^''[^\n]*'' is [^\n]*', 'match', 'once');
    if (! isempty (strtrim (doc)))
      out = doc;
      if (! isempty (header) && isempty (strfind (doc, header)))
        out = [header "; its help text, from the .m files on the path:\n\n" doc];
      end
    elseif (! isempty (octave_text))
      out = [octave_text "\n\n(no help text)\n"];
    end
  end
  try
    w = which (name);
    if (! isempty (w))
      out = [out sprintf("\nwhich: %s\n", w)];
    end
  catch
  end
  if (isempty (strtrim (out)))
    out = sprintf ("%s: no documentation found in this session.\n", name);
  end
  mep_ui_write ('help.txt', out);
  printf ('%% %s: in the Help tab\n', name);
end

% `x` (a value, or a variable's name as a string) in mep's Inspect tab.
function mep_view (x)
  label = inputname (1, false);
  if (ischar (x) && isrow (x) && evalin ('base', sprintf ('exist (''%s'', ''var'')', x)) == 1)
    label = x;
    x = evalin ('base', x);
  end
  if (isempty (label))
    label = mep_ui_short (x);
  end
  out = sprintf ("%s  (%s, %s)\n\n", label, class (x), mep_ui_size (x));
  try
    out = [out evalc('disp (x)')];
  catch
  end
  if (isstruct (x))
    out = [out sprintf("\nfields: %s\n", strjoin (fieldnames (x)(:)', ', '))];
  elseif (isobject (x))
    try
      out = [out sprintf("\nmethods: %s\n", strjoin (methods (x)(:)', ', '))];
    catch
    end
  end
  mep_ui_write ('inspect.txt', out);
end

% Remembers a file the session ran, for mep-octave-lsp --doc --file.
function mep_ui_ran (file)
  files = mep_ui_conf ('files');
  if (isempty (files))
    files = {};
  end
  if (! any (strcmp (files, file)))
    files{end+1} = file;
    mep_ui_conf ('files', files);
  end
end

% <leader>rr: `copy` (the buffer, named after `src`) in the base workspace,
% from `src`'s directory, which is on the path for the run (like `run`).
function mep_run (src, copy)
  if (nargin < 2)
    copy = src;
  end
  d = fileparts (src);
  if (! isempty (d) && ! any (strcmp (strsplit (path (), pathsep ()), d)))
    addpath (d);
  end
  mep_ui_ran (copy);
  old = pwd ();
  unwind_protect
    if (! isempty (d))
      cd (d);
    end
    evalin ('base', sprintf ('source (''%s'');', strrep (copy, '''', '''''')));
  unwind_protect_cleanup
    cd (old);
    mep_ui_refresh ();
  end_unwind_protect
end

function mep_ui_start ()
  mep_ui_conf ('base_path', strsplit (path (), pathsep ()));
  mep_ui_conf ('files', {});
  dirs = mep_ui_conf ('add_path');
  env = getenv ('MEP_OCTAVE_PATH');
  if (! isempty (env))
    dirs = [strsplit(env, pathsep ()), dirs];
  end
  for i = numel (dirs):-1:1
    if (! isempty (dirs{i}) && isfolder (dirs{i}))
      addpath (dirs{i});
    end
  end
  add_input_event_hook (@mep_ui_tick);
  evalin ('base', 'clear ans');  % the script marker `1;` above set it
  mep_ui_refresh ();
  printf ("Octave %s -- mep language UI: <leader>re runs the block at the cursor, <leader>rr the file; mep_help ('name') / gh shows help, mep_view (x) inspects.\n", OCTAVE_VERSION);
end
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    workspace = mep.sidebar_create('Workspace', 'right', 44),
    path = mep.sidebar_create('Path', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- A single-quoted Octave string literal.
local function mquote(s)
  return "'" .. tostring(s):gsub("'", "''") .. "'"
end
mep.octave_ui_quote = mquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_workspace()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.workspace_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and ('inspect ' .. name) or nil,
      on_click = (name and st) and function() send(st, 'mep_view (' .. mquote(name) .. ')') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(the workspace is empty)'} end
  mep.sidebar_set_sections(sidebars.workspace, {{id = 'workspace', title = '', collapsed = false, widgets = widgets}})
end

local function render_path()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local dirs, pkgs = {}, {}
  for line in ((st and st.path_text) or ''):gmatch('[^\n]+') do
    local dir = line:match('^path%s+(.+)$')
    if dir then
      dirs[#dirs + 1] = {id = line, text = dir}
    else
      local p = line:match('^package%s+(.+)$')
      if p then pkgs[#pkgs + 1] = {id = line, text = p} end
    end
  end
  if #dirs == 0 then dirs[1] = {id = 'empty', text = "(nothing added yet -- addpath ('dir') in the console)"} end
  if #pkgs == 0 then pkgs[1] = {id = 'empty', text = '(no package loaded -- pkg load NAME)'} end
  mep.sidebar_set_sections(sidebars.path, {
    {id = 'path', title = 'Added to the load path', collapsed = false, widgets = dirs},
    {id = 'packages', title = 'Loaded packages', collapsed = false, widgets = pkgs},
  })
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_workspace()
  render_path()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Workspace row, or mep_view (x) in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    "(no help viewed yet -- gh over a name, or mep_help ('name') in the console)")
end

-- Octave's block keywords: what opens a construct and what closes one.
local openers = {}
for _, w in ipairs({'if', 'for', 'parfor', 'while', 'switch', 'do', 'try', 'unwind_protect', 'function',
    'classdef', 'methods', 'properties', 'events', 'enumeration', 'spmd', 'arguments'}) do openers[w] = true end
local closers = {}
for _, w in ipairs({'end', 'endif', 'endfor', 'endparfor', 'endwhile', 'endswitch', 'end_try_catch',
    'end_unwind_protect', 'endfunction', 'endclassdef', 'endmethods', 'endproperties', 'endevents',
    'endenumeration', 'endspmd', 'until', 'endarguments'}) do closers[w] = true end

-- One line's effect: block depth change and bracket depth after it, and
-- whether it continues on the next line (`...`). Strings and comments are
-- skipped; `end` inside brackets is an index, not a closer.
local function scan_line(line, brackets)
  local delta, i, n = 0, 1, #line
  local prev = ''  -- the previous significant character
  local continued = false
  while i <= n do
    local c = line:sub(i, i)
    if c == '%' or c == '#' then break end
    if line:sub(i, i + 2) == '...' then continued = true break end
    if c == '"' then
      local j = i + 1
      while j <= n do
        local d = line:sub(j, j)
        if d == '\\' then j = j + 1 elseif d == '"' then break end
        j = j + 1
      end
      i = j + 1
      prev = '"'
    elseif c == "'" and not prev:match("[%w_%)%]}%.']") then
      local j = i + 1
      while j <= n do
        if line:sub(j, j) == "'" then
          if line:sub(j + 1, j + 1) == "'" then j = j + 1 else break end
        end
        j = j + 1
      end
      i = j + 1
      prev = "'"
    elseif c:match('[%a_]') then
      local word = line:match('^[%w_]+', i)
      if brackets == 0 and not (prev == '.') then
        if openers[word] then delta = delta + 1 elseif closers[word] then delta = delta - 1 end
      end
      i = i + #word
      prev = 'a'
    else
      if c == '(' or c == '[' or c == '{' then brackets = brackets + 1
      elseif c == ')' or c == ']' or c == '}' then brackets = math.max(0, brackets - 1) end
      if not c:match('%s') then prev = c end
      i = i + 1
    end
  end
  return delta, brackets, continued
end

-- The top-level statement around `row`: a whole function / if / for /
-- while / switch / try ... end, or a (continued) line. Returns the first
-- and last rows, or nil on a blank or comment line outside any statement.
function mep.octave_ui_block_at(lines, row)
  local n = #lines
  local starts = {}  -- row -> last row of the statement starting there
  local depth, brackets, in_block_comment = 0, 0, false
  local cur_start = nil
  for i = 1, n do
    local l = lines[i]
    local trimmed = l:match('^%s*(.-)%s*$')
    if in_block_comment then
      if trimmed == '%}' or trimmed == '#}' then in_block_comment = false end
    elseif trimmed == '%{' or trimmed == '#{' then
      in_block_comment = true
    elseif trimmed ~= '' and not trimmed:match('^[%%#]') then
      if not cur_start then cur_start = i end
      local delta, b, continued = scan_line(l, brackets)
      depth, brackets = math.max(0, depth + delta), b
      if depth == 0 and brackets == 0 and not continued then
        starts[cur_start] = i
        cur_start = nil
      end
    end
  end
  if cur_start then starts[cur_start] = n end
  for s = row, 1, -1 do
    local e = starts[s]
    if e then
      if e >= row then return s, e end
      return nil
    end
  end
  return nil
end

-- The session file a chunk is written to: session/<what>/<source name>.m.
local function chunk_path(st, what)
  local base = (st.src_abs or ''):match('([^/\\]+)%.m$') or 'mep_chunk'
  if not base:match('^[%a][%w_]*$') then base = 'mep_chunk' end
  return st.dir .. '/' .. what .. '/' .. base .. '.m'
end

-- Writes rows first..last of `lines` to the session's eval file, padded so
-- its line numbers are the source's, and has the console source it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  local path = chunk_path(st, 'eval')
  mep.language_ui_write_file(path, text)
  send(st, 'source (' .. mquote(path) .. ');')
end

-- <leader>re: the Visual selection if one is live, else the top-level
-- statement at the cursor.
function mep.octave_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Octave console in this tab -- <leader>uu opens one', 'warn')
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
  local s, e = mep.octave_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Octave statement at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.octave_ui_eval_selection = mep.octave_ui_eval_block
mep.language_ui_eval_handlers.octave = mep.octave_ui_eval_block

-- The --path / -p directories of an `octave` command in the file's `CMD:`
-- line (the Run button's header), with $(pwd) / $PWD as mep's directory.
function mep.octave_ui_cmd_paths(lines, cwd)
  local cmd
  for _, line in ipairs(lines) do
    local c = line:match('^%s*[^%w%s]+%s*[Cc][Mm][Dd]:%s*(.-)%s*$')
    if c and c ~= '' then cmd = c break end
  end
  local dirs = {}
  if not cmd or not cmd:match('octave') then return dirs end
  local words, i = {}, 1
  while i <= #cmd do
    local c = cmd:sub(i, i)
    if c:match('%s') then
      i = i + 1
    else
      local word = ''
      while i <= #cmd and not cmd:sub(i, i):match('%s') do
        local q = cmd:sub(i, i)
        if q == '"' or q == "'" then
          local j = cmd:find(q, i + 1, true) or (#cmd + 1)
          word = word .. cmd:sub(i + 1, j - 1)
          i = j + 1
        else
          word = word .. q
          i = i + 1
        end
      end
      words[#words + 1] = word
    end
  end
  local function expand(d)
    d = d:gsub('%$%(pwd%)', cwd):gsub('%${PWD}', cwd):gsub('%$PWD', cwd)
    if not d:match('^/') and cwd ~= '' then d = cwd .. '/' .. d end
    return d
  end
  for k, w in ipairs(words) do
    if (w == '--path' or w == '-p') and words[k + 1] then
      for d in words[k + 1]:gmatch('[^:]+') do dirs[#dirs + 1] = expand(d) end
    else
      local v = w:match('^%-%-path=(.+)$')
      if v then for d in v:gmatch('[^:]+') do dirs[#dirs + 1] = expand(d) end end
    end
  end
  return dirs
end

function mep.octave_ui_open()
  local session_dir = mep.language_ui_session_dir('octaveui')
  local paths = {
    workspace = session_dir .. '/variables.txt', path = session_dir .. '/path.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  if mep.fs_mkdir then
    mep.fs_mkdir(session_dir .. '/eval')
    mep.fs_mkdir(session_dir .. '/run')
  end
  local fname = mep.filename and mep.filename() or ''
  local src_abs = (fname ~= '') and mep_lsp_abspath(fname) or ''
  local src_buf = mep.current_buffer()
  local add = {}
  local src_dir = src_abs:match('^(.*)[/\\][^/\\]*$')
  if src_dir and src_dir ~= '' then add[#add + 1] = src_dir end
  local lines = mep.buffer_get_lines(src_buf) or {}
  for _, d in ipairs(mep.octave_ui_cmd_paths(lines, (mep.getcwd and mep.getcwd()) or '')) do add[#add + 1] = d end
  local quoted = {}
  for _, d in ipairs(add) do quoted[#quoted + 1] = mquote(d) end
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-octave-lsp') or 'mep-octave-lsp'
  local init_path = session_dir .. '/mep_init.m'
  mep.language_ui_write_file(init_path, mep_octave_ui_init_template
    .. '\nmep_ui_set (\'dir\', ' .. mquote(session_dir) .. ');\nmep_ui_set (\'lsp\', ' .. mquote(lsp)
    .. ');\nmep_ui_set (\'add_path\', {' .. table.concat(quoted, ', ') .. '});\nmep_ui_start ();\n')
  local argv = mep.language_ui_cmd(mep.opt.octave_ui_cmd)
  for _, a in ipairs({'-q', '--persist', '--eval', 'source (' .. mquote(init_path) .. ');'}) do argv[#argv + 1] = a end
  local layout = mep.language_ui_layout({
    info_share = mep.opt.octave_ui_info_share, bottom_share = mep.opt.octave_ui_bottom_share,
    bottom_info_share = mep.opt.octave_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'octave') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, argv = argv, added_path = add,
    workspace_text = '', path_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.workspace, sidebars.path, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Octave language UI: octave-cli below; Workspace/Path/Inspect top-right, Help bottom-right; <leader>re runs the block at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open (a
  -- file with a CMD: line runs that instead, as without the mode) -- the
  -- source buffer as it is now (unsaved edits too), run in the session from
  -- the file's directory.
  local function run_source(f)
    local abs = mep_lsp_abspath(f or fname)
    local buf_lines = mep.buffer_get_lines(st.src_buf)
    if not buf_lines then
      send(st, 'mep_run (' .. mquote(abs) .. ');')
      return
    end
    local copy = chunk_path(st, 'run')
    mep.language_ui_write_file(copy, table.concat(buf_lines, '\n') .. '\n')
    send(st, 'mep_run (' .. mquote(abs) .. ', ' .. mquote(copy) .. ');')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Octave language UI mode closed')
    end,
  }
end
mep.language_ui_modes.octave = {open = mep.octave_ui_open}

-- gh: the session's help for the name under the cursor in the Help tab, and
-- from a .m buffer the language server's hover as well.
function mep.octave_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_.]')
  if sym then sym = sym:gsub('^%.+', ''):gsub('%.+$', '') end
  if not sym or sym == '' or not sym:match('^[%a_]') then mep.notify('gh: no Octave name under the cursor', 'warn') return true end
  send(st, 'mep_help (' .. mquote(sym) .. ')')
  if mep.language_for_buffer() == 'octave' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('octave', mep.octave_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Octave mode is open).
function mep.octave_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, added_path = st.added_path,
    workspace = st.workspace_text, path = st.path_text, inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while an Octave mode
-- is open on the active tab.
function mep.octave_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed, help_changed = false, false
  for _, key in ipairs({'workspace', 'path', 'inspect', 'help'}) do
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[key .. '_text'] then
      st[key .. '_text'] = text
      changed = true
      if key == 'help' then help_changed = true end
    end
  end
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
    mep.octave_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
