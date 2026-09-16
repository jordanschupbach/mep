{
  description = "A basic flake";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.flake-utils = {
    url = "github:numtide/flake-utils";
    inputs.systems.follows = "systems";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # BUILD_PERFORMANCE_PLAN.md Round 2 Phase D: the project's default
        # compiler, switched from the implicit default (GCC) to Clang --
        # required for real C++20 modules (GCC 15's own module support was
        # verified broken in this environment: it corrupts unrelated
        # <iostream> parsing even in non-module translation units, where
        # Clang's modules work correctly end to end). clangStdenv (not a
        # bare `pkgs.clang` package inside the default GCC-based shell) so
        # every wrapper script/env var (NIX_CFLAGS_COMPILE and friends)
        # that make header discovery "just work" -- for both ordinary
        # compilation *and* clang-scan-deps' own P1689 module-dependency
        # scanning, which does NOT reuse the same implicit search paths a
        # bare `pkgs.clang` inside a GCC-default shell gets -- are set up
        # consistently by nixpkgs itself, not hand-reconstructed the way
        # justfile's lint-iwyu recipe has to for its own separate,
        # intentionally-pinned clang toolchain.
        mepStdenv = pkgs.clangStdenv;

        # webview_deno downloads a prebuilt libwebview.so with no RPATH of
        # its own; it (and the webkitgtk stack it dlopen()s) needs every
        # library it was linked against to be findable via LD_LIBRARY_PATH,
        # not just its immediate dependency. Pull in the full runtime
        # closure rather than hand-maintaining that list.
        # WebKitGTK also bind-mounts every LD_LIBRARY_PATH entry into the
        # bubblewrap sandbox it runs its WebProcess in, so the list must
        # only contain directories that actually exist -- closure members
        # like *.cfg outputs have no `lib/` and would crash bwrap.
        webviewRuntimeClosure = pkgs.closureInfo { rootPaths = [ pkgs.webkitgtk_6_0 ]; };
        webviewLibraryPath = pkgs.lib.concatMapStringsSep ":" (p: p + "/lib") (
          builtins.filter (
            p: p != "" && builtins.pathExists (p + "/lib") && builtins.readFileType (p + "/lib") == "directory"
          ) (pkgs.lib.splitString "\n" (builtins.readFile "${webviewRuntimeClosure}/store-paths"))
        );

        # Only C/C++/Lua/Python/JavaScript/Markdown/Org/R are compiled
        # into mep itself (see CMakeLists.txt's TS_GRAMMAR_NAMES) --
        # every other language's Treesitter highlighting is resolved at
        # runtime instead, by dlopen()ing a `<name>.so` found on
        # $MEP_TS_PARSER_PATH (src/treesitter.cpp's
        # LoadDynamicLanguage/DynamicSearchPaths). `withPlugins` bundles
        # a set of grammars into one output directory as exactly that
        # `<name>.so` naming (matching each grammar's own
        # `tree_sitter_<name>` symbol) -- point MEP_TS_PARSER_PATH at it
        # and mep picks up whatever's in this list with no further setup.
        # Extend this list (`pkgs.tree-sitter-grammars.<attr>`, `nix repl`
        # or `nix eval .#... ` to check an attr exists) for any language
        # not already covered; if it's genuinely unavailable on Nix,
        # mep.nvim/tree-sitter's own build cache or nvim-treesitter's
        # install dir are checked too (lower priority) -- see
        # DynamicSearchPaths' doc comment in treesitter.cpp for that list.

        # mep's CMakeLists.txt pulls Lua in at configure time via
        # FetchContent (URL), which needs network access -- fine for `just
        # build-native` in the devShell, but Nix's build sandbox has none.
        # Fetched here instead as a regular Nix derivation (network
        # allowed for fixed-output derivations, verified by hash) and
        # handed back to the same FetchContent machinery via
        # FETCHCONTENT_SOURCE_DIR_LUA, so CMakeLists.txt itself needs no
        # changes. This hash mirrors CMakeLists.txt's own URL_HASH and
        # must be kept in sync with it.
        luaTarball = pkgs.fetchurl {
          url = "https://www.lua.org/ftp/lua-5.4.7.tar.gz";
          sha256 = "9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30";
        };

        # pdfium (pdf_doc.cpp) was added to CMakeLists.txt's FetchContent
        # set after lua was already wired up above, and this file wasn't
        # updated to match -- with FETCHCONTENT_FULLY_DISCONNECTED=ON
        # below, CMake never downloads it, leaving pdfium_SOURCE_DIR empty
        # and failing configure (the pdfium imported-target paths don't
        # exist). fetchzip (unlike fetchurl+manual tar for lua above)
        # unpacks the archive itself and hands back a ready source
        # directory, so no preConfigure extraction step is needed.
        # Unlike fetchurl, fetchzip's fixed-output hash is of the
        # *unpacked* tree (post stripRoot), not the raw archive -- so
        # this hash is NOT the same as CMakeLists.txt's own URL_HASH pin
        # and had to be computed separately (`nix hash path` /
        # `nix-prefetch-url --unpack` against the archive).
        # Only the Linux x86_64 asset/hash is verified (mirrors
        # CMakeLists.txt's own `else()` branch, which is likewise the only
        # one it pins a real build/dev environment against); extend with
        # Darwin's arm64/x64 hashes from CMakeLists.txt if this flake ever
        # targets aarch64-linux or Darwin.
        pdfiumSrc = pkgs.fetchzip {
          url = "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8009/pdfium-linux-x64.tgz";
          hash = "sha256-/TbTmcM7FRmCXpk42WXYTT/Mw1I4WscLmFMWTLr/ZTo=";
          # Release tarball has multiple top-level entries (LICENSE,
          # include/, lib/, ...), not one wrapping directory, so
          # fetchzip's default stripRoot=true would error -- keep it flat.
          stripRoot = false;
        };

        tsGrammars = pkgs.tree-sitter.withPlugins (
          p: with p; [
            tree-sitter-bash
            tree-sitter-c-sharp
            tree-sitter-css
            tree-sitter-go
            tree-sitter-haskell
            tree-sitter-html
            tree-sitter-java
            tree-sitter-json
            tree-sitter-julia
            tree-sitter-ocaml
            tree-sitter-ocaml-interface
            tree-sitter-php
            tree-sitter-ruby
            tree-sitter-rust
            tree-sitter-scala
            tree-sitter-typescript
            tree-sitter-tsx
            tree-sitter-yaml
            tree-sitter-toml
            tree-sitter-nix
            tree-sitter-zig
            tree-sitter-elm
            tree-sitter-kotlin
            tree-sitter-scss
            tree-sitter-svelte
            tree-sitter-vue
            tree-sitter-hcl
            tree-sitter-make
            tree-sitter-vim
            tree-sitter-cmake
            tree-sitter-clojure
            tree-sitter-dart
            tree-sitter-dockerfile
            tree-sitter-graphql
            tree-sitter-proto
            tree-sitter-just
            tree-sitter-fish
            tree-sitter-elixir
            tree-sitter-erlang
            # Added alongside org-babel's multi-language port
            # (mep_org_babel_lang_ts_ft in main.cpp's kBuiltinSyntax) so
            # every babel-supported language also gets real syntax
            # highlighting inside its `#+begin_src` blocks, not just the
            # ones that already had a grammar for some other reason.
            tree-sitter-crystal
            tree-sitter-fortran
            tree-sitter-perl
            tree-sitter-nim
            tree-sitter-d
          ]
        );

        mepPackage = mepStdenv.mkDerivation {
          pname = "mep";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            # CMakeLists.txt's own Clang-modules workaround (top of that
            # file) shells out to a bare `gcc` at configure time to learn
            # libstdc++'s header search paths, which it then passes to
            # clang-scan-deps explicitly (that tool, unlike ordinary
            # clang++ compiles, doesn't inherit the wrapper's own
            # NIX_CFLAGS_COMPILE-injected -isystem paths). In the `nix
            # develop` devShell this `gcc` happened to already be on PATH
            # as a side effect of the gfortran babel toolchain below, which
            # masked that mepPackage's own nativeBuildInputs never actually
            # provided one -- so a plain sandboxed `nix build` had no real
            # `gcc`, execute_process() silently found nothing, no -isystem
            # flags got added, and every clang-scan-deps invocation failed
            # with "'cstdio' file not found" (or similar) once it reached a
            # target CMake schedules C++20 module dependency scanning for.
            pkgs.gcc
            # NOT pkgs.ccache here (unlike devShell below): CMakeLists.txt's
            # find_program(ccache) auto-detects and wires it in unconditionally
            # (no opt-out flag), and ccache needs a writable $HOME/cache dir to
            # create its cache -- but the Nix build sandbox's $HOME isn't a
            # real writable directory, so every compile fails with "ccache:
            # error: Permission denied". A fixed-output, one-shot sandboxed
            # build gets no benefit from ccache anyway (nothing persists
            # between builds), so it's simplest to just keep ccache off this
            # derivation's PATH and let devShell's copy handle interactive
            # incremental builds instead.
            #
            # BUILD_PERFORMANCE_PLAN.md Round 2 -- CMakeLists.txt
            # auto-detects both and prefers mold; only need to be on
            # PATH for that find_program()/check_cxx_compiler_flag()
            # logic to pick them up.
            pkgs.mold
            pkgs.lld
          ];
          buildInputs = [
            # gfx/backend_native.cpp's own hand-written X11/GLX windowing/
            # input/context implementation (GLFW_REMOVAL_PLAN.md) --
            # libGL provides both OpenGL and GLX (Mesa exports both from
            # the same shared object), libx11 is core Xlib, libxtst is
            # XTest (agent_ui_input.cpp's synthetic input). libxi is a
            # real transitive dependency of libxtst (libXtst.so links
            # libXi.so.6 on this distro), not something mep calls
            # directly -- agent_ui_input.h's own comment explains why
            # XInput2 itself was deliberately not used. No
            # libxrandr/libxinerama/libxcursor: those were GLFW's own X11
            # backend's dependencies (monitor enumeration, themed cursor
            # loading) for functionality this backend never uses (no
            # multi-monitor queries, no themed cursors -- XCreateFontCursor's
            # core-Xlib cursor font is enough).
            pkgs.libGL
            pkgs.libx11
            pkgs.libxi
            pkgs.libxtst
            pkgs.openssl
            # In-house ALSA audio backend (gfx/backend_native_audio.cpp,
            # see MINIAUDIO_REMOVAL_PLAN.md) -- PulseAudio/PipeWire users
            # are covered transparently via their ALSA compatibility shim.
            pkgs.alsa-lib
          ];

          # Lua isn't a CMake project of its own (see CMakeLists.txt's own
          # comment: it globs src/*.c by hand), so FetchContent just needs
          # the tarball extracted somewhere on disk -- unpack it ourselves
          # since FETCHCONTENT_FULLY_DISCONNECTED skips that step.
          # cmakeFlagsArray (not cmakeFlags) because $NIX_BUILD_TOP's value
          # isn't known until the build runs.
          preConfigure = ''
            mkdir -p "$NIX_BUILD_TOP/lua-src"
            tar xzf ${luaTarball} --strip-components=1 -C "$NIX_BUILD_TOP/lua-src"
            cmakeFlagsArray+=(
              "-DFETCHCONTENT_SOURCE_DIR_LUA=$NIX_BUILD_TOP/lua-src"
              "-DFETCHCONTENT_SOURCE_DIR_PDFIUM=${pdfiumSrc}"
              "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            )
          '';

          # BUILD_PERFORMANCE_PLAN.md -- explicit -GNinja rather than
          # relying on nixpkgs' cmake setup hook to auto-select it just
          # because pkgs.ninja is present in nativeBuildInputs; explicit
          # here is one line and doesn't depend on that behavior holding
          # across a nixpkgs bump.
          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" "-GNinja" ];

          installPhase = ''
            runHook preInstall
            install -Dm755 mep "$out/bin/mep"
            install -Dm644 "$NIX_BUILD_TOP/$sourceRoot"/help/*.html -t "$out/share/mep/help"
            runHook postInstall
          '';
        };

        # R DAP support (mep.dap_adapters.r, kBuiltinDap in src/main.cpp):
        # vscDebugger isn't on CRAN, so nixpkgs has no rPackages.vscDebugger
        # -- built by hand the same way nixpkgs itself packages GitHub-only
        # R packages, via rPackages.buildRPackage + fetchFromGitHub. Pinned
        # to the latest tag (v0.5.9) at the time this was added; bump `rev`/
        # `hash` together if a newer release is needed (`nix run
        # nixpkgs#nix-prefetch-github -- ManuelHentschel vscDebugger --rev
        # <new-rev>` prints the matching hash). jsonlite/R6 are its own
        # declared Imports (DESCRIPTION); `tcltk` is a base R package
        # already built into rWrapper's R, not a separate rPackages entry.
        vscDebuggerR = pkgs.rPackages.buildRPackage {
          name = "vscDebugger";
          src = pkgs.fetchFromGitHub {
            owner = "ManuelHentschel";
            repo = "vscDebugger";
            rev = "aab10b8412c04df12d2f9138c132ab73c336f0d3"; # v0.5.9
            hash = "sha256-xSrsZ/xPaqfRu0QvcFoTcfmjsPdaTarG8+MhH/wt1RM=";
          };
          propagatedBuildInputs = with pkgs.rPackages; [
            jsonlite
            R6
          ];
        };
      in
      {
        packages.default = mepPackage;
        apps.default = flake-utils.lib.mkApp { drv = mepPackage; };

        devShells.default = (pkgs.mkShell.override { stdenv = mepStdenv; }) {
          packages = [
            pkgs.cmake
            pkgs.ninja
            # BUILD_PERFORMANCE_PLAN.md -- CMakeLists.txt auto-detects
            # and wires this in via find_program(ccache); it only has to
            # be on PATH here for that to take effect. `ccache -s` shows
            # hit-rate stats; `ccache -C` clears the cache.
            pkgs.ccache
            # BUILD_PERFORMANCE_PLAN.md Round 2 -- CMakeLists.txt
            # auto-detects both (mold preferred, lld independently
            # selectable via -DMEP_LINKER=lld); just need to be on PATH.
            pkgs.mold
            pkgs.lld
            # BUILD_PERFORMANCE_PLAN.md Round 2 Phase B -- clang (for a
            # one-off -ftime-trace profiling build; the project's own
            # default compiler stays gcc, see CMakeLists.txt) and
            # ClangBuildAnalyzer, which aggregates those per-TU traces into
            # a report of the most expensive headers/templates to compile.
            # Not used by any `just` recipe by default -- an opt-in
            # `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-ftime-trace`
            # build in a separate build dir, per this phase's own notes.
            pkgs.clang
            pkgs.clangbuildanalyzer
            pkgs.emscripten
            pkgs.deno
            pkgs.just
            pkgs.pkg-config
            # Native (non-wasm) gfx:: backend build deps (X11/GLX/OpenGL,
            # see gfx/backend_native.cpp / GLFW_REMOVAL_PLAN.md), for
            # `just build-native` -- see mepPackage's own buildInputs
            # above for why this list is shorter than it used to be
            # (libxrandr/libxinerama/libxcursor/glfw dropped; libxi kept,
            # a real transitive dependency of libxtst).
            pkgs.libGL
            pkgs.libx11
            pkgs.libxi
            pkgs.libxtst
            pkgs.openssl
            # In-house ALSA audio backend (gfx/backend_native_audio.cpp,
            # see MINIAUDIO_REMOVAL_PLAN.md) -- PulseAudio/PipeWire users
            # are covered transparently via their ALSA compatibility shim.
            pkgs.alsa-lib
            # Runtime dep of webview_deno, used by the launcher to open a
            # native window around the wasm build.
            pkgs.webkitgtk_6_0

            # LaTeX/math-mode inline preview (<leader>otl, mep.org_latex_toggle_ui
            # in src/main.cpp's kBuiltinOrgLatex): tectonic compiles a fragment's
            # `\documentclass{standalone}` wrapper straight to a tightly-cropped
            # PDF (self-contained, no separate TeX Live install -- it fetches
            # packages into its own cache on first use), and pdftoppm (from
            # poppler-utils) rasterizes that PDF to the PNG mep.org_latex_scan
            # then displays via the same inline-image pipeline <leader>oti uses.
            pkgs.tectonic
            pkgs.poppler-utils

            # Interpreters/compilers for org-babel (mep.org_babel_langs in
            # src/main.cpp) code-block execution -- not needed to build mep
            # itself (only `just build-native`'s own cmake/ninja/gcc chain
            # is), just to run a `#+begin_src <lang>` block via Ctrl-C
            # Ctrl-C. Mirrors mep.nvim/flake.nix's own devShell list for the
            # same language set (see lua/mep/org/babel.lua's `M.languages`
            # there) so both editors work against the same fixture files
            # with no toolchain gaps between them; gcc/g++ need no separate
            # entry here since pkgs.mkShell already puts the default
            # stdenv's C/C++ compiler on PATH (needed to build mep itself).
            # Lua 5.4 (not LuaJIT) to match mep's own vendored interpreter
            # version exactly (see luaTarball above, lua-5.4.7) -- some
            # `#+begin_src lua` blocks (e.g. integer-division `//`,
            # bitwise operators) are 5.4 syntax LuaJIT's 5.1-with-
            # extensions dialect doesn't accept.
            pkgs.lua5_4
            # debugpy backs mep.dap_adapters.python (kBuiltinDap,
            # src/main.cpp: `python3 -m debugpy.adapter`), a real stdio DAP
            # server -- alongside numpy for org-babel Python blocks, and
            # matplotlib for the Jupyter notebook mode's inline figures
            # (src/notebook_doc.cpp's kernel captures every open figure as
            # PNG at the end of a cell; examples/notebook_example.ipynb
            # has a cell that draws one) and the Python language UI's plot
            # pane (kBuiltinLanguageUiPython).
            (pkgs.python3.withPackages (ps: [ ps.numpy ps.debugpy ps.matplotlib ])) # Python
            pkgs.nodejs # JavaScript
            pkgs.ruby
            # perl.withPackages, not bare pkgs.perl -- Perl::LanguageServer
            # (mep.lsp_servers.perl_languageserver) is a pure Perl module
            # with no standalone binary of its own; `perl -MPerl::
            # LanguageServer` needs it on @INC, same reasoning as
            # rWrapper/languageserver below.
            (pkgs.perl.withPackages (ps: [ ps.PerlLanguageServer ]))
            # rWrapper (not the bare pkgs.R) with the `languageserver`
            # package built in -- R is otherwise one of the babel
            # languages with no LSP server registered at all
            # (mep.lsp_servers.r_languageserver, `languageserver::run()`)
            # since nixpkgs' plain pkgs.R has no packages preinstalled and
            # `library(languageserver)` would fail without this. Still
            # provides the same `R`/`Rscript` binaries babel's own
            # execution already used.
            # ggplot2/rpart/rpart.plot cover reports/iris.org's babel
            # blocks (rpart itself ships as one of R's own "recommended"
            # packages so it's already on .libPaths() without being
            # listed here, but rpart.plot and ggplot2 are plain CRAN
            # packages that need to be pulled in explicitly, same as
            # languageserver above).
            # rmarkdown/knitr back the .Rmd/.Rnw Run button
            # (mep.run_button_run_rmd/run_button_run_rnw, kBuiltinRunButton
            # in src/main.cpp): rmarkdown::render() for .Rmd, knitr::knit()
            # (then tectonic, already above -- not knitr::knit2pdf's own
            # pdflatex/texi2pdf, to avoid a second LaTeX toolchain) for
            # .Rnw.
            (pkgs.rWrapper.override {
              packages = with pkgs.rPackages; [
                languageserver
                ggplot2
                rpart_plot
                rmarkdown
                knitr
              ] ++ [ vscDebuggerR ]; # mep.dap_adapters.r (see vscDebuggerR above)
            })
            # rmarkdown::render()'s HTML/Word output goes through pandoc --
            # nixpkgs' rPackages.rmarkdown does NOT vendor its own copy the
            # way RStudio Desktop's bundled installation does, so without
            # this a render() call fails outright looking for a `pandoc`
            # binary on PATH.
            pkgs.pandoc
            pkgs.php
            pkgs.rustc
            pkgs.cargo
            pkgs.go
            pkgs.bun # TypeScript, via `bun <file>` directly
            pkgs.beamPackages.elixir
            pkgs.julia-bin
            pkgs.babashka # Clojure, `bb` -- tried before the full `clojure` CLI below
            pkgs.clojure
            pkgs.gfortran
            # .NET 10, not the default pkgs.dotnet-sdk (.NET 8 as of this
            # writing) -- the `csharp` babel entry's `dotnet run <file>.cs`
            # "file-based apps" mode only exists starting .NET 10.
            pkgs.dotnet-sdk_10
            pkgs.scala
            # Zig/Nim/Crystal each compile-and-run a file in one step (`zig
            # run`/`nim r`/`crystal run`); Java is a real two-step
            # javac+java (see mep.nvim's own M.languages.java comment on
            # why its binary_path gets reused as a .class output directory).
            pkgs.zig
            pkgs.nim
            pkgs.crystal
            pkgs.jdk
            # Kotlin/Haskell/OCaml each run a script directly (`kotlin
            # <file>.kts`/`runghc <file>.hs`/`ocaml <file>.ml`); D is a real
            # two-step dmd compile-then-run.
            pkgs.kotlin
            pkgs.ghc
            pkgs.ocaml
            # Built with gcc14Stdenv, not the default gcc15: upstream dmd's
            # own C header importer can't parse gcc 15's system headers,
            # which now use the C23 `nullptr` keyword (stddef.h) -- a real
            # nixpkgs-unstable/dmd incompatibility, not specific to this
            # flake (same override mep.nvim/flake.nix uses, for the same
            # reason). Drop this once nixpkgs' dmd derivation itself
            # accounts for gcc 15 headers.
            (pkgs.dmd.override { stdenv = pkgs.gcc14Stdenv; })

            # LSP servers (mep.lsp_servers in src/main.cpp's kBuiltinLsp,
            # mep.lsp_attach) -- a completely separate concern from the
            # org-babel interpreters/compilers above despite the shared
            # per-language shape: those run a `#+begin_src` block's own
            # body, this is a long-lived `--stdio` JSON-RPC server process
            # mep talks textDocument/* requests to for hover (K)/goto-
            # definition (gd)/diagnostics/completion/etc, including
            # inside a src block (Phase 36 polyglot). Every entry here
            # matches an mep.lsp_servers `cmd[1]` exactly -- covers every
            # babel language with a real, freely-licensed server
            # available in nixpkgs.
            pkgs.pyright # python
            pkgs.lua-language-server # lua
            pkgs.bash-language-server # sh
            pkgs.typescript-language-server # javascript, typescript
            pkgs.clang-tools # c, cpp -- provides clangd (plus clang-tidy/
            # -format etc, harmless extras from the same derivation)
            # lldb-dap backs mep.dap_adapters.cpp/c/rust (kBuiltinDap,
            # src/main.cpp) -- LLVM's own DAP server, confirmed to ship as
            # a sibling binary in pkgs.lldb (not a separate attribute; same
            # "one derivation, multiple binaries" shape as clang-tools
            # above). Not an LSP server -- lives here next to clang-tools
            # since both are the same C/C++ toolchain family.
            pkgs.lldb

            # Static analysis for mep's own C++ (justfile's `lint` recipes):
            # clang-tidy/clangd come from pkgs.clang-tools above already.
            pkgs.cppcheck
            # include-what-you-use pulls in its own pinned clang (currently
            # 21.1.7, vs. clang-tools' 21.1.8 above) since its include-
            # analysis is tied tightly to a matching clang/libclang build --
            # a harmless, self-contained second clang toolchain, not a
            # conflict with clangd/clang-tidy's.
            pkgs.include-what-you-use
            pkgs.solargraph # ruby
            pkgs.rust-analyzer # rust
            pkgs.gopls # go
            # omnisharp-roslyn's own binary is `OmniSharp` (capitalized) --
            # mep.lsp_servers.omnisharp's cmd was fixed to match (was
            # lowercase 'omnisharp', which every csharp block silently
            # failed to spawn against, same class of bug as the earlier
            # python filetype/language-name mismatch).
            pkgs.omnisharp-roslyn # csharp
            pkgs.haskell-language-server # haskell -- provides both a GHC-
            # version-suffixed binary and haskell-language-server-wrapper
            # (what mep.lsp_servers.hls's cmd actually invokes, resolving
            # the right version itself)
            pkgs.ocamlPackages.ocaml-lsp # ocaml -- provides `ocamllsp`
            pkgs.zls # zig
            pkgs.elixir-ls # elixir
            pkgs.clojure-lsp # clojure
            pkgs.kotlin-language-server # kotlin
            # php's server (intelephense) is proprietary-licensed --
            # nixpkgs' own package refuses to build without a separate
            # `allowUnfree = true` opt-in this flake deliberately doesn't
            # make on anyone's behalf. Add `pkgs.nodePackages.intelephense`
            # here yourself (plus `nixpkgs.config.allowUnfree = true` for
            # this flake, e.g. via NIXPKGS_ALLOW_UNFREE=1 or a
            # config.nix override) if you want php polyglot support.
            pkgs.fortls # fortran
            pkgs.metals # scala
            pkgs.nimlsp # nim
            pkgs.crystalline # crystal
            pkgs.serve-d # d -- provides the `serve-d` binary
            # Julia's LanguageServer.jl is a real language *package*, not
            # a nixpkgs binary -- unlike every other server above, the
            # pkgs.julia-bin already listed above (for babel execution)
            # isn't enough on its own; run this once yourself after
            # entering the shell (a real Julia package install, so it
            # needs network access, same as any language's own package
            # manager -- not something a hermetic Nix build can do for
            # you the way rWrapper/perl.withPackages could for R/Perl):
            #   julia -e 'using Pkg; Pkg.add("LanguageServer")'
            # mep.lsp_servers.julials's own invocation is verified
            # correct once that's done.
          ];

          # MEP_WEBVIEW_LD_LIBRARY_PATH: scoped to a separate variable
          # (rather than exported globally as LD_LIBRARY_PATH) so it
          # doesn't shadow libraries for cmake/gcc/emcc; the `run` recipe
          # in the justfile applies it only to the deno launcher process.
          # MEP_TS_PARSER_PATH: read directly by mep itself at runtime
          # (src/treesitter.cpp) to dlopen additional Treesitter grammars
          # -- see the tsGrammars comment above.
          shellHook = ''
            export MEP_WEBVIEW_LD_LIBRARY_PATH="${webviewLibraryPath}"
            export MEP_TS_PARSER_PATH="${tsGrammars}"
            # BUILD_PERFORMANCE_PLAN.md: CMake reads this env var as its
            # default generator whenever a caller (the justfile's plain
            # `cmake -S -B`, or a dev running cmake by hand) doesn't pass
            # its own `-G` -- Ninja schedules the build graph with far
            # less overhead than the default Unix Makefiles generator, on
            # both a from-scratch build and a small incremental one. Only
            # takes effect on a *fresh* build directory -- CMake refuses
            # to change an existing one's generator in place, so an
            # already-configured build/native or build/web from before
            # this change needs a `rm -rf` + reconfigure once to pick it
            # up (this is what `just build-native`/`build-web` already do
            # every invocation anyway, just against a stale generator
            # until that one-time wipe).
            export CMAKE_GENERATOR=Ninja
          '';
        };
      }
    );
}
