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
- [ ] `XOpenDisplay`, `XCreateWindow` (or `XCreateSimpleWindow` +
  attribute changes), ICCCM/EWMH basics real window managers expect
  (`WM_DELETE_WINDOW` protocol for close-button handling --
  `glfwSetWindowShouldClose`'s trigger today; `_NET_WM_NAME`/window
  title; `WM_NORMAL_HINTS` if min-size behavior matters).
- [ ] GLX: `glXChooseFBConfig`/`glXCreateContext` (or
  `glXCreateContextAttribsARB` for a specific GL version/core-profile
  request, matching whatever `gl_loader.cpp`/the shader code currently
  assumes), `glXMakeCurrent`, `glXSwapBuffers`, `glXSwapIntervalEXT` (or
  the GLX_MESA/SGI vsync extension variants -- check which this
  sandbox's Mesa driver actually exposes) for `glfwSwapInterval` parity.
- [ ] `gl_loader.cpp`'s `glfwGetProcAddress` call becomes
  `glXGetProcAddressARB` -- both resolve to the same underlying
  `dlsym`-against-the-driver mechanism, minimal-risk swap.
- [ ] Milestone: window opens, clears to a color, closes cleanly on
  window-manager close button, resizes correctly
  (`glfwGetFramebufferSize` -> `XGetWindowAttributes` or tracking
  `ConfigureNotify` events).

### Phase 2: event loop + input
- [ ] Replace `glfwPollEvents` with `XPending`/`XNextEvent` processing:
  `KeyPress`/`KeyRelease` (-> the existing key-callback path, including
  `XLookupString`/`Xutf8LookupString` for character input, replacing
  `glfwSetCharCallback`'s job), `ButtonPress`/`ButtonRelease` (mouse
  buttons + the X11 legacy wheel-as-buttons-4/5 convention
  `agent_ui_input.cpp`'s own `Scroll` already uses for synthetic input --
  real hardware scroll events arrive the same way), `MotionNotify`
  (cursor position), `ConfigureNotify` (resize/move), `ClientMessage`
  (the `WM_DELETE_WINDOW` close protocol from Phase 1).
- [ ] Key-repeat: GLFW's own `IsKeyPressedRepeat` (just extended in this
  session's mod1-resize fix) is itself sourced from X11's native
  autorepeat `KeyPress` events with no `KeyRelease` in between at the
  same timestamp -- GLFW already does this detection *for* mep; an
  in-house backend needs to replicate that exact same "is this a real
  new press or an autorepeat continuation" heuristic, not reinvent a
  different one, to avoid regressing the behavior just fixed.
- [ ] Clipboard: X11 selections (`XA_PRIMARY`/`CLIPBOARD` atom,
  `XConvertSelection` + `SelectionNotify`/`SelectionRequest` handling for
  both directions) -- genuinely one of the fiddlier corners of raw X11
  programming (asynchronous, requires acting as both requestor and
  owner); worth budgeting real time for, and cross-checking against
  GLFW's own X11 clipboard implementation's approach (reading its
  approach for reference is fine; it's MIT-licensed and small).
- [ ] Cursor shapes: `XCreateFontCursor` (standard X cursor font) or
  `Xcursor` library shapes, `XDefineCursor`, replacing
  `glfwCreateStandardCursor`/`glfwSetCursor`.

### Phase 3: `agent_ui_input.cpp` simplification
- [ ] Once `backend_native.cpp` owns its `Display*`/`Window` directly,
  `agent_ui_input.cpp` no longer needs `GLFW_EXPOSE_NATIVE_X11`/
  `glfw3native.h` at all -- it can take the handle straight from
  `gfx::Platform::NativeWindowHandle()` (already the interface it uses
  today per the original raylib-removal plan's A2 stage) with the same
  underlying type, one dependency-on-GLFW's-header removed for free.

### Phase 4: swap-in + verification
- [ ] `CMakeLists.txt`: `find_package(X11 REQUIRED COMPONENTS Xtst)`
  already exists (for `agent_ui_input.cpp`); extend/reuse it rather than
  adding a second X11 discovery path. Add GLX linkage
  (`find_package(OpenGL)` already provides `OpenGL::GLX` on most
  distributions -- verify). Remove `find_package(glfw3 REQUIRED)` and
  every `target_link_libraries(... glfw ...)`.
- [ ] `flake.nix`: remove `pkgs.glfw` from `buildInputs`; keep
  `libGL`/X11-related packages already there for other reasons (Xtst,
  GLX itself needs `libX11`/`libGLX`, likely already present transitively
  via `pkgs.xorg.libX11`/`mesa` -- verify explicitly rather than assume).
- [ ] `nix develop --command cmake --build build/native -j$(nproc)`,
  `nix develop --command just test`.
- [ ] Full live-instance re-verification: window open/close/resize,
  every input path (typing, mouse click/drag, scroll, clipboard
  copy/paste, cursor shape changes over resizable pane borders), the
  agent-automation `ui.*` RPCs (screenshot, synthetic key/mouse) via
  `agent_ui_input.cpp`'s now-simplified handle resolution -- effectively
  re-running this whole session's own verification checklist
  (alignment, terminal rendering, mod1-resize repeat) plus every other
  interaction path, since window/input is the most foundational layer
  in the whole `gfx::` stack.

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
