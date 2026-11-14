// Tcl language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .tcl
// buffer), kBuiltinLanguageUi's Tcl consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Ruby mode's shape.
// Included by main.cpp only; BuiltinLanguageUiTcl() is the Lua chunk
// (help/tcl-mode.org is the user's page).
//
// The console is `tclsh mep_init.tcl` (mep.opt.tcl_ui_cmd, default
// {'tclsh'}; a `# REPL:` comment replaces it via mep.language_ui_cmd). tclsh
// only goes interactive without a script, so the init script ends in the
// same read-eval-print loop tclsh runs (prompt `% `, `> ` while a command is
// incomplete, results and error messages printed, the event loop serviced
// while it waits, tcl_interactive set). It inherits mep's environment:
// TCLLIBPATH (tclsh's own auto_path), and MEP_TCL_PATH and the source file's
// directory, which the init script appends to auto_path, so
// `package require datamunge` loads what it would in that project.
//
// The panels are files the poll loop re-reads; the loop rewrites them before
// each prompt (only when they change):
//   - Variables: `info vars` of the global namespace (what tclsh starts with
//     left out): scalars with their value, arrays with their size. Clicking
//     a row inspects it.
//   - Procs: `info procs` -- the global ones and those of the namespaces the
//     session created, with their arguments. Clicking one shows its Help.
//   - Packages: the packages loaded (`package present`), and auto_path.
//   - Inspect: ::mep::view name -- the value, as a list and a dict when it
//     is one, or an array's elements.
//   - Help: ::mep::help name -- for a proc, its arguments with defaults and
//     the comment block above its definition, read by `mep-tcl-lsp --doc`
//     from the files the session sourced (`source` is wrapped to record
//     them, `package require` included) and auto_path; for a built-in, the
//     usage line and manual text from the server's vocabulary.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, padded so its line
// numbers are the source's, and type one ::mep::eval_file line, which evaluates
// it at the global level with [info script] naming the source file (so
// `file dirname [info script]` finds its siblings); error traces count the
// source's lines. The block at the cursor is a top-level command: from the
// nearest line at or above it that starts one to where it is complete (a
// whole proc, namespace eval or if {...}).
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiTcl() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.tcl_ui_cmd = mep.opt.tcl_ui_cmd or {'tclsh'}
mep.opt.tcl_ui_info_share = mep.opt.tcl_ui_info_share or 0.3
mep.opt.tcl_ui_bottom_share = mep.opt.tcl_ui_bottom_share or 0.4
mep.opt.tcl_ui_bottom_info_share = mep.opt.tcl_ui_bottom_info_share or 0.4
mep.language_ui_extensions.tcl = mep.language_ui_extensions.tcl or 'tcl'
mep.language_ui_extensions.tm = mep.language_ui_extensions.tm or 'tcl'

-- The console's init script. Lua appends the start call below it:
-- ::mep::start DIR LSP {auto_path dirs} [repl].
local mep_tcl_ui_init_template = [==[
namespace eval ::mep {
    variable dir ""
    variable lsp ""
    variable lsp_usable 1
    variable files {}
    variable base_vars {}
    variable base_procs {}
    variable base_namespaces {}
    variable base_packages {}
    variable last {}
    variable buf ""
}

# Atomically (write, then rename), so a reader never sees a half-written panel.
proc ::mep::write {name text} {
    variable dir
    variable last
    if {[dict exists $last $name] && [dict get $last $name] eq $text} return
    dict set last $name $text
    set path [file join $dir $name]
    set tmp "$path.[pid].tmp"
    if {[catch {
        set f [open $tmp w]
        fconfigure $f -encoding utf-8
        puts -nonewline $f $text
        close $f
        file rename -force $tmp $path
    }]} {
        catch {close $f}
    }
}

proc ::mep::short {value {width 60}} {
    set s [string map {\n " " \r " " \t " "} $value]
    if {[string length $s] > $width} {set s "[string range $s 0 [expr {$width - 4}]]..."}
    return $s
}

proc ::mep::row {name kind detail} {
    return [format {%-22s %-10s %s} $name $kind $detail]
}

# The namespaces the session made (Tcl's own and mep's left out).
proc ::mep::namespaces {} {
    variable base_namespaces
    set out {}
    foreach ns [namespace children ::] {
        if {$ns in $base_namespaces || $ns eq "::mep"} continue
        lappend out $ns
        lappend out {*}[namespace children $ns]
    }
    return $out
}

proc ::mep::refresh {} {
    variable base_vars
    variable base_procs
    variable base_packages
    set rows {}
    foreach v [lsort [info vars ::*]] {
        set n [string range $v 2 end]
        if {$v in $base_vars || [string match auto_* $n] || [string match tcl_* $n]} continue
        if {[array exists $v]} {
            lappend rows [row $n array "[array size $v] elements"]
        } elseif {[info exists $v]} {
            lappend rows [row $n scalar [short [set $v]]]
        }
    }
    write variables.txt [expr {[llength $rows] ? "[join $rows \n]\n" : ""}]
    set rows {}
    foreach p [lsort [info procs ::*]] {
        # Tcl's own library procs, loaded on demand (auto_load, package require).
        if {$p in $base_procs || $p eq "::source" || [regexp {^::(tcl|pkg_|auto_)} $p]} continue
        lappend rows [row [string range $p 2 end] proc "{[info args $p]}"]
    }
    foreach ns [namespaces] {
        foreach p [lsort [info procs ${ns}::*]] {
            lappend rows [row [string range $p 2 end] proc "{[info args $p]}"]
        }
    }
    write procs.txt [expr {[llength $rows] ? "[join $rows \n]\n" : ""}]
    set rows {}
    foreach p [lsort [package names]] {
        if {$p in $base_packages} continue
        if {![catch {package present $p} v]} {lappend rows "package $p $v"}
    }
    foreach d $::auto_path {lappend rows "path $d"}
    write packages.txt [expr {[llength $rows] ? "[join $rows \n]\n" : ""}]
}

# A word list as one shell-free argv for exec.
proc ::mep::lsp_doc {name} {
    variable lsp
    variable lsp_usable
    variable files
    if {!$lsp_usable || $lsp eq ""} {return ""}
    set args [list $lsp --doc $name]
    foreach f $files {lappend args --file $f}
    foreach d $::auto_path {lappend args -L $d}
    if {[catch {exec {*}$args 2>@1} out opts]} {
        set code [lindex [dict get $opts -errorcode] end]
        if {[lindex [dict get $opts -errorcode] 0] ne "CHILDSTATUS" || $code > 1} {set lsp_usable 0}
        return ""
    }
    return [string trimright $out]
}

proc ::mep::help_text {name} {
    set out ""
    set q [namespace eval :: [list namespace which -command $name]]
    if {$q ne "" && [info procs $q] ne ""} {
        set doc [lsp_doc $q]
        if {$doc ne ""} {append out $doc \n\n}
        set params {}
        foreach a [info args $q] {
            if {[info default $q $a d]} {lappend params [list $a $d]} else {lappend params $a}
        }
        append out "## Runtime\n\n    proc $q {$params}\n"
        set body [string trim [info body $q]]
        set lines [split $body \n]
        if {[llength $lines] > 12} {set body "[join [lrange $lines 0 11] \n]\n    ..."}
        append out "\nbody:\n    [string map [list \n "\n    "] $body]\n"
        return $out
    }
    set doc [lsp_doc $name]
    if {$doc ne ""} {append out $doc \n}
    if {$q ne ""} {
        if {$out eq ""} {append out "$q: a command without documentation in this session\n"}
        append out "\ncommand: $q\n"
        return $out
    }
    set ns [expr {[string match ::* $name] ? $name : "::$name"}]
    if {[namespace exists $ns]} {
        append out "namespace [namespace qualifiers ${ns}::x]\n\n"
        foreach p [lsort [info procs ${ns}::*]] {append out "  proc [namespace tail $p] {[info args $p]}\n"}
        set exports [namespace eval $ns {namespace export}]
        if {$exports ne ""} {append out "\nexports: $exports\n"}
        return $out
    }
    if {$out eq ""} {set out "$name: no documentation found in this session.\n"}
    return $out
}

# `name`'s documentation in mep's Help tab.
proc ::mep::help {name} {
    if {[catch {help_text $name} body]} {set body "$name: $body\n"}
    write help.txt $body
    return "# $name: in the Help tab"
}

# A variable (by name) in mep's Inspect tab.
proc ::mep::view {name} {
    set v [expr {[string match ::* $name] ? $name : "::$name"}]
    if {[array exists $v]} {
        set out "$name  (array, [array size $v] elements)\n\n"
        foreach k [lsort [array names $v]] {append out "  $k = [short [set ${v}($k)] 70]\n"}
    } elseif {[info exists $v]} {
        set value [set $v]
        set out "$name  (scalar, [string length $value] chars)\n\n$value\n"
        if {![catch {llength $value} n] && $n > 1} {
            append out "\nas a list ($n elements):\n"
            set i 0
            foreach e [lrange $value 0 49] {append out [format "  %3d: %s\n" $i [short $e 70]]; incr i}
        }
        if {![catch {dict size $value} n] && $n > 0 && $n * 2 == [llength $value]} {
            append out "\nas a dict ($n keys):\n"
            dict for {k e} $value {append out "  $k = [short $e 70]\n"}
        }
    } else {
        set out "$name: no such variable\n"
    }
    write inspect.txt $out
    return
}

# `copy` (the source's code, padded to its line numbers) at the global level,
# with [info script] naming the source file `src`; the result is the console's to print.
proc ::mep::eval_file {copy src} {
    variable files
    # Where --doc looks for the doc comments of the procs this defines.
    foreach p [list $copy $src] {if {[file isfile $p] && $p ni $files} {lappend files $p}}
    set f [open $copy]
    fconfigure $f -encoding utf-8
    set code [read $f]
    close $f
    set old [info script]
    info script $src
    try {
        return [uplevel #0 $code]
    } finally {
        info script $old
        catch ::mep::refresh
    }
}
proc ::mep::run {copy src} {
    tailcall ::mep::eval_file $copy $src
}

# tclsh's own read-eval-print loop: `% ` (or tcl_prompt1), `> ` while incomplete.
proc ::mep::prompt {} {
    variable buf
    if {$buf eq ""} {
        catch ::mep::refresh
        if {[info exists ::tcl_prompt1]} {
            if {[catch {uplevel #0 $::tcl_prompt1}]} {puts -nonewline "% "}
        } else {
            puts -nonewline "% "
        }
    } else {
        puts -nonewline "> "
    }
    flush stdout
}
proc ::mep::on_input {} {
    variable buf
    if {[gets stdin line] < 0} {
        if {[eof stdin]} {exit}
        return
    }
    append buf $line \n
    if {![info complete $buf]} {
        prompt
        return
    }
    set cmd $buf
    set buf ""
    set code [catch {uplevel #0 $cmd} result]
    if {$code == 1} {
        puts stderr $result
    } elseif {$result ne ""} {
        puts $result
    }
    prompt
}

proc ::mep::start {dir lsp paths {repl 1}} {
    variable base_vars [info vars ::*]
    variable base_procs [info procs ::*]
    variable base_namespaces [namespace children ::]
    variable base_packages {}
    foreach p [package names] {if {![catch {package present $p}]} {lappend base_packages $p}}
    set ::mep::dir $dir
    set ::mep::lsp $lsp
    if {[info exists ::env(MEP_TCL_PATH)]} {
        foreach d [split $::env(MEP_TCL_PATH) [expr {$::tcl_platform(platform) eq "windows" ? ";" : ":"}]] {
            if {$d ne "" && $d ni $::auto_path} {lappend ::auto_path $d}
        }
    }
    foreach d $paths {if {$d ni $::auto_path} {lappend ::auto_path $d}}
    # Every file sourced from now on (package require's too): --doc reads their procs.
    rename ::source ::mep::_source
    proc ::source {args} {
        set f [file normalize [lindex $args end]]
        if {$f ni $::mep::files} {lappend ::mep::files $f}
        uplevel 1 [list ::mep::_source {*}$args]
    }
    lappend base_vars ::auto_path ::tcl_interactive ::errorInfo ::errorCode
    catch refresh
    puts "Tcl [info patchlevel] -- mep language UI: <leader>re runs the command at the cursor, <leader>rr the file;\
        ::mep::help name / gh shows help, ::mep::view var inspects."
    if {$repl} {
        set ::tcl_interactive 1
        fconfigure stdin -blocking 0 -buffering line
        fileevent stdin readable ::mep::on_input
        prompt
        vwait ::mep::forever
    }
}
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 44),
    procs = mep.sidebar_create('Procs', 'right', 44),
    packages = mep.sidebar_create('Packages', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- One Tcl word: braced when that keeps it verbatim, else backslash-escaped.
local function tclquote(s)
  s = tostring(s)
  if s ~= '' and not s:find('[{}\\]') and not s:find('^%s') then return '{' .. s .. '}' end
  if s == '' then return '{}' end
  return (s:gsub('([%[%]{}$;"\\%s])', '\\%1'))
end
mep.tcl_ui_quote = tclquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_rows(sidebar, text, empty_msg, action)
  local widgets = {}
  local st = state[mep.current_tab_id()]
  for line in (text or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and (action .. ' ' .. name) or nil,
      on_click = (name and st) and function()
        send(st, (action == 'inspect' and '::mep::view ' or '::mep::help ') .. tclquote(name))
      end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = empty_msg} end
  mep.sidebar_set_sections(sidebar, {{id = 'rows', title = '', collapsed = false, widgets = widgets}})
end

local function render_packages()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local pkgs, dirs = {}, {}
  for line in ((st and st.packages_text) or ''):gmatch('[^\n]+') do
    local p = line:match('^package%s+(.+)$')
    if p then
      pkgs[#pkgs + 1] = {id = line, text = p}
    else
      local d = line:match('^path%s+(.+)$')
      if d then dirs[#dirs + 1] = {id = line, text = d} end
    end
  end
  if #pkgs == 0 then pkgs[1] = {id = 'empty', text = '(no package loaded yet -- package require NAME)'} end
  mep.sidebar_set_sections(sidebars.packages, {
    {id = 'packages', title = 'Loaded packages', collapsed = false, widgets = pkgs},
    {id = 'auto_path', title = 'auto_path', collapsed = true, widgets = dirs},
  })
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_rows(sidebars.variables, st and st.variables_text, '(no variables set in this session yet)', 'inspect')
  render_rows(sidebars.procs, st and st.procs_text, '(no procs defined in this session yet)', 'help')
  render_packages()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Variables row, or ::mep::view var in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    "(no help viewed yet -- gh over a command, or ::mep::help name in the console)")
end

-- Scans one line of Tcl for the command-completeness state: brace depth,
-- an open quoted word or [ ] nesting, a trailing backslash. `s` carries
-- {depth, quote, bracket, at_start}; `at_start` is true when the next
-- character begins a new command (where `#` starts a comment).
local function scan_line(line, s)
  local i, n = 1, #line
  while i <= n do
    local c = line:sub(i, i)
    if c == '\\' then
      if i == n then s.continued = true return s end
      i = i + 2
      s.at_start = false
    elseif s.depth > 0 then
      if c == '{' then s.depth = s.depth + 1 elseif c == '}' then s.depth = s.depth - 1 end
      i = i + 1
    elseif s.at_start and c == '#' and not s.quote and s.bracket == 0 then
      -- A comment runs to the end of the line (a trailing backslash continues it).
      if line:sub(-1) == '\\' then s.continued = true end
      return s
    elseif c:match('%s') then
      i = i + 1
    else
      if c == '"' then
        s.quote = not s.quote
      elseif c == '{' and not s.quote then
        s.depth = s.depth + 1
      elseif c == '[' then
        s.bracket = s.bracket + 1
      elseif c == ']' and s.bracket > 0 then
        s.bracket = s.bracket - 1
      end
      s.at_start = (c == ';' and not s.quote and s.bracket == 0) or (c == '[')
      i = i + 1
    end
  end
  return s
end

-- The top-level command around `row`: from the nearest line at or above it
-- that starts one to the line where it is complete. Returns the first and
-- last rows, or nil on a blank or comment line between commands.
function mep.tcl_ui_block_at(lines, row)
  local n = #lines
  local starts = {}
  local cur, s, in_comment = nil, nil, false
  for i = 1, n do
    local l = lines[i]
    if not cur then
      local t = l:match('^%s*(.-)%s*$')
      if in_comment or t:match('^#') then
        in_comment = l:sub(-1) == '\\'  -- a trailing backslash continues the comment
      elseif t ~= '' then
        cur = i
        s = {depth = 0, quote = false, bracket = 0, at_start = true}
      end
    end
    if cur then
      s.continued = false
      scan_line(l, s)
      if s.depth <= 0 and not s.quote and s.bracket == 0 and not s.continued then
        starts[cur] = i
        cur = nil
      end
    end
  end
  if cur then starts[cur] = n end
  for r = row, 1, -1 do
    local e = starts[r]
    if e then
      if e >= row then return r, e end
      return nil
    end
  end
  return nil
end

-- Writes rows first..last of `lines` to the session's eval file, padded so
-- its line numbers are the source's, and has the console evaluate it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  mep.language_ui_write_file(st.eval_path, text)
  local fname = mep.filename and mep.filename()
  local name = (fname and fname ~= '') and mep_lsp_abspath(fname) or st.eval_path
  send(st, '::mep::eval_file ' .. tclquote(st.eval_path) .. ' ' .. tclquote(name))
end

-- <leader>re: the Visual selection if one is live, else the top-level
-- command at the cursor.
function mep.tcl_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Tcl console in this tab -- <leader>uu opens one', 'warn')
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
  local s, e = mep.tcl_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Tcl command at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.tcl_ui_eval_selection = mep.tcl_ui_eval_block
mep.language_ui_eval_handlers.tcl = mep.tcl_ui_eval_block

function mep.tcl_ui_open()
  local session_dir = mep.language_ui_session_dir('tclui')
  local paths = {
    variables = session_dir .. '/variables.txt', procs = session_dir .. '/procs.txt',
    packages = session_dir .. '/packages.txt', inspect = session_dir .. '/inspect.txt',
    help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local fname = mep.filename and mep.filename() or ''
  local src_abs = (fname ~= '') and mep_lsp_abspath(fname) or ''
  local src_dir = src_abs:match('^(.*)[/\\][^/\\]*$')
  local add = {}
  if src_dir and src_dir ~= '' then add[#add + 1] = tclquote(src_dir) end
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-tcl-lsp') or 'mep-tcl-lsp'
  local init_path = session_dir .. '/mep_init.tcl'
  mep.language_ui_write_file(init_path, mep_tcl_ui_init_template .. '\n::mep::start ' .. tclquote(session_dir) .. ' '
    .. tclquote(lsp) .. ' {' .. table.concat(add, ' ') .. '}\n')
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.tcl_ui_cmd)
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.tcl_ui_info_share, bottom_share = mep.opt.tcl_ui_bottom_share,
    bottom_info_share = mep.opt.tcl_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'tclsh') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, argv = argv,
    eval_path = session_dir .. '/eval.tcl', run_path = session_dir .. '/run.tcl',
    variables_text = '', procs_text = '', packages_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.procs, sidebars.packages, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Tcl language UI: tclsh below; Variables/Procs/Packages/Inspect top-right, Help bottom-right; <leader>re runs the command at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open (a file
  -- with a CMD: line runs that instead, as without the mode) -- the source
  -- buffer as it is now (unsaved edits too), evaluated in the session with
  -- [info script] naming the file.
  local function run_source(f)
    local abs = mep_lsp_abspath(f or fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, '::mep::run ' .. tclquote(abs) .. ' ' .. tclquote(abs))
      return
    end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, '::mep::run ' .. tclquote(st.run_path) .. ' ' .. tclquote(abs))
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Tcl language UI mode closed')
    end,
  }
end
mep.language_ui_modes.tcl = {open = mep.tcl_ui_open}

-- gh: the session's documentation for the command under the cursor in the
-- Help tab, and from a .tcl buffer the language server's hover as well.
function mep.tcl_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_:]')
  if sym then sym = sym:gsub(':+$', '') end
  if not sym or sym == '' then mep.notify('gh: no Tcl command under the cursor', 'warn') return true end
  send(st, '::mep::help ' .. tclquote(sym))
  if mep.language_for_buffer() == 'tcl' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('tcl', mep.tcl_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Tcl mode is open).
function mep.tcl_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, variables = st.variables_text,
    procs = st.procs_text, packages = st.packages_text, inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Tcl mode is
-- open on the active tab.
function mep.tcl_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed, help_changed = false, false
  for _, key in ipairs({'variables', 'procs', 'packages', 'inspect', 'help'}) do
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
    mep.tcl_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
