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

        # mep's CMakeLists.txt pulls raylib and Lua in at configure time via
        # FetchContent (GIT_REPOSITORY/URL), which needs network access --
        # fine for `just build-native` in the devShell, but Nix's build
        # sandbox has none. Fetched here instead as regular Nix derivations
        # (network allowed for fixed-output derivations, verified by hash)
        # and handed back to the same FetchContent machinery via
        # FETCHCONTENT_SOURCE_DIR_<NAME>, so CMakeLists.txt itself needs no
        # changes. raylib has no hash pin in CMakeLists.txt (GIT_TAG "5.5"
        # floats), so its hash below is only pinned here; the Lua one
        # mirrors CMakeLists.txt's own URL_HASH and must be kept in sync
        # with it.
        raylibSrc = pkgs.fetchFromGitHub {
          owner = "raysan5";
          repo = "raylib";
          rev = "5.5";
          hash = "sha256-J99i4z4JF7d6mJNuJIB0rHNDhXJ5AEkG0eBvvuBLHrY=";
        };
        luaTarball = pkgs.fetchurl {
          url = "https://www.lua.org/ftp/lua-5.4.7.tar.gz";
          sha256 = "9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30";
        };

        # pugixml/miniz/pdfium (office_doc.cpp/pdf_doc.cpp) were added to
        # CMakeLists.txt's FetchContent set after raylib/lua were already
        # wired up above, and this file wasn't updated to match -- with
        # FETCHCONTENT_FULLY_DISCONNECTED=ON below, CMake never downloads
        # them, leaving pugixml/miniz/pdfium_SOURCE_DIR empty and failing
        # configure (pugixml has no sources, `add_library(miniz ...)` has
        # no sources, and the pdfium imported-target paths don't exist).
        # fetchzip (unlike fetchurl+manual tar for lua above) unpacks the
        # archive itself and hands back a ready source directory, so no
        # preConfigure extraction step is needed for any of the three.
        # Unlike fetchurl, fetchzip's fixed-output hash is of the
        # *unpacked* tree (post stripRoot), not the raw archive -- so
        # these hashes are NOT the same as CMakeLists.txt's own URL_HASH
        # pins and had to be computed separately (`nix hash path` /
        # `nix-prefetch-url --unpack` against each archive).
        pugixmlSrc = pkgs.fetchzip {
          url = "https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz";
          hash = "sha256-h2LGUgdr2wcDoz6DosFU3fELSNtiRwNWiod0Cbnvhcc=";
        };
        # miniz's release zip is a flat file:file pair (miniz.c/miniz.h,
        # no wrapping directory) rather than the single wrapping directory
        # fetchzip's default stripRoot=true expects -- pass stripRoot=false
        # to keep it as a flat unpack instead of erroring.
        minizSrc = pkgs.fetchzip {
          url = "https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip";
          hash = "sha256-rG5ndTc+oiCqmSZE2+VgIBsItBDpCuLQsRR14C6UoWg=";
          stripRoot = false;
        };

        # Only the Linux x86_64 asset/hash is verified (mirrors
        # CMakeLists.txt's own `else()` branch, which is likewise the only
        # one it pins a real build/dev environment against); extend with
        # Darwin's arm64/x64 hashes from CMakeLists.txt if this flake ever
        # targets aarch64-linux or Darwin.
        pdfiumSrc = pkgs.fetchzip {
          url = "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8009/pdfium-linux-x64.tgz";
          hash = "sha256-/TbTmcM7FRmCXpk42WXYTT/Mw1I4WscLmFMWTLr/ZTo=";
          # Release tarball has multiple top-level entries (LICENSE,
          # include/, lib/, ...), not one wrapping directory -- same
          # flat-layout case as miniz above.
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

        mepPackage = pkgs.stdenv.mkDerivation {
          pname = "mep";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.glfw
            pkgs.libGL
            pkgs.libx11
            pkgs.libxrandr
            pkgs.libxinerama
            pkgs.libxcursor
            pkgs.libxi
            pkgs.openssl
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
              "-DFETCHCONTENT_SOURCE_DIR_RAYLIB=${raylibSrc}"
              "-DFETCHCONTENT_SOURCE_DIR_LUA=$NIX_BUILD_TOP/lua-src"
              "-DFETCHCONTENT_SOURCE_DIR_PUGIXML=${pugixmlSrc}"
              "-DFETCHCONTENT_SOURCE_DIR_MINIZ=${minizSrc}"
              "-DFETCHCONTENT_SOURCE_DIR_PDFIUM=${pdfiumSrc}"
              "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            )
          '';

          cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

          installPhase = ''
            runHook preInstall
            install -Dm755 mep "$out/bin/mep"
            runHook postInstall
          '';
        };
      in
      {
        packages.default = mepPackage;
        apps.default = flake-utils.lib.mkApp { drv = mepPackage; };

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.emscripten
            pkgs.deno
            pkgs.just
            pkgs.pkg-config
            # Native (non-wasm) raylib build deps, for `just build-native`.
            pkgs.glfw
            pkgs.libGL
            pkgs.libx11
            pkgs.libxrandr
            pkgs.libxinerama
            pkgs.libxcursor
            pkgs.libxi
            pkgs.openssl
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
            pkgs.python3 # Python
            pkgs.nodejs # JavaScript
            pkgs.ruby
            pkgs.perl
            pkgs.R
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
          '';
        };
      }
    );
}
