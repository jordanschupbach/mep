// mcp/server_test.ts: spawns a real mep instance AND the real MCP server
// wrapping it, then drives the server exactly as an MCP client (Claude
// Code, Claude Desktop, ...) would -- list tools, call several, confirm
// real results against the real running editor. Mirrors
// src/agent_rpc_test.cpp's own shape and reasoning (real transport, no
// mocking, no assert()-style checks that can silently no-op) one layer
// up the stack. Needs a live display (native `mep` requires one), same
// as that C++ test.
//
// Run with:
//   deno test --allow-net --allow-read --allow-env --allow-run mcp/server_test.ts
// Set MEP_BINARY to override the default ../build/native/mep path.

import { Client } from "npm:@modelcontextprotocol/sdk@^1.30/client/index.js";
import { StdioClientTransport } from "npm:@modelcontextprotocol/sdk@^1.30/client/stdio.js";
import { mepAgentSocketDir } from "./mep_client.ts";

interface ToolTextResult {
  isError?: boolean;
  content: Array<{ type: string; text?: string }>;
}

function parseToolJson(result: ToolTextResult): unknown {
  const text = result.content.find((c) => c.type === "text")?.text;
  if (text === undefined) throw new Error(`tool result had no text content: ${JSON.stringify(result)}`);
  return JSON.parse(text);
}

function assertNotError(result: ToolTextResult, label: string): void {
  if (result.isError) throw new Error(`${label} returned a tool error: ${JSON.stringify(result)}`);
}

async function waitForSocket(pid: number, timeoutMs = 10000): Promise<string> {
  const path = `${mepAgentSocketDir()}/${pid}.sock`;
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      await Deno.stat(path);
      return path;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  throw new Error(`mep never bound its agent socket at ${path} within ${timeoutMs}ms`);
}

Deno.test("mep MCP server drives a real mep instance end-to-end", async () => {
  const mepBinary = Deno.env.get("MEP_BINARY") ?? new URL("../build/native/mep", import.meta.url).pathname;
  // "null" for both, not "piped": nothing in this test ever reads mep's
  // stdout/stderr, and mep prints a lot of raylib boot logging -- an
  // unconsumed "piped" stream backs up the OS pipe buffer and blocks the
  // child's own writes once it fills, which would hang mep itself.
  // --no-session: a test run must neither inherit nor overwrite the real
  // per-project workspace session file for this checkout.
  const mepProcess = new Deno.Command(mepBinary, { args: ["--no-session"], stdout: "null", stderr: "null" }).spawn();

  try {
    const socketPath = await waitForSocket(mepProcess.pid);

    const transport = new StdioClientTransport({
      command: "deno",
      args: ["run", "--allow-net", "--allow-read", "--allow-write", "--allow-env", new URL("./server.ts", import.meta.url).pathname],
      env: { MEP_AGENT_SOCKET: socketPath },
    });
    const client = new Client({ name: "mep-test-client", version: "0.0.1" });
    await client.connect(transport);

    try {
      const tools = await client.listTools();
      const names = tools.tools.map((t: { name: string }) => t.name);
      if (!names.includes("mep_cursor_set")) throw new Error(`expected mep_cursor_set among tools: ${names.join(", ")}`);
      if (!names.includes("mep_identify")) throw new Error(`expected mep_identify among tools: ${names.join(", ")}`);
      if (tools.tools.length < 31) throw new Error(`expected ~33 tools, got only ${tools.tools.length}: ${names.join(", ")}`);
      for (const t of ["mep_workspace_list", "mep_workspace_switch", "mep_workspace_create", "mep_workspace_delete", "mep_project_list", "mep_project_switch", "mep_project_open"]) {
        if (!names.includes(t)) throw new Error(`expected ${t} among tools: ${names.join(", ")}`);
      }

      // --- Workspaces (WORKSPACES_PLAN.md Phase 11): the freshly launched
      // instance has exactly one project with one primary "main" workspace;
      // session.info and buffer.list carry the workspace fields.
      const wsList = parseToolJson(
        await client.callTool({ name: "mep_workspace_list", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: number; name: string; primary: boolean; active: boolean }>;
      if (wsList.length !== 1 || wsList[0].name !== "main" || !wsList[0].primary || !wsList[0].active) {
        throw new Error(`expected a single active primary "main" workspace, got: ${JSON.stringify(wsList)}`);
      }
      const projects = parseToolJson(
        await client.callTool({ name: "mep_project_list", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: number; name: string; root: string; workspace_count: number; active: boolean }>;
      if (projects.length !== 1 || projects[0].workspace_count !== 1 || !projects[0].active) {
        throw new Error(`expected one active project with one workspace, got: ${JSON.stringify(projects)}`);
      }
      const info = parseToolJson(
        await client.callTool({ name: "mep_session_info", arguments: {} }) as ToolTextResult,
      ) as { cwd: string; workspace: string; workspace_root: string; project: string };
      if (info.workspace !== "main" || info.workspace_root !== info.cwd || info.project !== projects[0].name) {
        throw new Error(`session.info should describe the main workspace (cwd == workspace_root), got: ${JSON.stringify(info)}`);
      }
      const scopedBuffers = parseToolJson(
        await client.callTool({ name: "mep_buffer_list", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: number; workspace_id: number }>;
      const allBuffers = parseToolJson(
        await client.callTool({ name: "mep_buffer_list", arguments: { workspace: "all" } }) as ToolTextResult,
      ) as Array<{ id: number; workspace_id: number }>;
      if (scopedBuffers.length === 0 || allBuffers.length < scopedBuffers.length) {
        throw new Error(`buffer.list scoping looks wrong: scoped=${JSON.stringify(scopedBuffers)} all=${JSON.stringify(allBuffers)}`);
      }
      const noWs = await client.callTool({ name: "mep_workspace_switch", arguments: { name: "no-such-workspace" } }) as ToolTextResult;
      if (noWs.isError !== true) throw new Error(`switching to a nonexistent workspace should be a tool error, got: ${JSON.stringify(noWs)}`);

      // server.ts auto-identifies on connect (default name "Claude" unless
      // MEP_AGENT_NAME is set) -- confirm mep_identify can rename that
      // same participant mid-session too.
      const identify = parseToolJson(
        await client.callTool({ name: "mep_identify", arguments: { name: "Test Agent" } }) as ToolTextResult,
      ) as { participant_id: string; name: string };
      if (identify.name !== "Test Agent") throw new Error(`expected mep_identify to report the new name, got: ${JSON.stringify(identify)}`);

      assertNotError(await client.callTool({ name: "mep_cursor_set", arguments: { row: 0, col: 0 } }), "mep_cursor_set");
      assertNotError(await client.callTool({ name: "mep_buffer_insert_text", arguments: { text: "mcp test" } }), "mep_buffer_insert_text");

      const cursor = parseToolJson(
        await client.callTool({ name: "mep_cursor_get", arguments: {} }) as ToolTextResult,
      ) as { row: number; col: number };
      if (cursor.col !== 8) throw new Error(`expected cursor col 8 after inserting "mcp test", got ${JSON.stringify(cursor)}`);

      const dump = parseToolJson(
        await client.callTool({ name: "mep_state_dump", arguments: {} }) as ToolTextResult,
      ) as { buffers: unknown[] };
      if (!Array.isArray(dump.buffers) || dump.buffers.length < 1) {
        throw new Error(`expected state.dump to report at least one buffer, got: ${JSON.stringify(dump)}`);
      }

      assertNotError(await client.callTool({ name: "mep_pane_split", arguments: { dir: "vertical" } }), "mep_pane_split");

      // Give PollOnce a few frames to compute/send the resulting events --
      // same "events can lag slightly behind the response" reasoning as
      // agent_rpc_test.cpp's own DrainEvents helper.
      await new Promise((resolve) => setTimeout(resolve, 300));
      const events = parseToolJson(
        await client.callTool({ name: "mep_poll_events", arguments: {} }) as ToolTextResult,
      ) as Array<{ method: string }>;
      if (!events.some((e) => e.method === "event.paneFocusChanged")) {
        throw new Error(`expected event.paneFocusChanged among polled events after pane.split, got: ${JSON.stringify(events)}`);
      }

      const badBuffer = await client.callTool({ name: "mep_buffer_get_lines", arguments: { buffer_id: 9999 } }) as ToolTextResult;
      if (badBuffer.isError !== true) throw new Error(`expected a bad buffer_id to be a tool error, got: ${JSON.stringify(badBuffer)}`);

      // mep_buffer_insert_text above should have already auto-set status
      // to "writing" (COLLAB_CURSORS_PLAN.md Phase 1g) -- confirm it's
      // visible via mep_list_participants before overriding it explicitly.
      const participantsAfterWrite = parseToolJson(
        await client.callTool({ name: "mep_list_participants", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: string; name: string; status: string }>;
      const self = participantsAfterWrite.find((p) => p.name === "Test Agent");
      if (!self) throw new Error(`expected to find "Test Agent" in mep_list_participants, got: ${JSON.stringify(participantsAfterWrite)}`);
      if (self.status !== "writing") throw new Error(`expected auto-status "writing" after an edit, got: ${JSON.stringify(self)}`);

      assertNotError(await client.callTool({ name: "mep_set_status", arguments: { status: "awaiting_input" } }), "mep_set_status");
      const participantsAfterStatus = parseToolJson(
        await client.callTool({ name: "mep_list_participants", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: string; status: string }>;
      const selfAfterStatus = participantsAfterStatus.find((p) => p.id === self.id);
      if (selfAfterStatus?.status !== "awaiting_input") {
        throw new Error(`expected mep_set_status("awaiting_input") to stick, got: ${JSON.stringify(selfAfterStatus)}`);
      }

      const badStatus = await client.callTool({ name: "mep_set_status", arguments: { status: "not-a-real-status" } }) as ToolTextResult;
      if (badStatus.isError !== true) throw new Error(`expected an unknown status to be a tool error, got: ${JSON.stringify(badStatus)}`);

      // --- Status auto-nudge (bug report: after "done", the badge never
      // moved again on the next question in the same conversation
      // because nothing forces the model to remember calling
      // mep_set_status("thinking") itself first). Reaching "done" should
      // NOT get undone by pure introspection (mep_state_dump), but SHOULD
      // get cleared to "thinking" by the next real action.
      assertNotError(await client.callTool({ name: "mep_set_status", arguments: { status: "done" } }), "mep_set_status done");
      await client.callTool({ name: "mep_state_dump", arguments: {} }); // read-only -- must NOT clear "done"
      const participantsStillDone = parseToolJson(
        await client.callTool({ name: "mep_list_participants", arguments: {} }) as ToolTextResult, // also read-only
      ) as Array<{ id: string; status: string }>;
      const stillDone = participantsStillDone.find((p) => p.id === self.id);
      if (stillDone?.status !== "done") {
        throw new Error(`read-only tool calls should not clear a "done" status, got: ${JSON.stringify(stillDone)}`);
      }

      assertNotError(await client.callTool({ name: "mep_cursor_set", arguments: { row: 0, col: 0 } }), "mep_cursor_set after done");
      const participantsAfterNudge = parseToolJson(
        await client.callTool({ name: "mep_list_participants", arguments: {} }) as ToolTextResult,
      ) as Array<{ id: string; status: string }>;
      const nudged = participantsAfterNudge.find((p) => p.id === self.id);
      if (nudged?.status !== "thinking") {
        throw new Error(`expected a real action after "done" to auto-nudge status to "thinking", got: ${JSON.stringify(nudged)}`);
      }

      assertNotError(await client.callTool({ name: "mep_command_run", arguments: { cmd: "qa!" } }), "mep_command_run qa!");
    } finally {
      await client.close();
    }

    const status = await mepProcess.status;
    if (status.code !== 0) throw new Error(`mep exited with code ${status.code}, expected a clean 0`);
    let socketStillExists = true;
    try {
      await Deno.stat(socketPath);
    } catch (err) {
      if (err instanceof Deno.errors.NotFound) socketStillExists = false;
      else throw err;
    }
    if (socketStillExists) throw new Error(`mep's socket file ${socketPath} should have been unlinked on clean exit`);
  } catch (err) {
    try {
      mepProcess.kill("SIGKILL");
    } catch {
      // already exited
    }
    throw err;
  }
});
