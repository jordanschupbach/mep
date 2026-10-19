# Dependency removal: everything in-house

Goal: mep builds from this repository with a C++ compiler and CMake, and
runs on macOS, Linux and Windows talking only to the operating system.
No third-party libraries are linked, nothing is fetched at configure time,
and no built-in feature quietly needs an external program on `PATH`.

**Kept on purpose:** the platform web engine behind the `:Browser` pane
(WebKit.framework on macOS, WebKitGTK + GTK4 + glib-networking in the
`mep-webview` helper on Linux, and whatever Windows gets later).

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---------------------------------------------------------------------------

## 1. Audit (2026-10-16)

### 1a. Linked third-party libraries

| Dependency | Where | Used for | Platforms |
| --- | --- | --- | --- |
| **OpenSSL** (libssl, libcrypto) | `find_package(OpenSSL)`, CMakeLists ~588 | TLS for native IMAP (`mail_imap.cpp`), `wss://` collab client (`collab_websocket.cpp`: also `SHA1`, `RAND_bytes`), `mep-collabd`, several tests, `mep-amalgam` | macOS (Homebrew `openssl@3`), Linux; optional on Windows (collab stubbed without it) |
| **Lua 5.4.7** | `FetchContent` from lua.org, CMakeLists ~406 | the whole scripting layer | all; **downloaded at configure time** |
| **tree-sitter** runtime v0.26.12 + 9 grammars | vendored in `third_party/` | syntax highlighting, folds, mepml parsing | all |
| tree-sitter CLI + Node | `just` recipe, CMakeLists comment | regenerating `grammars/tree-sitter-mepml/src/parser.c` | dev only |
| **libX11, libXtst, libXi** (+ Xrandr/Xdamage/XKB headers) | `find_package(X11 ...)` | Linux window/input backend (`gfx/backend_native.cpp`), synthetic input for agent UI, X11 GUI embedding (`gui_embed_x11.cpp`, `gui_embed_webview_x11.cpp`) | Linux |
| **ALSA** (libasound) | `find_package(ALSA)` | audio output | Linux |
| **libGL / GLX** | `find_package(OpenGL)` | 2D/3D rendering | all (opengl32 on Windows, OpenGL.framework on macOS) |
| ~~**GLFW (Emscripten port)**~~ | ~~`-sUSE_GLFW=3`~~ | web build windowing/input | wasm -- **removed 2026-10-16** (Phase 8) |
| WebKitGTK, GTK4, glib-networking / WebKit.framework | `mep-webview`, `-framework WebKit` | `:Browser` | **kept** |

OS frameworks/system libraries that are part of the platform rather than
third-party: Cocoa, AudioToolbox, Security, CoreFoundation,
ApplicationServices, ScreenCaptureKit (macOS); user32, gdi32, crypt32,
ws2_32, bcrypt (Windows); libc, libstdc++/libc++, pthreads, libdl. These
stay; the plan only changes how a few of them are reached.

Already in-house (good to build on): PNG/JPEG/GIF/BMP codecs, DEFLATE
(`deflate.cpp`), ZIP, H.264/AAC/MP4, PDF read/write/raster, TrueType/CFF/
Type1, SVG, HTML layout, JS engine, regex, TeX engine (in progress), CAD/
FEM, spell checker, formatters (R, Python, C++, Maxima), HTTP/1.1 over
plain TCP (`http_client.cpp`, `http_server.cpp`), MD5/RC4/AES-CBC
(`pdf_crypt.cpp`), SHA-256/HMAC-SHA256/PBKDF2 (`mail_crypto.cpp`, `mail.h`),
Base64, JSON, `mep-mcp` (replacing the Deno MCP server).

### 1b. External programs that built-in features run

These are the hidden dependencies: the binary links fine, but a feature
fails until the user installs something.

| Program | Call sites | Feature | Notes |
| --- | --- | --- | --- |
| **curl** | `http_client.cpp` (~212, all `https://`), `mail_net.cpp` (SMTP, IMAP fallback, OAuth token exchange), `youtube_player.cpp`, Lua: AI chat (main.cpp ~31024), LeetCode (~33615), STT upload | anything HTTPS | `curl.exe` on Windows |
| **git** | ~40 Lua call sites in main.cpp (~4478, 6326–7131: status, log, graph, diff, show, commit, push/pull/fetch, branch, stash, merge, rebase, clean), `editor.cpp` worktrees (~20716–20860) | git UI, file-tree status, workspaces | |
| **ffmpeg** | main.cpp ~31461 (`mep.stt_record_cmd`) | speech-to-text mic capture | hard-codes PulseAudio input -- **removed from the default path 2026-10-16** (Phase 5; now only an explicit `mep.stt_record_cmd` override) |
| **find / grep / sh** | main.cpp ~7494, 34604, 35864, 36525 | file pickers, grep, runner discovery | `rg` already has a builtin fallback (`lua_env.cpp` ~1939); no Windows equivalents |
| **secret-tool** | `mail_net.cpp` ~1297–1368 | Linux mail credential storage | falls back to a private file |
| **security** (CLI) | `mail_net.cpp` ~1157, 1210, 1365 | macOS keychain | redundant: `mail_keychain_macos.cpp` already uses Security.framework |
| **osascript** | main.cpp ~5054 | macOS file chooser | |
| **chromium / chrome / brave** | `MEP_WEB_RUN`, main.cpp ~26424 | web apps shown in a pane | should use mep's own webview (allowed) |
| **Xvfb** (XQuartz) | `gui_embed_xvfb.cpp` | embedding X11 programs on macOS | |
| **blender** | `model3d_blend_import.cpp` | `.blend` import | opt-in only now (`mep.opt.blend_importer`); in-house reader is the default |
| tectonic/pdflatex/…, pdftoppm | `tex/tex_backend.cpp`, main.cpp ~19820, 20238 | optional "system" TeX backends | in-house is already the default (TEX_ENGINE_PLAN.md) |

### 1c. Not dependencies (integrations with the user's own toolchain)

These run tools the user chose for their own code and stay external by
design: language REPLs/kernels (python3, R, node, …), LSP servers
(clangd, pyright, …), user-configured formatters (clang-format, black,
air — the in-house formatters remain the defaults), build/test runners
(just, make, cmake, npm, pytest, testthat), pandoc for knitr/rmarkdown,
document "post" commands and code blocks run through `sh -c`
(`mepml_export.cpp` ~2905, `lua_env.cpp` ~9837). Dynamic tree-sitter
grammar loading (`dlopen` of user `.so` files) is also optional.

### 1d. Build and dev tooling

CMake, a C++20 compiler (Homebrew LLVM on macOS for modules), Ninja.
Also: Python 3 scripts (`scripts/*.py`, `tools/*.py`: help pages,
amalgamation, benches, fixture generation), Deno (`deno.json`,
`mcp/*.ts`, wasm launcher), Emscripten, pkg-config, mold/lld, Nix flake
(including the `datamunge` input for the dev shell), R script
`gen_r_names.R`.

---------------------------------------------------------------------------

## 2. Target architecture

```
 features ──▶ mep::net::Https / Smtp / Imap / WebSocket ──▶ mep::tls::Client ──▶ mep::crypto ──▶ OS RNG + OS trust roots
          ──▶ mep::git (object DB, index, refs, smart-HTTP) ─────────┘
          ──▶ mep::audio (out + capture) ──▶ CoreAudio | WASAPI | Pulse native protocol / kernel ALSA
          ──▶ mep::gfx  ──▶ Cocoa | Win32 | X11 wire protocol (own client) ──▶ GL loaded with dlopen
          ──▶ mep::secrets ──▶ Security.framework | Credential Manager | Secret Service over own D-Bus client
```

Rules once this lands:

* A built-in feature never execs a program. External programs are only
  reached through explicit user settings (a formatter, a "system" TeX
  backend, an LSP) and every such path has an in-house default.
* Third-party code is allowed only under `third_party/`, vendored (no
  network at configure time), and listed in this file with a decision.
* The Linux binary has no `DT_NEEDED` entry besides libc/libm/libstdc++/
  libdl/libpthread; GL and X11 are loaded at runtime.

---------------------------------------------------------------------------

## 3. Phases

Ordered by value per effort. Each phase keeps the old path as a fallback
behind a setting until the in-house path has parity, then deletes it.

### Phase 0 -- guardrails

- [x] `just deps-audit`: prints linked libraries (`otool -L`, `ldd`)
      and fails on anything not in an allowlist.
      *(2026-10-16: `scripts/deps-audit.sh` + `scripts/deps-allowlist.txt`;
      every entry is classified os / kept / user / optional / temporary,
      temporary ones name their phase. Windows `dumpbin` not done.)*
- [x] Same recipe greps `src/` for argv literals passed to `job_start`,
      `JobManager::Spawn`, `RunProcess`, `posix_spawnp`, `execvp` (and
      argv-shaped tables) and fails on unclassified programs. *(A
      heuristic: `popen`/`system` of composed command lines are not
      caught.)*
- [x] Add both to `just test` so new dependencies cannot slip in.

### Phase 1 -- quick wins (small, independent)

- [x] **Lua: vendor it.** Copy lua-5.4.7 `src/` into `third_party/lua/`
      (plus `Lua-LICENSE.txt`, already present), drop the `FetchContent`
      and the flake's `luaTarball`. No network at configure time.
      *(2026-10-16: `third_party/lua/` (59 files, lua.c/luac.c left out);
      `include(FetchContent)` gone from CMakeLists.txt. Note: the flake
      only sees `third_party/lua` once it is committed.)*
- [x] **macOS keychain:** delete the `security` CLI branches in
      `mail_net.cpp`; `mail_keychain_macos.cpp` covers them.
      *(2026-10-16: an item an older mep made with `security` now costs
      one "allow access?" dialog, after which it is rewritten to trust
      this mep -- the same path as an item from another mep build.)*
- [x] **osascript file chooser:** use `NSOpenPanel` from
      `backend_native_macos.mm` (or mep's own file picker everywhere).
      Windows: `GetOpenFileNameW`/`IFileOpenDialog`.
      *(2026-10-16: `src/file_dialog.h`, `file_dialog_macos.mm`
      (NSOpenPanel), `file_dialog.cpp` (GetOpenFileNameW + comdlg32 --
      not compiled, no Windows machine). `mep.native_file_dialog`; Linux
      and wasm use mep's own directory picker instead of zenity/kdialog/
      yad. A D-Bus file-chooser portal could come with Phase 6's D-Bus
      client.)*
- [x] **find / grep in Lua:** route through the in-house walker in
      `project_search.cpp` (it already honours `.gitignore` and backs the
      builtin `rg`). Port the four call sites.
      *(2026-10-16: the builtin searcher now answers every rg argv it
      understands even when ripgrep is installed; `mep.opt.search_system_rg
      = true` opts into the system rg. Added `--max-depth`/`-d` to the
      builtin (tested). find_files, winbar_navigate and todoscan lost their
      find/grep fallbacks; the runner's workspace scan is an rg argv now.)*
- [x] **`MEP_WEB_RUN` → own webview:** show web apps in a `:Browser`-style
      pane (WebKit / `mep-webview`) instead of hunting for Chromium.
      Keep the URL-sniffing of the launched server, drop the browser
      loop and temp profiles.
      *(2026-10-16: `Editor::MepmlWebStart` / `mep.mepml_web_start`: the
      page in a WebView in the block's results; a web app's server runs as
      a job, its first printed http(s) address is loaded, it is killed with
      the run, and a server that exits (or prints no address for 2 min)
      ends the run with its output. The macOS WebView now takes pictures
      (async WKWebView snapshot) so stopping still leaves `\image(...)`
      results; the X11 one hands back the adopted window's (not compiled
      here). Verified live on macOS: an html page, a Python server, and the
      failure path.)*
- [x] **`sleep` via `sh -c`** (main.cpp ~33652): use a timer.
      *(2026-10-16: new `mep.defer(seconds, fn)` (an in-process job;
      `mep.job_kill(id)` cancels), verified live.)*
- [~] **Deno:** `mep-mcp` already replaces `mcp/server.ts`; delete
      `mcp/*.ts` and its test once `mep-mcp` has the same coverage. Port
      `deno task launch` (wasm in a native window) to `mep-webview` or
      drop it.
      *(2026-10-16: `mcp/` deleted; its end-to-end test is now
      `mep-mcp-live-test` (src/mcp_live_test.cpp, in `just test-gui`),
      covering everything the Deno test did plus handshake/error cases.
      Left: the wasm launcher (`launcher/*.ts`, `deno task launch`,
      `just run-wasm`).)*

### Phase 2 -- in-house crypto and TLS 1.3 client (keystone)

Unblocks phases 3, 4 and the OpenSSL removal. Client only: `mep-collabd`
is plain `ws://` and documents running behind a TLS reverse proxy.

Primitives (`src/crypto/`, each with RFC/NIST test vectors):

- [ ] Consolidate the existing SHA-256/HMAC/PBKDF2/AES/MD5 code into
      `mep::crypto` (from `mail_crypto.cpp`, `pdf_crypt.cpp`).
- [ ] SHA-1 (WebSocket handshake, git), SHA-384/512.
- [ ] HKDF (RFC 5869).
- [ ] AES-128/256-GCM with constant-time GHASH (table-free or bitsliced;
      AES-NI / ARMv8 crypto extensions as an optional fast path).
- [ ] ChaCha20-Poly1305 (RFC 8439).
- [ ] X25519 (RFC 7748).
- [ ] P-256 ECDHE + ECDSA verify (constant-time field arithmetic).
- [ ] Bignum + RSA PKCS#1 v1.5 and RSA-PSS **verify** (servers still
      chain to RSA roots).
- [ ] Ed25519 verify (useful for git/SSH later).
- [ ] CSPRNG: `getrandom` / `arc4random_buf` / `BCryptGenRandom`.

Certificates:

- [ ] ASN.1 DER parser, X.509 v3 parsing (SAN, basicConstraints,
      keyUsage, EKU, validity), PEM.
- [ ] Chain building + verification + hostname matching (RFC 6125).
- [ ] Trust roots from the OS: macOS `SecTrustCopyAnchorCertificates`
      (or hand the chain to `SecTrustEvaluateWithError`), Windows
      `CertOpenSystemStore("ROOT")`, Linux the usual bundle paths
      (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/...`,
      `$SSL_CERT_FILE`).

Protocol:

- [ ] TLS 1.3 client (RFC 8446): X25519 + P-256 groups, AES-GCM and
      ChaCha20 suites, ECDSA/RSA-PSS signatures, SNI, ALPN
      (`http/1.1`), session tickets optional, HelloRetryRequest,
      KeyUpdate, close_notify.
- [ ] TLS 1.2 ECDHE-{ECDSA,RSA}-AES-GCM fallback only if real servers
      we need (IMAP/SMTP of common providers, AI APIs, YouTube, GitHub)
      turn out to require it -- check first.
- [ ] Test against the RFC 8448 traces, a local test server in CI, and a
      live list (Gmail IMAP/SMTP, Outlook, api.anthropic.com,
      api.openai.com, youtube.com, github.com).
- [ ] Fuzz the record layer, handshake parser and DER parser.
- [ ] `mep::net::TlsStream` with the blocking/non-blocking shape that
      `mail_imap.cpp` and `collab_websocket.cpp` already use, then port
      both and delete every OpenSSL include. Remove `find_package(OpenSSL)`
      and the `collab_websocket_stub.cpp` path (TLS now exists everywhere,
      Windows included).

Risk: this is security-critical code. Constant-time discipline, strict
parsing (reject rather than tolerate), and a review pass before it
becomes the default. Keep OpenSSL selectable with
`-DMEP_TLS_BACKEND=openssl` until Phase 3 has shipped for a while.

### Phase 3 -- native HTTPS, SMTP, OAuth (removes curl)

- [ ] `http_client.cpp`: `https://` over `TlsStream`, redirects,
      chunked transfer, `Content-Encoding: gzip/deflate` (inflate already
      in `deflate.cpp`), timeouts, cancellation, proxy env vars
      (`HTTPS_PROXY` CONNECT) at least.
- [ ] Streaming responses (server-sent events) for AI chat.
- [ ] Lua API: `mep.http_request{method, url, headers, body, stream=fn}`
      replacing every `{'curl', ...}` argv: AI chat, LeetCode, STT upload
      (multipart/form-data builder), and anything in `assets/`.
- [ ] `youtube_player.cpp`: switch `FetchArgv` / InnerTube POST to the
      native client.
- [ ] `mail_net.cpp`: native SMTP (implicit TLS 465 and STARTTLS 587,
      AUTH PLAIN/LOGIN/XOAUTH2), OAuth token exchange over native HTTPS,
      and drop the curl IMAP fallback (native IMAP becomes the only path,
      Windows included).

### Phase 4 -- in-house git (removes git CLI)

Big, but the most visible dependency. Build `mep::git` as a library with
its own tests against repositories made by real git in CI (git stays a
*test* dependency only, to produce fixtures and cross-check output).

**Status (2026-10-16): the local half is in-house; network is not.**
`src/git/` (`git.h` is the API) is a dependency-free library -- no Editor,
Lua or JobManager -- linked into `mep_core` and into `mep-git-test`.
Its front door is `mep::git::RunCommand(argv, cwd, out, err)`, which takes
a git command line and prints exactly what git prints, so every existing
view keeps parsing the same text. `git_job.cpp` runs it as a JobManager
in-process task (`Job::TaskEx`: stdout *and* stderr lines, exit code via
`on_exit`), so nothing blocks the UI thread. Lua: `mep.job_start` routes a
`git` argv in-house when supported (like the builtin `rg`), plus
`mep.git.run(argv, opts, cb(code, out, err))` (in-house, CLI for the
rest), `mep.git.status(dir)`, `mep.git.log(dir, {n, rev})`,
`mep.git.toplevel(dir)`, `mep.git.supported(argv)`, `mep.git.use_cli(bool)`.
`MEP_GIT_BACKEND=cli` (or `mep.git.use_cli(true)`) is the fallback switch.
Ported call sites: every `mep_git_run` in kBuiltinGit (status, log, graph,
branches, stash, previews, commit, stage/unstage/discard/clean, checkout,
branch delete, stash push/apply/pop/drop, merge), the file tree's ignored
scan, the workspace dirty poll and picker, and in `editor.cpp` worktree
add/remove/list/prune, `rev-parse --show-toplevel`, the git gutter's
`git show <base>:<file>` and hunk staging (was `git apply --cached
--unidiff-zero`, now `StageContent` splices the index blob directly).

`mep-git-test` cross-checks byte-for-byte against the git CLI (507 checks:
status v1/v2/long incl. ignored/untracked modes, renames, conflicts,
submodules, intent-to-add, skip-worktree, symlinks, mode changes; log
formats; `log --graph --all --color=always`; show/diff/stat; combined
`--cc` diffs of evil and octopus merges; for-each-ref with upstream
tracking; packed repos after `git gc`; index v2/v3/v4 round trips; and
`git fsck --strict` after in-house commits, stash, branch, checkout,
worktree ops), and runs status/log on this repository (status ~7 ms here,
~0.12 s on a synthetic 40k-file repo). `git show --stat --patch` matched
git byte-for-byte on 431 of this repository's 438 commits; the other 7
differ only in hunk-header function names in `.mm` files, where Apple's
git applies its system `gitattributes` `diff=objc` driver (userdiff
funcname patterns are not implemented -- see below).

Read side first (covers status bar, file tree, log, diff, blame views):

- [x] Repository discovery (`.git` dir/file, worktrees, `commondir`,
      `GIT_DIR`/`GIT_WORK_TREE`/`GIT_CEILING_DIRECTORIES`, bare repos).
- [x] Objects: loose (zlib inflate), packfiles + `.idx` v1/v2, OFS/REF
      deltas, delta-base cache, alternates. SHA-1 in `src/git/sha1.cpp`
      (object ids only; Phase 2's crypto module can absorb it).
- [x] Refs: loose, `packed-refs`, symbolic refs, detached HEAD, per-worktree
      refs, reflog read/write, `shorten_unambiguous_ref`, revision parsing
      (`~`, `^`, `^{}`, `@{n}`, `@{u}`, `rev:path`, `:path`, `:n:path`).
- [x] Index v2/v3/v4 read *and* write (atomic `index.lock`), stat cache with
      the racy-git check; `.gitignore`/`info/exclude`/`core.excludesFile`
      (own wildmatch port); status incl. untracked-directory collapsing,
      `--ignored`, `-uall`, nested repos, submodules (`S<c><m><u>`).
- [x] Config parsing (system incl. Apple's, global XDG + `~/.gitconfig`,
      repo, `config.worktree`, `include.path`, `includeIf gitdir:/onbranch:`,
      `GIT_CONFIG_*`), user identity from env/config.
- [x] Log/graph walking (date order; git's `sort_in_topological_order` for
      `--graph`/`--topo-order`), pretty formats (`%h %H %ad %an %D %d %s %b
      %B %gd %gs %cr %x..` ...), `--graph` (a port of `graph.c`, colors
      included), xdiff-equivalent line diff (Myers with xdiff's
      preprocessing and cost heuristics, `xdl_change_compact` with the
      indent heuristic), unified patches, `--stat`, rename detection
      (diffcore-delta similarity), dense combined diff for merges, stash
      list, `for-each-ref` (sorting, `%(upstream:track)`), ahead/behind.
- [ ] userdiff drivers from `.gitattributes` (`diff=<lang>` funcname
      patterns, `-diff`, textconv): hunk headers always use git's default
      funcname heuristic. Needs a POSIX-ERE-compatible matcher.
- [ ] `git log -- <path>` (path-limited history / simplification) and
      `blame`: not used by mep yet, still CLI if a user script asks.

Write side:

- [x] Write blob/tree/commit objects (loose), index write, `add` (incl.
      `-A`, deletions, conflict resolution, embedded repos as gitlinks),
      `reset` (mixed/hard, paths), `rm`, `commit` (`-F`/`-m`, `--cleanup`,
      `--amend`, `-a`; author/committer from config + `GIT_AUTHOR_*`/
      `GIT_COMMITTER_*`; `pre-commit`, `prepare-commit-msg`, `commit-msg`,
      `post-commit` hooks run when present and executable, falling back to
      `/bin/sh` for a shebang-less hook, as git does), branch
      create/delete (`-d` merged check, `-D`), checkout of a branch/commit/
      remote-tracking DWIM with git's two-way merge rules (refuses to clobber
      local changes or untracked files, with git's messages), `checkout
      [<tree-ish>] -- <path>`, `clean -fd`, stash push (`-u`, `-m`)/apply/
      pop/drop/show/list, worktree add (`-b`, existing branch, admin files
      under `.git/worktrees/<name>`)/list/remove (dirty check, `--force`)/
      prune, `hash-object`, `cat-file`, `rev-parse`.
- [~] Merge (3-way, conflict markers) then rebase (non-interactive):
      **fast-forward merges are in-house**; a merge that needs a real
      three-way content merge, `rebase`, and a `stash apply/pop` whose
      changes overlap local edits run the git CLI from the worker thread
      (`RunCommand`'s fallback in `cli.cpp`). To do: an in-house 3-way
      file merge (xdiff's `xdl_merge` equivalent) + `merge-ort`-style tree
      merge would remove all three.

Network:

- [ ] Smart HTTP protocol v2 over native HTTPS: fetch/pull (packfile
      receive + index), push (pack generation, deltas optional at
      first). Credentials: Phase 6 secrets store, `GIT_ASKPASS` prompt
      in a mep dialog, existing credential helpers optional.
      **Still the git CLI** (push/pull/fetch/clone and `push --delete`):
      security-critical, waits for Phases 2/3.
- [ ] SSH remotes: own SSH client (curve25519-sha256, ssh-ed25519 /
      ecdsa / rsa-sha2, chacha20-poly1305@openssh, agent protocol for
      keys) reusing Phase 2 primitives. Until then, run the user's `ssh`
      as a *transport only* (git does the same) -- listed in the
      allowlist as a temporary exception.

Port order: Lua `mep_git_run` call sites get a `mep.git.*` API with the
same result shapes, one view at a time; `editor.cpp` worktree code next.
(Done for every local operation; see the status note above.)

### Phase 5 -- audio capture (removes ffmpeg) and Linux audio output

- [x] `mep::audio::Capture`: macOS AudioQueue/AudioUnit input
      (AudioToolbox is already linked; needs the microphone TCC entry in
      the signed `mep.app`), Windows WASAPI capture, Linux below.
      *(2026-10-16: `src/audio_capture.{h,cpp}` + `audio_capture_macos.mm`.
      macOS: AudioQueue float32 mono 48 kHz input; AVFoundation checks /
      requests the microphone permission first, so a refusal is an error,
      not a silent recording, and the responsible app's Info.plist is
      checked for `NSMicrophoneUsageDescription` before asking (TCC kills
      the process otherwise -- measured: the mep.app installed before this
      change lacks it, so `just install-app` must be re-run). `install-app`
      now writes the key. Windows: WASAPI shared-mode polling thread,
      float/PCM16/PCM32 mix formats -- written but never compiled (no
      Windows machine). Linux: libasound `snd_pcm` capture ("default",
      S16 48 kHz) -- not compiled here either (no Linux machine).
      Emscripten: Start() fails with an error. Real-microphone run of
      `mep-audio-capture-smoke` still to do from a terminal with mic access.)*
- [x] 16 kHz mono resampler + WAV chunk writer (`wav_doc.cpp`) feeding
      the STT uploader; delete `mep.stt_record_cmd`'s ffmpeg default.
      *(2026-10-16: Kaiser-windowed-sinc `Resampler` (32 zero crossings,
      cutoff 0.9 x lower Nyquist, exact rational positions, delay
      compensated), `WriteWavPcm16` in wav_doc, `SegmentWriter` producing
      `<prefix>_000.wav`... like ffmpeg's segment muxer. Lua:
      `mep.audio_record_start{dir, prefix, chunk_seconds, sample_rate,
      on_chunk, on_warn, on_error, on_exit}` / `mep.audio_record_stop(id)`
      (an in-process job). STT records in-house unless the user sets
      `mep.stt_record_cmd` (external override, the old ffmpeg flow).
      `mep-audio-capture-test` (ctest, no microphone): length, frequency,
      passband gain, >100 dB out-of-band rejection, block-size invariance,
      WAV header bytes, segment sizes.)*
- [ ] Linux output and capture without libasound: speak the PulseAudio
      native protocol over `$XDG_RUNTIME_DIR/pulse/native` (works on
      PipeWire desktops via pipewire-pulse), with direct kernel ALSA
      (`/dev/snd/pcmC*D*{p,c}` ioctls) as the fallback for systems
      without a sound server. Then drop `find_package(ALSA)`.

### Phase 6 -- secrets store (removes secret-tool)

- [ ] `mep::secrets` interface: get/set/delete by service + account.
- [ ] macOS: existing `mail_keychain_macos.cpp` (Security.framework).
- [ ] Windows: `CredReadW`/`CredWriteW`.
- [ ] Linux: minimal D-Bus client (Unix socket, SASL EXTERNAL, message
      marshalling) talking to `org.freedesktop.secrets`; encrypted file
      under `$XDG_DATA_HOME/mep` (PBKDF2 + AES-GCM, both in-house after
      Phase 2) when no Secret Service is running. The D-Bus client is
      reusable later (notifications, portals, Wayland file dialogs).

### Phase 7 -- Linux windowing without Xlib, GL loaded at runtime

- [ ] Load libGL/libGLX (and libEGL later) with `dlopen` and resolve
      everything through `gfx/gl_loader.cpp`, so GL is a runtime choice,
      not a link-time one; use the in-house software `rasterizer.cpp`
      when no GL is present (same on Windows/macOS). This is the most
      that can be done: the GL driver *is* the GPU interface.
- [ ] Own X11 wire-protocol client: connection setup + `.Xauthority`,
      core requests/events, keyboard mapping (core + XKB), XInput2,
      XTEST (agent UI input), RANDR, DAMAGE/Composite (GUI embedding),
      MIT-SHM for software-rendered frames, selections (clipboard).
      GLX context creation still needs libGL's GLX entry points with a
      `Display*`; options: (a) keep a dlopened libX11 *only* for the GLX
      handshake, or (b) create the GL context through EGL on our own
      window. Pick after a spike.
- [ ] Port `backend_native.cpp`, `agent_ui_input.cpp`, `gui_embed_x11.cpp`,
      `gui_embed_webview_x11.cpp`; drop `find_package(X11)`.
- [ ] Later, separately: a native Wayland backend speaking the wire
      protocol directly (no libwayland), EGL via dlopen.

### Phase 8 -- smaller replacements

- [x] **Emscripten GLFW:** replaced by `gfx/backend_native_emscripten.cpp`
      (emscripten/html5.h events + `emscripten_webgl_create_context`, WebGL 2);
      no `-sUSE_GLFW` anywhere. *(2026-10-16. Also fixed: the web build
      was broken at HEAD (host include paths leaking into emcc, no
      `emscan-deps`, PCH vs modules, -Werror on wasm-only warnings, a
      `</script>` in SINGLE_FILE output, WebGL 1 context); canvas buffer
      follows its CSS size; per-frame key edges cleared after each
      main-loop iteration (the GLFW build left keys stuck). Verified in
      headless Firefox: draw, typing, motions, :cmds, / search, Ctrl
      chords, click, wheel, resize, cursor. Not verified: IME/dead keys,
      touch, clipboard, Chrome/Safari/WebKitGTK.)*
- [x] **Blender import:** in-house `.blend` reader (header, file blocks,
      SDNA reflection) extracting meshes, materials and transforms into
      the existing glTF/model pipeline; keep the `blender` path as an
      opt-in for files the reader cannot handle.
      Done (2026-10-16): `blend_file.cpp` (classic 12-byte and Blender 5
      `BLENDER17-01` headers, 4/8-byte pointers, either endianness, SDNA
      field lookup by name, per-ID pointer resolution -- Blender 5 reuses
      DATA addresses between IDs), `blend_scene.cpp` (mesh objects with
      parent chains baked to world space, Y-up like the glTF exporter;
      Blender 5 `AttributeStorage`, 3.x/4.x CustomData attributes,
      MVert/MPoly/MLoop, 2.4x MFace/MTFace; UVs, smooth/flat faces,
      material slots incl. object-linked; Principled/Diffuse BSDF inputs
      or the legacy diffuse colour; base-colour image from disk or packed)
      and an in-house zstd decoder (`zstd_decode.cpp`, RFC 8878, checksum
      verified) for compressed files; gzip goes through `deflate.cpp`.
      `mep.opt.blend_importer` = `'builtin'` (default) / `'auto'` (CLI
      when the reader fails) / `'blender'`. Not evaluated: modifiers (a
      notification lists them), shape keys, armatures, constraints,
      drivers/animation, bone parenting, collection instances, custom
      normals. Tests: `mep-blend-import-test` on real 2.48-5.0 files in
      `test/fixtures/blend/`, plus zstd vectors from the reference CLI.
- [ ] **Xvfb on macOS:** embedding the user's X11 programs is an
      integration, not a core feature. Keep it optional (documented as
      needing XQuartz) unless we decide to build a minimal in-house X
      server -- not recommended.
- [ ] **System TeX / pdftoppm:** already optional with in-house defaults
      (TEX_ENGINE_PLAN.md); nothing to do beyond Phase 0's allowlist.

### Phase 9 -- build and dev tooling

- [~] Port `scripts/*.py` and `tools/*.py` that the normal build or
      `just test` needs (help page generation, `check_help.py`,
      amalgamation) to small C++ tools built by CMake, or to Lua run by
      `mep --headless`. Benchmarks/fixture generators can stay Python
      longer (dev only) but should follow.
      *(2026-10-16: `tools/check_help.cpp`, `gen_reference_pages.cpp`,
      `amalgamate.cpp`, `filter_compile_commands.cpp` (EXCLUDE_FROM_ALL
      host tools `mep-check-help`, ...), byte-identical output to the
      Python versions; Python is no longer needed to build or test.
      Still Python (dev-only QA/bench/fixtures): the `*_live_check.py`
      drivers, `mepml_golden.py`, benches, `pdf_make_*.py`,
      `tex_bundle.py`, `tex_compare.py`, `web2cpp/`, `font_coverage.py`,
      `r_kernel_check.py`, `rmd_*`, `ai_visual_shots.py`. Note:
      `mep-amalgam` does not compile on macOS (stale module list in
      amalgamate.cpp, carried over from the .py).)*
- [ ] `gen_r_names.R`, `gen_maxima_names.py`, `gen_c_names.py`: these
      generate committed tables; keep the outputs committed and treat the
      generators as optional.
- [ ] Remove `deno.json`/`deno.lock` after Phase 1's Deno items. *(Only
      the wasm launcher's `launch` task is left.)*
- [ ] Nix flake stays as a convenience, not a requirement: a plain
      `cmake -B build && cmake --build build` must work on a clean
      macOS/Linux/Windows with only a compiler + CMake (+ the WebKit
      SDK for `:Browser` on Linux). Add a CI job that proves it.

---------------------------------------------------------------------------

## 4. Decisions to make

**Lua and tree-sitter.** Both are third-party code compiled into mep.
Recommendation: vendor and own them (Phase 1 for Lua; tree-sitter is
already vendored) rather than rewrite now.

* Replacing Lua means a new scripting language and breaking every user's
  `init.lua` and every built-in Lua module in main.cpp; it is a product
  decision, not a dependency fix. If wanted later: an in-house Lua 5.4
  compatible interpreter is feasible (mep already has a JS engine) and
  keeps configs working.
* Replacing tree-sitter means an in-house incremental parser plus
  grammars for C, C++, Lua, Python, JS, Markdown, Org, R and mepml. A
  realistic middle step: an in-house parser *generator* that reads the
  existing `grammar.js`-derived `grammar.json` and emits tables, which
  also removes the Node + tree-sitter CLI dev dependency for mepml.
  mepml itself could get a hand-written parser (`mepml_doc.cpp` already
  parses it) and drop its tree-sitter grammar.
* The grammars themselves are being replaced one at a time by mep's own
  (R, Nix and justfiles so far): see `TREESITTER_GRAMMAR_PLAN.md`.

**TLS 1.2.** Only if Phase 2's live test list shows a server we need that
lacks 1.3.

**GLX vs EGL on X11.** Decide in the Phase 7 spike.

**SSH transport for git.** Temporary `ssh` exception vs. building the SSH
client in Phase 4.

---------------------------------------------------------------------------

## 5. Rough size and order

| Phase | Size | Removes |
| --- | --- | --- |
| 0 guardrails | S | -- (prevents regressions) |
| 1 quick wins | S–M | Lua fetch, `security`, osascript, find/grep/sh sleep, Chromium, Deno |
| 2 crypto + TLS | L | OpenSSL |
| 3 HTTPS/SMTP | M | curl |
| 4 git | XL | git CLI (ssh possibly later) |
| 5 audio | M | ffmpeg, ALSA |
| 6 secrets | M | secret-tool |
| 7 X11/GL | L | libX11/Xtst/Xi, link-time libGL |
| 8 misc | M | GLFW port, blender |
| 9 tooling | M | Python, Deno in the build |

Suggested order: 0 → 1 → 2 → 3 → 6 → 5 → 4 → 7 → 8 → 9 (git is large
and its network half needs 2+3; X11 is large and invisible to most macOS
users).
