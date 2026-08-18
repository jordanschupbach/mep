// Serves the wasm build output and opens it in a native window via webview.
import { Webview } from "jsr:@webview/webview";
import { serveDir } from "jsr:@std/http/file-server";

// WebKitGTK's accelerated (DMA-BUF) compositor is known to render a blank
// white/black surface on some GPU/driver combos; falling back to software
// compositing is the standard workaround and is a no-op when not needed.
Deno.env.set("WEBKIT_DISABLE_COMPOSITING_MODE", "1");
Deno.env.set("WEBKIT_DISABLE_DMABUF_RENDERER", "1");

const webRoot = new URL("../build/web/", import.meta.url);
const rootPath = await Deno.realPath(webRoot);

const server = Deno.serve(
  { port: 0, hostname: "127.0.0.1", onListen: () => {} },
  (req) => serveDir(req, { fsRoot: rootPath }),
);

const { port } = server.addr as Deno.NetAddr;
const url = `http://127.0.0.1:${port}/mep.html`;

// debug: true wires up console-to-stdout and the right-click inspector,
// so failures here are debuggable instead of a silent blank window.
const webview = new Webview(true);
webview.title = "mep";
webview.navigate(url);
webview.run();

await server.shutdown();
