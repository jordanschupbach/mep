# GLFW Removal Plan

Replace GLFW (`find_package(glfw3)`, linked into `mep_core`/`mep`) with an
in-house window/input/GL-context platform layer underneath
`gfx::backend_native` -- the same shape of project raylib itself was
(raylib's own desktop backend *is* GLFW under the hood), deliberately
scoped down to just windowing/input/context since 2D/3D rendering, text,
audio, and model import are already mep's own code post-raylib-removal.

**How to resume:** check the boxes below, `git log --oneline -- src/gfx/backend_native.cpp src/agent_ui_input.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`/the raylib plan: implement -> `nix develop --command
cmake --build build/native` -> verify against a live instance (screenshot
+ real key/mouse RPC input, see `live_mep_instance_testing` memory) ->
tick the box -> next phase, pausing for the user's go-ahead unless told
to keep going.

**This is the largest and lowest-priority candidate in
[DEPENDENCIES.md](DEPENDENCIES.md)'s queue** -- read
**Why this is different from every other plan here** before starting.

---

## Current usage (grounded in `backend_native.cpp`, 401 lines, +
`agent_ui_input.cpp`'s native-handle resolution)

27 distinct GLFW calls, all in one file:

- **Window/context lifecycle**: `glfwInit`, `glfwTerminate`,
  `glfwWindowHint`, `glfwCreateWindow`, `glfwDestroyWindow`,
  `glfwMakeContextCurrent`, `glfwSwapInterval`, `glfwGetProcAddress`
  (feeds `gl_loader.cpp`'s own GL function-pointer loading -- already
  in-house, this is just where the OS-level `dlsym`-equivalent comes
  from today).
- **Frame loop**: `glfwPollEvents`, `glfwWindowShouldClose`,
  `glfwSetWindowShouldClose`, `glfwGetTime`, `glfwGetFramebufferSize`,
  `glfwMaximizeWindow`.
- **Input callbacks** (registration only -- the actual key/mouse state
  tracking is already mep's own code in `backend_native.cpp`'s `ctx_`
  struct, not GLFW polling functions): `glfwSetKeyCallback`,
  `glfwSetCharCallback`, `glfwSetMouseButtonCallback`,
  `glfwSetCursorPosCallback`, `glfwSetScrollCallback`,
  `glfwSetWindowUserPointer`/`glfwGetWindowUserPointer` (how those
  callbacks reach back into `ctx_`).
- **Clipboard**: `glfwGetClipboardString`/`glfwSetClipboardString`.
- **Cursor**: `glfwCreateStandardCursor`/`glfwDestroyCursor`/`glfwSetCursor`.
- **Native handle passthrough**: `agent_ui_input.cpp`'s
  `glfwGetX11Display`/`glfwGetX11Window` (via `glfw3native.h`,
  `GLFW_EXPOSE_NATIVE_X11`) -- resolves mep's window down to a real X11
  `Display*`/`Window` for the agent-automation `ui.*` RPCs' XTest-based
  synthetic input. This is the one place mep's code reaches *past*
  GLFW's own abstraction into the underlying platform already -- a sign
  the X11-specific plumbing this plan would formalize is already assumed
  to exist, not a new commitment this plan introduces.

## Why this is different from every other plan here

Every other **Candidate** in `DEPENDENCIES.md` replaces a *leaf*
algorithm or format (a codec, a parser, a rasterizer) with a single,
well-specified correct answer to converge on. GLFW is a *platform
abstraction*: real parity means real per-OS backends (X11 or Wayland on
Linux, Win32 on Windows, Cocoa on macOS), each with its own window
creation, event loop integration, GL/EGL/WGL/NSOpenGL context creation,
and input event translation -- genuinely as large a project as raylib's
own removal was, not smaller. Unlike raylib, though, there's **no
forcing requirement** to do this at all: GLFW is a small, stable,
permissively-licensed dependency doing exactly one well-scoped job, and
nothing about mep's own architecture is straining against its
abstraction the way `main.cpp` was straining against raylib's baked-in
assumptions (no custom glyph-atlas fast path, no lighting-model control,
etc.). This is why `DEPENDENCIES.md` ranks it last and calls it
"genuinely optional."

## Scoping decisions

1. **Linux/X11 only for a first real milestone**, matching every other
   plan's platform scoping and this sandbox's own verified environment.
   Wayland, Win32, and Cocoa backends are real, separate, follow-on
   projects -- each roughly comparable in size to the X11 backend alone --
   not phases of *this* plan. Document them as explicitly deferred, not
   silently dropped.
2. **Reuse `agent_ui_input.cpp`'s existing X11 assumption.** That file
   already requires `GLFW_EXPOSE_NATIVE_X11`/a real X11 `Display`/
   `Window` today -- an in-house X11 backend doesn't add a new platform
   assumption, it just moves where mep obtains the `Display*`/`Window`
   it already needs (from its own `XOpenDisplay`/`XCreateWindow` instead
   of asking GLFW for its internal ones).
3. **Keep GLX for the GL context** (not EGL) to match GLFW's own default
   desktop-Linux behavior exactly, minimizing behavioral drift in
   context creation/pixel-format selection during the swap.
4. **Preserve the existing `gfx::Input`/callback-populated `ctx_` design
   entirely** -- this plan only replaces *how GLFW's callbacks get
   invoked*, not `backend_native.cpp`'s own input-state model, which
   already doesn't depend on any GLFW-specific behavior beyond "a
   callback fires with key/mouse/scroll data."

## Phases

### Phase 1: X11 window + GLX context skeleton
- [x] `XOpenDisplay`, `XCreateWindow` with `glXChooseFBConfig`/
  `glXGetVisualFromFBConfig`-selected RGBA8+depth24+doublebuffer visual,
  ICCCM/EWMH basics (`WM_DELETE_WINDOW` protocol, `_NET_WM_NAME`/
  `XStoreName` window title). `WM_NORMAL_HINTS` min-size skipped --
  nothing ever requests a non-resizable/min-size window.
- [x] GLX: `glXChooseFBConfig` + `glXCreateContextAttribsARB` (resolved
  via `glXGetProcAddressARB`, requesting GL 3.3 core, matching
  `gl_loader.cpp`'s own assumption), `glXMakeCurrent`, `glXSwapBuffers`,
  `glXSwapIntervalEXT` (this sandbox's Mesa driver exposes
  `GLX_EXT_swap_control`/`_MESA_swap_control`/`_SGI_swap_control` all
  three -- tries EXT first, falls back through MESA/SGI, silently
  unthrottled if none resolve).
- [x] `gl_loader.cpp`'s `get_proc_address` callback is now
  `glXGetProcAddressARB` (passed as a plain function pointer, no cast
  needed -- its signature already matches `GLProcAddressFn` exactly).
- [x] Deliberately did **not** `#include <GL/glx.h>`: it unconditionally
  pulls in `<GL/gl.h>` (no include guard around that line), whose GL_*
  macros collide with `gl_loader.h`'s own `inline constexpr GLenum
  GL_POINTS = ...`-style declarations -- exactly the vendored-system-
  GL-header dependency `gl_loader.h`'s own top comment says this
  codebase avoids. Hand-declared the dozen GLX types/functions/constants
  actually used instead (cross-checked against the real GLX 1.4/
  `GLX_ARB_create_context` headers' values, not guessed) -- same
  "no vendored header" pattern `gl_loader.h` already established for
  core GL.
- [x] Milestone verified: `mep-gfx-native-integration-smoke` (the
  existing gfx::-facade smoke test) opens a real window, renders 2D
  primitives, and reads back non-blank pixel content -- `smoke: OK`.

### Phase 2: event loop + input
- [x] Replaced `glfwPollEvents` with `XPending`/`XNextEvent` processing
  in `WindowShouldClose` (the same once-per-frame poll point the
  GLFW-era backend used): `KeyPress`/`KeyRelease` (physical-key identity
  via `XLookupKeysym` at group 0/level 0 -- the X11 equivalent of
  `GLFW_KEY_*`'s shift-invariant meaning -- plus `Xutf8LookupString`/
  `XLookupString` for the separate text-input stream), `ButtonPress`/
  `ButtonRelease` (buttons 1/2/3 -> Left/Middle/Right; buttons 4/5/6/7
  -> vertical/horizontal scroll ticks, the same X11 legacy-wheel
  convention `agent_ui_input.cpp`'s synthetic `Scroll` already
  targets), `MotionNotify` (cursor position), `ClientMessage`
  (`WM_DELETE_WINDOW`), `SelectionRequest`/`SelectionClear` (clipboard,
  below).
- [x] Key-repeat: used `XkbSetDetectableAutoRepeat` instead of GLFW's
  own release-then-press-at-same-timestamp heuristic -- with detectable
  autorepeat enabled, the X server itself suppresses the synthetic
  `KeyRelease` between repeats, so consecutive `KeyPress` events with no
  `KeyRelease` between them *are* the repeat signal directly (simpler
  and more robust than replicating GLFW's own workaround for a server
  that doesn't support the detectable mode). Verified live against the
  actual mod1-resize-repeat feature this session's earlier GLFW-era fix
  targeted (see below).
- [x] Clipboard: implemented as both requestor (`GetClipboardText`:
  `XConvertSelection` + a bounded `select()`-based wait loop on the X11
  connection fd, dispatching any other event that arrives while
  waiting -- including a `SelectionRequest` for mep's *own* clipboard
  arriving in that same window) and owner (`SetClipboardText`:
  `XSetSelectionOwner` + a `SelectionRequest` handler in the main event
  loop serving `TARGETS`/`UTF8_STRING`). Verified against a real,
  independent external X11 client (`xclip`) in both directions: `yy` in
  mep, then `xclip -o -selection clipboard` in a separate shell,
  returned the exact yanked line; `xclip -selection clipboard` from an
  external shell, then `"+p` in mep, pasted the external text exactly.
- [x] Cursor shapes: `XCreateFontCursor` (core Xlib cursor font --
  `XC_left_ptr`/`XC_hand2`/`XC_sb_h_double_arrow`/`XC_sb_v_double_arrow`,
  no separate `libxcursor` dependency needed) + `XDefineCursor`.

### Phase 3: `agent_ui_input.cpp` simplification
- [x] `agent_ui_input.cpp` no longer includes any GLFW header at all.
  Added `gfx/native_window_handle.h` (a tiny, deliberately Xlib-free
  header: `struct NativeWindowHandle { void *display; unsigned long
  window; };`) so `GetNativeWindowHandle()` can hand over an X11
  Display*/Window pair without `agent_ui_input.h` (included from
  `main.cpp`) ever seeing an X11 type -- preserving that header's own
  documented reason for existing (Xlib.h's `Font` typedef collision).
  `Init()` now just `reinterpret_cast`s the handle struct directly
  instead of calling `glfwGetX11Display`/`glfwGetX11Window`.

### Phase 4: swap-in + verification
- [x] `CMakeLists.txt`: removed `find_package(glfw3 REQUIRED)` and every
  `target_link_libraries(... glfw ...)` (`mep_core`, `mep`, the 4
  gfx-native-smoke targets, `mep-model3d-doc-test`, `mep-amalgam`).
  `find_package(X11 REQUIRED COMPONENTS Xtst Xi)` -- `Xi` had to be
  requested explicitly too: CMake's `FindX11` module models `X11::Xtst`'s
  imported target as link-depending on `X11::Xi`, and it's a *real*
  transitive shared-library dependency on this distro (`libXtst.so`
  actually links `libXi.so.6`), not just CMake bookkeeping -- configure
  fails otherwise. `OpenGL::GL` already provides GLX (Mesa exports both
  from the same `libGL.so`).
- [x] **Platform split, not in the original plan text**: this file
  (`gfx/backend_native.cpp`) is also compiled for the Emscripten/wasm
  build via `MEP_GFX_NATIVE_SOURCES`, which still needs a GLFW-shaped
  API there -- `-sUSE_GLFW=3` is emcc's own link-time shim over the
  browser's canvas/input APIs, not real desktop GLFW, and there's no
  in-house version of a browser's own event model worth writing (this
  plan's own Phase 5 non-goal). Split into two platform-specific files:
  `gfx/backend_native.cpp` (X11/GLX, native only) and the restored
  original GLFW-based implementation moved to
  `gfx/backend_native_emscripten_glfw.cpp` (Emscripten only), selected
  by `CMakeLists.txt`'s `if(EMSCRIPTEN)` branch of
  `MEP_GFX_NATIVE_SOURCES`. Verified the Emscripten file (and the two
  renderer files it shares with the native path) compiles clean under
  `em++ -sUSE_GLFW=3` -- full wasm link/run untestable in this sandbox
  (offline `FETCHCONTENT` limitations predate this plan, see
  `env_offline_worktree_configure` memory), but the compile-level risk
  this split could have introduced is confirmed absent.
- [x] `flake.nix`: removed `pkgs.glfw`, `pkgs.libxrandr`,
  `pkgs.libxinerama`, `pkgs.libxcursor` from both `buildInputs` (the
  `mepPackage` derivation) and the devShell's `packages` -- none of
  GLFW's own former X11-backend dependencies (monitor enumeration,
  themed cursor loading) are used by this hand-written backend. Kept
  `libGL`/`libx11`/`libxtst`, and kept `libxi` (a real transitive
  dependency of `libxtst`, confirmed via `ldd`, not something mep calls
  directly).
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  full clean build succeeds (`mep`, `mep_core`, all 4 gfx-native-smoke
  targets, `mep-model3d-doc-test`), zero warnings under
  `MEP_STRICT_FLAGS`. `nix develop --command just test`-equivalent (run
  individually, see STB_IMAGE_REMOVAL_PLAN.md's note on the pre-existing
  `mep-collab-session-test` justfile-arg issue) -- all 5 non-GUI tests
  pass. All 4 gfx-native-smoke binaries verified live against the real
  GPU (`mep-gfx-native-integration-smoke`: 2D primitives render;
  `mep-gfx-native-audio-smoke`: full audio lifecycle; `mep-gfx-native-model-smoke`:
  loads a real glTF model with an embedded texture and renders it to a
  PNG).
- [x] Full live-instance re-verification via the real `mep` binary and
  the agent-control socket: window opens and renders correctly (org-mode
  buffer, image pane, terminal, syntax highlighting all visible);
  keyboard typing including punctuation (`-+=[]`); mouse click correctly
  focuses a pane and places the cursor at the clicked character;
  `Ctrl+U` through a focused terminal pane (confirms Control-chord
  suppression of char injection *and* the chord itself reaching the
  terminal); **mod1+Shift+h pane resize** (the exact feature this
  session's earlier GLFW-era fix targeted) visibly resized the pane
  border through the new X11 key/modifier pipeline; mouse-wheel scroll
  (button-4/5 `delta`) moved a 578-line buffer from line 1 to line 46;
  bidirectional clipboard interop with a real external X11 client
  (`xclip`, see Phase 2). Not re-verified pixel-for-pixel: cursor-shape
  *icon* changes over resizable borders (low-risk, standard
  `XCreateFontCursor`/`XDefineCursor` calls, exercised without error
  throughout the session but not visually confirmed since screenshots
  don't capture the OS pointer icon).
- [x] Retired `mep-gfx-native-smoke`/`gfx/native_smoke_main.cpp` (the
  original B1 walking-skeleton smoke test, which opened its own window
  via raw GLFW calls independently of `gfx::backend_native.cpp`'s own
  window/context creation): once GLFW is gone there's no lower-level
  "raw" path left to test separately from the real `gfx::InitWindow`
  path, and `mep-gfx-native-integration-smoke` already covers that
  end-to-end.

### Phase 5 (deferred, not part of this plan's completion criteria): other platforms
- [ ] Wayland backend (`libwayland-client`, `xdg-shell` protocol, a
  second, independent input/clipboard implementation) -- only worth
  starting if X11 (via XWayland) ever becomes insufficient for real
  Wayland-native users.
- [ ] Win32 backend.
- [ ] Cocoa backend.
- [ ] Emscripten: already doesn't use this plan's X11 code at all --
  `-sUSE_GLFW=3` is emcc's own link-time shim, entirely separate from
  desktop GLFW, and out of scope for removal here (there's no
  "in-house version" of a browser's own input/canvas event model to
  write; the wasm build's "GLFW" is already just a thin JS shim over
  browser APIs mep doesn't need to reimplement).

## Non-goals

- Gamepad input, multi-monitor/DPI-scaling queries, fullscreen toggle,
  window icon -- confirmed unused today by the original raylib-removal
  plan's own research pass; still true, still out of scope.
- Wayland/Win32/macOS backends -- explicitly deferred (Phase 5), not
  part of what "done" means for this plan.
- Vulkan or any non-GL context type -- mep's renderer is GL-only
  end to end; no reason to add a second context-creation path.
