#pragma once

// The built-in reference manager, loaded once per editor.
constexpr const char *kBuiltinReferenceManager = R"MEPREF(
local ref = {}
mep.references = ref

local manager, sidebar, visible
local refresh

local function warn(message)
  mep.notify(message, 'warn')
end

local function sorted_keys(t)
  local keys = {}
  for key in pairs(t) do keys[#keys + 1] = key end
  table.sort(keys)
  return keys
end

local function trim(text)
  return text:match('^%s*(.-)%s*$')
end

local function dirname(path)
  return path:match('^(.*)/[^/]*$') or '.'
end

local function basename(path)
  return path:match('[^/]+$') or path
end

local function resolve(path, base)
  if path:sub(1, 2) == '~/' then
    local home = os.getenv('HOME') or os.getenv('USERPROFILE')
    if not home then return end
    path = home .. path:sub(2)
  elseif not path:match('^[/\\]') and not path:match('^%a:[/\\]') then
    path = base .. '/' .. path
  end
  return mep.ref_path(path)
end

local function project_root()
  local root = mep.workspace_current().root
  if root == '' then root = mep.getcwd() end
  if root ~= '' then return mep.ref_path(root) end
end

local function library_dir()
  local dir = mep.ref_data_dir()
  if dir ~= '' then return dir .. '/references' end
end

local function read(path)
  if not path then return '' end
  local resolved, kind = mep.ref_path(path)
  if kind == 'missing' then return '' end
  if kind ~= 'file' then return nil, 'Cannot read ' .. path end
  local file, err = io.open(resolved, 'rb')
  if not file then return nil, err end
  local text, read_err = file:read('*a')
  file:close()
  return text, read_err
end

local function write(path, before, after)
  local resolved = mep.ref_path(path)
  if not resolved or not mep.fs_mkdirp(dirname(resolved)) then
    warn('Cannot create the library directory for ' .. path)
    return false
  end
  local ok, err = mep.ref_write(resolved, before, after)
  if not ok then warn(err) end
  return ok
end

-- The settings are MEPML metadata lines and are read as data.
local function read_config(dir)
  local config = {folders = {}, collections = {}}
  local text, err = read(dir and (dir .. '/config.mepml'))
  if not text then return nil, err end
  local keys
  for line in (text .. '\n'):gmatch('(.-)\n') do
    local field, value = line:match('^//%? Reference ([%a]+): ([^\r\n]*)\r?$')
    if field == 'folder' and value ~= '' then
      config.folders[value] = true
    elseif field == 'collection' and value ~= '' then
      keys = config.collections[value] or {}
      config.collections[value] = keys
    elseif field == 'key' and keys and value ~= '' then
      keys[value] = true
    end
  end
  return config, text
end

local function config_text(config)
  local lines = {}
  for _, path in ipairs(sorted_keys(config.folders)) do
    lines[#lines + 1] = '//? Reference folder: ' .. path
  end
  for _, name in ipairs(sorted_keys(config.collections)) do
    lines[#lines + 1] = '//? Reference collection: ' .. name
    for _, key in ipairs(sorted_keys(config.collections[name])) do
      lines[#lines + 1] = '//? Reference key: ' .. key
    end
  end
  return #lines > 0 and (table.concat(lines, '\n') .. '\n') or ''
end

local function is_bib(name)
  return name:lower():sub(-4) == '.bib'
end

local function source_entries(path, target, entries, text)
  for _, entry in ipairs(entries) do
    entry.source, entry.target, entry.snapshot = path, target, text
    local file = entry.fields.file
    if file and file ~= '' then entry.file = resolve(file, dirname(path)) end
  end
  return entries
end

local function append_entries(destination, entries)
  for _, entry in ipairs(entries) do destination[#destination + 1] = entry end
end

local function read_mepml(path)
  local text, err = read(path)
  if not text then warn(err) return {} end
  return source_entries(path, path, mep.mepml_citations(text), text)
end

local function bib_files(dir)
  local paths = {}
  if dir then
    for _, item in ipairs(mep.list_dir(dir)) do
      if not item.is_dir and is_bib(item.name) then paths[#paths + 1] = dir .. '/' .. item.name end
    end
  end
  table.sort(paths)
  return paths
end

local function unique(entries)
  local list, by_key = {}, {}
  for _, entry in ipairs(entries) do
    if not by_key[entry.key] then
      by_key[entry.key] = entry
      list[#list + 1] = entry
    end
  end
  return list, by_key
end

local function file_key(path, used)
  local title = basename(path):gsub('%.[^.]+$', '')
  if title == '' then title = basename(path) end
  local stem = title:lower():gsub('[^%w]+', '-'):gsub('^-', ''):gsub('-$', '')
  if stem == '' then stem = 'reference' end
  local key, n = stem, 2
  while used[key] do
    key = stem .. '-' .. n
    n = n + 1
  end
  return key, title
end

local function is_pdf(path)
  return path:lower():sub(-4) == '.pdf'
end

local function folder_prefix(folder)
  return folder:gsub('/+$', '') .. '/'
end

local function load_library(root, dir)
  local project_path = root and (root .. '/citations.mepml')
  local global_path = dir and (dir .. '/library.mepml')
  local config, err = read_config(dir)
  if not config then
    warn(err)
    config = {folders = {}, collections = {}}
  end

  local project = read_mepml(project_path)
  local global = read_mepml(global_path)
  local texts, files, folders = {}, {}, {}
  local function queue(paths, target, list)
    for _, path in ipairs(paths) do
      local text, problem = read(path)
      if text then
        texts[#texts + 1] = text
        files[#files + 1] = {path = path, target = target, list = list}
      else
        warn(problem)
      end
    end
  end
  queue(bib_files(root), project_path, project)
  local folder_names = sorted_keys(config.folders)
  for _, folder in ipairs(folder_names) do
    folders[folder] = {}
    queue(bib_files(folder), global_path, folders[folder])
  end
  -- Parse the .bib files in one pass so @string and crossref can span files.
  for _, entry in ipairs(mep.org_bib_parse_files(texts, true)) do
    local source = files[entry.file_index]
    source_entries(source.path, source.target, {entry})
    source.list[#source.list + 1] = entry
  end

  local entries = {}
  append_entries(entries, project)
  append_entries(entries, global)
  for _, folder in ipairs(folder_names) do append_entries(entries, folders[folder]) end
  local _, used = unique(entries)
  local by_file = {}
  for _, entry in ipairs(entries) do
    if entry.file and not by_file[entry.file] then by_file[entry.file] = entry end
  end
  for _, folder in ipairs(folder_names) do
    local listed = {}
    for _, entry in ipairs(folders[folder]) do
      if entry.file then listed[entry.file] = true end
    end
    for _, item in ipairs(mep.list_dir(folder)) do
      if not item.is_dir and not is_bib(item.name) then
        local path, kind = mep.ref_path(folder .. '/' .. item.name)
        if kind == 'file' and not listed[path] then
          local entry = by_file[path]
          if not entry then
            local key, title = file_key(path, used)
            entry = {key = key, fields = {title = title, file = path}, file = path,
                     source = folder, target = global_path, stub = true}
            used[key], by_file[path] = entry, entry
            entries[#entries + 1] = entry
          end
          folders[folder][#folders[folder] + 1] = entry
          listed[path] = true
        end
      end
    end
    local prefix = folder_prefix(folder)
    for _, path in ipairs(sorted_keys(by_file)) do
      if not listed[path] and path:sub(1, #prefix) == prefix and is_pdf(path) then
        folders[folder][#folders[folder] + 1] = by_file[path]
      end
    end
    folders[folder] = unique(folders[folder])
  end

  local all, by_key = unique(entries)
  local collections = {}
  for name, keys in pairs(config.collections) do
    local collection = {}
    for _, key in ipairs(sorted_keys(keys)) do
      if by_key[key] then collection[#collection + 1] = by_key[key] end
    end
    collections[name] = collection
  end
  return {all = all, project = unique(project), global = unique(global), by_key = by_key,
          folders = folders, collections = collections, by_file = by_file,
          project_path = project_path, global_path = global_path}
end

local function key_ok(key)
  return key ~= '' and not key:find('[%c%s,\\(){}]')
end

local function citation_text(entry)
  if not key_ok(entry.key) then return nil, 'Invalid citation key: ' .. entry.key end
  local fields = {}
  for name, value in pairs(entry.fields) do fields[name] = value end
  if entry.file then fields.file = entry.file end
  if entry.type and entry.type ~= '' then fields.type = fields.type or entry.type end
  local lines = {'\\citation(' .. entry.key}
  for _, name in ipairs(sorted_keys(fields)) do
    if not name:match('^[%w_%-]+$') or fields[name]:find('%z') then
      return nil, 'Invalid citation field: ' .. name
    end
    lines[#lines + 1] = '  ' .. name .. ' = {' .. fields[name] .. '}'
  end
  local text = table.concat(lines, ',\n') .. ')'
  local parsed = mep.mepml_citations(text)
  if #parsed ~= 1 or parsed[1].key ~= entry.key then
    return nil, 'The citation contains an unbalanced field'
  end
  for name, value in pairs(fields) do
    if parsed[1].fields[name] ~= value:gsub('\r\n', '\n') then
      return nil, 'Cannot store citation field: ' .. name
    end
  end
  return text
end

local function save_entry(path, entry, operation)
  if not path then warn('No writable reference library is available') return false end
  local before, read_err = read(path)
  if not before then warn(read_err) return false end
  if operation == 'edit' and before ~= entry.snapshot then
    warn('Library changed on disk. Refresh and retry the edit.')
    return false
  end
  local existing
  for _, item in ipairs(mep.mepml_citations(before)) do
    if item.key == entry.key then existing = item break end
  end
  if existing and operation ~= 'edit' then
    if operation == 'insert' then return true end
    warn('That citation key already exists in the destination library')
    return false
  end

  local text, err = citation_text(entry)
  if not text then warn(err) return false end
  local eol = before:find('\r\n', 1, true) and '\r\n' or '\n'
  text = text:gsub('\r?\n', eol)
  local after
  if existing then
    local start = 1
    for _ = 2, existing.first do start = assert(before:find('\n', start, true)) + 1 end
    local finish = start
    for _ = existing.first, existing.last do
      finish = (before:find('\n', finish, true) or #before) + 1
    end
    after = before:sub(1, start - 1) .. text .. eol .. before:sub(finish)
  else
    local separator = before ~= '' and before:sub(-1) ~= '\n' and eol or ''
    after = before .. separator .. text .. eol
    local found = false
    for _, item in ipairs(mep.mepml_citations(after)) do
      if item.key == entry.key then found = true break end
    end
    if not found then
      warn('The new citation could not be parsed in ' .. path)
      return false
    end
  end
  return write(path, before, after)
end

local function one_line(text)
  return (text:gsub('%s+', ' '))
end

local function label(entry)
  local text = entry.key .. '  ' .. (entry.fields.title or '')
  if entry.fields.author then text = text .. '  ' .. entry.fields.author end
  if entry.fields.year then text = text .. '  ' .. entry.fields.year end
  return one_line(text)
end

local function preview(entry)
  local lines = {entry.key, entry.stub and ('Folder: ' .. entry.source) or ('Source: ' .. entry.source)}
  if entry.type and entry.type ~= '' then lines[#lines + 1] = 'type = ' .. entry.type end
  for _, name in ipairs(sorted_keys(entry.fields)) do
    local value = name == 'file' and entry.file or entry.fields[name]
    lines[#lines + 1] = name .. ' = ' .. value
  end
  return table.concat(lines, '\n')
end

local function pick_entry(title, entries, on_select)
  if #entries == 0 then warn('No references found') return end
  local items = {}
  for i, entry in ipairs(entries) do items[i] = {display = label(entry), data = tostring(i)} end
  local function selected(index)
    return entries[tonumber(index)]
  end
  local function show(index)
    local entry = selected(index)
    mep.picker_set_preview(entry and preview(entry) or '')
  end
  mep.picker_open(title, items, function(index)
    local entry = selected(index)
    if entry then on_select(entry) end
  end, function() show(mep.picker_selected_data()) end, nil, show)
  show('1')
end

function ref.insert()
  if not mep.ref_can_insert() then warn('Choose an editable text buffer to insert a citation') return end
  local root = project_root()
  if not root then warn('No project directory for citations.mepml') return end
  local library = load_library(root, library_dir())
  local buffer, pane = mep.current_buffer(), mep.current_pane_id()
  local tex = mep.filename():lower():sub(-4) == '.tex'
  pick_entry('Insert citation', library.all, function(entry)
    if mep.current_buffer() ~= buffer or mep.current_pane_id() ~= pane or project_root() ~= root then
      warn('The citation destination changed')
      return
    end
    if not key_ok(entry.key) or (tex and entry.key:find('[%%#]')) then
      warn('Invalid citation key: ' .. entry.key)
      return
    end
    if save_entry(library.project_path, entry, 'insert') then
      mep.insert_text(tex and ('\\cite{' .. entry.key .. '}') or ('\\cite(' .. entry.key .. ')'))
      if visible then refresh() end
    end
  end)
end

local manager_buffers = {}
local group, name = 'all', nil
local displayed_entries = {}

local function manage_config(change)
  local dir = library_dir()
  if not dir then warn('The global reference directory is unavailable') return end
  local config, before = read_config(dir)
  if not config then warn(before) return end
  if change(config) and write(dir .. '/config.mepml', before, config_text(config)) then
    refresh()
    return dir
  end
end

local function input_name(title, initial, done)
  mep.ui_input(title, initial, function(value)
    if not value then return end
    value = trim(value)
    if value == '' or value:find('%c') then warn('Enter a nonempty name') return end
    done(value)
  end)
end

function ref.collection_add()
  input_name('New collection', '', function(value)
    manage_config(function(config)
      if config.collections[value] then warn('That collection already exists') return end
      config.collections[value] = {}
      return true
    end)
  end)
end

local function pick_collection(title, done)
  local config, err = read_config(library_dir())
  if not config then warn(err) return end
  local items = sorted_keys(config.collections)
  if #items == 0 then warn('No user collections') return end
  mep.picker_open(title, items, function(value) if value then done(value) end end)
end

function ref.collection_rename()
  pick_collection('Rename collection', function(old)
    input_name('New collection name', old, function(value)
      if old == value then return end
      manage_config(function(config)
        if not config.collections[old] then warn('Collection no longer exists') return end
        if config.collections[value] then warn('That collection already exists') return end
        config.collections[value] = config.collections[old]
        config.collections[old] = nil
        return true
      end)
    end)
  end)
end

function ref.collection_remove()
  pick_collection('Remove collection', function(old)
    manage_config(function(config)
      if not config.collections[old] then return end
      config.collections[old] = nil
      return true
    end)
  end)
end

local function change_membership(remove)
  pick_collection(remove and 'Remove from collection' or 'Add to collection', function(collection)
    local function change(key)
      manage_config(function(config)
        local keys = config.collections[collection]
        if not keys then warn('Collection no longer exists') return end
        keys[key] = not remove or nil
        return true
      end)
    end
    if remove then
      local config, err = read_config(library_dir())
      if not config then warn(err) return end
      local keys = sorted_keys(config.collections[collection] or {})
      if #keys == 0 then warn('The collection is empty') return end
      mep.picker_open('Remove reference', keys, function(key) if key then change(key) end end)
    else
      local library = load_library(project_root(), library_dir())
      pick_entry('Add reference', library.all, function(entry) change(entry.key) end)
    end
  end)
end

local pdf_imports = {}

local function pdf_entry(library, path, metadata)
  local key, filename_title = file_key(path, library.by_key)
  local stub = library.by_file[path]
  if stub and stub.stub then key = stub.key end
  local fields = {file = path, medium = 'PDF'}
  for field, value in pairs(metadata) do
    value = trim(value:gsub('%s+', ' '))
    if value ~= '' and not value:find('%c') then fields[field] = value end
  end
  local title = fields.title and fields.title:lower()
  if title == 'untitled' or title == 'unknown' or title == 'document' or
     (title and (title:match('%.pdf$') or title:match('^microsoft word %-'))) then
    fields.title = nil
  end
  fields.title_source = fields.title and 'pdf-metadata' or 'filename'
  fields.title = fields.title or filename_title
  if fields.date then fields.year = fields.date:match('^(%d%d%d%d)%f[%D]') end
  local entry = {key = key, fields = fields}
  local text = citation_text(entry)
  if not text then
    entry.fields = {file = path, medium = 'PDF', title = filename_title, title_source = 'filename'}
    text = citation_text(entry)
  end
  if text then library.by_key[key] = entry end
  return text
end

local function import_pdfs(folder, root, dir)
  if pdf_imports[folder] then return end
  local prefix = folder_prefix(folder)
  local known = {}
  for path, entry in pairs(load_library(root, dir).by_file) do
    if path:sub(1, #prefix) == prefix and is_pdf(path) and not entry.stub then
      known[#known + 1] = path
    end
  end
  pdf_imports[folder] = mep.ref_scan_pdfs(folder, known, function(results)
    pdf_imports[folder] = nil
    local config, config_error = read_config(dir)
    if not config then warn(config_error) return end
    if not config.folders[folder] then return end
    if not results then warn('Could not scan PDFs in ' .. folder) return end
    if #results == 0 then return end

    local path = dir .. '/library.mepml'
    local before, err = read(path)
    if not before then warn(err) return end
    local library = load_library(root, dir)
    local additions, skipped = {}, 0
    for _, row in ipairs(results) do
      local current = library.by_file[row.path]
      if not current or current.stub then
        local actual, kind = mep.ref_path(row.path)
        if actual == row.path and kind == 'file' then
          local text = pdf_entry(library, row.path, row.fields)
          if text then additions[#additions + 1] = text
          else skipped = skipped + 1 end
        end
      end
    end
    if #additions > 0 then
      local eol = before:find('\r\n', 1, true) and '\r\n' or '\n'
      local separator = before ~= '' and before:sub(-1) ~= '\n' and eol or ''
      local after = before .. separator .. table.concat(additions, '\n'):gsub('\n', eol) .. eol
      local parsed = mep.mepml_citations(after)
      if #parsed ~= #mep.mepml_citations(before) + #additions then
        warn('Could not append PDF citations to ' .. path)
        return
      end
      if not write(path, before, after) then return end
      mep.notify('Imported ' .. #additions .. ' PDF references from ' .. basename(folder))
      refresh()
    end
    if skipped > 0 then warn('Could not store ' .. skipped .. ' PDF references from ' .. folder) end
  end)
end

local function scan_subscriptions()
  local dir = library_dir()
  if not dir then return end
  local config, err = read_config(dir)
  if not config then warn(err) return end
  local root = project_root()
  for _, folder in ipairs(sorted_keys(config.folders)) do
    local _, kind = mep.ref_path(folder)
    if kind == 'directory' then import_pdfs(folder, root, dir) end
  end
end

local function subscribe(path)
  local folder, kind = mep.ref_path(path)
  if kind ~= 'directory' then warn('Folder not found: ' .. path) return end
  local dir = manage_config(function(config)
    if config.folders[folder] then warn('Folder is already subscribed') return end
    config.folders[folder] = true
    return true
  end)
  if dir then import_pdfs(folder, project_root(), dir) end
end

local function browse_subscription(dir, selected)
  local items, rows = {}, {}
  local function add(label, kind, path)
    local index = #items + 1
    items[index] = {display = label, data = tostring(index)}
    rows[index] = {kind = kind, path = path}
  end
  local function child(path, name)
    return path:gsub('/+$', '') .. '/' .. name
  end
  local function entry_label(entry)
    return entry.is_dir and (entry.name .. '/') or entry.name
  end
  local contents = mep.list_dir(dir)
  add('Subscribe to this folder', 'subscribe', dir)
  local parent, parent_kind = mep.ref_path(child(dir, '..'))
  if parent_kind == 'directory' and parent ~= dir then add('../', 'directory', parent) end
  add('Type a folder path…', 'path', dir)
  for _, entry in ipairs(contents) do
    add(entry_label(entry), entry.is_dir and 'directory' or 'file', child(dir, entry.name))
  end

  local last_preview
  local function preview(index)
    if last_preview == index then return end
    last_preview = index
    local row = rows[tonumber(index)]
    if not row then mep.picker_set_preview('') return end
    if row.kind == 'file' then
      local path, kind = mep.ref_path(row.path)
      if kind == 'file' then mep.picker_preview_file(path, 100, true)
      else mep.picker_set_preview('Preview unavailable: ' .. row.path) end
      return
    end
    local entries = row.path == dir and contents or mep.list_dir(row.path)
    local lines = {row.path, ''}
    for i = 1, math.min(#entries, 60) do lines[#lines + 1] = entry_label(entries[i]) end
    if #entries == 0 then lines[#lines + 1] = 'No folder contents' end
    if #entries > 60 then lines[#lines + 1] = '…' end
    mep.picker_set_preview(table.concat(lines, '\n'))
  end

  mep.picker_open(dir, items, function(index)
    local row = rows[tonumber(index)]
    if not row then return end
    if row.kind == 'subscribe' then
      subscribe(dir)
    elseif row.kind == 'directory' then
      local path, kind = mep.ref_path(row.path)
      if kind == 'directory' then browse_subscription(path)
      else
        warn('Folder not found: ' .. row.path)
        browse_subscription(dir)
      end
    elseif row.kind == 'path' then
      mep.ui_input('Folder path', '', function(value)
        if not value then browse_subscription(dir) return end
        local path, kind = resolve(trim(value), dir)
        if kind == 'directory' then browse_subscription(path)
        else
          warn('Folder not found: ' .. value)
          browse_subscription(dir)
        end
      end)
    else
      browse_subscription(dir, tonumber(index))
    end
  end, function() preview(mep.picker_selected_data()) end, function(key)
    if key == 'a' then
      mep.picker_close()
      subscribe(dir)
    end
  end, preview)
  mep.picker_set_hint('Ctrl+A: subscribe here')
  if selected then mep.picker_set_selected(selected) end
  preview(mep.picker_selected_data())
end

function ref.subscribe()
  local dir = project_root()
  if not dir then warn('No folder available to browse') return end
  browse_subscription(dir)
end

function ref.unsubscribe()
  local config, err = read_config(library_dir())
  if not config then warn(err) return end
  local folders = sorted_keys(config.folders)
  if #folders == 0 then warn('No subscribed folders') return end
  mep.picker_open('Unsubscribe from folder', folders, function(path)
    if not path then return end
    manage_config(function(current)
      current.folders[path] = nil
      return true
    end)
  end)
end

function ref.add_file()
  local root, dir = project_root(), library_dir()
  if not dir then warn('The global reference directory is unavailable') return end
  input_name('Add reference file', '', function(value)
    local path, kind = resolve(value, root or mep.getcwd())
    if kind ~= 'file' then warn('File not found: ' .. value) return end
    local library = load_library(root, dir)
    for _, entry in ipairs(library.global) do
      if entry.file == path then warn('That file is already in the global library') return end
    end
    local key, title = file_key(path, library.by_key)
    for _, entry in ipairs(library.all) do
      if entry.stub and entry.file == path then key, title = entry.key, entry.fields.title break end
    end
    input_name('Citation key', key, function(new_key)
      local entry = {key = new_key, fields = {title = title, file = path}, file = path}
      if save_entry(library.global_path, entry, 'add') then refresh() end
    end)
  end)
end

local function edit(original)
  local entry = {}
  for key, value in pairs(original) do entry[key] = value end
  entry.fields = {}
  for key, value in pairs(original.fields) do entry.fields[key] = value end
  local title = entry.stub and ('Promote ' .. entry.key) or ('Edit ' .. entry.key)
  local fields = {title = '', author = '', year = '', file = '', type = entry.type or ''}
  for key, value in pairs(entry.fields) do fields[key] = value end
  if entry.file then fields.file = entry.file end
  local choices = {{display = 'Add field', data = '+'}}
  if entry.stub or not entry.snapshot then
    choices[#choices + 1] = {display = 'Save to library', data = '='}
  end
  for _, field in ipairs(sorted_keys(fields)) do
    choices[#choices + 1] = {display = field .. '  ' .. one_line(fields[field]), data = field}
  end
  local function save()
    if save_entry(entry.target, entry, entry.snapshot and 'edit' or 'add') then refresh() end
  end
  local function edit_field(field)
    if not field:match('^[%w_%-]+$') then warn('Invalid field name') return end
    mep.ui_input(field, fields[field] or '', function(value)
      if value == nil then return end
      if field == 'file' then
        local path, kind
        if value ~= '' then
          path, kind = resolve(value, dirname(entry.source))
          if kind ~= 'file' then warn('File not found: ' .. value) return end
        end
        entry.file = path
        value = path or ''
      elseif field == 'type' then
        entry.type = value
      end
      entry.fields[field] = value ~= '' and value or nil
      save()
    end)
  end
  mep.picker_open(title, choices, function(field)
    if field == '+' then
      input_name('Field name', '', function(value) edit_field(value:lower()) end)
    elseif field == '=' then
      save()
    elseif field then
      edit_field(field)
    end
  end)
end

local function shown(library)
  if group == 'folder' then return library.folders[name] or {} end
  if group == 'collection' then return library.collections[name] or {} end
  return library[group]
end

function ref.edit()
  local library = load_library(project_root(), library_dir())
  pick_entry('Edit or promote reference', visible and shown(library) or library.all, edit)
end

local function open_entry(entry)
  if not entry.file then
    pick_entry('Reference details', {entry}, edit)
    return
  end
  local path, kind = mep.ref_path(entry.file)
  if kind ~= 'file' then warn('File not found: ' .. entry.file) return end
  mep.ref_open_file(path)
end

local function render(library)
  local function choice(label_text, kind, value)
    return {text = label_text, current = group == kind and name == value, on_click = function()
      group, name = kind, value
      render(library)
    end}
  end
  local groups = {{id = 'libraries', title = 'Libraries', widgets = {
    choice('All references', 'all'), choice('Project', 'project'), choice('Global library', 'global'),
  }}}
  local collections, folders = {}, {}
  for _, collection in ipairs(sorted_keys(library.collections)) do
    collections[#collections + 1] = choice(collection, 'collection', collection)
  end
  for _, folder in ipairs(sorted_keys(library.folders)) do
    local row = choice(basename(folder), 'folder', folder)
    row.tooltip = folder
    folders[#folders + 1] = row
  end
  groups[#groups + 1] = {id = 'collections', title = 'Collections', widgets = collections}
  groups[#groups + 1] = {id = 'folders', title = 'Subscribed folders', widgets = folders}
  mep.sidebar_set_sections(sidebar, groups)

  displayed_entries = shown(library)
  local widgets = {}
  for _, entry in ipairs(displayed_entries) do
    widgets[#widgets + 1] = {
      id = entry.key, text = label(entry), tooltip = preview(entry),
      trailing_icon = ' ✎ ', trailing_on_click = function() edit(entry) end,
      on_click = function() open_entry(entry) end,
    }
  end
  if #widgets == 0 then widgets[1] = {text = 'No references'} end
  mep.sidebar_set_sections(manager, {
    {id = 'actions', widgets = {
      {text = 'Add file', on_click = ref.add_file},
      {text = 'Edit or promote reference', on_click = ref.edit},
      {text = 'Manage collections and folders', on_click = ref.collections},
      {text = 'Refresh', on_click = ref.refresh},
    }},
    {id = 'entries', title = name or ({all = 'All references', project = 'Project', global = 'Global library'})[group],
     widgets = widgets},
  })
end

function refresh()
  if not manager then return end
  local library = load_library(project_root(), library_dir())
  if (group == 'folder' and not library.folders[name]) or
     (group == 'collection' and not library.collections[name]) then
    group, name = 'all', nil
  end
  render(library)
end
function ref.refresh()
  refresh()
  scan_subscriptions()
end

function ref.collections()
  local actions = {
    {'Add collection', ref.collection_add}, {'Rename collection', ref.collection_rename},
    {'Remove collection', ref.collection_remove}, {'Add reference to collection', function() change_membership(false) end},
    {'Remove reference from collection', function() change_membership(true) end},
    {'Subscribe to folder', ref.subscribe}, {'Unsubscribe from folder', ref.unsubscribe},
  }
  local items = {}
  for i, action in ipairs(actions) do items[i] = {display = action[1], data = tostring(i)} end
  mep.picker_open('Collections and folders', items, function(index)
    local action = actions[tonumber(index)]
    if action then action[2]() end
  end)
end

function ref.open()
  if not project_root() and not library_dir() then warn('Reference libraries require a native filesystem') return end
  if not manager then
    manager = mep.sidebar_create('Reference Manager', 'left')
    mep.sidebar_set_on_key(manager, function(key)
      if key == '/' then pick_entry('Search references', displayed_entries, open_entry) end
    end)
    mep.sidebar_set_help(manager, {{'/', 'Search the current collection'}})
    sidebar = mep.sidebar_create('Reference Collections', 'left', mep.sidebar_default_cols(0.2))
  end
  refresh()
  mep.sidebar_open_pane(manager)
  manager_buffers[mep.current_buffer()] = true
  mep.sidebar_open(sidebar, false)
  visible = true
  scan_subscriptions()
end

mep.on_frame(function()
  if not manager then return end
  local shown_now = false
  for _, id in ipairs(mep.pane_buffers()) do
    if manager_buffers[id] then shown_now = true break end
  end
  if shown_now == visible then return end
  visible = shown_now
  if visible then
    refresh()
    mep.sidebar_open(sidebar, false)
  else
    mep.sidebar_close(sidebar)
  end
end)

mep.on_workspace_changed(function() if visible then refresh() end end)

)MEPREF";
