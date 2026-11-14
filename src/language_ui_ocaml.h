// OCaml language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .ml
// buffer), kBuiltinLanguageUi's OCaml consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Ruby and Julia
// modes, whose shape it follows. Included by main.cpp only;
// BuiltinLanguageUiOcaml() is the Lua chunk (help/ocaml-mode.org is the
// user's page).
//
// The console is utop when it is on PATH, else the stock `ocaml` toplevel
// (mep.opt.ocaml_ui_cmd overrides; a `(* REPL: ... *)` comment replaces it
// via mep.language_ui_cmd), started with `-init mep_init.ml`, so line
// editing, history, completion and the display of values are its own. It
// inherits mep's environment (OCAMLPATH, OPAM_SWITCH_PREFIX, ...), so
// `#require "pkg"` / `#load` find what they would in a terminal.
//
// The init script (mep_ocaml_ui_init_template below) defines a module MepUI
// whose help and view work in any toplevel, then `#use`s mep_env.ml, which
// loads the compiler libraries (`#directory "+compiler-libs"`, ocamlcommon)
// to read the toplevel's environment: the panel files are rewritten before
// each phrase is read (a wrapper around Toploop.parse_toplevel_phrase, and
// UTop.at_new_command under utop). Without the compiler libraries the
// panels stay empty and the rest still works. As in the other modes,
// nothing reads the console's output; the panels are files the poll loop
// re-reads on a timer.
//   - Variables: the values the session defined, with type and a short
//     value, and its types. Clicking a value inspects it.
//   - Modules: the modules the session defined (clicking one shows its
//     help), then the toplevel's load path (#directory, #require).
//   - Inspect: MepUI.view "x" -- the type and the whole value, or a
//     module's signature.
//   - Help: MepUI.help "List.map" -- the odoc documentation from the .mli,
//     read by `mep-ocaml-lsp --doc` as the source file sees it (its opens,
//     with the toplevel's load path), then the session's own view: the
//     value's type or the module's signature.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, after a line-number
// directive (`# 12 "/path/src.ml"`), and type one `#use "...";;` line, so the
// toplevel's messages name the source file and line. The phrase at the
// cursor is a top-level item: from the nearest line at or above it that
// starts one at column 0 to just before the next.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiOcaml() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.ocaml_ui_info_share = mep.opt.ocaml_ui_info_share or 0.3
mep.opt.ocaml_ui_bottom_share = mep.opt.ocaml_ui_bottom_share or 0.4
mep.opt.ocaml_ui_bottom_info_share = mep.opt.ocaml_ui_bottom_info_share or 0.4
mep.language_ui_extensions.ml = mep.language_ui_extensions.ml or 'ocaml'
mep.language_ui_extensions.mli = mep.language_ui_extensions.mli or 'ocaml'

-- The console's init script. Lua puts a `module MepUI_config` above it
-- naming the session directory, mep-ocaml-lsp, the source file and whether
-- the console is utop.
local mep_ocaml_ui_init_template = [==[
module MepUI = struct
  let dir = MepUI_config.dir
  let lsp = MepUI_config.lsp
  let context = ref MepUI_config.context
  (* Filled in by mep_env.ml when the compiler libraries load. *)
  let refresh_hook = ref (fun () -> ())
  let type_hook : (string -> string option) ref = ref (fun _ -> None)
  let view_hook : (string -> string) ref =
    ref (fun name -> name ^ ": inspecting needs the compiler libraries (+compiler-libs)\n")
  let dirs_hook : (unit -> string list) ref = ref (fun () -> [])

  (* Atomically (write, then rename), so a reader never sees a half-written panel. *)
  let write_text name text =
    let path = Filename.concat dir name in
    let tmp = path ^ ".tmp" in
    try
      let oc = open_out_bin tmp in
      output_string oc text;
      close_out oc;
      Sys.rename tmp path
    with _ -> ()

  let read_text path =
    try
      let ic = open_in_bin path in
      let s = really_input_string ic (in_channel_length ic) in
      close_in ic;
      s
    with _ -> ""

  let refresh () = try !refresh_hook () with _ -> ()

  let help_text name =
    let q = Filename.quote in
    let out = Filename.concat dir "help.out" in
    let ctx = if !context <> "" && Sys.file_exists !context then " --context " ^ q !context else "" in
    let dirs = try !dirs_hook () with _ -> [] in
    let incl = String.concat "" (List.map (fun d -> " -I " ^ q d) dirs) in
    let cmd = q lsp ^ " --doc " ^ q name ^ ctx ^ incl ^ " > " ^ q out ^ " 2>/dev/null" in
    let doc = if Sys.command cmd = 0 then read_text out else "" in
    let session =
      match (try !type_hook name with _ -> None) with
      | Some t -> "## In this session\n\n```ocaml\n" ^ t ^ "\n```\n"
      | None -> ""
    in
    if doc = "" && session = "" then name ^ ": no documentation found.\n"
    else doc ^ (if doc <> "" && session <> "" then "\n" else "") ^ session

  (* [name]'s documentation and the session's view of it, in mep's Help tab. *)
  let help name =
    write_text "help.txt" (try help_text name with e -> name ^ ": " ^ Printexc.to_string e ^ "\n");
    print_endline ("(* " ^ name ^ ": in the Help tab *)")

  (* The value (or module) [name] in mep's Inspect tab. *)
  let view name =
    write_text "inspect.txt" (try !view_hook name with e -> name ^ ": " ^ Printexc.to_string e ^ "\n")
end;;
]==]

-- Loaded by the init script with #use: everything that needs the compiler
-- libraries. A failure here leaves the panels empty, nothing else.
local mep_ocaml_ui_env_template = [==[
#directory "+compiler-libs";;
#load "ocamlcommon.cma";;
let () =
  let own = function
    | Path.Pident id ->
      let n = Ident.name id in
      n <> "" && n.[0] <> '_' && n <> "MepUI" && n <> "MepUI_config" && not (Ident.is_predef id)
      && not (Ident.global id)
    | _ -> false
  in
  let one_line s = String.map (fun c -> if c = '\n' then ' ' else c) s in
  let ty_str ty = one_line (Format.asprintf "%a" Printtyp.type_scheme ty) in
  let short env path ty =
    try
      let obj = Toploop.eval_value_path env path in
      let depth = !Toploop.max_printer_depth and steps = !Toploop.max_printer_steps in
      Toploop.max_printer_depth := 3;
      Toploop.max_printer_steps := 40;
      let s = Format.asprintf "%a" (fun ppf () -> Toploop.print_value env obj ppf ty) () in
      Toploop.max_printer_depth := depth;
      Toploop.max_printer_steps := steps;
      let s = one_line s in
      if String.length s > 60 then String.sub s 0 57 ^ "..." else s
    with _ -> ""
  in
  let pad s n = if String.length s >= n then s else s ^ String.make (n - String.length s) ' ' in
  (* The stock toplevel links only part of compiler-libs, so the Env and
     Load_path loaded above are second copies that start with no load path:
     give them the standard library and every #directory (recorded by
     wrapping the directives), so List.map and the session's libraries
     resolve. *)
  let dirs = ref [] in
  let expand d =
    if d <> "" && d.[0] = '+' then Filename.concat Config.standard_library (String.sub d 1 (String.length d - 1))
    else if Filename.is_relative d then Filename.concat (Sys.getcwd ()) d
    else d
  in
  let sync () =
    (try Load_path.init ~auto_include:Load_path.no_auto_include
           ~visible:(List.rev !dirs @ [Config.standard_library]) ~hidden:[]
     with _ -> ());
    (try Env.reset_cache () with _ -> ())
  in
  let wrap name update =
    match Toploop.get_directive name, Toploop.get_directive_info name with
    | Some (Toploop.Directive_string f), Some info ->
      Toploop.add_directive name (Toploop.Directive_string (fun d -> f d; update (expand d); sync ())) info
    | _ -> ()
  in
  wrap "directory" (fun d -> dirs := d :: List.filter (( <> ) d) !dirs);
  wrap "remove_directory" (fun d -> dirs := List.filter (( <> ) d) !dirs);
  sync ();
  MepUI.refresh_hook := (fun () ->
    let env = !Toploop.toplevel_env in
    let values = Env.fold_values (fun name path vd acc ->
      if own path then (name, ty_str vd.Types.val_type, short env path vd.Types.val_type) :: acc else acc)
      None env [] in
    let names f = List.sort_uniq compare (f (fun name path _ acc -> if own path then name :: acc else acc) None env []) in
    let b = Buffer.create 1024 in
    List.iter (fun (n, t, v) ->
      Buffer.add_string b (pad n 22 ^ " val     " ^ t ^ (if v = "" then "" else " = " ^ v) ^ "\n"))
      (List.sort_uniq compare values);
    List.iter (fun n -> Buffer.add_string b (pad n 22 ^ " type\n")) (names Env.fold_types);
    MepUI.write_text "variables.txt" (Buffer.contents b);
    let m = Buffer.create 256 in
    List.iter (fun n -> Buffer.add_string m (pad n 22 ^ " module\n")) (names Env.fold_modules);
    List.iter (fun d -> Buffer.add_string m (pad "" 22 ^ " directory " ^ d ^ "\n")) (List.rev !dirs);
    MepUI.write_text "modules.txt" (Buffer.contents m));
  let lid name = Parse.longident (Lexing.from_string name) in
  MepUI.type_hook := (fun name ->
    let env = !Toploop.toplevel_env in
    match Env.find_value_by_name (lid name) env with
    | (_, vd) -> Some ("val " ^ name ^ " : " ^ ty_str vd.Types.val_type)
    | exception _ ->
      (match Env.find_module_by_name (lid name) env with
       | (_, md) -> Some (Format.asprintf "@[<v 2>module %s :@ %a@]" name Printtyp.modtype md.Types.md_type)
       | exception _ -> None));
  MepUI.view_hook := (fun name ->
    let env = !Toploop.toplevel_env in
    match Env.find_value_by_name (lid name) env with
    | (path, vd) ->
      let obj = Toploop.eval_value_path env path in
      Format.asprintf "%s : %s@.@.%a@." name (ty_str vd.Types.val_type)
        (fun ppf () -> Toploop.print_value env obj ppf vd.Types.val_type) ()
    | exception _ ->
      (match Env.find_module_by_name (lid name) env with
       | (_, md) -> Format.asprintf "@[<v 2>module %s :@ %a@]@." name Printtyp.modtype md.Types.md_type
       | exception _ -> name ^ ": not a value or module of this session\n"));
  MepUI.dirs_hook := (fun () -> List.rev !dirs);
  let read = !Toploop.parse_toplevel_phrase in
  Toploop.parse_toplevel_phrase := (fun lb -> MepUI.refresh (); read lb);;
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 44),
    modules = mep.sidebar_create('Modules', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- An OCaml string literal.
local function mlquote(s)
  return '"' .. tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n') .. '"'
end
mep.ocaml_ui_quote = mlquote

-- Whether an executable is on PATH.
local function on_path(exe)
  for dir in (os.getenv('PATH') or ''):gmatch('[^:]+') do
    local f = io.open(dir .. '/' .. exe, 'r')
    if f then f:close() return true end
  end
  return false
end

-- utop when installed, else the stock toplevel.
function mep.ocaml_ui_default_cmd()
  if mep.opt.ocaml_ui_cmd then return mep.opt.ocaml_ui_cmd end
  if on_path('utop') then return {'utop'} end
  return {'ocaml'}
end

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_variables()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.variables_text) or ''):gmatch('[^\n]+') do
    local name, kind = line:match('^(%S+)%s+(%S+)')
    local target = kind == 'val' and name or nil
    widgets[#widgets + 1] = {id = line, text = line, tooltip = target and ('inspect ' .. target) or nil,
      on_click = (target and st) and function() send(st, 'MepUI.view ' .. mlquote(target) .. ';;') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing defined in this session yet)'} end
  mep.sidebar_set_sections(sidebars.variables, {{id = 'variables', title = '', collapsed = false, widgets = widgets}})
end

local function render_modules()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local modules, dirs = {}, {}
  for line in ((st and st.modules_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)%s+module')
    if name then
      modules[#modules + 1] = {id = line, text = name, tooltip = 'help for ' .. name,
        on_click = st and function() send(st, 'MepUI.help ' .. mlquote(name) .. ';;') end or nil}
    else
      local dir = line:match('directory%s+(.+)$')
      if dir then dirs[#dirs + 1] = {id = line, text = dir} end
    end
  end
  if #modules == 0 then modules[1] = {id = 'empty', text = '(no modules defined in this session yet)'} end
  mep.sidebar_set_sections(sidebars.modules, {
    {id = 'modules', title = 'Session modules', collapsed = false, widgets = modules},
    {id = 'dirs', title = 'Load path', collapsed = true, widgets = dirs},
  })
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_variables()
  render_modules()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    '(nothing inspected yet -- click a Variables row, or MepUI.view "x";; in the console)')
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(no help viewed yet -- gh over a name, or MepUI.help "List.map";; in the console)')
end

-- Which lines start inside a comment or a string (never the start of an
-- item). Comments nest; strings and character literals inside comments are
-- skipped as OCaml's lexer does, roughly.
local function opaque_lines(lines)
  local inside, comment = {}, {}
  local depth, in_string = 0, false
  for i, l in ipairs(lines) do
    inside[i] = depth > 0 or in_string
    comment[i] = depth > 0 and not in_string
    local k, n = 1, #l
    while k <= n do
      local c, c2 = l:sub(k, k), l:sub(k, k + 1)
      if in_string then
        if c == '\\' then k = k + 1 elseif c == '"' then in_string = false end
      elseif c2 == '(*' and l:sub(k, k + 2) ~= '(*)' then
        depth = depth + 1
        k = k + 1
      elseif depth > 0 and c2 == '*)' then
        depth = depth - 1
        k = k + 1
      elseif c == '"' then
        in_string = true
      elseif c == "'" and l:sub(k + 2, k + 2) == "'" then
        k = k + 2
      end
      k = k + 1
    end
  end
  return inside, comment
end

-- The top-level item around `row`: from the nearest line at or above it
-- that starts one at column 0 (not a closing keyword or a continuation, not
-- a comment, not inside a comment or string) to just before the next.
-- Returns the first and last rows, or nil.
function mep.ocaml_ui_block_at(lines, row)
  local n = #lines
  local inside, comment = opaque_lines(lines)
  local function closer(l)
    for _, w in ipairs({'and', 'in', 'end', 'done', 'then', 'else', 'with', 'do', 'of', 'to', 'downto'}) do
      if l:sub(1, #w) == w and not l:sub(#w + 1, #w + 1):match("[%w_']") then return true end
    end
    return l:match('^[|%)%]}]') ~= nil or l:match('^;;') ~= nil
  end
  local function continues(k)
    local j = k - 1
    while j >= 1 and (lines[j]:match('^%s*$')) do j = j - 1 end
    if j < 1 or inside[j] then return false end
    local p = lines[j]:gsub('%s+$', '')
    if p:match(';;$') then return false end
    if p:match('[=,%(%[{:@^|&<>+%-*/]$') then return true end
    for _, w in ipairs({'in', 'then', 'else', 'do', 'of', 'with', 'begin', 'struct', 'sig', 'object', 'fun',
                        'function', 'match', 'try', 'let', 'and'}) do
      if p:sub(-#w) == w and not p:sub(-#w - 1, -#w - 1):match("[%w_'.]") then return true end
    end
    return false
  end
  local function starts(k)
    local l = lines[k]
    return l ~= nil and not inside[k] and l:match('^[^%s]') ~= nil and not l:match('^%(%*')
      and not closer(l) and not continues(k)
  end
  local s = row
  while s > 1 and not starts(s) do s = s - 1 end
  if not starts(s) then return nil end
  local e = s
  while e < n and not starts(e + 1) do e = e + 1 end
  -- Trailing blank and comment lines belong to no phrase.
  while e > s and (lines[e]:match('^%s*$') or comment[e] or (lines[e]:match('^%s*%(%*') and not inside[e])) do
    e = e - 1
  end
  return s, e
end

-- The source's name for the toplevel's messages.
local function source_name()
  local fname = mep.filename and mep.filename()
  return (fname and fname ~= '') and mep_lsp_abspath(fname) or '//toplevel//'
end

-- Writes rows first..last of `lines` to the session's eval file after a
-- line-number directive, ending the phrase with `;;`, and has the console
-- #use it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local body = table.concat(chunk, '\n'):gsub('%s+$', '')
  if not body:match(';;$') then body = body .. '\n;;' end
  local text = '# ' .. first .. ' ' .. mlquote(source_name()) .. '\n' .. body .. '\n'
  mep.language_ui_write_file(st.eval_path, text)
  send(st, '#use ' .. mlquote(st.eval_path) .. ';;')
end

-- <leader>re: the Visual selection if one is live, else the top-level item
-- at the cursor.
function mep.ocaml_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No OCaml console in this tab -- <leader>uu opens one', 'warn')
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
  local s, e = mep.ocaml_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No OCaml phrase at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.ocaml_ui_eval_selection = mep.ocaml_ui_eval_block
mep.language_ui_eval_handlers.ocaml = mep.ocaml_ui_eval_block

function mep.ocaml_ui_open()
  local session_dir = mep.language_ui_session_dir('ocamlui')
  local paths = {
    variables = session_dir .. '/variables.txt', modules = session_dir .. '/modules.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local argv = mep.language_ui_cmd(mep.ocaml_ui_default_cmd())
  local utop = false
  for _, a in ipairs(argv) do if a:match('utop') then utop = true end end
  local init_path = session_dir .. '/mep_init.ml'
  local env_path = session_dir .. '/mep_env.ml'
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-ocaml-lsp') or 'mep-ocaml-lsp'
  local fname = mep.filename and mep.filename()
  local context = (fname and fname ~= '') and mep_lsp_abspath(fname) or ''
  mep.language_ui_write_file(env_path, mep_ocaml_ui_env_template
    .. (utop and 'let () = UTop.at_new_command MepUI.refresh;;\n' or ''))
  mep.language_ui_write_file(init_path, 'module MepUI_config = struct\n  let dir = ' .. mlquote(session_dir)
    .. '\n  let lsp = ' .. mlquote(lsp) .. '\n  let context = ' .. mlquote(context) .. '\nend;;\n'
    .. mep_ocaml_ui_init_template
    .. '#use ' .. mlquote(env_path) .. ';;\n'
    .. 'MepUI.refresh ();;\n'
    .. 'print_endline ("OCaml " ^ Sys.ocaml_version ^ " -- mep language UI: <leader>re runs the phrase at the cursor, "\n'
    .. '  ^ "<leader>rr the file; MepUI.help \\"name\\" / gh shows help, MepUI.view \\"x\\" inspects.");;\n')
  local src_buf = mep.current_buffer()
  argv[#argv + 1] = '-init'
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.ocaml_ui_info_share, bottom_share = mep.opt.ocaml_ui_bottom_share,
    bottom_info_share = mep.opt.ocaml_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, utop and 'utop' or 'ocaml') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, eval_path = session_dir .. '/eval.ml', run_path = session_dir .. '/run.ml',
    variables_text = '', modules_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.modules, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('OCaml language UI: ' .. (utop and 'utop' or 'ocaml')
    .. ' below; Variables/Modules/Inspect top-right, Help bottom-right; <leader>re runs the phrase at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- the
  -- source buffer as it is now (unsaved edits too), #use'd under the file's
  -- own name.
  local function run_source(file)
    local abs = mep_lsp_abspath(file)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, '#use ' .. mlquote(abs) .. ';;')
      return
    end
    mep.language_ui_write_file(st.run_path, '# 1 ' .. mlquote(abs) .. '\n' .. table.concat(lines, '\n') .. '\n')
    send(st, '#use ' .. mlquote(st.run_path) .. ';;')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('OCaml language UI mode closed')
    end,
  }
end
mep.language_ui_modes.ocaml = {open = mep.ocaml_ui_open}

-- gh: the documentation of the name under the cursor in the Help tab (read
-- by mep-ocaml-lsp --doc, plus the session's type for it), and from an
-- OCaml buffer the language server's hover as well.
function mep.ocaml_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor("[%w_.']")
  if sym then sym = sym:gsub('^%.+', ''):gsub('%.+$', '') end
  if not sym or sym == '' then mep.notify('gh: no OCaml name under the cursor', 'warn') return true end
  send(st, 'MepUI.help ' .. mlquote(sym) .. ';;')
  if mep.language_for_buffer() == 'ocaml' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('ocaml', mep.ocaml_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no OCaml mode is open).
function mep.ocaml_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, variables = st.variables_text, modules = st.modules_text,
    inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while an OCaml mode
-- is open on the active tab.
function mep.ocaml_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed = false
  local function take(key, field)
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[field] then
      st[field] = text
      changed = true
      return true
    end
    return false
  end
  take('variables', 'variables_text')
  take('modules', 'modules_text')
  take('inspect', 'inspect_text')
  local help_changed = take('help', 'help_text')
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
    mep.ocaml_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
