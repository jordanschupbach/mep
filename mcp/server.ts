// MCP server wrapping mep's agent-control socket (src/agent_rpc.cpp, see
// MEP_AGENT_API.md) as normal MCP tools -- lets Claude (or any MCP
// client) drive a running mep instance: move/split/resize/focus panes,
// move the cursor, read/edit buffer text, open/save files, run arbitrary
// ex-commands, inspect full editor state, poll for live events
// (cursor/buffer/pane/mode changes, notifications), and drive the real
// GUI window directly (screenshots, synthetic mouse/keyboard).
//
// Every tool here is a thin 1:1 wrapper around one JSON-RPC method the
// C++ side already implements -- no new behavior, just another way to
// reach the same primitives the embedded-Lua `mep.*` API and the raw
// Unix-socket protocol already expose. See MEP_AGENT_API.md for the full
// tool reference this mirrors.
//
// Superseded by src/mcp_bridge.cpp (built as `mep-mcp`): a dependency-
// free C++ binary speaking the same protocol, needing no Deno/Node
// runtime -- prefer that for `claude mcp add`. This file (+
// mep_client.ts) is kept as a reference/fallback implementation.
//
// Requires a native (non-wasm) `mep` already running, since only that
// build binds the agent socket at all (see src/agent_rpc.h's own
// Emscripten/Windows gating). Run with:
//   deno task mcp
// or point an MCP client (Claude Code: `claude mcp add`, Claude Desktop's
// config, ...) at `deno run --allow-net --allow-read --allow-write
// --allow-env mcp/server.ts` directly (--allow-write is for the Unix
// socket connection itself, not for writing files). `MEP_AGENT_SOCKET=
// /path/to/x.sock` picks a
// specific instance when more than one mep window is running --
// mep_client.ts's discoverSocketPath() explains the default/error
// behavior when it's unset.
//
// Events (M3) are push-based on the underlying socket, but MCP tool
// calls are fundamentally pull-based -- there is no current, portable
// way for a pushed event to reach the calling model without the model
// choosing to call a tool (checked against the current MCP TypeScript
// SDK and Claude Code's own docs before writing this: ordinary MCP
// resource-subscription/notification pushes are not surfaced to the
// model by Claude Code or Claude Desktop, only used to invalidate cached
// tool/resource lists). `mep_poll_events` is therefore a pollable tool,
// not a live stream -- an agent that wants to react to editor activity
// calls it periodically (or after taking an action, to see what
// happened) and gets everything queued since the last call.

import { McpServer } from "npm:@modelcontextprotocol/sdk@^1.30/server/mcp.js";
import { StdioServerTransport } from "npm:@modelcontextprotocol/sdk@^1.30/server/stdio.js";
import { z } from "npm:zod@^3.25";

import { MepClient } from "./mep_client.ts";

const mep = new MepClient();

function toolResult(value: unknown) {
  return { content: [{ type: "text" as const, text: JSON.stringify(value) }] };
}

function errorResult(err: unknown) {
  const message = err instanceof Error ? err.message : String(err);
  return { content: [{ type: "text" as const, text: message }], isError: true };
}

// Client-side cache of the last status *this process* reported, purely
// to decide when to auto-nudge a fresh "thinking" below -- mep's own
// Connection::status (agent_rpc.cpp) remains the real source of truth,
// and this cache starts fresh (== every status looks "stale") on every
// reconnect, same as the mep-side status itself does.
let lastKnownStatus = "";

// Pure introspection -- deliberately excluded from the auto-nudge below,
// so an agent (or a human debugging) checking state doesn't itself
// perturb the very status it's trying to read. Only real activity
// (an edit, a command, a pane/file operation, (re-)identifying) counts
// as "a new task has started."
const READ_ONLY_METHODS = new Set([
  "cursor.get",
  "buffer.list",
  "buffer.getLines",
  "buffer.filename",
  "session.info",
  "session.listParticipants",
  "state.dump",
  "pane.get",
  "ui.screenshot",
  "model.listObjects",
  "model.sceneStats",
  "model.primitiveInfo",
  "model.getSelection",
  "model.cameraGet",
  "model.renderToImage",
  "model.listVertices",
  "model.listTriangles",
]);

// Every tool below funnels through this -- issues the RPC call and turns
// either outcome into an MCP tool result rather than throwing, so a mep-
// side JSON-RPC error (bad buffer_id, unknown pane, ...) or a connection
// failure (mep isn't running, socket discovery ambiguous) comes back to
// the model as a normal, readable tool result with isError set, not a
// protocol-level exception.
//
// Also auto-nudges the status badge (COLLAB_CURSORS_PLAN.md Phase 1g)
// out of a stale "done"/unset state the moment any *other*, non-read-
// only tool gets called -- reported bug: after a task finishes ("done",
// checkmark shown), the badge never moved again on the next question in
// the same conversation, because nothing forces the model to remember
// calling mep_set_status("thinking") itself at the start of every new
// task. mep_set_status's own description asks it to, but that's
// advisory, not enforced -- this makes the cycle self-driving instead of
// relying on the model's discipline: the next real-activity tool call
// after a "done"/never-reported status clears the stale badge on its
// own.
async function callTool(method: string, params: unknown) {
  const isStale = lastKnownStatus === "done" || lastKnownStatus === "" || lastKnownStatus === "idle";
  if (method !== "session.setStatus" && !READ_ONLY_METHODS.has(method) && isStale) {
    try {
      await mep.call("session.setStatus", { status: "thinking" });
      lastKnownStatus = "thinking";
    } catch {
      // Best-effort -- if mep is unreachable the real call below will
      // fail too and surface that error to the model normally.
    }
  }
  try {
    const result = await mep.call(method, params);
    if (method === "session.setStatus") {
      const status = (params as Record<string, unknown> | undefined)?.status;
      if (typeof status === "string") lastKnownStatus = status;
    } else if (method === "buffer.insertText" || method === "buffer.setLine" || method === "buffer.replaceLines") {
      lastKnownStatus = "writing"; // mirrors agent_rpc.cpp's own auto-transition, keeps this cache accurate
    }
    return toolResult(result);
  } catch (err) {
    return errorResult(err);
  }
}

const server = new McpServer({ name: "mep-agent", version: "0.1.0" });

// --- Identity -----------------------------------------------------------

server.registerTool(
  "mep_identify",
  {
    description:
      "Set your own display name, shown at your cursor and in mep's tab-bar participant list (with a robot icon, since you're an AI agent) so the human can see who's editing what. Called automatically once when this MCP server starts (default name from MEP_AGENT_NAME, else \"Claude\") -- call this again any time to rename yourself mid-session.",
    inputSchema: { name: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("session.identify", args),
);

server.registerTool(
  "mep_set_status",
  {
    description:
      "Report what you're currently doing, shown as a small badge on your tab-bar chip so the human can tell at a glance without reading your output: \"thinking\" (reasoning, no edits yet), \"writing\" (set automatically by mep_buffer_insert_text/set_line/replace_lines too -- call it yourself only if you want the badge to show sooner, e.g. right before a long tool-call sequence), \"awaiting_input\" (you've asked the human a question and are waiting on their reply), \"done\" (finished this task), or \"idle\" (clear the badge). mep has no way to see your own reasoning or your conversation with the human, so \"thinking\"/\"awaiting_input\"/\"done\" only ever change when you call this -- call it whenever your state changes, not just once. If you start a new task after a previous one reported \"done\" and forget to call this first, your next real action (an edit, a command, opening/saving a file, a pane operation -- not a plain read like mep_cursor_get/mep_state_dump) automatically clears the stale checkmark to \"thinking\" for you -- but call it explicitly right when you start if you want the badge to update immediately rather than on your first other action.",
    inputSchema: { status: z.enum(["idle", "thinking", "writing", "awaiting_input", "done"]) },
  },
  async (args: Record<string, unknown>) => callTool("session.setStatus", args),
);

server.registerTool(
  "mep_list_participants",
  {
    description:
      "List everyone currently present in this mep instance -- other connected AI agents and human :CollabJoin peers -- with each one's name, kind, buffer_id/cursor (if positioned), and status badge (agents only). Useful for checking whether another agent is already working on something before you touch the same file.",
  },
  async () => callTool("session.listParticipants", {}),
);

// --- Cursor -----------------------------------------------------------
//
// Your cursor is your own -- independent of the human's real, on-screen
// cursor and of any other connected participant's (another agent, or a
// human collaborator via :CollabJoin). Setting it or typing through it
// never moves what the human sees their own cursor doing, and mep
// renders it as a separate labeled caret in the buffer (once you're
// positioned in one -- see mep_cursor_set's buffer_id).

server.registerTool(
  "mep_cursor_get",
  { description: "Get your own cursor's position (0-indexed row/col) and which buffer it's in." },
  async () => callTool("cursor.get", {}),
);

server.registerTool(
  "mep_cursor_set",
  {
    description:
      "Move your own cursor to a specific position (0-indexed row/col), optionally in a different buffer_id -- does not affect the human's real cursor or any other participant's.",
    inputSchema: {
      buffer_id: z.number().int().optional().describe("defaults to wherever your cursor already is"),
      row: z.number().int().describe("0-indexed line number"),
      col: z.number().int().describe("0-indexed column"),
    },
  },
  async (args: Record<string, unknown>) => callTool("cursor.set", args),
);

// --- Buffer -------------------------------------------------------------

server.registerTool(
  "mep_buffer_insert_text",
  {
    description:
      "Insert text at your own cursor position, as if typed, and advance your cursor past it -- does not touch the human's real cursor or type into whatever buffer they currently have open.",
    inputSchema: { text: z.string().describe("Text to insert; use \\n for newlines") },
  },
  async (args: Record<string, unknown>) => callTool("buffer.insertText", args),
);

server.registerTool(
  "mep_buffer_set_line",
  {
    description: "Replace one line (0-indexed) of the buffer your own cursor is currently in with new text.",
    inputSchema: { row: z.number().int(), text: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("buffer.setLine", args),
);

server.registerTool(
  "mep_buffer_replace_lines",
  {
    description:
      "Replace lines [start, end) (0-indexed, end exclusive) of the buffer your own cursor is currently in with the given lines -- a general multi-line splice.",
    inputSchema: { start: z.number().int(), end: z.number().int(), lines: z.array(z.string()) },
  },
  async (args: Record<string, unknown>) => callTool("buffer.replaceLines", args),
);

server.registerTool(
  "mep_buffer_set_lines",
  {
    description:
      "Replace a specific buffer's *entire* content by id, regardless of which pane is active -- for writing to a buffer you aren't currently viewing. Does not create undo history.",
    inputSchema: { buffer_id: z.number().int(), lines: z.array(z.string()) },
  },
  async (args: Record<string, unknown>) => callTool("buffer.setLines", args),
);

server.registerTool(
  "mep_buffer_switch",
  {
    description: "Move your own cursor to a different buffer by id (does not change what the human's real pane is showing).",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("buffer.switch", args),
);

server.registerTool(
  "mep_buffer_create",
  { description: "Create a new empty buffer without switching any pane to it. Returns its buffer_id." },
  async () => callTool("buffer.create", {}),
);

server.registerTool(
  "mep_buffer_filename",
  {
    description: "Get a buffer's filename by id (empty string for an unsaved/terminal buffer).",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("buffer.filename", args),
);

server.registerTool(
  "mep_buffer_list",
  {
    description:
      "List open buffers: id, filename, modified flag, line count, workspace_id. Scoped to the active workspace by default; pass workspace: \"all\" for every workspace, or a workspace id for one specific workspace.",
    inputSchema: { workspace: z.union([z.number().int(), z.literal("all")]).optional() },
  },
  async (args: Record<string, unknown>) => callTool("buffer.list", args),
);

// --- Workspaces & projects (WORKSPACES_PLAN.md Phase 11) -------------------

server.registerTool(
  "mep_workspace_list",
  {
    description:
      "List the active project's workspaces: id, name, root directory, git branch, primary flag, creating flag (git worktree still being added), active flag.",
  },
  async () => callTool("workspace.list", {}),
);

server.registerTool(
  "mep_workspace_switch",
  {
    description: "Switch to a workspace by id or name. Changes the working directory to that workspace's root (its git worktree).",
    inputSchema: { id: z.number().int().optional(), name: z.string().optional() },
  },
  async (args: Record<string, unknown>) => callTool("workspace.switch", args),
);

server.registerTool(
  "mep_workspace_create",
  {
    description:
      "Create a workspace. On a git project this adds a worktree on a new branch of the same name (asynchronously: the reply has creating=true until git finishes -- poll mep_workspace_list, or watch mep_poll_events for workspaceChanged / a notify with git's error). attach=true attaches to an existing branch instead of creating one.",
    inputSchema: { name: z.string(), attach: z.boolean().optional() },
  },
  async (args: Record<string, unknown>) => callTool("workspace.create", args),
);

server.registerTool(
  "mep_workspace_delete",
  {
    description:
      "Delete a workspace by id or name (removes its git worktree; the branch is kept). Refuses the primary workspace, and one with unsaved buffers unless force=true.",
    inputSchema: { id: z.number().int().optional(), name: z.string().optional(), force: z.boolean().optional() },
  },
  async (args: Record<string, unknown>) => callTool("workspace.delete", args),
);

server.registerTool(
  "mep_project_list",
  { description: "List the loaded projects: id, name, root, is_git, workspace_count, active flag." },
  async () => callTool("project.list", {}),
);

server.registerTool(
  "mep_project_switch",
  {
    description: "Switch to a loaded project by id or name.",
    inputSchema: { id: z.number().int().optional(), name: z.string().optional() },
  },
  async (args: Record<string, unknown>) => callTool("project.switch", args),
);

server.registerTool(
  "mep_project_open",
  {
    description: "Load a directory as a project (or switch to it if already loaded) and make it active; its saved workspaces/tabs are restored.",
    inputSchema: { root: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("project.open", args),
);

server.registerTool(
  "mep_buffer_get_lines",
  {
    description: "Read a range of lines [start, end) (0-indexed, end exclusive) from a buffer by id. Omit start/end for the whole buffer.",
    inputSchema: { buffer_id: z.number().int(), start: z.number().int().optional(), end: z.number().int().optional() },
  },
  async (args: Record<string, unknown>) => callTool("buffer.getLines", args),
);

// --- Files --------------------------------------------------------------

server.registerTool(
  "mep_file_open",
  { description: "Open a file by path (creates it, same as :e in vim, if it doesn't exist yet).", inputSchema: { path: z.string() } },
  async (args: Record<string, unknown>) => callTool("file.open", args),
);

server.registerTool(
  "mep_file_save",
  {
    description: "Save a buffer to disk. Omit path to save the active buffer to its own existing filename.",
    inputSchema: { path: z.string().optional() },
  },
  async (args: Record<string, unknown>) => callTool("file.save", args),
);

// --- Panes ----------------------------------------------------------------

server.registerTool(
  "mep_pane_split",
  {
    description: "Split the active pane, focusing the new one. Returns the new pane's id.",
    inputSchema: {
      dir: z.enum(["horizontal", "vertical"]).optional().describe("default horizontal"),
      file: z.string().optional().describe("file to open in the new pane; default reuses the current buffer"),
    },
  },
  async (args: Record<string, unknown>) => callTool("pane.split", args),
);

server.registerTool("mep_pane_close", { description: "Close the active pane." }, async () => callTool("pane.close", {}));

server.registerTool(
  "mep_pane_resize",
  {
    description: "Nudge the active pane's split-tree share in a direction.",
    inputSchema: { direction: z.string().describe("e.g. \"left\"/\"right\"/\"up\"/\"down\""), step: z.number().optional() },
  },
  async (args: Record<string, unknown>) => callTool("pane.resize", args),
);

server.registerTool(
  "mep_pane_focus",
  { description: "Make a specific pane (by id) the active one within its tab.", inputSchema: { pane_id: z.number().int() } },
  async (args: Record<string, unknown>) => callTool("pane.focus", args),
);

server.registerTool(
  "mep_pane_split_with_buffer",
  {
    description: "Move a buffer tab from one pane into a new split off another pane (drag-and-drop-onto-an-edge equivalent).",
    inputSchema: {
      source_pane_id: z.number().int(),
      buffer_id: z.number().int(),
      dest_pane_id: z.number().int(),
      dir: z.enum(["horizontal", "vertical"]),
      before: z.boolean().describe("place the new split before (left/top of) dest_pane_id, else after"),
    },
  },
  async (args: Record<string, unknown>) => callTool("pane.splitWithBuffer", args),
);

server.registerTool(
  "mep_pane_get",
  {
    description: "Get one pane's id/buffer_id/cursor/selection/scroll. Omit pane_id for the active pane.",
    inputSchema: { pane_id: z.number().int().optional() },
  },
  async (args: Record<string, unknown>) => callTool("pane.get", args),
);

// --- Commands / introspection / session ------------------------------------

server.registerTool(
  "mep_command_run",
  {
    description:
      "Run any mep `:` ex-command (without the leading colon), e.g. \"w\", \"s/foo/bar/g\", \"split\", \"qa!\". The general escape hatch for anything not covered by a more specific tool.",
    inputSchema: { cmd: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("command.run", args),
);

server.registerTool(
  "mep_session_info",
  {
    description:
      "Get this mep instance's pid, working directory (== the active workspace's root), active project/workspace names, workspace_root, git branch, and list of open file paths -- useful for telling multiple running instances apart.",
  },
  async () => callTool("session.info", {}),
);

server.registerTool(
  "mep_state_dump",
  {
    description:
      "Full editor state snapshot: every open buffer (id/filename/modified/line count), every tab's complete pane split-tree (layout, per-pane buffer/cursor/selection/scroll), and which tab/pane is active.",
  },
  async () => callTool("state.dump", {}),
);

// --- Events (M3) ------------------------------------------------------------

server.registerTool(
  "mep_poll_events",
  {
    description:
      "Drain and return every editor event (cursor moved, buffer changed, pane focus changed, mode changed, a notification fired) queued since the last call to this tool -- this connection's own event backlog, not a live stream. Call it periodically, or right after taking an action, to see what happened in the editor (including the human user's own activity, not just this agent's own actions) since you last checked.",
  },
  async () => toolResult(mep.drainEvents()),
);

// --- UI automation ("ui.*" methods, src/agent_ui_input.cpp) ----------------
// Drives mep's *actual* window like Playwright drives a browser: click,
// drag, type, scroll -- real X11 events (XTest) injected at the window's
// current screen position -- and read back what's on screen. Unlike
// every tool above (a thin wrapper over an Editor method that also works
// headless, e.g. under the collab relay or a script with no display),
// these need a real GUI window on a real X server; they're a no-op
// (mouse/key tools silently do nothing, mep_screenshot errors) if mep
// was built/run without one. Coordinates are window-client pixels -- the
// same space mep_screenshot's own image is in, and what mep's own UI
// code (RegisterClickRegion rectangles) uses -- so a screenshot's pixel
// coordinates line up directly with the x/y you'd pass here.

server.registerTool(
  "mep_screenshot",
  {
    description:
      "Capture mep's current window as a PNG and return its file path (read the file to see it). Takes no arguments. Coordinates in every other mep_mouse_*/mep_scroll tool are in this same pixel space.",
  },
  async () => callTool("ui.screenshot", {}),
);

server.registerTool(
  "mep_mouse_move",
  {
    description: "Move the mouse pointer to (x, y) in mep's window (no click).",
    inputSchema: { x: z.number().int(), y: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("ui.mouse_move", args),
);

server.registerTool(
  "mep_mouse_click",
  {
    description:
      "Move to (x, y) and click a mouse button there. Use clicks:2 for a double-click. For a plain click-and-hold-a-modifier (e.g. Shift-click), call mep_key_down first, then this, then mep_key_up.",
    inputSchema: {
      x: z.number().int(),
      y: z.number().int(),
      button: z.enum(["left", "middle", "right"]).default("left"),
      clicks: z.number().int().min(1).max(3).default(1),
    },
  },
  async (args: Record<string, unknown>) => callTool("ui.mouse_click", args),
);

server.registerTool(
  "mep_mouse_down",
  {
    description: "Press (and hold) a mouse button at (x, y). Pair with mep_mouse_up -- use this instead of mep_mouse_click to drag by hand with your own mep_mouse_move calls in between.",
    inputSchema: { x: z.number().int(), y: z.number().int(), button: z.enum(["left", "middle", "right"]).default("left") },
  },
  async (args: Record<string, unknown>) => callTool("ui.mouse_down", args),
);

server.registerTool(
  "mep_mouse_up",
  {
    description: "Release a mouse button at (x, y). See mep_mouse_down.",
    inputSchema: { x: z.number().int(), y: z.number().int(), button: z.enum(["left", "middle", "right"]).default("left") },
  },
  async (args: Record<string, unknown>) => callTool("ui.mouse_up", args),
);

server.registerTool(
  "mep_mouse_drag",
  {
    description:
      "Press a button at (x1, y1), move smoothly to (x2, y2) in `steps` increments, then release -- one call for a paint stroke, a slider drag, a selection drag, etc.",
    inputSchema: {
      x1: z.number().int(),
      y1: z.number().int(),
      x2: z.number().int(),
      y2: z.number().int(),
      button: z.enum(["left", "middle", "right"]).default("left"),
      steps: z.number().int().min(1).max(200).default(12),
    },
  },
  async (args: Record<string, unknown>) => callTool("ui.mouse_drag", args),
);

server.registerTool(
  "mep_scroll",
  {
    description: "Scroll the mouse wheel at (x, y). Positive delta scrolls up, negative scrolls down; each unit is one wheel click.",
    inputSchema: { x: z.number().int(), y: z.number().int(), delta: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("ui.scroll", args),
);

server.registerTool(
  "mep_key_press",
  {
    description:
      "Press and release one key: a single character (\"e\", \"[\", \"?\") or an X11 keysym name for anything without one (\"Escape\", \"Return\", \"Tab\", \"BackSpace\", \"Left\"/\"Right\"/\"Up\"/\"Down\", \"F1\"..\"F12\", \"Control_L\", \"Shift_L\", \"Alt_L\"). For an uppercase letter or shifted symbol, either pass it directly (Shift is applied automatically) or wrap with mep_key_down(\"Shift_L\")/mep_key_up(\"Shift_L\") for a held modifier across other calls (e.g. a Ctrl-click).",
    inputSchema: { key: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("ui.key_press", args),
);

server.registerTool(
  "mep_key_down",
  {
    description: "Press and hold one key (see mep_key_press for name syntax) without releasing it -- for held modifiers (\"Shift_L\", \"Control_L\", \"Alt_L\") spanning other mep_mouse_*/mep_key_* calls. Pair with mep_key_up.",
    inputSchema: { key: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("ui.key_down", args),
);

server.registerTool(
  "mep_key_up",
  {
    description: "Release a key previously held with mep_key_down.",
    inputSchema: { key: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("ui.key_up", args),
);

server.registerTool(
  "mep_type_text",
  {
    description: "Type a string one keystroke at a time (printable ASCII only; auto-shifts uppercase letters and symbols). For most editing, mep_buffer_insert_text is far more direct -- reach for this only when you specifically need real keystrokes, e.g. exercising mep's own key handling or a text field with no buffer-level API.",
    inputSchema: { text: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("ui.type_text", args),
);

// --- In-pane 3D modeler (MODEL3D.md) -------------------------------------
//
// Mirrors src/mcp_bridge.cpp's mep_model_* rows one-for-one (kept here on
// a best-effort basis per this file's own "superseded, fallback" framing
// above -- mcp_bridge.cpp is the source of truth). Unlike the raster
// image editor, this has a real scripting surface: a scene can be built,
// inspected, and saved through these tools alone, no mep_mouse_*/
// mep_screenshot automation needed. See MEP_AGENT_API.md's "in-pane 3D
// modeler" section for the full reference and a worked example.

const vec3Schema = z.object({ x: z.number().optional(), y: z.number().optional(), z: z.number().optional() }).optional();
const transformSchema = z
  .object({
    position: vec3Schema.describe("optional {x,y,z}"),
    rotation: vec3Schema.describe("optional {x,y,z}, Euler XYZ degrees"),
    scale: vec3Schema.describe("optional {x,y,z}"),
  })
  .optional();

server.registerTool(
  "mep_model_new",
  {
    description:
      'Create a fresh, empty 3D-modeler scene (no source file needed) and switch to it -- the "build from scratch" entry point. Returns the new buffer\'s id.',
  },
  async () => callTool("model.new", {}),
);

server.registerTool(
  "mep_model_list_objects",
  {
    description:
      "List every object in a 3D-modeler scene: id, name, visibility, position/rotation/scale, base color, and triangle count.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.listObjects", args),
);

server.registerTool(
  "mep_model_scene_stats",
  {
    description: "Get a 3D-modeler scene's object count and total triangle count.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.sceneStats", args),
);

server.registerTool(
  "mep_model_primitive_info",
  {
    description:
      "Look up a primitive kind's pivot point and default dimensions (e.g. cylinder/cone are base-pivoted and extend +Y, not centered) -- pass kind for just that one, or omit it to get every kind at once. No buffer_id needed, this is static reference data, not scene state.",
    inputSchema: { kind: z.enum(["cube", "sphere", "cylinder", "cone", "plane", "torus", "wedge"]).optional() },
  },
  async (args: Record<string, unknown>) => callTool("model.primitiveInfo", args),
);

server.registerTool(
  "mep_model_add_primitive",
  {
    description:
      "Add a procedurally generated primitive object (cube/sphere/cylinder/cone/plane/torus/wedge) to a 3D-modeler scene, optionally setting its initial transform. Returns the new object's id.",
    inputSchema: {
      buffer_id: z.number().int(),
      kind: z.enum(["cube", "sphere", "cylinder", "cone", "plane", "torus", "wedge"]),
      transform: transformSchema,
    },
  },
  async (args: Record<string, unknown>) => callTool("model.addPrimitive", args),
);

server.registerTool(
  "mep_model_delete_object",
  {
    description:
      "Delete an object from a 3D-modeler scene. cascade (default false) also deletes every transitive descendant instead of just un-parenting them -- a real 'delete this group and everything in it.'",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), cascade: z.boolean().optional() },
  },
  async (args: Record<string, unknown>) => callTool("model.deleteObject", args),
);

server.registerTool(
  "mep_model_duplicate_object",
  {
    description:
      "Duplicate an object (same mesh, transform, and material) in a 3D-modeler scene. Returns the new object's id. cascade (default false) also duplicates every transitive descendant, re-parented to mirror the original hierarchy under the new copy -- a real 'duplicate this group and everything in it,' rather than just the one top-level node (whose children would otherwise still point at the original).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), cascade: z.boolean().optional() },
  },
  async (args: Record<string, unknown>) => callTool("model.duplicateObject", args),
);

server.registerTool(
  "mep_model_set_transform",
  {
    description:
      "Set an object's position/rotation/scale in a 3D-modeler scene -- each of the three is optional, only the ones given are changed.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      position: vec3Schema,
      rotation: vec3Schema.describe("Euler XYZ, degrees"),
      scale: vec3Schema,
    },
  },
  async (args: Record<string, unknown>) => callTool("model.setTransform", args),
);

server.registerTool(
  "mep_model_set_material",
  {
    description: "Set an object's base color (0..1 floats; a defaults to 1.0) in a 3D-modeler scene.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      color: z.object({ r: z.number(), g: z.number(), b: z.number(), a: z.number().optional() }),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.setMaterial", args),
);

server.registerTool(
  "mep_model_set_texture",
  {
    description:
      "Set (or, with an empty path, clear) an object's base-color/albedo texture in a 3D-modeler scene, loaded from an image file (PNG/JPG/BMP/...). The texture is sampled and then tinted by the object's own mep_model_set_material color, same as glTF's baseColorTexture + baseColorFactor. No normal/metallic-roughness/emissive maps -- this is base color only.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), path: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("model.setTexture", args),
);

server.registerTool(
  "mep_model_rename_object",
  {
    description: "Rename an object in a 3D-modeler scene.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), name: z.string() },
  },
  async (args: Record<string, unknown>) => callTool("model.renameObject", args),
);

server.registerTool(
  "mep_model_set_visible",
  {
    description: "Show or hide an object in a 3D-modeler scene (kept, not deleted).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), visible: z.boolean() },
  },
  async (args: Record<string, unknown>) => callTool("model.setVisible", args),
);

server.registerTool(
  "mep_model_select",
  {
    description:
      "Replace the current selection in a 3D-modeler scene (silently drops any object_id that doesn't exist). Not an undoable edit.",
    inputSchema: { buffer_id: z.number().int(), object_ids: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.select", args),
);

server.registerTool(
  "mep_model_get_selection",
  {
    description: "Get the currently selected object ids in a 3D-modeler scene.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.getSelection", args),
);

server.registerTool(
  "mep_model_camera_set",
  {
    description:
      "Update a 3D-modeler pane's orbit camera (target/yaw/pitch/distance/fov) -- each field is optional, only the ones given are changed. Not an undoable edit.",
    inputSchema: {
      buffer_id: z.number().int(),
      target: vec3Schema,
      yaw: z.number().optional().describe("degrees"),
      pitch: z.number().optional().describe("degrees, clamped to [-89,89]"),
      distance: z.number().optional(),
      fov: z.number().optional().describe("degrees"),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.cameraSet", args),
);

server.registerTool(
  "mep_model_camera_get",
  {
    description: "Get a 3D-modeler pane's current orbit camera state.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.cameraGet", args),
);

server.registerTool(
  "mep_model_undo",
  {
    description: "Undo the last edit in a 3D-modeler scene.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.undo", args),
);

server.registerTool(
  "mep_model_redo",
  {
    description: "Redo the last undone edit in a 3D-modeler scene.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.redo", args),
);

server.registerTool(
  "mep_model_render_to_image",
  {
    description:
      "Render a 3D-modeler scene's viewport to a PNG file -- a clean render (no selection outline, no menubar/sidebars) at whatever resolution you ask for, unlike mep_screenshot which always captures the whole mep window. Needs the real GUI window (same requirement as mep_screenshot/mep_mouse_*).",
    inputSchema: {
      buffer_id: z.number().int(),
      path: z.string().describe("destination PNG path"),
      width: z.number().int().optional().describe("default 1024, clamped to [16,4096]"),
      height: z.number().int().optional().describe("default 768, clamped to [16,4096]"),
      transparent: z.boolean().optional().describe("clear to a transparent background instead of the theme background; default false"),
      show_grid: z.boolean().optional().describe("default: the pane's own current grid setting"),
      wireframe: z.boolean().optional().describe("default: the pane's own current wireframe setting"),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.renderToImage", args),
);

server.registerTool(
  "mep_model_set_view",
  {
    description:
      "Set a 3D-modeler pane's grid/wireframe/snap view toggles -- each field optional, only the ones given are changed. Not an undoable edit. snap rounds subsequent Move/Rotate/Scale drags to a fixed grid/angle/scale step.",
    inputSchema: {
      buffer_id: z.number().int(),
      show_grid: z.boolean().optional(),
      wireframe: z.boolean().optional(),
      snap: z.boolean().optional(),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.setView", args),
);

server.registerTool(
  "mep_model_frame_all",
  {
    description:
      "Reframe a 3D-modeler pane's orbit camera (target + distance) to fit the whole scene's true world bounds -- yaw/pitch are left as they are. Not an undoable edit.",
    inputSchema: { buffer_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.frameAll", args),
);

const modelTransformUpdateSchema = z.object({
  object_id: z.number().int(),
  position: vec3Schema,
  rotation: vec3Schema.describe("Euler XYZ, degrees"),
  scale: vec3Schema,
});

server.registerTool(
  "mep_model_set_transforms",
  {
    description:
      "Set position/rotation/scale on many objects in one call (each entry's fields are independently optional, only given ones are changed). Returns how many updates were actually applied.",
    inputSchema: { buffer_id: z.number().int(), updates: z.array(modelTransformUpdateSchema) },
  },
  async (args: Record<string, unknown>) => callTool("model.setTransforms", args),
);

const modelMaterialUpdateSchema = z.object({
  object_id: z.number().int(),
  color: z.object({ r: z.number(), g: z.number(), b: z.number(), a: z.number().optional() }),
});

server.registerTool(
  "mep_model_set_materials",
  {
    description: "Set base color on many objects in one call. Returns how many updates were actually applied.",
    inputSchema: { buffer_id: z.number().int(), updates: z.array(modelMaterialUpdateSchema) },
  },
  async (args: Record<string, unknown>) => callTool("model.setMaterials", args),
);

server.registerTool(
  "mep_model_delete_objects",
  {
    description:
      "Delete many objects from a 3D-modeler scene in one call. Returns how many were actually deleted. " +
      "Optional cascade (default false, applied to every object_id) also deletes each one's whole " +
      "descendant subtree, same as mep_model_delete_object's own cascade flag.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_ids: z.array(z.number().int()),
      cascade: z.boolean().optional(),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.deleteObjects", args),
);

server.registerTool(
  "mep_model_duplicate_mirrored",
  {
    description:
      "Duplicate an object with its position mirrored across the given world axis through the origin, reflecting the copy's own rotation to match (exact for a simple single-axis rotation -- a compound rotation may need a manual touch-up afterward). Returns the new object's id.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), axis: z.enum(["x", "y", "z"]) },
  },
  async (args: Record<string, unknown>) => callTool("model.duplicateMirrored", args),
);

server.registerTool(
  "mep_model_radial_array",
  {
    description:
      "Duplicate an object count-1 times, evenly spaced in a ring around the given axis through the origin -- e.g. a fin offset on X, arrayed 4x around Y, lands one at each 90-degree step, each still facing outward the way the original did. The original object is left as-is and not counted. Returns the new objects' ids, in order.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      count: z.number().int().describe("total copies including the original -- this many minus one new objects are created"),
      axis: z.enum(["x", "y", "z"]),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.radialArray", args),
);

server.registerTool(
  "mep_model_group_objects",
  {
    description:
      "Create a new empty group node (no mesh, invisible in the viewport, positioned at the centroid of the grouped objects) and parent every object in object_ids under it. Object3D.parent is purely an organizational/group-move link -- it is never composed into a child's own transform, so grouping does not move or change how anything renders. Returns the new group's object id.",
    inputSchema: { buffer_id: z.number().int(), object_ids: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.groupObjects", args),
);

server.registerTool(
  "mep_model_set_parent",
  {
    description:
      "Set (or clear) one object's parent, for Outliner grouping/nesting and Move-tool group-drag cascading. Fails (ok: false) on a nonexistent object/parent, parent_id == object_id, or a parent_id that's already a descendant of object_id (would create a cycle).",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      parent_id: z.number().int().optional().describe("-1 (or omit) to clear/un-parent"),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.setParent", args),
);

server.registerTool(
  "mep_model_list_vertices",
  {
    description:
      "List every vertex of an object's mesh: index, local-space (pre-object-transform) x/y/z, and (when the mesh has normals) nx/ny/nz. The first real vertex-level mesh-editing primitive -- combine with mep_model_set_vertex_position to nudge individual vertices instead of only whole-object transforms.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.listVertices", args),
);

server.registerTool(
  "mep_model_list_triangles",
  {
    description:
      "List every triangle of an object's mesh: index and the vertex-unit indices (a/b/c) of its 3 corners. Read-only mesh-connectivity introspection -- without this, there's no way to discover which vertices actually form a triangle together (the thing mep_model_subdivide_faces/extrude_faces/inset_faces/dissolve_vertex all key off of) besides positions alone. Combine with mep_model_list_vertices to find, e.g., which vertex triples share the same position (candidates for mep_model_merge_vertices) or which edges only appear in one triangle (a mesh boundary).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.listTriangles", args),
);

server.registerTool(
  "mep_model_set_vertex_position",
  {
    description:
      "Set one vertex's local-space (pre-object-transform) position on an object's mesh. If the object currently shares its mesh with another object (a radial array, a mirrored duplicate, a multi-object import), it's transparently given its own private copy first, so this never deforms other objects. vertex_index is in vertex units (from mep_model_list_vertices or mep_model_list_objects' own vertex_count), not a raw float offset.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      vertex_index: z.number().int(),
      position: z.object({ x: z.number(), y: z.number(), z: z.number() }),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.setVertexPosition", args),
);

server.registerTool(
  "mep_model_delete_vertices",
  {
    description:
      "Delete the given vertices (vertex-units indices) and every triangle referencing any of them from an object's mesh -- leaves a hole rather than retriangulating/filling it, and does not attempt to reconnect the surrounding geometry. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Out-of-range/duplicate indices are harmless no-ops.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), vertex_indices: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.deleteVertices", args),
);

server.registerTool(
  "mep_model_merge_vertices",
  {
    description:
      "Weld the given vertices (vertex-units indices) of an object's mesh into a single vertex at their averaged position/normal/texcoord -- any triangle that becomes degenerate as a result (two or more of its corners now the same vertex) is dropped rather than kept as zero-area. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Out-of-range/duplicate indices are harmless no-ops; fewer than 2 distinct valid indices is a no-op (nothing to merge).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), vertex_indices: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.mergeVertices", args),
);

server.registerTool(
  "mep_model_recalculate_normals",
  {
    description:
      "Recompute an object's mesh's per-vertex normals from its current triangle geometry (smooth -- each vertex's normal is the normalized, area-weighted average of every adjacent triangle's own normal). Has zero visible effect on this app's own rendering (its default shaders never read vertex normals) -- use it to fix up normals left stale by mep_model_set_vertex_position/delete_vertices/merge_vertices before exporting for a tool that does read them, e.g. Blender or a glTF viewer with real lighting. Safely clones a shared mesh first, same as mep_model_set_vertex_position.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.recalculateNormals", args),
);

server.registerTool(
  "mep_model_subdivide_faces",
  {
    description:
      "Centroid-subdivide every triangle of an object's mesh whose all 3 corners are in vertex_indices -- this app's stand-in for real face-selection tooling (same convention as merge/extrude): a 'face' here just means whichever triangles are fully covered by the given vertex set. Each such triangle gets one new vertex at its centroid and is replaced by 3 new triangles fanning out to it; a triangle with fewer than all 3 corners selected is left untouched. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns the newly-created centroid vertex indices (empty if no triangle was fully covered).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), vertex_indices: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.subdivideFaces", args),
);

server.registerTool(
  "mep_model_extrude_faces",
  {
    description:
      "Extrude the face formed by every triangle of an object's mesh whose all 3 corners are in vertex_indices (same 'face' convention as mep_model_subdivide_faces), by distance along that face's own geometrically-derived normal -- not read from stored per-vertex normals, which may be stale or absent. Every vertex used by a selected triangle is duplicated and offset; the selected triangles are re-pointed at the duplicates (lifting the cap into place) while a wall quad connects the untouched original ring to the new one along each boundary edge of the selected group (an edge shared between two selected triangles, e.g. a face's own diagonal, is correctly left un-walled). A vertex also used by a triangle outside the selection (e.g. a cube's adjacent side face sharing a top corner) naturally stays attached there too. Extrusion follows actual mesh connectivity, not spatial adjacency -- a raylib-generated primitive's unwelded per-face vertices (see mep_model_merge_vertices) mean selecting an entire such mesh extrudes each of its faces independently; weld first if that's not wanted. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns the new cap vertex indices (empty if no triangle was fully covered, or the selection was degenerate/zero-area) so the caller can immediately continue editing the just-extruded face.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      vertex_indices: z.array(z.number().int()),
      distance: z.number(),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.extrudeFaces", args),
);

server.registerTool(
  "mep_model_dissolve_vertex",
  {
    description:
      "Remove one vertex from an object's mesh and patch the surrounding faces back together, unlike mep_model_delete_vertices' own deliberately-left hole. Only works cleanly on a proper interior vertex whose incident triangles form a single closed fan around it; when they don't (a mesh-boundary vertex, a non-manifold fan, or an isolated vertex with no incident triangles) this silently falls back to the same hole-leaving removal mep_model_delete_vertices does, rather than risk a malformed retriangulation -- there's no way to tell from the response which path was taken besides comparing triangle counts before/after. The retriangulation is a simple fan (not a 'nicest possible' one), so a very non-convex surrounding ring can produce a visibly thin sliver triangle or two. Safely clones a shared mesh first, same as mep_model_set_vertex_position.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), vertex_index: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.dissolveVertex", args),
);

server.registerTool(
  "mep_model_inset_faces",
  {
    description:
      "Inset the face formed by every triangle of an object's mesh whose all 3 corners are in vertex_indices (same 'face' convention as mep_model_subdivide_faces/mep_model_extrude_faces): every vertex the face uses is duplicated and moved toward the face group's own centroid (the average position of every vertex it uses) by amount, a 0..1 fraction (clamped) -- 0 is a degenerate zero-width inset, 1 fully collapses the new cap onto the centroid. The selected triangles are re-pointed at the duplicates (shrinking the cap in place, no lift along any normal, unlike mep_model_extrude_faces), and a wall quad connects the untouched original ring to the new shrunk one along each boundary edge of the group (an edge shared between two selected triangles, e.g. a face's own diagonal, is correctly left un-walled). Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns the new cap vertex indices (empty if no triangle was fully covered) -- chain straight into mep_model_extrude_faces on the returned indices for a raised-platform-with-border look.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      vertex_indices: z.array(z.number().int()),
      amount: z.number(),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.insetFaces", args),
);

server.registerTool(
  "mep_model_add_vertex",
  {
    description:
      "Append one new, isolated vertex to an object's mesh at the given local-space (pre-object-transform) position -- no triangle references it, so it won't render until connected via mep_model_make_face or similar. The deliberate counterpart to subdivide/extrude/inset (which all work on existing triangles): this is how to build genuinely new geometry from scratch. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns the new vertex's index (vertex units), or -1 if the object doesn't exist or has no mesh.",
    inputSchema: {
      buffer_id: z.number().int(),
      object_id: z.number().int(),
      position: z.object({ x: z.number(), y: z.number(), z: z.number() }),
    },
  },
  async (args: Record<string, unknown>) => callTool("model.addVertex", args),
);

server.registerTool(
  "mep_model_make_face",
  {
    description:
      "Create new triangle(s) of an object's mesh connecting existing vertices -- fan-triangulated from the first of vertex_indices (3 vertices become 1 new triangle, 4 become 2, a pentagon 3), Blender's own 'Make Edge/Face' (F key) in spirit. Unlike mep_model_subdivide_faces/extrude_faces/inset_faces, the given vertices don't need to already form a triangle -- this is how to connect vertices (including ones just added via mep_model_add_vertex) that aren't adjacent yet. No new vertices are created, and no check is made for whether the resulting triangle(s) duplicate or overlap ones that already exist; winding (and so which side ends up 'front') follows the given vertex order. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns false if fewer than 3 distinct valid vertices remain after filtering duplicates/out-of-range entries.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), vertex_indices: z.array(z.number().int()) },
  },
  async (args: Record<string, unknown>) => callTool("model.makeFace", args),
);

server.registerTool(
  "mep_model_merge_by_distance",
  {
    description:
      "Automatically weld every group of an object's mesh's vertices whose positions are all mutually within threshold of each other -- Blender's own 'Merge by Distance'/'Remove Doubles', and the 'just fix all of them' counterpart to mep_model_merge_vertices (which needs the caller to already know which indices are duplicates). Especially useful right after importing/generating a primitive, since raylib's own generated meshes emit unwelded duplicate vertices at every shared corner. threshold=0 welds only exact (bit-identical) position duplicates; grouping is transitive through a chain of close-enough pairs. Each group is welded to its averaged position/normal/texcoord, and a triangle that becomes degenerate as a result is dropped, same as mep_model_merge_vertices. Safely clones a shared mesh first, same as mep_model_set_vertex_position. Returns how many vertices were removed (0 if nothing was within threshold of anything else).",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int(), threshold: z.number() },
  },
  async (args: Record<string, unknown>) => callTool("model.mergeByDistance", args),
);

server.registerTool(
  "mep_model_flip_normals",
  {
    description:
      "Reverse every triangle's winding and negate every vertex normal of an object's mesh, flipping which side renders as 'front' -- the fix for geometry that came out inside-out, e.g. a mep_model_make_face call given vertices in the wrong order, or some imported files. Operates on the whole mesh, not a selection. Safely clones a shared mesh first, same as mep_model_set_vertex_position.",
    inputSchema: { buffer_id: z.number().int(), object_id: z.number().int() },
  },
  async (args: Record<string, unknown>) => callTool("model.flipNormals", args),
);

// Best-effort: identify ourselves right away so the human sees a real
// name from the start rather than the server-side default of "AI Agent"
// -- but don't let a failure here (mep not running yet, socket discovery
// ambiguous) crash the MCP server itself. A tool call will surface that
// same error clearly to the model when it actually tries to do something.
//
// MEP_TERMINAL_BUFFER is set by mep itself in every `:terminal` it opens
// (Editor::TerminalSpawn) and inherited down through claude to this
// process -- reporting it pairs this agent with the terminal pane it's
// running in, which is what mep's AI-agents sidebar jumps to. Absent
// (agent started from an outside terminal) it's simply not sent.
try {
  const identity: Record<string, unknown> = { name: Deno.env.get("MEP_AGENT_NAME") ?? "Claude" };
  const terminalBuffer = Number(Deno.env.get("MEP_TERMINAL_BUFFER") ?? "");
  if (Number.isInteger(terminalBuffer) && terminalBuffer >= 0) identity.terminal_buffer_id = terminalBuffer;
  await mep.call("session.identify", identity);
} catch (err) {
  // Not fatal -- see comment above -- but still worth a trace: this is the
  // one failure mode with no other visible symptom (mep's tab-bar agent
  // chip just silently never appears), so a silent catch here left users
  // with no way to find out why. Goes to the MCP server's stderr, which
  // the client generally logs even though it isn't shown inline.
  console.error(`mep-agent: session.identify failed at startup: ${err instanceof Error ? err.message : err}`);
}

await server.connect(new StdioServerTransport());
