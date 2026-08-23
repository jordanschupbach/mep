// Minimal embedded browser window: mep.browse(url_or_path) (kBuiltinTextTools,
// src/main.cpp) spawns this via mep.job_start to open real HTML/JS content in
// a native window -- the same jsr:@webview/webview + WebKitGTK stack the wasm
// launcher's own webview_worker.ts already uses to host mep.html, just
// navigated to arbitrary content instead. A standalone process rather than a
// Worker (contrast webview_worker.ts's own header comment on why *it* needs
// one): nothing else needs this process's event loop free while the window
// is open, so there's no reason to isolate webview.run()'s blocking FFI call
// onto a separate thread here.
//
// LD_LIBRARY_PATH (for the dlopen() of libwebview.so behind the jsr:@webview/
// webview import below) has to be set in the *outer* process environment
// before `deno run` even starts, not from in here: ES module imports always
// finish evaluating before any of this file's own top-level code runs,
// import hoisting, so a Deno.env.set() in this file -- no matter how early
// it's written -- is already too late for an import's own side effects.
// mep.browse's own Lua side handles this (wrapping the actual `deno run` in a
// tiny `sh -c` that exports LD_LIBRARY_PATH from $MEP_WEBVIEW_LD_LIBRARY_PATH
// first), mirroring justfile's `run-wasm` recipe.
//
// The three WEBKIT_*/LIBGL_* vars below are different: WebKit only reads
// them once it actually initializes (new Webview() further down), a later
// runtime action, not an import-time one -- so setting them here, after the
// import, still works, the same way serve.ts's own top-of-file Deno.env.set()
// calls reach webview_worker.ts's later `new Webview()` despite running in a
// separate Worker/isolate spawned afterward.
import { SizeHint, Webview } from "jsr:@webview/webview";

Deno.env.set("WEBKIT_DISABLE_COMPOSITING_MODE", "1");
Deno.env.set("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
Deno.env.set("LIBGL_ALWAYS_SOFTWARE", "1");

const target = Deno.args[0];
if (!target) {
  console.error("usage: browser.ts <url-or-path>");
  Deno.exit(1);
}

// A bare filesystem path (no URL scheme) opens as a local file -- lets
// mep.browse(target) pass either a real URL or a plain path straight
// through unexamined, the same way a browser's own address bar does.
const isUrl = /^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(target);
const url = isUrl ? target : `file://${await Deno.realPath(target)}`;

const webview = new Webview(true, { width: 1280, height: 860, hint: SizeHint.NONE });
webview.title = isUrl ? url : (target.split("/").pop() ?? target);

// Best-effort Linux-only maximize, same call/same trade-off as
// webview_worker.ts's own (see its comment) -- not attempted on other
// platforms, which just get the fixed size set above.
if (Deno.build.os === "linux") {
  try {
    const gtk = Deno.dlopen("libgtk-4.so.1", {
      gtk_window_maximize: { parameters: ["pointer"], result: "void" },
    });
    gtk.symbols.gtk_window_maximize(webview.unsafeWindowHandle);
  } catch {
    // Not fatal: falls back to the fixed size set above.
  }
}

webview.navigate(url);
webview.run();
