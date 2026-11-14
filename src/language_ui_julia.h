// Julia language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .jl
// buffer), kBuiltinLanguageUi's Julia consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Lua mode, whose
// shape it follows. Included by main.cpp only; BuiltinLanguageUiJulia() is
// the Lua chunk (help/julia-mode.org is the user's page).
//
// The console is the stock REPL -- `julia -i -L mep_init.jl`
// (mep.opt.julia_ui_cmd, default {'julia'}; a `# REPL:` comment replaces it
// via mep.language_ui_cmd) -- so line editing, history, help mode (`?`), pkg
// mode (`]`) and value display are Julia's own. It inherits mep's
// environment: JULIA_LOAD_PATH and JULIA_PROJECT from the dev shell, so
// `using Package` loads what it would in a terminal in that project (in
// datamunge, the Rosetta-generated Datamunge package).
//
// The init script (mep_julia_ui_init_template below) defines a module MepUI
// in Main and installs, through atreplinit, a prompt *function*: the REPL
// calls it before every prompt, so the panel files are rewritten after each
// evaluation -- the Lisp mode's prompt hook, with Julia's public API. As in
// the other modes, nothing reads the console's output; the panels are files
// the poll loop re-reads on a timer.
//   - Variables: the bindings the session has made in Main, with type and a
//     one-line value. Clicking a row inspects it.
//   - Modules: packages loaded since the console started, with their files.
//     Clicking one shows its docstring and exports in Help.
//   - Inspect: MepUI.view(x) -- the value displayed as at the REPL, plus
//     dump() of its structure.
//   - Help: MepUI.help("name") -- the docstring (what `?name` prints: for a
//     Rosetta-generated package, the C++ documentation) and the methods.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, padded so its line
// numbers are the source's, and type one MepUI.eval_file(...) line, which
// runs it with include_string under the source file's name -- errors,
// @__FILE__ and @__DIR__ point at the real file. The block at the cursor is
// a top-level statement: from the nearest line at or above it that starts
// one at column 0 to just before the next (a whole function, struct, module
// or begin/let block, a docstring with what it documents).
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiJulia() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.julia_ui_cmd = mep.opt.julia_ui_cmd or {'julia'}
mep.opt.julia_ui_info_share = mep.opt.julia_ui_info_share or 0.3
mep.opt.julia_ui_bottom_share = mep.opt.julia_ui_bottom_share or 0.4
mep.opt.julia_ui_bottom_info_share = mep.opt.julia_ui_bottom_info_share or 0.4
mep.language_ui_extensions.jl = mep.language_ui_extensions.jl or 'julia'

-- The console's init script. Lua puts one line above this naming the
-- session directory (const __mep_dir = "...").
local mep_julia_ui_init_template = [==[
module MepUI

import REPL
import InteractiveUtils

const dir = Main.__mep_dir
const base_names = Set{Symbol}(names(Main; all = true))
const base_modules = Set{Base.PkgId}(keys(Base.loaded_modules))
const own = Set{Symbol}([:MepUI, :__mep_dir, :ans, :err])

function write_text(name, text)
    try
        open(joinpath(dir, name), "w") do io
            write(io, text)
        end
    catch
    end
    nothing
end

function short(x, width = 60)
    s = try
        sprint(show, x; context = (:limit => true, :compact => true, :displaysize => (1, 200)))
    catch
        "?"
    end
    s = replace(s, r"[\r\n]+" => " ")
    length(s) > width ? first(s, width - 3) * "..." : s
end

function kind(x)
    x isa Module && return "module"
    x isa Function && return "function"
    x isa Type && return "type"
    string(nameof(typeof(x)))
end

function refresh()
    rows = String[]
    for name in sort!(Base.invokelatest(names, Main; all = true))
        (name in base_names || name in own) && continue
        s = string(name)
        startswith(s, "#") && continue
        # invokelatest: names defined after MepUI was compiled live in a newer world (Julia 1.12+).
        Base.invokelatest(isdefined, Main, name) || continue
        x = Base.invokelatest(getfield, Main, name)
        x isa Module && continue
        detail = x isa Function ? "$(length(methods(x))) methods" : x isa Type ? string(supertype(x)) : short(x)
        push!(rows, rpad(s, 22) * " " * rpad(kind(x), 10) * " " * detail)
    end
    write_text("variables.txt", join(rows, "\n") * (isempty(rows) ? "" : "\n"))
    rows = String[]
    for (id, m) in sort!(collect(Base.loaded_modules); by = p -> p.first.name)
        id in base_modules && continue
        file = something(pathof(m), "")
        push!(rows, rpad(id.name, 22) * " " * file)
    end
    write_text("modules.txt", join(rows, "\n") * "\n")
    nothing
end

# "name", "Mod.name" or "Mod.Sub.name", resolved from Main without evaluating anything else.
function resolve(text::AbstractString)
    parts = split(strip(text), '.')
    m = Main
    for (i, p) in enumerate(parts)
        isempty(p) && return nothing
        sym = Symbol(p)
        i == length(parts) && return (m, sym)
        isdefined(m, sym) || return nothing
        next = getfield(m, sym)
        next isa Module || return nothing
        m = next
    end
    nothing
end

function help_text(text::AbstractString)
    found = resolve(text)
    found === nothing && return "$text: not a name in this session.\n"
    m, sym = found
    out = IOBuffer()
    doc = try
        string(Base.Docs.doc(Base.Docs.Binding(m, sym)))
    catch
        ""
    end
    println(out, doc)
    if isdefined(m, sym)
        x = getfield(m, sym)
        if x isa Function && !(x isa Type)
            ms = methods(x)
            println(out, "\n# $(length(ms)) methods\n")
            for (i, method) in enumerate(ms)
                i > 60 && (println(out, "  ..."); break)
                println(out, "  ", sprint(show, method; context = :compact => true))
            end
        elseif x isa Module
            exported = filter(n -> n !== nameof(x), names(x))
            println(out, "\n# Exports\n\n", join(string.(sort(exported)), ", "))
            pathof(x) === nothing || println(out, "\nfile: ", pathof(x))
        elseif x isa DataType || x isa UnionAll
            println(out, "\nsupertype: ", supertype(x))
        end
    end
    String(take!(out))
end

"""
    MepUI.help(name)

`name`'s documentation (what `?name` prints) and methods, in mep's Help tab.
"""
function help(text::AbstractString)
    write_text("help.txt", try help_text(text) catch e; "$text: " * sprint(showerror, e) * "\n" end)
    println("# ", text, ": in the Help tab")
    nothing
end

"""
    MepUI.view(x)

`x` (a value, or a name as a string) in mep's Inspect tab: its display and its structure.
"""
function view(x)
    label = x isa AbstractString ? x : short(x)
    if x isa AbstractString
        found = resolve(x)
        if found !== nothing && isdefined(found...)
            x = getfield(found...)
        end
    end
    out = IOBuffer()
    println(out, label, "  (", typeof(x), ")\n")
    try
        show(IOContext(out, :limit => true, :displaysize => (40, 100)), MIME"text/plain"(), x)
    catch
    end
    println(out, "\n")
    try
        dump(IOContext(out, :limit => true), x; maxdepth = 3)
    catch
    end
    write_text("inspect.txt", String(take!(out)))
    nothing
end

# Runs `path` (the source's code, padded to its line numbers) in Main under
# the source file's name, displaying the last value as the REPL would.
function eval_file(path::AbstractString, name::AbstractString = path)
    code = read(path, String)
    try
        value = Base.include_string(Main, code, name)
        quiet = endswith(rstrip(code), ';')
        if value !== nothing && !quiet
            display(value)
        end
    catch e
        Base.display_error(stderr, e, Base.catch_backtrace())
    end
    # In the newest world: what the code just defined is visible to names() and getfield.
    Base.invokelatest(refresh)
    nothing
end
run(path::AbstractString, name::AbstractString = path) = eval_file(path, name)

function __init_repl__(repl)
    # The prompt as a function: called before every prompt, after each evaluation.
    try
        repl.interface = REPL.setup_interface(repl)
        mode = repl.interface.modes[1]
        prompt = mode.prompt
        mode.prompt = () -> (try Base.invokelatest(refresh) catch end; prompt isa Function ? prompt() : prompt)
    catch
        # Some other REPL: refresh on a timer instead.
        Timer(_ -> try Base.invokelatest(refresh) catch end, 1; interval = 1)
    end
end

end # module MepUI

atreplinit(MepUI.__init_repl__)
MepUI.refresh()
println("Julia ", VERSION, " -- mep language UI: <leader>re runs the block at the cursor, <leader>rr the file; MepUI.help(\"name\") / gh shows help, MepUI.view(x) inspects.")
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

-- A Julia string literal (no interpolation).
local function jlquote(s)
  return '"' .. tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('%$', '\\$'):gsub('\n', '\\n') .. '"'
end
mep.julia_ui_quote = jlquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_variables()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.variables_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and ('inspect ' .. name) or nil,
      on_click = (name and st) and function() send(st, 'MepUI.view(' .. jlquote(name) .. ')') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing defined in this session yet)'} end
  mep.sidebar_set_sections(sidebars.variables, {{id = 'variables', title = '', collapsed = false, widgets = widgets}})
end

local function render_modules()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.modules_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and ('help ' .. name) or nil,
      on_click = (name and st) and function()
        send(st, 'MepUI.help(' .. jlquote(name) .. ')')
        if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
      end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no packages loaded yet -- `using Package` in the console)'} end
  mep.sidebar_set_sections(sidebars.modules, {{id = 'modules', title = '', collapsed = false, widgets = widgets}})
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_variables()
  render_modules()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Variables row, or MepUI.view(x) in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    '(no help viewed yet -- gh over a name, or MepUI.help("name") in the console)')
end

-- Which lines are inside a multi-line triple-quoted string or #= =# comment:
-- never the start of a statement. Single-line strings are not tracked.
local function string_lines(lines)
  local inside, open = {}, nil
  for i, l in ipairs(lines) do
    local pos = 1
    if open then inside[i] = true end
    while true do
      if open then
        local _, e = l:find(open, pos, true)
        if not e then break end
        pos = e + 1
        open = nil
      else
        local s1 = l:find('"""', pos, true)
        local s2 = l:find('#=', pos, true)
        local s = s1 and s2 and math.min(s1, s2) or s1 or s2
        if not s then break end
        local closer = (s == s1) and '"""' or '=#'
        local _, ce = l:find(closer, s + (closer == '"""' and 3 or 2), true)
        if ce then pos = ce + 1 else open = closer break end
      end
    end
  end
  return inside
end

-- The top-level statement around `row`: from the nearest line at or above it
-- that starts a statement at column 0 (not a closing keyword, not a comment,
-- not continuing the line before, not inside a triple-quoted string) to just
-- before the next one. A docstring and the definition after it are one
-- statement. Returns the first and last rows, or nil.
function mep.julia_ui_block_at(lines, row)
  local n = #lines
  local inside = string_lines(lines)
  local function closer(l)
    for _, w in ipairs({'end', 'else', 'elseif', 'catch', 'finally'}) do
      if l:sub(1, #w) == w and not l:sub(#w + 1, #w + 1):match('[%w_!]') then return true end
    end
    return l:match('^[%)%]}]') ~= nil
  end
  local function continues(k)
    local p = (lines[k - 1] or ''):gsub('%s+#[^=].*$', ''):gsub('%s+$', '')
    if inside[k - 1] and not (lines[k - 1] or ''):find('"""', 1, true) then return false end
    -- A line ending in a docstring's closing quotes documents the next one.
    if p == '"""' or (p:match('"""$') and inside[k - 1]) then return true end
    if p:match('[,%(%[{=+%-*/%%%^<>~&|\\]$') or p:match('%.%.%.$') then return true end
    for _, w in ipairs({'&&', '||', '->', 'in', 'where', 'do'}) do
      if p:sub(-#w) == w and not p:sub(-#w - 1, -#w - 1):match('[%w_]') then return true end
    end
    return false
  end
  local function starts(k)
    local l = lines[k]
    return l ~= nil and not inside[k] and l:match('^[^%s]') ~= nil and not l:match('^#')
      and not closer(l) and not continues(k)
  end
  local s = row
  while s > 1 and not starts(s) do s = s - 1 end
  if not starts(s) then return nil end
  local e = s
  while e < n and not starts(e + 1) do e = e + 1 end
  while e > s and not inside[e] and (lines[e]:match('^%s*$') or lines[e]:match('^%s*#')) do e = e - 1 end
  return s, e
end

-- Writes rows first..last of `lines` to the session's eval file, padded so
-- its line numbers are the source's, and has the console run it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  mep.language_ui_write_file(st.eval_path, text)
  local fname = mep.filename and mep.filename()
  local name = (fname and fname ~= '') and mep_lsp_abspath(fname) or 'REPL'
  send(st, 'MepUI.eval_file(' .. jlquote(st.eval_path) .. ', ' .. jlquote(name) .. ')')
end

-- <leader>re: the Visual selection if one is live, else the top-level
-- statement at the cursor.
function mep.julia_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Julia console in this tab -- <leader>uu opens one', 'warn')
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
  local s, e = mep.julia_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Julia statement at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.julia_ui_eval_selection = mep.julia_ui_eval_block
mep.language_ui_eval_handlers.julia = mep.julia_ui_eval_block

function mep.julia_ui_open()
  local session_dir = mep.language_ui_session_dir('juliaui')
  local paths = {
    variables = session_dir .. '/variables.txt', modules = session_dir .. '/modules.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local init_path = session_dir .. '/mep_init.jl'
  mep.language_ui_write_file(init_path, 'const __mep_dir = ' .. jlquote(session_dir) .. '\n' .. mep_julia_ui_init_template)
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.julia_ui_cmd)
  argv[#argv + 1] = '-i'
  argv[#argv + 1] = '-L'
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.julia_ui_info_share, bottom_share = mep.opt.julia_ui_bottom_share,
    bottom_info_share = mep.opt.julia_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'julia') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane, src_buf = src_buf,
    eval_path = session_dir .. '/eval.jl', run_path = session_dir .. '/run.jl',
    variables_text = '', modules_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.modules, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Julia language UI: REPL below; Variables/Modules/Inspect top-right, Help bottom-right; <leader>re runs the block at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- the
  -- source buffer as it is now (unsaved edits too), run in Main under the
  -- file's own name.
  local function run_source(fname)
    local abs = mep_lsp_abspath(fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, 'MepUI.run(' .. jlquote(abs) .. ')')
      return
    end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, 'MepUI.run(' .. jlquote(st.run_path) .. ', ' .. jlquote(abs) .. ')')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Julia language UI mode closed')
    end,
  }
end
mep.language_ui_modes.julia = {open = mep.julia_ui_open}

-- gh: the session's docstring and methods for the name under the cursor in
-- the Help tab, and from a .jl buffer the language server's hover as well
-- (mep-julia-lsp reads `using`ed packages' docstrings without loading them).
function mep.julia_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_.!@]')
  if sym then sym = sym:gsub('^[.]+', ''):gsub('[.]+$', '') end
  if not sym or sym == '' then mep.notify('gh: no Julia name under the cursor', 'warn') return true end
  send(st, 'MepUI.help(' .. jlquote(sym) .. ')')
  if mep.language_for_buffer() == 'julia' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('julia', mep.julia_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Julia mode is open).
function mep.julia_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, variables = st.variables_text, modules = st.modules_text,
    inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Julia mode is
-- open on the active tab.
function mep.julia_ui_poll(st)
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
  take('variables', 'variables_text')
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
    mep.julia_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
