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

          # Scoped to a separate variable (rather than exported globally as
          # LD_LIBRARY_PATH) so it doesn't shadow libraries for cmake/gcc/
          # emcc; the `run` recipe in the justfile applies it only to the
          # deno launcher process.
          shellHook = ''
            export MEP_WEBVIEW_LD_LIBRARY_PATH="${webviewLibraryPath}"
          '';
        };
      }
    );
}
