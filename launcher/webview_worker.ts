// Runs the native webview window. This lives in its own Deno Worker (a
// separate OS thread with its own V8 isolate) rather than the main script,
// because webview_run() -- the FFI call behind Webview.run() -- is a plain
// synchronous native call (see jsr:@webview/webview's src/ffi.ts: no
// `nonblocking: true` on its dlopen symbol declarations). A synchronous FFI
// call blocks its calling isolate's entire event loop for as long as it
// runs, i.e. for as long as the window is open: nothing else in that
// isolate -- timers, an HTTP server's request handling, any pending Promise
// continuation -- gets a chance to run. Confirmed concretely: a `curl` to a
// Deno.serve() HTTP server started in the *same* script as webview.run()
// hung indefinitely with the window open, same as page-side fetch() calls
// to it did. Isolating webview.run() to a worker keeps the main thread's
// event loop (running the file-bridge server in serve.ts) completely free.
/// <reference lib="deno.worker" />
import { SizeHint, Webview } from "jsr:@webview/webview";

const worker = self as unknown as Worker;

// The target page URL is passed via this worker's own URL query string
// rather than postMessage: postMessage'd messages sent right after
// `new Worker(...)` can arrive before the worker's module (which starts
// with an async jsr: import) has reached the point of attaching an
// onmessage listener, and are silently lost -- reproduced concretely (the
// worker logged "waiting for a message" and then hung forever). A query
// string on the worker's own module URL is available synchronously via
// `location`, with no such race.
const targetUrl = new URLSearchParams(self.location.search).get("target")!;

// width/height are just a fallback for platforms the maximize call below
// doesn't cover -- the wasm build's own canvas fills whatever size the
// window ends up at (web/shell.html), so a plain large window still looks
// right even when maximizing isn't available. debug: true wires up
// console-to-stdout and the right-click inspector, so failures here are
// debuggable instead of a silent blank window.
const webview = new Webview(true, { width: 1600, height: 1000, hint: SizeHint.NONE });
webview.title = "mep";

// unsafeWindowHandle is a raw GtkWindow* on the GTK backend, and
// libgtk-4 is already on LD_LIBRARY_PATH (see flake.nix) for the webview
// library itself -- used below for a best-effort maximize-on-launch, and
// for window.mepQuit()'s graceful native close (see the bind() call
// further down). Cocoa/Win32 would need their own native calls for
// either, which this doesn't attempt -- those platforms just get the
// fixed size set above and, for now, no working :qa/:qa! (same
// already-accepted trade-off as the maximize call: best-effort Linux,
// nothing attempted elsewhere).
let gtk: Deno.DynamicLibrary<{
  gtk_window_maximize: { parameters: ["pointer"]; result: "void" };
  gtk_window_close: { parameters: ["pointer"]; result: "void" };
}> | null = null;
if (Deno.build.os === "linux") {
  try {
    gtk = Deno.dlopen("libgtk-4.so.1", {
      gtk_window_maximize: { parameters: ["pointer"], result: "void" },
      gtk_window_close: { parameters: ["pointer"], result: "void" },
    });
    gtk.symbols.gtk_window_maximize(webview.unsafeWindowHandle);
  } catch {
    gtk = null;  // Not fatal: falls back to the fixed size set above.
  }
}

// Exposes window.mepQuit() to the page: the wasm build's own render loop
// (see UpdateDrawFrame in main.cpp) can stop itself on :q/:qa/:qa!/etc,
// but has no way to close *this* native window or end *this* Deno
// process -- that's what's actually holding things open.
//
// gtk_window_close() -- the same native call GTK itself makes when the
// user clicks the window's own close button -- rather than Deno.exit()
// or worker.postMessage() here: this callback runs on webview_run()'s own
// call stack, dispatched synchronously by the native library while
// run() below is still blocked, so this worker's own event loop/isolate
// never regains control to act on anything JS-level -- confirmed by
// testing the *real* serve.ts+worker pipeline end to end (not just this
// worker in isolation) that both Deno.exit() and postMessage() called
// from in here just sit there having no effect, exactly like the bug
// this is fixing. gtk_window_close() sidesteps that entirely: it's a
// plain synchronous native call (no JS/Deno event loop involved) that
// triggers GTK's normal close-request handling in-process, which is what
// makes webview_run() below actually return -- at which point this
// worker's own code resumes normally and the ordinary post-run()
// postMessage() a few lines down runs the same way it does for a real
// click on the close button (confirmed that path works via wmctrl -c).
//
// Not webview.destroy() either: destroy() frees the webview state that
// the still-running native call stack above this callback is about to
// unwind back through -- a use-after-free.
webview.bind("mepQuit", () => {
  if (gtk) gtk.symbols.gtk_window_close(webview.unsafeWindowHandle);
  // bind()'s wrapper JSON.stringify()s whatever's returned here to send
  // back to the page's (unawaited, fire-and-forget) Promise; returning
  // undefined makes that JSON.stringify(undefined) -> the bare word
  // `undefined`, which isn't valid JSON, so the page-side JSON.parse()
  // of it logs a spurious "Failed to parse binding result as JSON"
  // console error. null stringifies to "null", which parses fine.
  return null;
});

webview.navigate(targetUrl);
webview.run();

// webview.run() only returns once the window has closed (it also destroys
// the webview itself before returning -- see Webview.run()'s source).
// Signal the main thread so it can shut down the bridge server and exit;
// otherwise Deno.serve() there would keep the process alive forever.
worker.postMessage({ type: "closed" });
