# Deno + webview_deno Removal Plan

**Verdict: keep.** No phases below -- recorded in
[DEPENDENCIES.md](DEPENDENCIES.md)'s inventory for completeness, not
because removal is planned.

## What it's used for

Not linked into the native `mep` binary at all -- this is `just
run-wasm`'s launcher only. `launcher/serve.ts` builds the wasm target,
starts a local HTTP server, and `launcher/webview_worker.ts` opens it in
a native OS window via `jsr:@webview/webview` (a Deno FFI binding to the
upstream `webview/webview` C library), run in its own Deno Worker because
`webview.run()` is a blocking synchronous FFI call (see that file's own
comment for why isolating it to a worker thread matters). `flake.nix`
pulls in `pkgs.deno` and, on Linux, `pkgs.webkitgtk_6_0` as the actual
native webview backend `webview/webview` embeds.

## Why not

`webview/webview` itself isn't rendering HTML/CSS/JS from scratch -- it's
a thin cross-platform wrapper that embeds the *OS's own* web engine:
WebKitGTK on Linux, WebView2 (Chromium-based) on Windows, WKWebView
(WebKit) on macOS. "Remove this dependency and write an in-house
version" would mean writing an in-house **web browser engine** -- an HTML
parser, a CSS cascade/layout engine, a JavaScript engine, and a
compositor/renderer -- to display mep's own wasm build's UI. That's not
a scoped, finishable project the way raylib's rendering surface was; it's
one of the largest software engineering undertakings that exists,
multiple orders of magnitude past everything else in this series
combined (PDFium included).

It's also worth noting mep already *has* a real in-house HTML/CSS engine
(`html_doc.cpp`, used for mep's own in-app HTML viewer buffer) -- but
that's a document viewer for content mep opens, not a JS-capable app
host capable of running mep's own wasm+JS glue and Worker-based
architecture. Repurposing it into a full browser engine is the same
undertaking under a different starting point, not a shortcut.

## Why this doesn't matter much anyway

Unlike every other dependency in this series, this one is **entirely
optional and orthogonal to the main native build**: `just run`/`just
build-native` (the default, primary way to build and run mep -- see
`README.org`'s own Usage section) never touches Deno, webview, or
WebKitGTK at all. `just run-wasm` is a second, independent build target
(Emscripten/wasm) with its own launcher, useful for testing the wasm
build or a browser-hosted deployment story, not mep's main distribution
form. Removing this dependency would mean dropping wasm-in-a-window
support entirely, which is a product-scope decision, not a "worth
reimplementing" one -- and dropping it *for cost reasons* doesn't apply
either, since it isn't part of what most users build or ship.

## If this ever gets revisited

If the wasm target's launcher story changes, the realistic alternatives
are "run the wasm build directly in the user's own real browser" (no
launcher at all, already implicitly possible since `serve.ts` is just a
static file server) rather than an in-house embedded browser -- not
something to plan here.
