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

Every `mep_*` tool is a thin wrapper around one JSON-RPC method
(`src/mcp_bridge.cpp`'s `kTools` table, buffer_ids visible in the payload
below match that table's `rpc_method` field) -- this is a second way to
reach the same primitives, not new editor behavior. A handful of newer
methods (the 3D modeler's `model.*` and the image editor's `image.*`,
covered in their own sections below) don't yet have every field of this
document's worked examples memorized by heart the way the older buffer/
pane tools do -- when in doubt, `mep_model_primitive_info`/`mep_image_info`
and this file are the source of truth, not assumption.

**Writing your own raw-socket batch script** (bypassing `mep-mcp`, e.g.
to build a large scene without one subprocess per call): the agent
socket can interleave server-initiated event notifications with RPC
responses on the same connection. A client that assumes "the next
Content-Length frame I read back is the response to the request I just
sent" can desync -- match each response's `"id"` field against the id
you actually sent, and skip/requeue anything that doesn't match, rather
than consuming it as if it were your answer. This bit a real batch
script while building a 32-piece chess set (CHESS_SET_BENCHMARK_PLAN.md
Phase 7) -- a response got silently swapped for an unrelated
notification frame, and the fix was exactly this id-matching loop.
`mep-mcp` and any one-shot script that opens a fresh connection per call
are unaffected (nothing to interleave with on a connection used for
exactly one request/response); this only matters for a script that
opens one persistent connection and fires many requests over it.

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
  human should see them. The 3D modeler and image editor are the
  exception -- `model.*`/`image.*` buffers are always edited through
  their own RPC methods regardless, there's no raw-file equivalent.
- **Never run destructive ex-commands** (`qa!`, `q!`, `wsdelete`,
  `projectclose`, ...) via `mep_command_run` unless explicitly asked.
- Rows/columns are 0-indexed; line ranges are `[start, end)`.

## Developing mep itself

If your task is changing mep's own code (this repo), you are almost
certainly running inside the very instance you'd want to rebuild and
test: an agent terminal's shell, and anything its Bash tool starts
without detaching it, is a child process in that mep instance's own
process group (`Editor::TerminalSpawn`). Closing/quitting/restarting
that instance -- `:qa!`, `:q`, the human closing the window, or a shell
`kill`/`pkill` that happens to target its pid -- sends SIGTERM then
SIGKILL to its *whole process group* on shutdown (`JobManager::
ShutdownAll`), which takes the agent's own session down with it, not
just the editor. `mep_session_info`'s `pid` field (or the `<pid>.sock`
filename in the agent's own `MEP_AGENT_SOCKET`) identifies that pid --
never target it, and never run a destructive quit against the
containing instance while developing mep, even indirectly (a blanket
`pkill mep` included).

To test a change, build then launch a separate, detached instance
rather than restarting the containing one:

```
setsid ./build/native/mep [path] </dev/null >/tmp/mep-test.log 2>&1 &
```

via the agent's own Bash tool -- not mep's own `:terminal`/
`mep_command_run("terminal ...")`, which would just make the new
instance another child of the containing one. `setsid` puts it in its
own session so it survives even if the containing instance later
closes. It binds its own `<new-pid>.sock` automatically (see
Architecture above); drive it with its own `mep_*` tools via a fresh
MCP connection pointed at `MEP_AGENT_SOCKET=<that path>`, or a raw
JSON-RPC script (see "Writing your own raw-socket batch script"
above), independently of whatever socket the agent's own tools are
already wired to. When done testing, kill only that spawned pid --
never anything upstream of the agent's own session.

This is also why several independent agents are safe to run at once,
each in its own mep window: every `mep-mcp` process is tied to exactly
one socket (`MEP_AGENT_SOCKET`, inherited from the instance that
spawned it), so concurrent agents never contend for or cross-talk over
a shared server.

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
just mep-aware widgets. This is also the *only* way to reach the image
editor's by-hand paint tools and its "Texture" menu's Fill Gradient/
Noise/Wood/Marble/Blur dialogs -- there's no buffer-level equivalent for
those, unlike text editing (see "The in-pane image editor" below for
`mep_image_*`, the headless alternative for procedural fills).

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
  "?") or an **X11 keysym name** for anything else ("Escape", "Return"
  -- not "Enter", "Tab", "BackSpace", "Delete", "Left"/"Right"/"Up"/
  "Down", "F1".."F12", "Control_L", "Shift_L", "Alt_L"). Shift is applied
  automatically for an uppercase letter or shifted symbol passed
  directly.
- `mep_key_down(key)`/`mep_key_up(key)` -- hold/release one key across
  other calls (modifiers, or a manual key-repeat).
- `mep_type_text(text)` -- types a string one keystroke at a time
  (printable ASCII only). For editing an open buffer, `mep_buffer_
  insert_text` is far more direct -- reach for this only when you need
  real keystrokes (exercising mep's own key handling, a text field with
  no buffer-level API, e.g. a `BeginPromptNative` numeric-parameter
  prompt like the image editor's Texture menu opens).
- Hint mode (the Vimium-style link/widget labels a plain `f` opens in an
  HTML pane, or mod1+f opens for everything on screen) also has a
  focus-independent path for a raw socket client, with no XTest keystroke
  involved: `ui.hints` (`{"scope":"links"}` for the active pane's own
  visible links -- the default, the same route `f` takes -- or
  `{"scope":"all"}` for the mod1+f set) opens it, `ui.hint_targets`
  lists every `{label, x, y}` plus what has been typed so far,
  `ui.hint_pick` `{"label":"s"}` types a label (a full one fires that
  target, a partial one narrows), and `ui.hint_cancel` is Escape. These
  are not (yet) wrapped as `mep_*` MCP tools.

**Reliability note**: input is injected via the real X server and mep's
own per-frame event queue, so a single action can very occasionally not
land (e.g. if mep's window doesn't have focus yet, or a frame was
dropped). If a screenshot right after an action doesn't show the expected
change, don't assume it silently failed differently than usual -- just
retry the same action once and re-check.

## CAD and finite elements

<!-- BEGIN GENERATED cad-fem -->

The CAD kernel and finite-element solver, as 34 methods. Each one is reachable three ways, with
the name mechanically derived from the method: `part.box` is the agent-RPC
method, `mep_part_box` the MCP tool, and `mep.part_box` the Lua function. All
three go to one implementation (`src/cad_fem_api.cpp`) driven by one table
(`src/cad_fem_methods.cpp`), and this section is generated from that table --
run `just cad-fem-docs` after changing it.

Handles are integers and share one counter across documents, studies, meshes
and results, so a handle of one kind is never mistaken for another. Closing a
document closes everything built on it.

### Analysis

- `part_new(title?)` -- Opens an empty CAD document and returns its handle.
  - `title` (string, optional) -- A name for the document.
  - returns `{document}`
- `part_close(document)` -- Closes a document and everything built on it.
  - `document` (number, required) -- The document handle.
  - returns `{closed}`
- `part_list()` (read-only) -- Lists open documents, studies, meshes and results.
  - returns `{documents, studies, meshes, results}`
- `part_box(document, size, at?)` -- Adds a rectangular block.
  - `document` (number, required) -- The document handle.
  - `size` (array, required) -- Its extent, [x, y, z].
  - `at` (array, optional) -- The minimum corner, [x, y, z]. Defaults to the origin.
  - returns `{body}`
- `part_cylinder(document, radius, height, at?, axis?)` -- Adds a cylinder.
  - `document` (number, required) -- The document handle.
  - `radius` (number, required) -- Its radius.
  - `height` (number, required) -- Its height along the axis.
  - `at` (array, optional) -- The centre of the base. Defaults to the origin.
  - `axis` (array, optional) -- The axis direction. Defaults to +z.
  - returns `{body}`
- `part_sphere(document, radius, at?)` -- Adds a sphere.
  - `document` (number, required) -- The document handle.
  - `radius` (number, required) -- Its radius.
  - `at` (array, optional) -- Its centre. Defaults to the origin.
  - returns `{body}`
- `part_boolean(document, op, a, b)` -- Combines two bodies and replaces them with the result.
  - `document` (number, required) -- The document handle.
  - `op` (string, required) -- One of union, difference, intersection.
  - `a` (number, required) -- The first body.
  - `b` (number, required) -- The second body.
  - returns `{body}`
- `part_shell(document, body, thickness, open_faces?)` -- Hollows a body out to a wall thickness.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body to hollow.
  - `thickness` (number, required) -- The wall thickness.
  - `open_faces` (array, optional) -- Face handles to leave open.
  - returns `{body}`
- `part_info(document)` (read-only) -- Counts what a document holds.
  - `document` (number, required) -- The document handle.
  - returns `{bodies, faces, edges, vertices}`
- `part_faces(document, body)` (read-only) -- Lists a body's faces: a normal, and where each one actually is.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - returns `{faces: [{id, point, normal, centroid, low, high}]}`
- `part_edges(document, body, along?)` (read-only) -- Lists a body's edges, with a point, a direction and a length on each.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - `along` (array, optional) -- Keep only the edges running along this direction.
  - returns `{edges: [{id, point, direction, from, to, length}]}`
- `part_edge_at(document, body, point, along?)` (read-only) -- Finds the edge nearest a point, which is how a script names one.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - `point` (array, required) -- Look for the edge nearest here.
  - `along` (array, optional) -- Consider only edges running along this direction.
  - returns `{edge, point, direction, from, to, length, distance}`
- `part_face_at(document, body, normal)` (read-only) -- Finds the face whose outward normal is nearest a direction.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - `normal` (array, required) -- The direction to look along.
  - returns `{face, point, normal, agreement}`
- `part_mass(document, body, density?)` (read-only) -- Volume, area, centre of mass and inertia of a body.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - `density` (number, optional) -- kg/m^3. Defaults to 1, so mass equals volume.
  - returns `{volume, area, mass, centroid, inertia, principal}`
- `part_validate(document, body)` (read-only) -- Checks a body's topology and geometry.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body.
  - returns `{ok, problems}`
- `part_export(document, path, format?, body?)` -- Writes a body to STEP, STL, OBJ or glTF.
  - `document` (number, required) -- The document handle.
  - `path` (string, required) -- Where to write it.
  - `format` (string, optional) -- step, stl, obj or gltf. Defaults to the extension.
  - `body` (number, optional) -- One body, or every body if omitted.
  - returns `{path, bytes, format}`
- `part_import(path, format?)` -- Reads a STEP file, or a .mepcad feature tree, into a new document.
  - `path` (string, required) -- The file to read.
  - `format` (string, optional) -- step or mepcad. Defaults to the extension.
  - returns `{document, bodies, warnings, features, volume}`
- `fem_mesh(document, body, size?, order?, method?, layers?)` -- Meshes a body, into tetrahedra or by sweeping a profile into hexahedra.
  - `document` (number, required) -- The document handle.
  - `body` (number, required) -- The body to mesh.
  - `size` (number, optional) -- Target element size. Defaults to the model's own scale.
  - `order` (number, optional) -- 1 for Tet4, 2 for curved Tet10. Defaults to 1.
  - `method` (string, optional) -- tetrahedra (the default) or sweep, which needs a prismatic body and gives Hex8.
  - `layers` (number, optional) -- Elements along a sweep. Defaults to the sizing field.
  - returns `{mesh, nodes, elements, shape, order, worst_dihedral, target_size}`
- `fem_study(document, name?)` -- Starts a study on a document.
  - `document` (number, required) -- The document handle.
  - `name` (string, optional) -- A name for it.
  - returns `{study}`
- `fem_material(study, youngs_modulus?, poissons_ratio?, density?, thermal_expansion?, name?)` -- Sets the study's material.
  - `study` (number, required) -- The study handle.
  - `youngs_modulus` (number, optional) -- Pa. Defaults to steel.
  - `poissons_ratio` (number, optional) -- Defaults to 0.3.
  - `density` (number, optional) -- kg/m^3. Defaults to 7850.
  - `thermal_expansion` (number, optional) -- 1/K.
  - `name` (string, optional) -- A name for the material.
  - returns `{materials}`
- `fem_support(study, face?, edge?, fixed?)` -- Holds a face, or an edge -- which is what a simple support is.
  - `study` (number, required) -- The study handle.
  - `face` (number, optional) -- The face to hold.
  - `edge` (number, optional) -- The edge to hold, instead of a face.
  - `fixed` (array, optional) -- Which of x, y, z are held. Defaults to all three.
  - returns `{supports}`
- `fem_load(study, kind, face?, magnitude?, vector?)` -- Applies a load to a face, or gravity to the whole model.
  - `study` (number, required) -- The study handle.
  - `kind` (string, required) -- force, pressure, traction or gravity.
  - `face` (number, optional) -- The face, for everything but gravity.
  - `magnitude` (number, optional) -- Pa, for a pressure.
  - `vector` (array, optional) -- The force, traction or acceleration.
  - returns `{loads}`
- `fem_solve(study, mesh)` -- Runs a linear static solve.
  - `study` (number, required) -- The study handle.
  - `mesh` (number, required) -- The mesh handle.
  - returns `{result, nodes, elements, max_displacement, max_von_mises, strain_energy, equilibrium_residual, relative_error}`
- `fem_modal(study, mesh, modes?)` -- Finds natural frequencies and mode shapes.
  - `study` (number, required) -- The study handle.
  - `mesh` (number, required) -- The mesh handle.
  - `modes` (number, optional) -- How many. Defaults to 6.
  - returns `{result, frequencies, rigid_body_modes}`
- `fem_adapt(study, body, size?, target?, cycles?)` -- Meshes, solves, estimates the error and refines, repeatedly.
  - `study` (number, required) -- The study handle.
  - `body` (number, required) -- The body to mesh each cycle.
  - `size` (number, optional) -- The starting element size.
  - `target` (number, optional) -- The relative energy-norm error to aim for.
  - `cycles` (number, optional) -- How many at most. Defaults to 3.
  - returns `{result, cycles, reached_target, stopped_early, stop_reason}`
- `fem_summary(result)` (read-only) -- Everything scalar about a result.
  - `result` (number, required) -- The result handle.
  - returns `{nodes, elements, max_displacement, max_von_mises, strain_energy, relative_error}`
- `fem_field(result, field)` (read-only) -- The range of one field over a result.
  - `result` (number, required) -- The result handle.
  - `field` (string, required) -- von_mises, displacement, max_principal and so on.
  - returns `{field, min, max, min_node, max_node, at_min, at_max}`
- `fem_probe(result, field, points)` (read-only) -- Reads a field at points.
  - `result` (number, required) -- The result handle.
  - `field` (string, required) -- Which field.
  - `points` (array, required) -- An array of [x, y, z].
  - returns `{probes: [{at, value, inside, distance}]}`
- `fem_path(result, field, from, to, samples?)` (read-only) -- Samples a field along a line, for plotting.
  - `result` (number, required) -- The result handle.
  - `field` (string, required) -- Which field.
  - `from` (array, required) -- The start point.
  - `to` (array, required) -- The end point.
  - `samples` (number, optional) -- How many. Defaults to 20.
  - returns `{rows: [{distance, at, value}], csv}`
- `fem_animate(result, mode?, frames?, amplitude?)` (read-only) -- Frames of a mode shape, as displacement per node.
  - `result` (number, required) -- A result from fem.modal.
  - `mode` (number, optional) -- Which mode, from zero. Defaults to the first.
  - `frames` (number, optional) -- How many. Defaults to 24.
  - `amplitude` (number, optional) -- Peak movement as a fraction of the diagonal.
  - returns `{frames, period, amplitude_scale, displacement}`
- `fem_nodes(result)` (read-only) -- The result's node coordinates, three numbers per node.
  - `result` (number, required) -- The result handle.
  - returns `{count, nodes}`
- `fem_movie(result, path, field?, mode?, frames?, fps?, width?, height?, amplitude?, yaw?, pitch?, orbit?, caption?, color_map?)` -- Renders a result as a playable Motion-JPEG .mov.
  - `result` (number, required) -- The result handle.
  - `path` (string, required) -- Where to write the film.
  - `field` (string, optional) -- Which field colours it. Defaults to von_mises, or displacement for a modal result.
  - `mode` (number, optional) -- For a modal result, which mode. Defaults to the first.
  - `frames` (number, optional) -- How many. Defaults to 36.
  - `fps` (number, optional) -- Frames per second. Defaults to 20.
  - `width` (number, optional) -- Pixels. Defaults to 720.
  - `height` (number, optional) -- Pixels. Defaults to 540.
  - `amplitude` (number, optional) -- Peak movement as a fraction of the diagonal.
  - `yaw` (number, optional) -- Camera bearing in degrees.
  - `pitch` (number, optional) -- Camera elevation in degrees.
  - `orbit` (bool, optional) -- Turn the camera a full circle over the film.
  - `caption` (string, optional) -- A line of text along the top.
  - `color_map` (string, optional) -- viridis, blue_to_red or greyscale.
  - returns `{path, frames, seconds, width, height, field_min, field_max, bytes}`
- `fem_image(result, path, field?, mode?, width?, height?, amplitude?, yaw?, pitch?, caption?, color_map?)` -- Renders one frame of a result as a PNG.
  - `result` (number, required) -- The result handle.
  - `path` (string, required) -- Where to write the picture.
  - `field` (string, optional) -- Which field colours it.
  - `mode` (number, optional) -- For a modal result, which mode to draw at its extreme.
  - `width` (number, optional) -- Pixels. Defaults to 720.
  - `height` (number, optional) -- Pixels. Defaults to 540.
  - `amplitude` (number, optional) -- Peak movement as a fraction of the diagonal.
  - `yaw` (number, optional) -- Camera bearing in degrees.
  - `pitch` (number, optional) -- Camera elevation in degrees.
  - `caption` (string, optional) -- A line of text along the top.
  - `color_map` (string, optional) -- viridis, blue_to_red or greyscale.
  - returns `{path, width, height, field_min, field_max, bytes}`
- `fem_export(result, field, path, format?, from?, to?, samples?)` -- Writes a path plot or a field to CSV or an org table.
  - `result` (number, required) -- The result handle.
  - `field` (string, required) -- Which field.
  - `path` (string, required) -- Where to write it.
  - `format` (string, optional) -- csv or org. Defaults to the extension.
  - `from` (array, optional) -- A path start; omit to export every node.
  - `to` (array, optional) -- A path end.
  - `samples` (number, optional) -- Path samples.
  - returns `{path, rows, format}`

<!-- END GENERATED cad-fem -->

### Working with it

Build geometry, mesh it, attach a study, solve, read the numbers. Faces
are named by direction rather than by clicking: `cad_face_at` returns the
face whose outward normal is nearest a direction you give, which is the
only way to refer to one that survives the body being rebuilt at a
different size -- and the adaptive loop rebuilds it on every cycle.

Every `fem_solve` comes back with a `relative_error`: the estimated
energy-norm error of that mesh. A stress number without it is a number
nobody can act on. Above about 10% the mesh is telling you it has not
resolved the problem; `fem_adapt` refines until it has, or says why it
could not.

Headless, with no display, for CI and scripts:

```
mep --cad-fem study.json out.json     # a list of these calls, in order
mep --fem-solve study.json            # the same thing, named for what it does
mep --cad-export part.step part.stl   # one file to another
```

A script is a JSON list of `{"method": ..., "params": {...}}`. Name a
call with `"as"` and later calls can write `$name.field` anywhere a value
goes, so handles never appear in the file.

## The in-pane 3D modeler

`mep_model_new()` creates a brand-new empty scene headlessly (no source
file needed, no window focus required) and returns its `buffer_id` --
pass that id to every other `mep_model_*` tool. Opening an existing
`.obj`/`.gltf`/`.glb`/`.iqm`/`.vox`/`.m3d`/`.blend` file with
`mep_file_open` also lands in the 3D modeler and produces a usable
`buffer_id` the same way (find it via `mep_state_dump`). Every tool below
works headlessly, the same as the buffer/pane tools above -- no
`mep_mouse_*`/`mep_key_press` needed for anything in this section, unlike
the image editor's by-hand paint tools.

Data model: a scene is a flat list of `meshes`/`textures`/`objects` (not
a hierarchy, except an optional `parent` for Outliner display and
`mep_model_group_objects`/`mep_model_set_parent`). An object has a
position/rotation (Euler XYZ, degrees)/scale, a base color plus optional
roughness/metallic scalars and up to four texture map slots (albedo/
normal/roughness/metallic), and references one mesh (meshes can be
shared by multiple objects until a vertex-editing tool clones one -- see
below).

### Building geometry
- `mep_model_add_primitive(buffer_id, kind, transform?)` -- cube/sphere/
  cylinder/cone/plane/torus/wedge. `mep_model_primitive_info(kind?)`
  reports each primitive's pivot point and dimensions without needing to
  already know or guess.
- `mep_model_add_lathe(buffer_id, profile, segments?, cap_top?,
  cap_bottom?, transform?)` -- revolves a 2D profile (ordered
  `[radius, height]` pairs) around the Y axis into a smooth, closed,
  welded mesh with real UVs. The tool for any *turned* form a fixed
  primitive can't express: bottles, bowls, table legs, most chess
  pieces. A profile ending at radius ~0 needs no cap on that end (it's
  already a point); ending at a nonzero radius with `cap_*` true gets a
  flat disc there instead. Only covers rotationally symmetric
  silhouettes -- for anything asymmetric (a chess knight's horse-head
  profile), see `mep_model_add_custom_mesh` next.
- `mep_model_add_custom_mesh(buffer_id, vertices, triangles, uvs?, name?, transform?)`
  -- the escape hatch for geometry no primitive or lathe combination can
  produce: `vertices` is a flat `[[x,y,z],...]` list, `triangles` a
  flat `[[i,j,k],...]` list of 0-indexed vertex triples (CCW winding
  from outside, same convention every other mep mesh uses). Normals are
  computed automatically (smooth, area-weighted) -- don't pass your
  own. `uvs` is optional: a flat `[[u,v],...]` list, one pair per vertex
  in the same order as `vertices` -- give it and the object can sample a
  wrapped texture via `mep_model_set_texture` like any other mesh; omit
  it and the object falls back to a solid color/roughness/metallic from
  `mep_model_set_material` instead. Built for exactly this case: a chess
  knight's asymmetric horse-head silhouette, hand-authored as a 2D
  profile, extruded into a 3-layer "loaf" (front/back plus a wider
  bulged middle ring, for real volume instead of a flat cardboard-cutout
  look), triangulated with ear-clipping (a real algorithm, not a naive
  vertex-fan -- a concave silhouette like a horse head, with notches for
  the ear valley and jaw, breaks a naive fan-from-one-vertex
  triangulation), and UV-mapped (u wraps once around its own closed
  perimeter -- seamless, matching how a `mep_image_fill_wood_turned`
  texture is already designed to wrap; v marks which extrusion layer)
  so it could be textured the same as the rest of the piece instead of
  standing out as the one flat-colored object. See
  CHESS_SET_BENCHMARK_PLAN.md's Phase 8/9 for the full worked example,
  the profile-widening lesson (a custom mesh's own base needs to
  match/exceed whatever lathe stem it sits on, or the join reads as a
  thin blade stuck on a dowel rather than a solid neck), and how the
  correct `rotation.y` for "facing a given world direction" was worked
  out from the actual rotation-matrix math rather than guessed from
  renders (easy to misread at oblique angles).
- `mep_model_duplicate_object(buffer_id, object_id, cascade?)`,
  `mep_model_duplicate_mirrored(buffer_id, object_id, axis, ...)`,
  `mep_model_radial_array(buffer_id, object_id, count, axis, ...)` --
  bulk-populate a scene (a full chess set's 8 pawns from one, a radial
  fence from one post) without one add-primitive call per copy.
- `mep_model_delete_object(buffer_id, object_id, cascade?)`,
  `mep_model_delete_objects(buffer_id, object_ids)`.
- **glTF import caveat**: opening a `.gltf`/`.glb` file (`mep_file_open`)
  flattens its node hierarchy into one `Object3D` per mesh primitive,
  baking each node's transform into the mesh's own vertex positions at
  import time -- every re-imported object's live `position`/`rotation`/
  `scale` read back as the identity (`{0,0,0}`/`{0,0,0}`/`{1,1,1}`),
  not its visual placement in the scene. This is expected, not a bug.
  It matters for anything driven by an object's `position` field after
  a round-trip through a saved glTF file -- in particular
  `mep_model_anim_move_object`'s `from`/`to` are a *relative* offset on
  top of the already-baked mesh, not an absolute board/world coordinate.

### Transform, material, texture
- `mep_model_set_transform(buffer_id, object_id, position?, rotation?,
  scale?)` and the batch form `mep_model_set_transforms(buffer_id,
  updates)` -- each field is sparse (omitted = unchanged), so
  repositioning doesn't require reading the current transform back
  first.
- `mep_model_set_material(buffer_id, object_id, color, roughness?,
  metallic?)` and the batch form `mep_model_set_materials`. `color` is
  0..1 floats (not 0..255 -- unlike the image editor's colors, see
  below). `roughness`/`metallic` (0..1, each optional/sparse) drive the
  renderer's simplified metallic-roughness shading: low roughness + high
  metallic gives a tight, color-tinted specular highlight (polished
  metal); high roughness + low metallic gives a soft/matte look (raw
  wood, stone, cloth). There is no full Cook-Torrance/GGX PBR here, no
  shadows or environment reflections -- Blinn-Phong over up to 8 scene
  lights (see "Lighting" below) plus a flat ambient floor term,
  deliberately kept cheap.
- `mep_model_set_texture(buffer_id, object_id, path, kind?)` -- `kind`
  (default `"albedo"`) selects which of the four map slots: `"albedo"`
  (tinted by the object's own color, like glTF's baseColorTexture +
  baseColorFactor), `"normal"` (tangent-space normal map), `"roughness"`/
  `"metallic"` (single-channel maps, multiplied by the corresponding
  material scalar). Load from any PNG/JPG/BMP file on disk -- including
  one just written by `mep_image_export_png` (see below), the way this
  document's own end-to-end example builds a textured piece.
- `mep_model_rename_object`, `mep_model_set_visible`.

### Vertex/face editing (Edit Mesh mode's own tools, headless)
`mep_model_list_vertices`/`_list_triangles` (read back positions/
connectivity), `mep_model_set_vertex_position`, `mep_model_delete_vertices`,
`mep_model_merge_vertices`, `mep_model_merge_by_distance`,
`mep_model_recalculate_normals`, `mep_model_subdivide_faces`,
`mep_model_extrude_faces`, `mep_model_inset_faces`, `mep_model_dissolve_vertex`,
`mep_model_add_vertex`, `mep_model_make_face`, `mep_model_flip_normals`.
Every one of these safely clones a mesh still shared by other objects
before mutating it (so editing one primitive instance never deforms a
sibling that started from the same `mep_model_add_primitive` call).

### Selection, camera, rendering
- `mep_model_select(buffer_id, object_ids)`, `mep_model_get_selection`.
- `mep_model_camera_set(buffer_id, target?, yaw?, pitch?, distance?,
  fov?)` (each field sparse), `mep_model_camera_get`,
  `mep_model_set_view(buffer_id, preset)` (front/back/left/right/top/
  bottom/perspective -- a snap, not a smooth orbit), `mep_model_frame_all`
  (fit the camera to the whole scene -- do this before your first render
  of a freshly built scene rather than guessing a camera position).
- `mep_model_render_to_image(buffer_id, path, width?, height?, supersample?)`
  -- offscreen render to a PNG file, independent of whatever the live
  viewport shows; read the file back to actually look at the result
  (never assume a render "looks right" without viewing it). Rendered at
  `width*supersample x height*supersample` and box-downsampled to
  `width x height` for anti-aliasing (CHESS_REALISM_PLAN.md Phase 2);
  `supersample` defaults to 2, clamped to `[1,4]` -- drop it to 1 for a
  faster, jaggier draft render.
- `mep_model_undo(buffer_id)`, `mep_model_redo(buffer_id)`.
- `mep_model_group_objects`, `mep_model_set_parent` -- Outliner grouping only,
  doesn't affect transforms.

### Lighting (MULTILIGHT_ANIMATION_PLAN.md, CHESS_REALISM_PLAN.md)
`Scene::lights` is real scene content -- undo-aware in the live session
and saved to the glTF file, round-tripping through a custom
`extras.mep_lights` array (`SaveModel3DGltf`/`LoadModel3DGltfLights` in
model3d_doc.cpp) rather than the standard `KHR_lights_punctual`
extension -- a deliberate choice, not an oversight: `KHR_lights_punctual`
encodes a directional light's direction via the owning node's own
rotation quaternion, real complexity this app's `Light` struct has no
need for since it already stores direction/position directly. This
means an exported `.gltf` file's lights are round-trip-losslessly
readable by mep itself, but not by other glTF tools/viewers, which will
just see plain nodes and no lights at all (same as any other
`extras`-based data this exporter writes, e.g. each node's own
`extras.visible`). An empty light list renders exactly like before this
feature existed (a single hardcoded key light), so existing scenes/
scripts need no changes.
- `mep_model_add_light(buffer_id, type)` -- `type` is `"directional"` or
  `"point"`; returns `{light_id}`.
- `mep_model_set_light(buffer_id, light_id, type?, position?, direction?,
  color?, intensity?, range?, visible?)` -- every field sparse (omitted =
  unchanged), same convention as `mep_model_set_material`. `direction`
  (directional lights) and `position` (point lights) are both
  world-space; `direction` points *from* a lit surface *toward* the
  light, matching the old hardcoded key light's own convention. `range`
  is a point light's falloff distance (simple linear falloff, not
  inverse-square/physically based). Up to 8 lights are actually used by
  the renderer per scene (`MEP_MAX_LIGHTS`); extras are ignored.
- `mep_model_delete_light(buffer_id, light_id)`, `mep_model_list_lights(buffer_id)`
  (returns every light's full state -- useful for round-tripping what
  you just set).
- No environment-map/IBL reflections, no configurable ambient term --
  the existing flat ambient floor stays as-is regardless of how many
  lights are added.

**Shadows** (CHESS_REALISM_PLAN.md Phase 3): automatic, not a separate
API -- every render (offscreen and the live viewport) casts a real
shadow map from one designated light: `Scene::lights[0]` if it's a
visible directional light, else the same legacy key-light direction the
renderer falls back to when a scene has no lights at all. Point lights
and every light past index 0 never cast shadows -- a single
shadow-casting light, deliberately, not a full multi-light shadow
system. Shadowed areas blend toward a dim floor rather than pure black,
matching this renderer's non-physically-based lighting throughout. If
you want a specific light to cast the shadow, make it light index 0
(`mep_model_add_light` appends, so add your intended shadow-caster
first, or reorder by deleting/re-adding).

### Animation & video export (ANIMATION_VIDEO_PLAN.md, MULTILIGHT_ANIMATION_PLAN.md)
Camera keyframes and per-object keyframes are two independent,
session-only track sets -- neither is saved to the scene file or
undoable (scrubbing previews by writing straight into the live
camera/object state, the same honest tradeoff as any other live-preview
feature: a `mep_model_save` mid-scrub captures whatever the scrub last
wrote). A scene's camera keyframes are a separate list from its live
orbit camera (`mep_model_camera_set`/`_get`) -- keyframes only take
effect where you explicitly sample them. Object keyframes are keyed by
`object_id` and equally only take effect where sampled.
- `mep_model_anim_orbit_camera(buffer_id, target, distance?, pitch?,
  fov?, duration?, start_yaw?, revolutions?)` -- the one-call way to get
  "pan the camera around the scene once": replaces any existing
  keyframes with an evenly-spaced full orbit, yaw sweeping from
  `start_yaw` through `start_yaw + revolutions*360` over `duration`
  seconds at constant pitch/distance/fov.
- `mep_model_anim_add_camera_keyframe(buffer_id, time, target, yaw,
  pitch, distance, fov?)` / `mep_model_anim_clear_camera(buffer_id)` --
  the low-level building blocks for a custom (non-orbit) camera move,
  or to remove keyframes entirely. Keyframes are linearly interpolated
  by time (added/replaced-at-that-time, kept sorted).
- `mep_model_anim_set_camera_time(buffer_id, time)` -- samples the
  keyframes at `time` and applies the result to the pane's live camera,
  for previewing/scrubbing (follow with `mep_model_render_to_image` to
  actually see it) before spending time on a full video export.
- `mep_model_anim_move_object(buffer_id, object_id, from, to, duration)`
  -- the one-call way to animate an object moving: replaces that
  object's keyframes with a simple two-keyframe linear move from `from`
  to `to` over `duration` seconds (position only -- rotation/scale hold
  at the object's current values throughout the move). `from`/`to` are
  the object's `position` field, so for a re-imported glTF object (see
  the caveat under "Building geometry" -- import bakes each node's
  transform into its mesh, leaving every object's live `position` at
  `{0,0,0}`) they're a *relative* offset on top of the baked mesh, not
  an absolute world/board coordinate.
- `mep_model_anim_add_object_keyframe(buffer_id, object_id, time,
  position, rotation?, scale?)` / `mep_model_anim_clear_object_keyframes(
  buffer_id, object_id)` -- the low-level building blocks for a custom
  (non-linear-move) object animation, e.g. more than two keyframes, or
  to remove an object's keyframes entirely. Keyframes are linearly
  interpolated by time (added/replaced-at-that-time, kept sorted), same
  as camera keyframes.
- `mep_model_anim_set_object_time(buffer_id, object_id, time)` -- samples
  that object's keyframes at `time` and writes the result into the
  live object's transform, for previewing/scrubbing.
- `mep_model_render_animation_to_video(buffer_id, path, fps?, width?,
  height?, quality?, show_grid?, wireframe?, show_textures?, unlit?)` --
  renders to a real, standard Motion-JPEG `.mov` file (a hand-written
  in-house muxer -- no ffmpeg/external dependency; independently
  verified with `ffprobe`/`ffmpeg`, so it's a genuinely standard file
  any player can open, not just something mep itself can read back).
  Works with camera keyframes, object keyframes, both, or (if neither
  exists) fails -- at least one animated track is required. The
  rendered duration is the *longest* of the camera track and every
  animated object's track; anything shorter holds its last keyframe's
  value for the remainder (so a camera-only and object-only animation
  can be exported together and the shorter one just holds still once
  it finishes). Same offscreen-render machinery as
  `mep_model_render_to_image`, just sampling the camera and every
  animated object once per frame instead of rendering the live viewport
  once -- including the same `supersample?` anti-aliasing param
  (default 2, clamped `[1,4]`; costs render time roughly with its
  square, so drop to 1 for a fast draft pass across many frames).
- Play the result back **inside mep itself**: `mep_file_open` on a
  `.mov` path opens a video-playback pane (space toggles play/pause,
  left/right step one frame, click-drag the scrub bar to seek) --
  `mep_state_dump` shows its `buffer_id` the same as any other open
  file.

### Worked example: a textured turned object end to end
```
mep_model_new()                                        -> {buffer_id: 10}
mep_model_add_lathe(10, [[0.5,0],[0.3,0.5],[0,1.0]])    -> {object_id: 1}
mep_image_new(256, 256)                                 -> {buffer_id: 11}
mep_image_fill_wood_turned(11, color_a={60,35,15}, color_b={190,140,90})
mep_image_export_png(11, "/tmp/wood.png")
mep_model_set_texture(10, 1, "/tmp/wood.png", kind="albedo")
mep_model_set_material(10, 1, color={r:1,g:1,b:1,a:1}, roughness=0.4, metallic=0.0)
mep_model_frame_all(10)
mep_model_render_to_image(10, "/tmp/render.png", 500, 500)
```
Then actually read `/tmp/render.png` and look at it -- a render call
succeeding only means the RPC round-tripped, not that the geometry or
texture came out looking right.

### Worked example: orbit the camera around a scene and export/play it back
```
mep_model_anim_orbit_camera(10, target={x:0,y:0,z:0}, distance=8, pitch=30, duration=4)
mep_model_render_animation_to_video(10, "/tmp/orbit.mov", fps=24, width=960, height=720)
mep_file_open("/tmp/orbit.mov")
```
That last call opens `/tmp/orbit.mov` in a video-playback pane in the
live GUI -- the same as if a human had opened it themselves. Confirm it
actually shows a moving orbit (not a static/broken frame) by rendering
a couple of `mep_model_anim_set_camera_time` preview frames beforehand,
or by extracting frames from the exported file with an external tool
(e.g. `ffmpeg -i /tmp/orbit.mov -vf "select=eq(n\,12)" -vframes 1
/tmp/frame12.png`, then read that PNG) and checking they actually
differ, the same "don't assume it looks right" discipline as the render
example above.

### Worked example: lights + an object move + an orbiting camera together
```
mep_model_add_light(10, "directional")                 -> {light_id: 1}
mep_model_set_light(10, 1, direction={x:0.4,y:0.8,z:0.5}, color={r:1,g:1,b:1}, intensity=1.0)
mep_model_add_light(10, "point")                        -> {light_id: 2}
mep_model_set_light(10, 2, position={x:5,y:2.5,z:0}, color={r:1,g:0.6,b:0.35}, intensity=1.6, range=14)

mep_model_anim_move_object(10, 24, from={x:0,y:0,z:0}, to={x:0,y:0,z:2}, duration=4)
mep_model_anim_orbit_camera(10, target={x:0,y:0.89,z:0}, distance=12, pitch=30, duration=4)
mep_model_render_animation_to_video(10, "/tmp/lit_move.mov", fps=30, width=1280, height=960)
```
The object move and the camera orbit are independent track sets, so
both apply in the same export -- `duration` here matches on purpose,
but they don't have to (the render runs as long as the longer one).
Verify the same way as any other export: extract frames at a few points
across the clip with `ffmpeg` and read them, and confirm the moving
object visibly separates from its starting position while the
background/other objects rotate around with the camera, not the other
way around.

## The in-pane 2D sketcher

`mep_sketch_new()` opens an empty CAD sketch headlessly and returns its
`buffer_id` -- pass that to every other `mep_sketch_*` tool. Everything
in this section works without the window: the same `Editor::CadSketch*`
calls sit behind both these tools and the sketch pane's own keys, so a
sketch built by tool and one drawn by hand are the same sketch.

**A sketch is constrained geometry, not a drawing.** Place things roughly
and then say what must be true of them; the solver moves the geometry to
satisfy it. That is the difference from the 3D modeler above, where a
transform you set is where the object is. Here, setting a dimension to a
new value and re-solving is how a parametric change is made.

Data model: a sketch is *points*, *entities* built out of shared points,
and *constraints* between them. An arc is a centre and two endpoints, not
a centre plus angles, so joining one curve's end to another is a
coincidence between two points rather than a constraint against a derived
quantity. A circle keeps a radius instead of a point on its rim. Every
sketch starts with a fixed origin point (id 0) so that it cannot drift.

### Drawing
- `mep_sketch_add_point(buffer_id, x, y, fixed?)`
- `mep_sketch_add_line(buffer_id, x0, y0, x1, y1, construction?)`
- `mep_sketch_add_rectangle(buffer_id, x0, y0, x1, y1, construction?)` --
  four lines sharing their corner points, already horizontal/vertical.
  Prefer this to four separate lines: the shared corners mean dragging
  one moves both edges that meet there, with no constraints needed.
- `mep_sketch_add_circle(buffer_id, cx, cy, radius, construction?)`
- `mep_sketch_add_arc(buffer_id, cx, cy, sx, sy, ex, ey, ccw?, construction?)`
- `mep_sketch_add_ellipse(buffer_id, cx, cy, major, minor, rotation?, construction?)`

`construction: true` makes an entity a reference line -- constraints may
use it, profile extraction ignores it.

### Constraining
`mep_sketch_list(buffer_id)` first: every constraint names geometry by
id. Then `mep_sketch_constrain(buffer_id, kind, points?, entities?,
value?)`. What each kind takes:

| kind | arguments |
|---|---|
| `coincident` | two points |
| `horizontal`, `vertical` | two points, or one line entity |
| `parallel`, `perpendicular` | two line entities |
| `angle` | two line entities + `value` (radians) |
| `equal` | two lines, or two circles/arcs |
| `tangent` | a line and a circle/arc, or two circles/arcs (`value < 0` = internal) |
| `concentric` | two circles/arcs |
| `collinear` | two line entities |
| `symmetric` | two points + a line entity |
| `point_on_object` | a point + an entity |
| `distance`, `horizontal_distance`, `vertical_distance` | two points + `value` |
| `radius`, `diameter` | a circle or arc + `value` |

A constraint that does not fit its arguments is **refused**, not quietly
dropped -- so a failure here means the selection was wrong, not that the
solver could not manage it.

### Solving and diagnosis
`mep_sketch_solve(buffer_id)` re-solves and reports `status`:

- `solved` -- every constraint satisfied and no freedom left. What a
  finished sketch should be.
- `under_constrained` -- satisfied, but the sketch can still move.
  `degrees_of_freedom` says how much. Not an error; most sketches are
  here most of the time.
- `redundant` -- the constraints depend on one another but agree. The
  sketch solves; `redundant` lists the ones saying nothing new.
- `conflicting` -- they depend on one another and *disagree*. Nothing
  satisfies them; `conflicting` lists the ones involved. Remove one with
  `mep_sketch_remove_constraint`.

Those last two are different problems with different fixes, which is why
they are reported separately rather than both as "over-constrained".

### Changing and extracting
- `mep_sketch_set_constraint_value(buffer_id, constraint_id, value)` --
  the parametric edit: change a dimension, everything downstream follows.
- `mep_sketch_drag_point(buffer_id, point_id, x, y)` -- moves a point
  while honouring the constraints, going as far as the sketch's remaining
  freedom allows rather than refusing an unreachable target.
- `mep_sketch_set_point`, `..._set_point_fixed`, `..._set_construction`,
  `..._delete_entity`, `..._select`/`..._selection`.
- `mep_sketch_profiles(buffer_id)` -- the closed regions the
  non-construction geometry bounds, each with its exact area (integrated
  along the curves, not off a polygon) and its hole count. A washer drawn
  as two circles is one profile with one hole, not two profiles.

### By hand
The sketch pane's own keys, should you want to drive it that way: digits
`1`-`6` pick the tool (select / point / line / rectangle / circle / arc),
letters apply a constraint to the selection by first letter (`c`
coincident, `h` horizontal, `v` vertical, `p` parallel, `l`
perpendicular, `t` tangent, `e` equal, `o` concentric, `n` collinear,
`y` symmetric, `g` point-on-object), and `d`/`x`/`z`/`a`/`r`/`m` open a
prompt for a distance / horizontal distance / vertical distance / angle /
radius / diameter. `f` anchors the selected points, `k` toggles
construction on the selected entities, `K` places new geometry as
construction, `s` re-solves, `F` fits the view, `G` toggles grid snap,
`q` leaves. Escape abandons a half-drawn shape, then clears the
selection.

## The in-pane image editor

Two ways to work with it: **by hand** (mouse/keyboard UI automation,
covers every tool including freehand painting) and **headlessly**
(`mep_image_*`, covers procedural fills but not freehand strokes). Use
the headless path whenever the goal is a texture/pattern rather than a
specific hand-drawn shape -- it's far more direct and doesn't need a
window, screenshots, or coordinate math.

### Headless: procedural textures (`mep_image_*`)
- `mep_image_new(width, height, color?)` -- creates a brand-new buffer
  headlessly (no source image file needed, unlike opening the editor by
  hand, which always requires an already-open image), seeded with one
  opaque layer. Returns `buffer_id`.
- `mep_image_info(buffer_id)` -- width/height/active layer/layer list.
- `mep_image_new_layer(buffer_id, name?)`, `mep_image_set_active_layer
  (buffer_id, layer_index)` -- every `mep_image_fill_*`/`mep_image_blur`
  call below targets the active layer unless a specific `layer_index` is
  passed.
- `mep_image_fill_gradient(buffer_id, mode, color_a, color_b, angle?)` --
  `mode`: `"linear"` (angle in degrees, 0 = left-to-right) or `"radial"`
  (centered on the buffer).
- `mep_image_fill_noise(buffer_id, color_a, color_b, scale?, octaves?, seed?)`
  -- fractal value noise (fBm), a general mottled/grainy base.
- `mep_image_fill_wood(buffer_id, color_a, color_b, ring_scale?, warp?, seed?)`
  -- concentric rings warped by noise: a wood *cross-section* (a tree's
  end grain, face-on) -- right for a round tabletop or a piece's flat
  cap, wrong for a lathe object's sides (wrapping concentric rings
  around a cylinder by angle gives a spiral/barber-pole, not wood
  grain -- a mistake this benchmark made and fixed once, don't repeat
  it). Use `mep_image_fill_wood_turned` below for that instead.
- `mep_image_fill_wood_turned(buffer_id, color_a, color_b, ring_scale?, warp?, seed?)`
  -- wood grain for a surface UV-wrapped circumferentially around a
  cylinder (u=angle/2pi, v=length -- exactly `mep_model_add_lathe`'s own
  convention): streaks run lengthwise along v, the way real turned-wood
  grain looks, and are seamless across the u=0/u=1 wrap by construction
  (no visible seam). This is the one to apply as an albedo texture on a
  `mep_model_add_lathe` object (see the worked example above).
- `mep_image_fill_marble(buffer_id, color_a, color_b, scale?, turbulence?, seed?)`
  -- fBm turbulence through a sine wave, the classic marble-vein recipe.
- `mep_image_fill_checkerboard(buffer_id, color_a, color_b, squares_x?, squares_y?)`
  -- alternating tiles (default 8x8), color_a in the (0,0) corner tile --
  a board texture for a single plane object, the fast alternative to
  placing 64 separate tile objects.
- `mep_image_blur(buffer_id, radius?, layer_index?)` -- box blur, softens
  raw noise output or anything else.
- `mep_image_export_png(buffer_id, path)` -- flattens visible layers,
  writes a PNG. Colors everywhere in this section are `{r,g,b,a}` 0..255
  ints (`a` optional, defaults 255) -- unlike the 3D modeler's 0..1-float
  `mep_model_set_material` colors.
- Every fill/blur call pushes one undo entry (`:u`/Ctrl-R by hand, or
  `mep_command_run("undo")`/`mep_command_run("redo")` -- there's no
  dedicated `mep_image_undo` tool, unlike the 3D modeler's
  `mep_model_undo`) and respects an active selection if one exists
  (drawn by hand with the Rectangle-select/Ellipse-select/Lasso tools --
  see below), otherwise overwrites the whole layer.

### By hand: layout
Press **`e`** while viewing an image (any PNG/JPG/etc. buffer -- open one
with `mep_file_open` or `mep_pane_split`'s `file` argument, or a
freshly `mep_image_new`-created buffer -- image.new already focuses the
current pane on it, no `e` press needed) to open the editor in that same
pane. **Esc** returns to the plain image viewer without losing anything:
layers and undo history are kept per-buffer, so pressing `e` again
resumes exactly where you left off. `:w`/`:wq` (or
`mep_command_run("w")`) flattens the visible layers and writes a real PNG
(native build only -- the wasm build doesn't support this yet); so does
`mep_image_export_png` for a headlessly-created buffer that was never
`:w`-savable to begin with (no filename).

Left to right: a two-column icon **tool sidebar**, the **canvas**, and a
**Layers** panel on the right. A thin **toolbar** (brush size, recent
colors) sits above the canvas, under the **menubar** (File/Edit/Layer/
View/**Texture**). The foreground/background color swatches are at the
*bottom* of the tool sidebar -- click one to open an HSV picker; the
**Texture** menu's Fill Gradient/Noise/Wood/Marble items use these same
two swatches as their color_a/color_b (no separate color input in that
dialog).

The pane's status bar (bottom) shows the active tool, brush size, zoom
%, canvas size, the cursor's live canvas-pixel position while hovering,
the active layer's name, and `[selection]` when one exists.

### By hand: paint tools (hotkey -- click the sidebar icon, or use the hotkey directly)

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
drops the selection outline entirely; a `mep_image_fill_*`/
`mep_image_blur` call also respects it (see above). `[`/`]` shrink/grow
the brush. `u`/Ctrl-R undo/redo (one entry per completed stroke/shape/
fill/move, not per intermediate frame).

### By hand: driving the Texture menu (numeric-parameter dialogs)
The Texture menu's Fill Gradient (Linear)/Fill Noise/Fill Wood/Fill
Marble/Blur items each gather 1-2 numeric parameters via a chained
sequence of native text-input prompts (the same modal `:` command-line
uses) rather than a single multi-field form -- click the menu item, a
prompt titled with the parameter's name and a pre-filled default appears
centered on the canvas; `mep_type_text` to replace the default (or leave
it and just press Return to accept it), `mep_key_press("Return")` to
confirm and either apply (last parameter) or advance to the next prompt.
`mep_key_press("Escape")` cancels the whole chain without applying
anything. Fill Gradient (Radial) and Blur (when accepting its one
default) apply immediately with no prompt.

### By hand: driving free-hand painting (worked example)

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
6. `Layer > New Layer` (menu, or `mep_image_new_layer` if you don't need
   to also switch to it by hand) to keep distinct parts of a drawing
   separable, if you expect to redo one part.
7. `mep_command_run("w")` to save when done (a file-backed buffer), or
   `mep_image_export_png` (a headlessly-created one).
