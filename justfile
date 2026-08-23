web_build_dir := "build/web"
native_build_dir := "build/native"
ts_grammar_dir := ".ts-grammars/lib"

# Build the native (X11) binary and launch it directly (default).
default: run

# Configure and build the wasm target via emscripten.
build-web:
    emcmake cmake -S . -B {{web_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{web_build_dir}} -j

# Configure and build a native desktop binary.
build-native:
    cmake -S . -B {{native_build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{native_build_dir}} -j

# Build every Treesitter grammar mep has a highlight query for but doesn't
# compile in (scripts/ts_grammars.tsv, ~49 languages -- see
# src/treesitter.cpp's own DynamicLanguageTable) into .ts-grammars/lib/,
# so org-babel src blocks (and any other file) get real syntax
# highlighting for all of them without needing Nix. Safe to re-run --
# already-built grammars are skipped; pass `--force` to rebuild everyone.
# `run` below picks these up automatically once this has been run once.
fetch-grammars *ARGS:
    ./scripts/fetch-grammars.sh {{ARGS}}

# Build and launch the native (X11) binary directly -- no wasm/Emscripten,
# WebKitGTK, or deno anywhere in the loop, for isolating whether a given
# issue is specific to the wasm+webview path (`just run-wasm`) or present
# natively too. Prepends .ts-grammars/lib (see `fetch-grammars` above) to
# $MEP_TS_PARSER_PATH when it exists, composing with -- not replacing --
# whatever a Nix devShell may have already exported there, so `just
# fetch-grammars` and the flake.nix/devShell path both just add more
# languages rather than competing over the same variable.
run: build-native
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -d "{{ts_grammar_dir}}" ]; then
        export MEP_TS_PARSER_PATH="$(realpath {{ts_grammar_dir}})${MEP_TS_PARSER_PATH:+:$MEP_TS_PARSER_PATH}"
    fi
    # mep.browse() (kBuiltinTextTools, src/main.cpp) shells out to this --
    # an absolute path since mep.chdir()/:cd (a real chdir(2)) can move the
    # process's own cwd away from the repo root at any point in the session,
    # same reasoning as MEP_TS_PARSER_PATH above needing a resolved path.
    export MEP_BROWSER_LAUNCHER="$(realpath launcher/browser.ts)"
    exec ./{{native_build_dir}}/mep

# Build the wasm target and open it in a native window via deno + webview.
run-wasm: build-web
    LD_LIBRARY_PATH="${MEP_WEBVIEW_LD_LIBRARY_PATH:-}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" deno task launch

# Remove all build output.
clean:
    rm -rf build
