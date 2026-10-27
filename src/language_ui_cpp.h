// C++ language UI runtime adapters and crepl session panels. Included by main.cpp only.
#pragma once
#include <string>

/// @brief Returns the C++ extension layered over the existing C/C++ compiler panels.
inline const char* BuiltinLanguageUiCpp() {
  static const std::string chunk = std::string("local runtime = [==[\n") + R"MEPCPP(// mep C++ UI adapters. Loaded into a crepl session, never into the user's file.
#pragma once
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
namespace mep_ui {
inline const std::filesystem::path directory = MEP_UI_DIRECTORY;
inline unsigned figure_count = 0;
inline void publish(const std::string& name, const std::string& text) {
  auto path = directory / name;
  auto tmp = path.string() + ".tmp";
  { std::ofstream out(tmp); out << text; if (!out) throw std::runtime_error("cannot write UI preview"); }
  std::filesystem::rename(tmp, path);
}
template<class T> std::string element(const T& value) {
  std::ostringstream out;
  if constexpr (requires { out << value; }) out << value;
  else if constexpr (requires { value.r; value.g; value.b; })
    out << '(' << +value.r << ", " << +value.g << ", " << +value.b << ')';
  else out << "<object>";
  return out.str();
}
template<class T> void view(const T& value) {
  std::string text;
  if constexpr (requires { value.head(100).to_string(); }) text = value.head(100).to_string();
  else if constexpr (requires { value.to_string(); }) text = value.to_string();
  else if constexpr (std::is_convertible_v<const T&, std::string_view>) text = std::string(std::string_view(value));
  else if constexpr (std::ranges::input_range<const T>) {
    unsigned count = 0;
    for (const auto& item : value) {
      if (count == 100) { text += "... (preview limited to 100 items)\n"; break; }
      text += '[' + std::to_string(++count) + "] " + element(item) + '\n';
    }
    if (count == 0) text = "(empty range)";
  } else if constexpr (requires { value.values(); }) {
    unsigned count = 0;
    for (const auto& item : value.values()) {
      if (count == 100) { text += "... (preview limited to 100 items)\n"; break; }
      text += '[' + std::to_string(++count) + "] " + element(item) + '\n';
    }
  } else text = element(value);
  if (text.size() > 128 * 1024) text = text.substr(0, 128 * 1024) + "\n... (truncated)";
  publish("preview.txt", text);
}
template<class T> void show(T& value) {
  if constexpr (requires { value.save(std::string{}); }) {
    std::ostringstream number;
    number << std::setw(4) << std::setfill('0') << ++figure_count;
    const auto path = directory / "figures" / ("fig_" + number.str() + ".png");
    const auto tmp = path.parent_path() / ("pending_" + number.str() + ".png");
    value.save(tmp.string());
    if (!std::filesystem::is_regular_file(tmp)) throw std::runtime_error("plot save() did not create a PNG");
    std::filesystem::rename(tmp, path);
  } else if constexpr (requires { value.show(); }) value.show();
  else throw std::runtime_error("This type needs a mep_ui::show adapter");
}
}
)MEPCPP" + "]==]\n" + R"MEPLUA(-- crepl-backed extension of the existing C/C++ layout. Assembly/Build/Hex remain available.
mep.opt.cpp_ui_cmd = mep.opt.cpp_ui_cmd or {'crepl'}
mep.opt.cpp_ui_flags = mep.opt.cpp_ui_flags or ''
-- Optional include-prefix -> pkg-config module adapters; compiler flags from Run Setup still apply.
mep.opt.cpp_ui_packages = mep.opt.cpp_ui_packages or {['dm/'] = 'datamunge', ['datamunge/'] = 'datamunge'}
local states, sidebars = {}, nil
local function decode(text)
  return (text:gsub('\\([nrt\\])', function(c) return ({n='\n', r='\r', t='\t', ['\\']='\\'})[c] end))
end
local function active() return states[mep.current_tab_id()] end
local function send(st, text)
  if not st or not mep.is_terminal_buffer(st.term_buf) then return false end
  local info = mep.terminal_info(st.term_buf)
  if info and info.exited then mep.notify('C++ console exited; :MepCppRestart starts a fresh session', 'warn') return false end
  mep.terminal_write(st.term_buf, text .. '\n')
  return true
end
local function render()
  if not sidebars then return end
  local st = active()
  local widgets = {}
  for line in ((st and st.cpp.objects) or ''):gmatch('[^\n]+') do
    local name, ty, value = line:match('^([^\t]+)\t([^\t]*)\t(.*)$')
    if name then
      name, ty, value = decode(name), decode(ty), decode(value)
      widgets[#widgets+1] = {id=name, text=name .. ' : ' .. ty .. (value ~= '' and (' = ' .. value) or ''),
        tooltip='Inspect ' .. name, on_click=function() mep.cpp_ui_inspect(name) end}
    end
  end
  if #widgets == 0 then widgets[1] = {id='empty', text='(no live objects yet -- evaluate a declaration)'} end
  mep.sidebar_set_sections(sidebars.objects, {{id='objects', title='', widgets=widgets}})
  local data = st and st.cpp.inspect or ''
  local preview = st and st.cpp.preview or ''
  if preview ~= '' then data = data .. '\nValue preview:\n' .. preview end
  mep.language_ui_render_textbox(sidebars.data, data, '(click an Objects row to inspect its type, members, and value)')
  mep.language_ui_render_textbox(sidebars.definitions, st and st.cpp.definitions, '(no functions or types declared yet)')
  widgets = {}
  local history = st and st.cpp.history or {}
  for i = #history, math.max(1, #history-199), -1 do
    local code = history[i]
    widgets[#widgets+1] = {id='h' .. i, text=code, wrap=true,
      on_click=function() send(st, ':begin\n' .. code .. '\n:end') end}
  end
  if #widgets == 0 then widgets[1] = {id='empty', text='(successful inputs appear here; clicking re-executes them)'} end
  mep.sidebar_set_sections(sidebars.history, {{id='history', title='', widgets=widgets}})
end
function mep.cpp_ui_start_console(st)
  if st.ext == 'c' then mep.terminal_here() return end
  local dir = st.session_dir
  mep.fs_mkdir(dir .. '/figures')
  local paths = {objects=dir .. '/objects.tsv', definitions=dir .. '/definitions.txt',
    inspect=dir .. '/inspect.txt', preview=dir .. '/preview.txt', history=dir .. '/history.txt', status=dir .. '/status.txt'}
  for _, path in pairs(paths) do mep.language_ui_write_file(path, '') end
  st.cpp = {paths=paths, objects='', definitions='', inspect='', preview='', history={}, raw={}}
  local adapter = dir .. '/mep_ui.hpp'
  -- %q is a Lua literal, not a C++ literal; quote paths with JSON, whose string escapes C++ accepts here.
  local cpp_dir = '"' .. dir:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r') .. '"'
  mep.language_ui_write_file(adapter, '#define MEP_UI_DIRECTORY ' .. cpp_dir .. '\n' .. runtime)
  local shq = mep.language_ui_shq
  local argv = {}
  for _, arg in ipairs(mep.opt.cpp_ui_cmd) do
    if arg == 'crepl' and mep.bundled_tool then arg = mep.bundled_tool('crepl') end
    argv[#argv+1] = shq(arg)
  end
  argv[#argv+1] = '--state-dir ' .. shq(dir)
  argv[#argv+1] = '--display-function mep_ui::show -include ' .. shq(adapter)
  argv[#argv+1] = '-I' .. shq(st.src_dir)
  local cfg = mep.run_button_config_for and mep.run_button_config_for(st.ext)
  local flags = (cfg and cfg.flags) or ''
  local compiler_flags = {flags, mep.opt.cpp_ui_flags}
  if not (flags .. ' ' .. mep.opt.cpp_ui_flags):find('%-std=') then compiler_flags[#compiler_flags+1] = '-std=c++23' end
  local seen = {}
  for row = 1, mep.line_count() do
    local include = (mep.get_line(row) or ''):match('^%s*#%s*include%s*[<"]([^>"]+)')
    for prefix, module in pairs(mep.opt.cpp_ui_packages) do
      if include and include:sub(1, #prefix) == prefix and not seen[module] then
        seen[module] = true
        compiler_flags[#compiler_flags+1] = '$(pkg-config --cflags --libs ' .. shq(module) .. ')'
      end
    end
  end
  st.cpp.compiler_flags = table.concat(compiler_flags, ' ')
  argv[#argv+1] = st.cpp.compiler_flags
  local command = 'cd ' .. shq(st.src_dir) .. ' && exec ' .. table.concat(argv, ' ')
  mep.terminal_here_argv({'sh', '-c', command}, 'crepl')
end
function mep.cpp_ui_setup(st, layout)
  if not st.cpp then return end
  if not sidebars then
    sidebars = {objects=mep.sidebar_create('Objects', 'right', 60), data=mep.sidebar_create('Data', 'right', 60),
      definitions=mep.sidebar_create('Definitions', 'right', 60), history=mep.sidebar_create('History', 'right', 60)}
  end
  states[mep.current_tab_id()] = st
  st.cpp.editor_pane = layout.editor_pane
  st.cpp.eval_path = st.session_dir .. '/source.cpp'
  mep.pane_focus(layout.top_pane)
  local bufs = {}
  for _, key in ipairs({'history','definitions','data','objects'}) do
    mep.sidebar_open_pane(sidebars[key]); bufs[key] = mep.current_buffer()
  end
  st.cpp.data_buf = bufs.data
  render()
  local placeholder = st.session_dir .. '/No_figure_yet.txt'
  mep.language_ui_write_file(placeholder, 'No figure yet.\nEvaluate plot.show(); or mep_ui::show(plot); to capture a PNG in this pane.\n')
  st.cpp.gallery = mep.language_ui_gallery(layout.bottom_pane, st.session_dir .. '/figures', placeholder)
  mep.pane_focus(layout.editor_pane)
end
function mep.cpp_ui_close(st)
  if not st.cpp then return end
  for tid, current in pairs(states) do if current == st then states[tid] = nil end end
end
function mep.cpp_ui_poll(st)
  if not st.cpp then return end
  local changed = false
  for key, path in pairs(st.cpp.paths) do
    local text = mep.language_ui_read_file(path) or ''
    if st.cpp.raw[key] ~= text then
      st.cpp.raw[key] = text
      if key == 'history' then
        st.cpp.history = {}
        for line in text:gmatch('[^\n]+') do st.cpp.history[#st.cpp.history+1] = decode(line) end
      else st.cpp[key] = text end
      changed = true
    end
  end
  if changed and active() == st then render() end
  if st.cpp.gallery then st.cpp.gallery:poll() end
end
function mep.cpp_ui_inspect(name)
  local st = active()
  if not st then return false end
  -- Only named session objects can trigger generated C++ expressions.
  if not name:match('^[%a_][%w_:]*$') then return false end
  st.cpp.preview = ''; mep.language_ui_write_file(st.cpp.paths.preview, '')
  send(st, ':inspect ' .. name)
  send(st, 'mep_ui::view(' .. name .. ');')
  if st.cpp.data_buf and mep.reveal_buffer then mep.reveal_buffer(st.cpp.data_buf, true) end
  return true
end
local function source_snapshot(st)
  local lines = mep.buffer_get_lines(st.src_buf) or {}
  st.cpp.submission = (st.cpp.submission or 0) + 1
  local path = st.session_dir .. '/source_' .. st.cpp.submission .. '.cpp'
  mep.language_ui_write_file(path, table.concat(lines, '\n') .. '\n')
  return path
end
function mep.cpp_ui_eval_block()
  local st = active()
  if not st then mep.notify('Open C++ UI mode first', 'warn') return end
  if mep.current_buffer() ~= st.src_buf then mep.notify('Send code from the source pane', 'warn') return end
  local path = source_snapshot(st)
  local row = mep.cursor()
  send(st, ':evalat ' .. row .. ' ' .. path)
end
function mep.cpp_ui_run(st, fname)
  if not st.cpp then return false end
  -- Submit the live source buffer, including unsaved edits.
  local path = source_snapshot(st)
  send(st, ':load-auto ' .. path)
  return true
end
mep.language_ui_eval_handlers.c = mep.cpp_ui_eval_block
mep.command('MepCppInspect', function() local name = mep.help_symbol_at_cursor('[%w_:]'); if name then mep.cpp_ui_inspect(name) end end)
mep.command('MepCppRestart', function()
  if not active() then return end
  mep.language_ui_toggle(); mep.language_ui_toggle()
end)
-- Stable read-only session information for diagnostics and the live regression check.
function mep.cpp_ui_info()
  local st = active()
  if not st then return nil end
  return {directory=st.session_dir, console=st.term_buf, source=st.src_buf,
    objects=st.cpp.objects, inspect=st.cpp.inspect, preview=st.cpp.preview,
    figures=st.cpp.gallery and #st.cpp.gallery.figures or 0, status=st.cpp.status}
end
)MEPLUA";
  return chunk.c_str();
}
