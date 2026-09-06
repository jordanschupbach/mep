# mep agent API

What an AI agent can do to/with a running `mep` instance, and how. This is
the canonical reference; `<leader>a<CR>`'s spawned Claude Code terminal
gets a condensed version of the essentials injected automatically (see
`mep.opt.ai_terminal_instructions`, `src/main.cpp`'s `kBuiltinAiTerminal`)
so it doesn't depend on this file being present in whatever project is
active -- read this file when you want more detail than that summary, or
when working on mep's own agent-integration code.

## Architecture

`mep` (native build only) binds a Unix-domain socket per running instance
(`~/.local/share/mep/agent-sockets/<pid>.sock`), speaking Content-Length-
framed JSON-RPC 2.0 (`src/agent_rpc.h`/`.cpp`). `src/mcp_bridge.cpp`
(built as `mep-mcp`) is a small, dependency-free C++ program that speaks
the Model Context Protocol over stdio and relays every tool call onto
that socket -- register it once with:

```
claude mcp add mep-agent -- /path/to/mep/build/native/mep-mcp
```

(`mcp/server.ts` + `mcp/mep_client.ts` are an older Deno/TypeScript
implementation of the same bridge, kept as a reference/fallback; prefer
`mep-mcp` since it needs no runtime beyond the compiled binary.)

Every `mep_*` tool is a thin wrapper around one JSON-RPC method that
already exists for mep's embedded-Lua `mep.*` API -- this is a second way
to reach the same primitives, not new editor behavior.

## Ground rules

- **Orient first**: `mep_session_info` (pid, cwd, active project/
  workspace, git branch, open files) and `mep_state_dump` (every buffer's
  id/filename/modified/line-count, the full pane split-tree, which
  tab/pane is active). Do this before touching anything, and again after
  a pause or before acting on assumptions -- `mep_poll_events` returns
  everything that happened (cursor moves, buffer edits, pane/mode
  changes, notifications) since your last check, including the human's
  own activity.
- **Report status**: `mep_set_status("thinking"|"writing"|
  "awaiting_input"|"done"|"idle")` drives a badge next to your name in
  mep's tab bar -- the human's only window into what you're doing besides
  your own messages. `writing` is set for you automatically by
  `mep_buffer_insert_text`/`set_line`/`replace_lines`.
- **Edit through mep's buffer tools, not raw disk writes**, for any file
  the human has open: mep does not watch for external file changes, so a
  disk write is invisible to an open buffer and the human saving it later
  would clobber your edit. Files *not* open in mep are fine to edit with
  ordinary file tools; open them afterward with `mep_file_open` if the
  human should see them.
- **Never run destructive ex-commands** (`qa!`, `q!`, `wsdelete`,
  `projectclose`, ...) via `mep_command_run` unless explicitly asked.
- Rows/columns are 0-indexed; line ranges are `[start, end)`.

## Tool reference

### Identity & session
- `mep_identify(name)` -- rename yourself (shown at your cursor and tab-bar chip).
- `mep_set_status(status)` -- see above.
- `mep_list_participants()` -- every connected agent/human, with status.
- `mep_session_info()`, `mep_state_dump()`, `mep_poll_events()` -- see Ground rules.

### Cursor & buffers
Your cursor is your own virtual one (`mep_cursor_get`/`set`), independent
of the human's real cursor and any other participant's -- moving it or
typing through it never touches what they see.
- `mep_cursor_set(buffer_id?, row, col)`, `mep_cursor_get()`.
- `mep_buffer_insert_text(text)`, `mep_buffer_set_line(row, text)`,
  `mep_buffer_replace_lines(start, end, lines)` -- act on the buffer your
  cursor is in.
- `mep_buffer_set_lines(buffer_id, lines)` -- replace a buffer's *entire*
  content by id regardless of which pane shows it; no undo history.
- `mep_buffer_get_lines(buffer_id, start?, end?)`, `mep_buffer_list
  (workspace?)`, `mep_buffer_filename(buffer_id)`, `mep_buffer_create()`,
  `mep_buffer_switch(buffer_id)`.

### Files & panes
- `mep_file_open(path)`, `mep_file_save(path?)`.
- `mep_pane_split(dir?, file?)` -- horizontal (default) or vertical,
  optionally opening a file in the new pane; focuses it, so do this
  deliberately (e.g. to show the human something).
- `mep_pane_close()`, `mep_pane_focus(pane_id)`,
  `mep_pane_resize(direction, step?)`, `mep_pane_get(pane_id?)`,
  `mep_pane_split_with_buffer(source_pane_id, buffer_id, dest_pane_id, dir, before)`.

### Workspaces & projects
`mep_workspace_list/_switch/_create/_delete`,
`mep_project_list/_switch/_open`. A workspace is a git worktree;
switching one changes mep's working directory to it.

### Commands
`mep_command_run(cmd)` runs any `:` ex-command without the leading colon
-- the escape hatch for anything without a dedicated tool.

### UI automation: screenshots, clicks, keystrokes
Unlike everything above (which works headlessly, e.g. under the collab
relay), these need mep's actual GUI window on a real X11 display, and
drive it the way Playwright drives a browser -- real synthetic input at
the OS level (XTest), so they work on *anything* drawn in the window, not
just mep-aware widgets.

- `mep_screenshot()` -- captures the current window as a PNG, returns its
  path (read the file to see it). Coordinates for every tool below are in
  that same pixel space (window-client, origin top-left) -- screenshot
  first to find where things are.
- `mep_mouse_move(x, y)`, `mep_mouse_down(x, y, button?)`,
  `mep_mouse_up(x, y, button?)` -- low-level primitives; compose your own
  gesture with these (e.g. hold a modifier across other calls:
  `mep_key_down("Shift_L")`, click/drag, `mep_key_up("Shift_L")`).
- `mep_mouse_click(x, y, button?, clicks?)` -- move + click (clicks:2 for
  a double-click).
- `mep_mouse_drag(x1, y1, x2, y2, button?, steps?)` -- press, move
  smoothly in `steps` increments, release -- one call for a paint
  stroke, slider drag, or selection drag.
- `mep_scroll(x, y, delta)` -- wheel clicks, positive = up.
- `mep_key_press(key)` -- one keystroke: a single character ("e", "[",
  "?") or an X11 keysym name for anything else ("Escape", "Return",
  "Tab", "BackSpace", "Delete", "Left"/"Right"/"Up"/"Down", "F1".."F12",
  "Control_L", "Shift_L", "Alt_L"). Shift is applied automatically for an
  uppercase letter or shifted symbol passed directly.
- `mep_key_down(key)`/`mep_key_up(key)` -- hold/release one key across
  other calls (modifiers, or a manual key-repeat).
- `mep_type_text(text)` -- types a string one keystroke at a time
  (printable ASCII only). For editing an open buffer, `mep_buffer_
  insert_text` is far more direct -- reach for this only when you need
  real keystrokes (exercising mep's own key handling, a text field with
  no buffer-level API).

**Reliability note**: input is injected via the real X server and mep's
own per-frame event queue, so a single action can very occasionally not
land (e.g. if mep's window doesn't have focus yet, or a frame was
dropped). If a screenshot right after an action doesn't show the expected
change, don't assume it silently failed differently than usual -- just
retry the same action once and re-check.

## The in-pane image editor

Press **`e`** while viewing an image (any PNG/JPG/etc. buffer -- open one
with `mep_file_open` or `mep_pane_split`'s `file` argument) to open the
editor in that same pane. **Esc** returns to the plain image viewer
without losing anything: layers and undo history are kept per-buffer, so
pressing `e` again resumes exactly where you left off. `:w`/`:wq` (or
`mep_command_run("w")`) flattens the visible layers and writes a real PNG
(native build only -- the wasm build doesn't support this yet).

### Layout

Left to right: a two-column icon **tool sidebar**, the **canvas**, and a
**Layers** panel on the right. A thin **toolbar** (brush size, recent
colors) sits above the canvas, under the **menubar** (File/Edit/Layer/
View). The foreground/background color swatches are at the *bottom* of
the tool sidebar -- click one to open an HSV picker.

The pane's status bar (bottom) shows the active tool, brush size, zoom
%, canvas size, the cursor's live canvas-pixel position while hovering,
the active layer's name, and `[selection]` when one exists.

### Tools (hotkey -- click the sidebar icon, or use the hotkey directly)

| Key | Tool | What dragging does |
| --- | --- | --- |
| `b` | Pencil | Freehand stroke in the foreground color, `brush_size` wide. |
| `x` | Eraser | Freehand stroke to transparent. |
| `l` | Line | Straight line from press to release. |
| `r` | Rectangle | Outline (Shift+release = filled). |
| `c` | Ellipse | Outline (Shift+release = filled), bounded by the drag box. |
| `f` | Bucket | Click only -- 4-connected flood fill from that pixel. |
| `i` | Eyedropper | Click only -- samples the composited color into the foreground swatch. |
| `h` | Pan | Drag to scroll the canvas (also: middle-mouse drag with any tool, Ctrl+scroll or `+`/`-`/`=` to zoom). |
| `m` | Rectangle select | Drags out a rectangular selection. |
| `o` | Ellipse select | Drags out an elliptical selection. |
| `w` | Lasso | Freehand selection -- trace with the drag, released path is closed automatically. |
| `v` | Move | Drags the selection's content (or the *whole active layer*, if no selection) to a new position -- cuts from the old spot, leaving transparency behind. |

With any selection active: **Delete**/**BackSpace** clears its pixels
(the selection itself stays, ready to move/fill again); **Edit > Deselect**
drops the selection outline entirely. `[`/`]` shrink/grow the brush.
`u`/Ctrl-R undo/redo (one entry per completed stroke/shape/fill/move, not
per intermediate frame).

### Driving it as an agent: worked example

To draw something recognizable, work out the shape in **canvas-pixel**
coordinates, then convert to **screen** coordinates for your
`mep_mouse_*` calls:

```
screen_x = canvas_rect_x + pixel_x * zoom
screen_y = canvas_rect_y + pixel_y * zoom
```

`canvas_rect_x/y` (the canvas's top-left corner on screen) and `zoom`
(shown in the status bar, e.g. "100%" = 1.0) are easiest to work out by
taking one `mep_screenshot` right after opening the editor and reading
the pixel grid/ruler and status bar directly from the image, rather than
computing pane geometry from scratch.

A minimal drawing session:
1. `mep_file_open` an image (or `mep_pane_split` with `file:`), then
   `mep_mouse_click` somewhere inside that pane (X11 keyboard focus needs
   a real click, not just a pointer move) and `mep_key_press("e")`.
2. `mep_screenshot` to see the editor's layout and work out canvas
   coordinates.
3. Pick a tool: `mep_key_press` with its hotkey letter is more reliable
   than clicking the tiny sidebar icon (no coordinate math needed).
4. Prefer a handful of large `mep_mouse_drag` shapes (Line/Rectangle/
   Ellipse are exact) over many tiny Pencil strokes for anything
   geometric; use Pencil freehand only for short details. Bucket-fill
   closed outlines rather than dragging a filled shape when the outline
   is already the boundary you want filled.
5. Use `mep_screenshot` again after a few strokes to check progress
   before continuing -- coordinates are easy to get slightly wrong on
   the first try.
6. `Layer > New Layer` (menu, or `mep_command_run` can't reach menu
   items -- click "Layer" then "New Layer", or drive it entirely via
   separate Pencil/shape strokes on the one layer if that's simpler) to
   keep distinct parts of a drawing separable, if you expect to redo one
   part.
7. `mep_command_run("w")` to save when done.
