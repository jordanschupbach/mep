# Lua Removal Plan

**Verdict: keep.** No phases below -- this file exists so Lua shows up in
[DEPENDENCIES.md](DEPENDENCIES.md)'s inventory with its reasoning on
record, matching every other dependency there, not because removal is
planned.

## What it's used for

Lua 5.4 (`third_party` via `FetchContent`, built by hand from the
upstream tarball -- no CMake build of its own) is mep's embedded
scripting and configuration language: `src/lua_env.cpp` exposes the whole
`mep.*` API (key mappings, buffer/pane manipulation, LSP hooks, agent
automation, the built-in `init.lua` that defines mep's own default
keymaps -- see `kDefaultMod1Bindings` in `main.cpp`) to real user-authored
Lua running in a real Lua VM. This is qualitatively different from every
other dependency in this series: raylib/GLFW/stb_*/miniz/pugixml are all
*implementations of a spec mep's own code calls into*; Lua is *a
programming language mep's users write code in*.

## Why not

Removing Lua doesn't mean deleting a rendering backend and writing a
replacement against the same internal API -- it means either:

1. **Writing a Lua-compatible interpreter from scratch** (lexer, parser,
   bytecode VM or tree-walker, garbage collector, the full standard
   library surface `init.lua` and any user config might touch). This is
   a multi-month project in its own right for a mature, correct
   implementation, with a huge surface for subtle behavioral bugs
   (metatables, coroutines, integer/float arithmetic rules, `string.*`
   pattern matching) that would silently break existing user configs in
   ways that are hard to test for. The "in-house version" here would
   essentially be reimplementing PUC-Rio Lua, not writing something
   *specific to mep*.
2. **Designing an entirely new configuration language/API** and
   rewriting every `mep.*` binding plus the built-in `init.lua` against
   it -- a breaking change for every existing user config, for no
   functional gain (mep isn't outgrowing what Lua provides; nothing in
   `lua_env.cpp`'s ~7700 lines suggests a Lua limitation is being worked
   around).

Neither trade makes sense the way replacing raylib did: raylib's own
scope (windowing/2D/3D/text/audio/input) was already something mep
*wanted* full control over (custom text-rendering fast paths, no
lighting model, exact glyph-atlas behavior) and gained real value from
owning outright. Lua's scope -- "run arbitrary user scripts safely and
fast" -- isn't something mep needs to own; PUC-Rio's implementation *is*
the reference implementation, not a third-party stand-in for one.

## If this ever gets revisited

The actual dependency-reduction lever here isn't "replace Lua," it's
"reduce how much of Lua's stdlib mep exposes/relies on" -- already
effectively minimal (`lua_env.cpp` registers a curated `mep.*` table, not
`luaopen_*` for every stdlib module). Nothing to act on now.
