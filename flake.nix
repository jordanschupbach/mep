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
          ]
        );
      in
      {
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
            # Runtime dep of webview_deno, used by the launcher to open a
            # native window around the wasm build.
            pkgs.webkitgtk_6_0
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
