// Opens the wasm build in a native window via webview.

// WebKitGTK's accelerated (DMA-BUF) compositor is known to render a blank
// white/black surface on some GPU/driver combos; falling back to software
// compositing/rendering is a safe default and a no-op when not needed.
// Set here (not in the worker) since env vars are process-wide and must be
// in place before the webview/webkit process machinery initializes.
Deno.env.set("WEBKIT_DISABLE_COMPOSITING_MODE", "1");
Deno.env.set("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
Deno.env.set("LIBGL_ALWAYS_SOFTWARE", "1");

// mep.html itself is opened directly via file:// rather than served over
// HTTP -- the wasm is embedded into mep.js (CMakeLists.txt's
// -sSINGLE_FILE=1) so there's no separate .wasm fetch a file:// origin
// would need to pull off, and it keeps the window openable with zero
// server setup.
const htmlPath = await Deno.realPath(
  new URL("../build/web/mep.html", import.meta.url),
);

// mep's persisted project-bookmark list (mep.projects() picker). Mirrors
// src/persist.h's MepDataDir()/projects.json convention for the native
// build, since this Deno process -- not the wasm sandbox -- is the one
// with real env vars/filesystem access to resolve and own that path.
function mepDataDir(): string {
  const xdg = Deno.env.get("XDG_DATA_HOME");
  if (xdg) return `${xdg}/mep`;
  const home = Deno.env.get("HOME");
  if (!home) throw new Error("cannot determine home directory");
  return Deno.build.os === "darwin"
    ? `${home}/Library/Application Support/mep`
    : `${home}/.local/share/mep`;
}
const projectListPath = `${mepDataDir()}/projects.json`;

async function loadProjects(): Promise<string[]> {
  try {
    const doc = JSON.parse(await Deno.readTextFile(projectListPath));
    return Array.isArray(doc.projects) ? doc.projects.filter((p: unknown) => typeof p === "string") : [];
  } catch {
    return [];
  }
}
async function saveProjects(projects: string[]): Promise<void> {
  await Deno.mkdir(mepDataDir(), { recursive: true });
  await Deno.writeTextFile(projectListPath, JSON.stringify({ projects }));
}

// `:terminal`/`:term` (src/vterm.h/editor.cpp's TerminalSpawn wasm
// branch): the wasm sandbox has no subprocess concept of its own, so a
// terminal pane's child process actually runs here, tunneled to the page
// over this one WebSocket per pane. Not a real PTY -- Deno's subprocess
// API doesn't expose pty allocation, only plain pipes -- so shells run
// with TERM set and (for a bare interactive shell) `-i` forced to get as
// close to normal prompt/color behavior as a non-tty stdin/stdout allows;
// readline-style line editing (arrow-key history, etc.) still won't work
// the way it does against a real tty. Protocol: first client message is
// `{"cmd": [...]}` (spawns); after that, JSON text frames are control
// messages ({"type":"resize",...}, currently a no-op with no real PTY to
// resize) and binary frames are raw bytes each direction (keystrokes in,
// merged stdout+stderr out, matching how a real PTY also merges the two).
//
// A real PTY's kernel line discipline applies ONLCR by default -- a bare
// `\n` a program writes arrives at the reading end as `\r\n`, which is why
// ordinary Unix programs get away with writing only `\n` between lines and
// still have the cursor return to column 0. Plain pipes (all Deno.Command
// gives us -- see the block comment above) do no such translation, so
// without doing it ourselves here, VTerm (correctly, per the bare LF/CR
// semantics it implements) would just move down a row without resetting
// the column, and consecutive output lines would stagger diagonally
// across the pane instead of stacking -- reproduced concretely with a
// plain `ls -la`, no readline/interactive-echo involved at all. `state`
// is one shared { lastWasCR } flag reused across both stdout and stderr
// for one connection, matching how a real terminal's ONLCR is a property
// of the single fd both streams would actually share there.
function onlcr(chunk: Uint8Array, state: { lastWasCR: boolean }): Uint8Array {
  const out: number[] = [];
  for (const b of chunk) {
    if (b === 0x0a && !state.lastWasCR) out.push(0x0d);
    out.push(b);
    state.lastWasCR = b === 0x0d;
  }
  return new Uint8Array(out);
}

async function pumpPtyStream(
  stream: ReadableStream<Uint8Array>,
  socket: WebSocket,
  onlcrState: { lastWasCR: boolean },
) {
  const reader = stream.getReader();
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (socket.readyState === WebSocket.OPEN) socket.send(onlcr(value, onlcrState));
    }
  } catch {
    // socket closed or the stream errored -- nothing more to send
  }
}

function handlePtyUpgrade(req: Request): Response {
  const { socket, response } = Deno.upgradeWebSocket(req);
  let child: Deno.ChildProcess | null = null;
  let stdinWriter: WritableStreamDefaultWriter<Uint8Array> | null = null;

  socket.onmessage = async (ev) => {
    if (typeof ev.data === "string") {
      if (!child) {
        try {
          const { cmd } = JSON.parse(ev.data) as { cmd: string[] };
          // LD_LIBRARY_PATH is set on *this* process only for
          // webview_worker.ts's own libgtk-4 dlopen() (see the top of
          // this file) -- irrelevant to whatever the user's shell runs,
          // and Deno's --allow-run explicitly refuses to spawn at all
          // with it present (a real subprocess-hijack vector it singles
          // out) unless clearEnv strips it, which also means nothing
          // else from this process's own environment leaks in unless
          // explicitly copied below.
          const childEnv: Record<string, string> = {
            ...Deno.env.toObject(),
            TERM: "xterm-256color",
            // No real PTY means no ioctl(TIOCSWINSZ) to tell the shell
            // its actual size; COLUMNS/LINES is the best sizing hint
            // available without one (some programs consult it as a
            // fallback when isatty() fails). Not a fix for wrapping
            // itself -- that's ONLCR, see the comment above pumpPtyStream
            // -- just a reasonable-width default for anything that reads
            // it directly.
            COLUMNS: "500",
            LINES: "50",
          };
          delete childEnv.LD_LIBRARY_PATH;
          child = new Deno.Command(cmd[0], {
            args: cmd.slice(1),
            stdin: "piped",
            stdout: "piped",
            stderr: "piped",
            env: childEnv,
            clearEnv: true,
          }).spawn();
          stdinWriter = child.stdin.getWriter();
          socket.send(JSON.stringify({ type: "ready" }));
          const onlcrState = { lastWasCR: false };
          pumpPtyStream(child.stdout, socket, onlcrState);
          pumpPtyStream(child.stderr, socket, onlcrState);
          child.status.then((status) => {
            try {
              socket.send(JSON.stringify({ type: "exit", code: status.code }));
            } catch {
              // socket already gone
            }
            try {
              socket.close();
            } catch {
              // already closed
            }
          });
        } catch (err) {
          try {
            socket.send(JSON.stringify({ type: "spawn_error", error: String(err) }));
          } catch {
            // socket already gone
          }
          socket.close();
        }
        return;
      }
      // Control message after spawn (currently only "resize", a no-op --
      // see the block comment above) -- parsed just to validate the
      // protocol, not acted on.
      try {
        JSON.parse(ev.data);
      } catch {
        // malformed -- ignore
      }
      return;
    }
    if (!stdinWriter) return;
    const data = ev.data instanceof ArrayBuffer ? new Uint8Array(ev.data) : (ev.data as Uint8Array);
    try {
      await stdinWriter.write(data);
    } catch {
      // child's stdin already closed (process exited) -- drop the keystroke
    }
  };
  socket.onclose = () => {
    try {
      child?.kill();
    } catch {
      // already exited
    }
  };
  return response;
}

// The wasm build's C++ has no real filesystem of its own (browsers/wasm
// sandboxes don't grant one), but this Deno process does. :e/:w/:source
// reach it over this loopback-only HTTP server (src/editor.cpp fetch()es
// it via EM_ASYNC_JS) rather than through webview.bind(): bind()'s reply
// is delivered back into the page by the native webview library calling
// back into WebKit's JS engine, and in testing that delivery reliably
// never arrived (the underlying Deno.readTextFile/writeTextFile call
// completed for real -- the file really was read/written -- but the
// page's `await` on the bind() call's promise hung forever). Root cause:
// webview_run() (see launcher/webview_worker.ts) is a synchronous FFI
// call that blocks its entire isolate's event loop for as long as the
// window is open, so nothing async -- including a bind() callback's own
// `await Deno.writeTextFile(...)` settling and its `.then()` continuation
// running -- reliably progresses while it's running, in whichever isolate
// it's called from. Running this server on the *main* thread, with
// webview.run() isolated to a worker (a separate OS thread with its own
// V8 isolate), keeps this server's isolate completely free of that block.
//
// Bound to 127.0.0.1 (not 0.0.0.0) and to a random free port (port: 0) so
// nothing off-box can reach it; the page (opened directly in a plain
// browser tab, with no Deno process backing it) has no bridge to call.
const bridgeServer = Deno.serve(
  { hostname: "127.0.0.1", port: 0, onListen: () => {} },
  async (req) => {
    const cors = {
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type",
    };
    if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
    const url = new URL(req.url);
    if (url.pathname === "/pty" && req.headers.get("upgrade") === "websocket") {
      return handlePtyUpgrade(req);
    }
    try {
      if (req.method === "GET" && url.pathname === "/read") {
        const path = url.searchParams.get("path") ?? "";
        const content = await Deno.readTextFile(path);
        return Response.json({ ok: true, content }, { headers: cors });
      }
      if (req.method === "POST" && url.pathname === "/write") {
        const { path, content } = await req.json();
        await Deno.writeTextFile(path, content);
        return Response.json({ ok: true }, { headers: cors });
      }
      if (req.method === "GET" && url.pathname === "/list") {
        const path = url.searchParams.get("path") ?? ".";
        const entries = [];
        for await (const entry of Deno.readDir(path)) {
          entries.push({ name: entry.name, is_dir: entry.isDirectory });
        }
        return Response.json({ ok: true, entries }, { headers: cors });
      }
      if (req.method === "GET" && url.pathname === "/projects") {
        return Response.json({ ok: true, projects: await loadProjects() }, { headers: cors });
      }
      if (req.method === "POST" && url.pathname === "/projects") {
        const { action, path } = await req.json();
        let projects = await loadProjects();
        if (action === "add") {
          // Resolved to an absolute, symlink-free path before storing --
          // the caller passes "." (mep.projects()'s "add current
          // directory"), and a bare "." would both display uselessly in
          // the picker (every entry just says ".") and silently mean
          // "wherever mep's cwd happens to be later" once
          // mep.project_open() chdir()s to it, rather than the directory
          // actually meant at add time.
          const resolved = await Deno.realPath(path);
          if (!projects.includes(resolved)) projects.push(resolved);
        } else if (action === "remove") {
          projects = projects.filter((p) => p !== path);
        }
        await saveProjects(projects);
        return Response.json({ ok: true, projects }, { headers: cors });
      }
      return new Response("not found", { status: 404, headers: cors });
    } catch (err) {
      return Response.json({ ok: false, error: String(err) }, { headers: cors });
    }
  },
);
const bridgePort = (bridgeServer.addr as Deno.NetAddr).port;

const targetUrl = `file://${htmlPath}?bridgePort=${bridgePort}`;
const workerUrl = new URL("./webview_worker.ts", import.meta.url);
workerUrl.searchParams.set("target", targetUrl);
const worker = new Worker(workerUrl.href, {
  type: "module",
  deno: { permissions: "inherit" },
});
worker.onmessage = async (e) => {
  if (e.data?.type === "closed") {
    await bridgeServer.shutdown();
    Deno.exit(0);
  }
};
worker.onerror = (e) => {
  console.error("[main] worker error:", e.message, e.error);
};
