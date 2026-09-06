// Connects to a running mep instance's agent-control Unix socket
// (src/agent_rpc.cpp) and exposes it as a plain call()/drainEvents() API
// for server.ts's tool handlers to use. One persistent connection is
// reused across every tool call for this MCP server's whole lifetime
// (lazily established on the first call, reconnecting on demand after a
// disconnect) -- the socket also carries M3's pushed event.* notifications,
// which only a *held-open* connection can receive at all.

import { frameMessage, RpcFramer } from "./rpc_framing.ts";

export interface MepEvent {
  method: string;
  params: unknown;
}

// Mirrors src/persist.h's MepDataDir()/MepAgentSocketDir() -- this Deno
// process (not mep's own wasm sandbox) is the one with real env var/
// filesystem access to resolve it, same reasoning as launcher/serve.ts's
// own mepDataDir() re-implementation for the same underlying path.
export function mepAgentSocketDir(): string {
  const xdg = Deno.env.get("XDG_DATA_HOME");
  if (xdg) return `${xdg}/mep/agent-sockets`;
  const home = Deno.env.get("HOME");
  if (!home) throw new Error("cannot determine home directory (no $HOME/$XDG_DATA_HOME)");
  return Deno.build.os === "darwin"
    ? `${home}/Library/Application Support/mep/agent-sockets`
    : `${home}/.local/share/mep/agent-sockets`;
}

// `MEP_AGENT_SOCKET` (an explicit path) always wins. Otherwise: exactly
// one `*.sock` in the agent-sockets directory is used automatically;
// zero or several are both errors with a message telling the caller how
// to resolve it -- several running mep windows is a real, expected case,
// not something to guess at silently.
async function discoverSocketPath(): Promise<string> {
  const override = Deno.env.get("MEP_AGENT_SOCKET");
  if (override) return override;

  const dir = mepAgentSocketDir();
  const candidates: string[] = [];
  try {
    for await (const entry of Deno.readDir(dir)) {
      // Unix domain socket dirents are neither isFile, isDirectory, nor
      // isSymlink under Deno.readDir (it only sets those from DT_REG/DT_DIR/
      // DT_LNK) -- so `entry.isFile` is always false for the very *.sock
      // files this is meant to find. Match on the name only.
      if (entry.name.endsWith(".sock")) candidates.push(`${dir}/${entry.name}`);
    }
  } catch (err) {
    if (err instanceof Deno.errors.NotFound) {
      throw new Error(`no mep agent sockets found (${dir} doesn't exist) -- is mep running? it must be a native build, not wasm`);
    }
    throw err;
  }
  if (candidates.length === 0) {
    throw new Error(`no mep agent sockets found in ${dir} -- is mep running? it must be a native build, not wasm`);
  }
  if (candidates.length > 1) {
    throw new Error(
      `multiple mep agent sockets found: ${candidates.join(", ")} -- set MEP_AGENT_SOCKET to the one you want ` +
        `(call the "mep_session_info" tool against each, or run :AgentSocket in the mep window, to tell them apart)`,
    );
  }
  return candidates[0];
}

interface PendingCall {
  resolve: (result: unknown) => void;
  reject: (err: Error) => void;
}

export class MepClient {
  #conn: Deno.UnixConn | null = null;
  #connecting: Promise<void> | null = null;
  #framer = new RpcFramer();
  #nextId = 1;
  #pending = new Map<number, PendingCall>();
  #events: MepEvent[] = [];
  // A dropped/reconnected connection loses whatever events happened
  // during the gap -- inherent to a push stream with no replay (same as
  // the C++ side's own "no history replay" design) -- so an unbounded
  // queue isn't a real risk in practice, but cap it anyway against a
  // pathological case where nothing ever calls mep_poll_events.
  static readonly #MAX_QUEUED_EVENTS = 2000;

  async call(method: string, params: unknown = {}): Promise<unknown> {
    await this.#ensureConnected();
    const id = this.#nextId++;
    const request = { jsonrpc: "2.0", id, method, params };
    const bytes = frameMessage(JSON.stringify(request));
    return await new Promise<unknown>((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#conn!.write(bytes).catch((err) => {
        this.#pending.delete(id);
        reject(err instanceof Error ? err : new Error(String(err)));
      });
    });
  }

  // Returns and clears every event.* notification received since the
  // last call to this method (or since connecting, for the first call).
  drainEvents(): MepEvent[] {
    const out = this.#events;
    this.#events = [];
    return out;
  }

  get connected(): boolean {
    return this.#conn !== null;
  }

  async #ensureConnected(): Promise<void> {
    if (this.#conn) return;
    if (!this.#connecting) this.#connecting = this.#connectNow();
    return await this.#connecting;
  }

  async #connectNow(): Promise<void> {
    try {
      const path = await discoverSocketPath();
      const conn = await Deno.connect({ path, transport: "unix" });
      this.#conn = conn;
      this.#readLoop(conn); // deliberately not awaited -- runs for the connection's whole lifetime
    } finally {
      this.#connecting = null;
    }
  }

  async #readLoop(conn: Deno.UnixConn): Promise<void> {
    const buf = new Uint8Array(65536);
    let disconnectErr = new Error("mep agent socket connection closed");
    try {
      for (;;) {
        const n = await conn.read(buf);
        if (n === null) break; // clean EOF -- mep exited or closed the connection
        this.#framer.push(buf.subarray(0, n), (body) => this.#handleMessage(body));
      }
    } catch (err) {
      disconnectErr = err instanceof Error ? err : new Error(String(err));
    }
    if (this.#conn === conn) this.#conn = null; // a later #connectNow() may have already replaced it
    for (const pending of this.#pending.values()) pending.reject(disconnectErr);
    this.#pending.clear();
    try {
      conn.close();
    } catch {
      // already closed
    }
  }

  #handleMessage(body: string): void {
    let msg: Record<string, unknown>;
    try {
      const parsed = JSON.parse(body);
      if (typeof parsed !== "object" || parsed === null) return;
      msg = parsed as Record<string, unknown>;
    } catch {
      return; // shouldn't happen -- agent_rpc.cpp only frames valid JSON -- but don't crash the whole client over it
    }
    if ("id" in msg) {
      const id = msg.id as number;
      const pending = this.#pending.get(id);
      if (!pending) return; // no one waiting on this id (shouldn't happen on a strictly synchronous-per-call protocol)
      this.#pending.delete(id);
      if (msg.error) {
        const error = msg.error as { code?: number; message?: string };
        pending.reject(new Error(`mep RPC error ${error.code ?? "?"}: ${error.message ?? "(no message)"}`));
      } else {
        pending.resolve(msg.result);
      }
    } else if ("method" in msg) {
      this.#events.push({ method: msg.method as string, params: msg.params });
      if (this.#events.length > MepClient.#MAX_QUEUED_EVENTS) {
        this.#events.splice(0, this.#events.length - MepClient.#MAX_QUEUED_EVENTS);
      }
    }
  }
}
