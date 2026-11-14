// Ruby language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in a .rb
// buffer), kBuiltinLanguageUi's Ruby consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) like the Julia mode,
// whose shape it follows. Included by main.cpp only; BuiltinLanguageUiRuby()
// is the Lua chunk (help/ruby-mode.org is the user's page).
//
// The console is the stock irb -- `irb -r mep_init.rb` (mep.opt.ruby_ui_cmd,
// default {'irb'}; a `# REPL:` comment replaces it via mep.language_ui_cmd)
// -- so line editing, history, completion and value display are irb's own.
// It inherits mep's environment: RUBYLIB, GEM_HOME/GEM_PATH and BUNDLE_GEMFILE
// from the dev shell, so `require 'datamunge'` loads what it would in a
// terminal in that project.
//
// The init script (mep_ruby_ui_init_template below) defines a module MepUI
// and prepends a hook to IRB::Context#evaluate, so the panel files are
// rewritten after each evaluation (a one-second timer thread stands in when
// there is no irb). As in the other modes, nothing reads the console's
// output; the panels are files the poll loop re-reads on a timer.
//   - Variables: the session's local variables, instance variables of
//     main, constants and top-level methods it defined, with class and a
//     one-line value. Clicking a row inspects it.
//   - Features: files `require`d since the console started ($LOADED_FEATURES)
//     and the $LOAD_PATH. Clicking one opens it.
//   - Inspect: MepUI.view(x) -- the value pretty-printed, its class,
//     ancestors, instance variables and own methods.
//   - Help: MepUI.help("name") -- the YARD documentation, read from the
//     source by `mep-ruby-lsp --doc` (for a Rosetta-generated gem, the C++
//     documentation in its .rb stubs), after resolving the name in the live
//     session: `frame.filter` finds frame's class and the method's owner,
//     and the files that define it go along with the session's $LOAD_PATH.
//     The runtime's own view (parameters, owner, source location,
//     ancestors) follows.
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, padded so its line
// numbers are the source's, and type one MepUI.eval_file(...) line, which
// evaluates it in irb's binding under the source file's name -- locals it
// assigns stay in the session, and errors, __FILE__, __dir__ and
// require_relative point at the real file. The value is irb's to display.
// The block at the cursor is a top-level statement: from the nearest line at
// or above it that starts one at column 0 to just before the next (a whole
// class, module, def or do...end call).
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiRuby() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.ruby_ui_cmd = mep.opt.ruby_ui_cmd or {'irb'}
mep.opt.ruby_ui_info_share = mep.opt.ruby_ui_info_share or 0.3
mep.opt.ruby_ui_bottom_share = mep.opt.ruby_ui_bottom_share or 0.4
mep.opt.ruby_ui_bottom_info_share = mep.opt.ruby_ui_bottom_info_share or 0.4
mep.language_ui_extensions.rb = mep.language_ui_extensions.rb or 'ruby'
mep.language_ui_extensions.rake = mep.language_ui_extensions.rake or 'ruby'

-- The console's init script. Lua puts two lines above this naming the
-- session directory and the mep-ruby-lsp binary ($mep_dir, $mep_ruby_lsp).
local mep_ruby_ui_init_template = [==[
require 'pp'

module MepUI
  DIR = $mep_dir
  LSP = $mep_ruby_lsp
  BASE_FEATURES = $LOADED_FEATURES.dup.freeze
  BASE_CONSTANTS = (Object.constants + [:MepUI]).freeze
  BASE_METHODS = Object.private_instance_methods(false).freeze
  OWN_LOCALS = %i[_ __ ___].freeze
  @lsp_usable = true

  module_function

  # Atomically (write, then rename), so a reader never sees a half-written panel.
  def write_text(name, text)
    path = File.join(DIR, name)
    tmp = "#{path}.#{Process.pid}.#{Thread.current.object_id}.tmp"
    File.binwrite(tmp, text)
    File.rename(tmp, path)
  rescue StandardError
    nil
  end

  def short(value, width = 60)
    s = begin
      value.inspect
    rescue Exception # rubocop:disable Lint/RescueException
      "#<#{value.class}>"
    end
    s = s.gsub(/[\r\n]+/, ' ')
    s.length > width ? "#{s[0, width - 3]}..." : s
  end

  # irb's binding (where the session's locals live), or the top level.
  def session_binding
    ctx = defined?(IRB) && IRB.respond_to?(:CurrentContext) ? (IRB.CurrentContext rescue nil) : nil
    ctx ? ctx.workspace.binding : TOPLEVEL_BINDING
  end

  def row(name, kind, detail)
    "#{name.to_s.ljust(22)} #{kind.to_s.ljust(12)} #{detail}"
  end

  def signature(meth)
    params = meth.parameters.map do |type, name|
      case type
      when :req then name.to_s
      when :opt then "#{name} = ..."
      when :rest then "*#{name}"
      when :keyreq then "#{name}:"
      when :key then "#{name}: ..."
      when :keyrest then "**#{name}"
      when :block then "&#{name}"
      else name.to_s
      end
    end
    "#{meth.name}(#{params.join(', ')})"
  end

  def refresh
    b = session_binding
    rows = []
    (b.local_variables - OWN_LOCALS).sort.each do |name|
      next if name.to_s.start_with?('_')
      value = b.local_variable_get(name)
      rows << row(name, value.class, short(value))
    end
    main = b.receiver
    main.instance_variables.sort.each do |name|
      value = main.instance_variable_get(name)
      rows << row(name, value.class, short(value))
    end
    (Object.constants - BASE_CONSTANTS).sort.each do |name|
      value = begin
        Object.const_get(name)
      rescue Exception # rubocop:disable Lint/RescueException
        next
      end
      if value.is_a?(Class)
        rows << row(name, 'class', value.superclass ? "< #{value.superclass}" : '')
      elsif value.is_a?(Module)
        rows << row(name, 'module', '')
      else
        rows << row(name, value.class, short(value))
      end
    end
    (Object.private_instance_methods(false) - BASE_METHODS).sort.each do |name|
      rows << row(name, 'method', signature(Object.instance_method(name)))
    end
    write_text('variables.txt', rows.empty? ? '' : "#{rows.join("\n")}\n")
    features = ($LOADED_FEATURES - BASE_FEATURES).map { |f| f.to_s }
    lines = features.map { |f| row(File.basename(f, '.*'), 'feature', f) }
    lines += $LOAD_PATH.map { |d| row('', 'load path', d.to_s) }
    write_text('features.txt', "#{lines.join("\n")}\n")
    nil
  end

  # A constant path ("Foo::Bar"), a local or instance variable, or nil -- never evaluates anything else.
  def resolve_value(text, b = session_binding)
    text = text.strip
    if text.match?(/\A(::)?[A-Z]\w*(::[A-Z]\w*)*\z/)
      return [true, text.sub(/\A::/, '').split('::').inject(Object) { |m, c| m.const_get(c, false) }]
    end
    return [true, b.local_variable_get(text.to_sym)] if text.match?(/\A[a-z_]\w*\z/) && b.local_variable_defined?(text.to_sym)
    if text.match?(/\A@\w+\z/) && b.receiver.instance_variable_defined?(text)
      return [true, b.receiver.instance_variable_get(text)]
    end
    [false, nil]
  rescue NameError
    [false, nil]
  end

  def module_name(mod)
    if mod.singleton_class?
      attached = mod.respond_to?(:attached_object) ? mod.attached_object : nil
      return [attached.is_a?(Module) ? attached.name : nil, true]
    end
    [mod.name, false]
  end

  # name -> [query for mep-ruby-lsp --doc, Method/UnboundMethod or nil, Module or nil]
  def lookup(text)
    text = text.strip
    b = session_binding
    if (m = text.match(/\A(.+?)(#|\.|::)([a-z_]\w*[?!=]?|\[\]=?|[+\-*\/%<>=!~^&|]+)\z/))
      recv, sep, name = m[1], m[2], m[3]
      found, value = resolve_value(recv, b)
      return ["#{recv}#{sep == '#' ? '#' : '.'}#{name}", nil, nil] unless found
      meth = if sep == '#' && value.is_a?(Module)
               (value.instance_method(name) rescue nil)
             else
               (value.method(name) rescue nil)
             end
      return ["#{value.is_a?(Module) ? value.name : value.class.name}#{sep == '#' || !value.is_a?(Module) ? '#' : '.'}#{name}", nil, nil] unless meth
      owner, singleton = module_name(meth.owner)
      query = owner ? "#{owner}#{singleton ? '.' : '#'}#{name}" : name
      return [query, meth, nil]
    end
    found, value = resolve_value(text, b)
    if found
      return [value.name, nil, value] if value.is_a?(Module)
      return [value.class.name, nil, value.class]
    end
    if text.match?(/\A[a-z_]\w*[?!]?\z/)
      meth = (b.receiver.method(text) rescue nil)
      if meth
        owner, singleton = module_name(meth.owner)
        return [owner && owner != 'Object' ? "#{owner}#{singleton ? '.' : '#'}#{text}" : text, meth, nil]
      end
    end
    [text, nil, nil]
  end

  def lsp_doc(query, files)
    return nil unless @lsp_usable && LSP && !LSP.empty?
    args = [LSP, '--doc', query]
    $LOAD_PATH.each { |d| args.push('-I', d.to_s) }
    files.each { |f| args.push('--file', f) }
    out = IO.popen(args, 'r', external_encoding: Encoding::UTF_8, err: File::NULL, &:read)
    status = $?
    return out.strip if status&.success? && !out.strip.empty?
    @lsp_usable = false if status.nil? || status.exitstatus.to_i > 1
    nil
  rescue SystemCallError
    @lsp_usable = false
    nil
  end

  def help_text(text)
    query, meth, mod = lookup(text)
    files = []
    if meth&.source_location
      files << meth.source_location[0]
    elsif mod
      mod.instance_methods(false).first(5).each do |n|
        loc = mod.instance_method(n).source_location
        files << loc[0] if loc
      end
    end
    files += ($LOADED_FEATURES - BASE_FEATURES).select { |f| f.end_with?('.rb') }.last(50)
    files = files.select { |f| f.end_with?('.rb') && File.file?(f) }.uniq
    out = String.new(encoding: Encoding::UTF_8)
    doc = lsp_doc(query, files)
    out << doc << "\n\n" if doc
    out << "## Runtime\n\n" if meth || mod
    if meth
      out << "    #{signature(meth)}\n\n"
      owner, singleton = module_name(meth.owner)
      out << "owner: #{owner || meth.owner.inspect}#{singleton ? ' (singleton)' : ''}\n"
      loc = meth.source_location
      out << (loc ? "defined at: #{loc[0]}:#{loc[1]}\n" : "defined in: C (no Ruby source)\n")
    elsif mod
      out << "#{mod.is_a?(Class) ? 'class' : 'module'} #{mod.name}\n"
      out << "ancestors: #{mod.ancestors.first(12).map(&:to_s).join(', ')}\n"
      own = mod.public_instance_methods(false).sort
      out << "\ninstance methods: #{own.join(', ')}\n" unless own.empty?
      singletons = mod.singleton_methods(false).sort
      out << "class methods: #{singletons.join(', ')}\n" unless singletons.empty?
    end
    out = "#{text}: no documentation found in this session.\n" if out.empty?
    out
  end

  # `name`'s YARD documentation and runtime signature, in mep's Help tab.
  def help(text)
    body = begin
      help_text(text.to_s)
    rescue Exception => e # rubocop:disable Lint/RescueException
      "#{text}: #{e.class}: #{e.message}\n"
    end
    write_text('help.txt', body)
    puts "# #{text}: in the Help tab"
    nil
  end

  # `x` (a value, or a name as a string) in mep's Inspect tab.
  def view(value)
    label = value.is_a?(String) ? value : short(value)
    if value.is_a?(String)
      found, v = resolve_value(value)
      value = v if found
    end
    out = +"#{label}  (#{value.class})\n\n"
    out << value.pretty_inspect << "\n"
    mod = value.is_a?(Module) ? value : value.class
    out << "ancestors: #{mod.ancestors.first(12).map(&:to_s).join(', ')}\n"
    ivars = value.instance_variables
    unless ivars.empty?
      out << "\ninstance variables:\n"
      ivars.each { |iv| out << "  #{iv} = #{short(value.instance_variable_get(iv), 70)}\n" }
    end
    own = value.is_a?(Module) ? value.singleton_methods(false) : value.class.public_instance_methods(false)
    out << "\nmethods: #{own.sort.join(', ')}\n" unless own.empty?
    write_text('inspect.txt', out)
    nil
  rescue Exception => e # rubocop:disable Lint/RescueException
    write_text('inspect.txt', "#{label}: #{e.class}: #{e.message}\n")
    nil
  end

  # Evaluates `path` (the source's code, padded to its line numbers) in irb's
  # binding under the source file's name; the value is irb's to display.
  def eval_file(path, name = path)
    code = File.read(path)
    eval(code, session_binding, name, 1) # rubocop:disable Security/Eval
  ensure
    refresh rescue nil
  end

  def run(path, name = path)
    eval_file(path, name)
  end
end

if defined?(IRB::Context)
  IRB::Context.prepend(Module.new do
    def evaluate(*args, **kwargs, &block)
      super
    ensure
      begin
        MepUI.refresh
      rescue Exception # rubocop:disable Lint/RescueException
        nil
      end
    end
  end)
else
  Thread.new do
    loop do
      sleep 1
      MepUI.refresh rescue nil
    end
  end
end
MepUI.refresh
puts "Ruby #{RUBY_VERSION} -- mep language UI: <leader>re runs the block at the cursor, <leader>rr the file; " \
     "MepUI.help(\"name\") / gh shows help, MepUI.view(x) inspects."
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 44),
    features = mep.sidebar_create('Features', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- A single-quoted Ruby string literal.
local function rbquote(s)
  return "'" .. tostring(s):gsub('\\', '\\\\'):gsub("'", "\\'") .. "'"
end
mep.ruby_ui_quote = rbquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_variables()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.variables_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    local kind = line:match('^%S+%s+(%S+)')
    local target = name
    if kind == 'method' then target = nil end
    widgets[#widgets + 1] = {id = line, text = line, tooltip = target and ('inspect ' .. target) or nil,
      on_click = (target and st) and function() send(st, 'MepUI.view(' .. rbquote(target) .. ')') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing defined in this session yet)'} end
  mep.sidebar_set_sections(sidebars.variables, {{id = 'variables', title = '', collapsed = false, widgets = widgets}})
end

local function render_features()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local features, load_path = {}, {}
  for line in ((st and st.features_text) or ''):gmatch('[^\n]+') do
    local path = line:match('feature%s+(.+)$')
    if path then
      features[#features + 1] = {id = line, text = line, tooltip = 'open ' .. path,
        on_click = function()
          if st and st.editor_pane then mep.pane_focus(st.editor_pane) end
          mep.cmd('edit ' .. path:gsub(' ', '\\ '))
        end}
    else
      local dir = line:match('load path%s+(.+)$')
      if dir then load_path[#load_path + 1] = {id = line, text = dir} end
    end
  end
  if #features == 0 then features[1] = {id = 'empty', text = "(nothing required yet -- `require 'gem'` in the console)"} end
  mep.sidebar_set_sections(sidebars.features, {
    {id = 'features', title = 'Loaded features', collapsed = false, widgets = features},
    {id = 'load_path', title = '$LOAD_PATH', collapsed = true, widgets = load_path},
  })
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_variables()
  render_features()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Variables row, or MepUI.view(x) in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    "(no help viewed yet -- gh over a name, or MepUI.help('name') in the console)")
end

-- Which lines are inside a heredoc body, an =begin/=end block or after
-- __END__: never the start of a statement.
local function opaque_lines(lines)
  local inside, terminators, in_doc = {}, {}, false
  for i, l in ipairs(lines) do
    if #terminators > 0 then
      inside[i] = true
      if l:match('^%s*(.-)%s*$') == terminators[1].id and (terminators[1].indented or not l:match('^%s')) then
        table.remove(terminators, 1)
      end
    elseif in_doc then
      inside[i] = true
      if l:match('^=end') then in_doc = false end
    elseif l:match('^=begin') then
      inside[i] = true
      in_doc = true
    elseif l:match('^__END__%s*$') then
      for k = i, #lines do inside[k] = true end
      break
    else
      -- <<~ID, <<-ID, <<ID, with optional quotes; not `x << y` or `<<=`.
      local code = l:gsub('#[^{].*$', '')
      for flag, id in code:gmatch('<<([~-]?)["\'`]?([%a_][%w_]*)["\'`]?') do
        terminators[#terminators + 1] = {id = id, indented = flag ~= ''}
      end
    end
  end
  return inside
end

-- The top-level statement around `row`: from the nearest line at or above it
-- that starts a statement at column 0 (not a closing keyword, not a comment,
-- not continuing the line before, not inside a heredoc or =begin block) to
-- just before the next one. Returns the first and last rows, or nil.
function mep.ruby_ui_block_at(lines, row)
  local n = #lines
  local inside = opaque_lines(lines)
  local function closer(l)
    for _, w in ipairs({'end', 'else', 'elsif', 'when', 'in', 'rescue', 'ensure', 'then', 'do'}) do
      if l:sub(1, #w) == w and not l:sub(#w + 1, #w + 1):match('[%w_?!:]') then return true end
    end
    return l:match('^[%)%]}.&|]') ~= nil
  end
  local function continues(k)
    local prev = lines[k - 1]
    if not prev or inside[k - 1] then return false end
    local p = prev:gsub('%s+#.*$', ''):gsub('%s+$', '')
    if p:match('[,%(%[{=+%-*/%%%^<>~&|\\:.]$') and not p:match('^%s*#') then return true end
    for _, w in ipairs({'and', 'or', 'not'}) do
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
  while e < n and not starts(e + 1) and not (inside[e + 1] and lines[e + 1]:match('^=begin')) do e = e + 1 end
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
  local name = (fname and fname ~= '') and mep_lsp_abspath(fname) or '(irb)'
  send(st, 'MepUI.eval_file(' .. rbquote(st.eval_path) .. ', ' .. rbquote(name) .. ')')
end

-- <leader>re: the Visual selection if one is live, else the top-level
-- statement at the cursor.
function mep.ruby_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Ruby console in this tab -- <leader>uu opens one', 'warn')
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
  local s, e = mep.ruby_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Ruby statement at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.ruby_ui_eval_selection = mep.ruby_ui_eval_block
mep.language_ui_eval_handlers.ruby = mep.ruby_ui_eval_block

function mep.ruby_ui_open()
  local session_dir = mep.language_ui_session_dir('rubyui')
  local paths = {
    variables = session_dir .. '/variables.txt', features = session_dir .. '/features.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local init_path = session_dir .. '/mep_init.rb'
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-ruby-lsp') or 'mep-ruby-lsp'
  mep.language_ui_write_file(init_path, '$mep_dir = ' .. rbquote(session_dir) .. '\n$mep_ruby_lsp = '
    .. rbquote(lsp) .. '\n' .. mep_ruby_ui_init_template)
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.ruby_ui_cmd)
  argv[#argv + 1] = '-r'
  argv[#argv + 1] = init_path
  local layout = mep.language_ui_layout({
    info_share = mep.opt.ruby_ui_info_share, bottom_share = mep.opt.ruby_ui_bottom_share,
    bottom_info_share = mep.opt.ruby_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'irb') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane, src_buf = src_buf,
    eval_path = session_dir .. '/eval.rb', run_path = session_dir .. '/run.rb',
    variables_text = '', features_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.variables, sidebars.features, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Ruby language UI: irb below; Variables/Features/Inspect top-right, Help bottom-right; <leader>re runs the block at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- the
  -- source buffer as it is now (unsaved edits too), evaluated in the
  -- session under the file's own name.
  local function run_source(fname)
    local abs = mep_lsp_abspath(fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, 'MepUI.run(' .. rbquote(abs) .. ')')
      return
    end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, 'MepUI.run(' .. rbquote(st.run_path) .. ', ' .. rbquote(abs) .. ')')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Ruby language UI mode closed')
    end,
  }
end
mep.language_ui_modes.ruby = {open = mep.ruby_ui_open}

-- gh: the session's documentation for the name under the cursor in the Help
-- tab (resolved in the live session, read by mep-ruby-lsp --doc), and from a
-- .rb buffer the language server's hover as well.
function mep.ruby_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_.:#?!@]')
  if sym then sym = sym:gsub('^[.:]+', ''):gsub('[.:]+$', '') end
  if not sym or sym == '' then mep.notify('gh: no Ruby name under the cursor', 'warn') return true end
  send(st, 'MepUI.help(' .. rbquote(sym) .. ')')
  if mep.language_for_buffer() == 'ruby' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('ruby', mep.ruby_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Ruby mode is open).
function mep.ruby_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, variables = st.variables_text, features = st.features_text,
    inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Ruby mode is
-- open on the active tab.
function mep.ruby_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
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
  take('features', 'features_text')
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
    mep.ruby_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
