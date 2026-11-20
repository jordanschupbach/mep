# Reference manager plan

Status: design only. Nothing is implemented yet.

## What the request asks for

1. A reference manager that opens in its own buffer, like mep's other document modes.
2. A sidebar, shaped like the file tree, that lists collections:
   - collections the user defines (by project, by topic)
   - folders the user subscribes to (for example `~/projects/books`), each shown as a
     collection of the files inside it
3. Opening a reference that has a stored file opens that file in a pane on the right.
4. A command, usable from any buffer, that opens a fuzzy picker of citations with a
   preview of the selected one. Choosing an entry:
   - inserts an mepml citation (`\cite(key)`) at the cursor (`\cite{key}` in a .tex buffer)
   - adds the entry to the project's `citations.mepml` if it isn't there yet
5. Support for .bib files. The picker reads both mepml citations and .bib entries. When
   the same key appears more than once, the first match wins.
6. Two levels of library: one global library and one per project.

## Proposed design: global and project libraries

- **Global library**: `~/.local/share/mep/references/library.mepml`, which stores
  `\citation(...)` entries plus a `file = path` field. Its settings file
  (`config.lua` or `.mepml`) also lists the subscribed folders and the user collections
  as lists of keys.
- **Project library**: `citations.mepml` and any `*.bib` file in the project root.
- **Lookup order in the picker**: project `citations.mepml`, then project .bib files,
  then the global library, then .bib files in subscribed folders. Each entry is tagged
  with the place it came from. The first entry found for a key is the one used.
- **Inserting from the global library** copies the entry into the project's
  `citations.mepml`, so the project still builds on its own.
- **Files in subscribed folders** that have no entry yet get a stub entry. Its key comes
  from the file name, the title from the file name, and `file` holds the path. A command
  promotes the stub to a full entry.

## Existing code to reuse (`src/main.cpp`, unless a different file is named)

- `kBuiltinOrgBib` (around line 30172) already does .bib loading, the picker and the
  preview:
  - `mep.org_bib_parse_files(texts)` is the C++ BibTeX parser. It handles `@string`
    and crossref.
  - `mep.picker_open(title, items, cb)` is the fuzzy picker. Check whether it accepts a
    preview callback; if not, add one in C++.
  - `mep.hover_show` and `mep.insert_text`.
- `kBuiltinFileTree` (around line 4453) and `kBuiltinSidebarPopout` /
  `kBuiltinRightSidebarPanes` show how sidebars are built. The collections sidebar
  should copy the file tree's pattern.
- `mepml::Citation` / `BlockKind::Citation` (`src/mepml_doc.h:115` and `:312`).
  `mepml_doc.cpp` around line 1623 already parses `\citation(key, k = v)` and plain
  BibTeX entries inside .mepml files. The library file can use this parser directly.
- The mepml snippets (around line 31018) already define `\cite(key)` / `\citep(key)`.
- `mep.pane_open(path)` opens files. Check how a vertical split on the right is made
  (search `editor.h` for split or pane APIs).

## Open question

- The manager buffer could be a new C++ `Mode::RefManager` (a table of entries drawn in
  `main.cpp`), or a Lua scratch buffer built on the existing buffer and sidebar APIs.
  Lua is faster to build. Choose a C++ mode only if a richer table view is needed.

## Steps

1. Lua: the library model (load and save the global library, scan subscribed folders,
   merge the project and global sources, apply first-match order).
2. Lua: the picker command `MepRefInsert`, with a preview, the insert, and the copy into
   `citations.mepml`. Bind it globally.
3. Lua: the collections sidebar (user collections, subscribed folders, project), with
   commands to add, remove, rename and subscribe.
4. The manager buffer: list entries, edit fields, add a file, and open the file in a
   right split.
5. Documentation in `docs/` and the help files, plus tests for the merge order.
