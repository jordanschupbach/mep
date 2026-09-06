// MCP server wrapping mep's agent-control socket (src/agent_rpc.cpp, see
// AGENT_RPC_PLAN.md) as normal MCP tools -- lets Claude (or any MCP
// client) drive a running mep instance: move/split/resize/focus panes,
// move the cursor, read/edit buffer text, open/save files, run arbitrary
// ex-commands, inspect full editor state, and poll for live events
// (cursor/buffer/pane/mode changes, notifications).
//
// Every tool here is a thin 1:1 wrapper around one JSON-RPC method the
// C++ side already implements -- no new behavior, just another way to
// reach the same primitives the embedded-Lua `mep.*` API and the raw
// Unix-socket protocol already expose. See AGENT_RPC_PLAN.md's M1/M2/M3
// sections for the full method table this mirrors.
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
