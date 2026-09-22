web_build_dir := "build/web"
native_build_dir := "build/native"
native_build_dev_dir := "build/native-dev"
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

# Same as build-native, but -O0 (CMake's stock Debug flags) in a
# separate build/native-dev directory that never collides with
# build-native's own Release one -- both stay independently
# incrementally-buildable side by side (BUILD_PERFORMANCE_PLAN.md).
# Real payoff on the actual inner loop -O3 can't share with ccache: a
# genuine same-file edit (not just a touch/no-op) still has to actually
# recompile that one translation unit, and -O3's optimizer alone was
# measured at over half of main.cpp's ~17s compile (-O0: ~7s, -O3:
# ~17s) -- most of that difference is pure optimizer time this build
# type skips, not something ccache or the build-graph generator can get
# back. Use `./build/native-dev/mep` for fast local edit/test iteration
# -- NOT for anything performance-sensitive or for shipping; `mep`
# itself (build-native, unaffected by this recipe) stays the one built
# and tested at full -O3 throughout this repo (`just test`/`just run`).
build-native-dev:
    cmake -S . -B {{native_build_dev_dir}} -DCMAKE_BUILD_TYPE=Debug
    cmake --build {{native_build_dev_dir}} -j

# Opt-in unity/jumbo build of mep_core (BUILD_PERFORMANCE_PLAN.md Round 2
# Phase C): CMake's native UNITY_BUILD, batching mep_core's ~49
# translation units into groups of 8 so each batch's shared headers are
# parsed once per batch instead of once per file -- a real clean-build
# win, but it comes straight out of incremental-rebuild speed (touching
# any file now recompiles its whole batch). Its own build dir, never
# build-native/build-native-dev -- neither of *those* pay this cost, and
# `just test`/day-to-day iteration keep using them unchanged.
build-native-unity:
    cmake -S . -B build/native-unity -DCMAKE_BUILD_TYPE=Release -DMEP_UNITY_BUILD=ON
    cmake --build build/native-unity -j

# Build every test/smoke/benchmark binary (BUILD_PERFORMANCE_PLAN.md
# Round 3's `tests` CMake target -- every EXCLUDE_FROM_ALL binary in
# CMakeLists.txt, i.e. everything build-native's own default `all` build
# no longer builds for you). Convenience for "I want all of them on disk
# right now" (e.g. before going offline, or poking at one by hand) --
# `test`/`bench`/`test-gui` above each already build only the specific
# subset they actually run, so none of them need this first.
build-native-tests: build-native
    cmake --build {{native_build_dir}} -j --target tests

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

# Re-render the built-in help workspace: every help/*.org through mep's own
# Org exporter (`mep --export-org`, src/main.cpp's RunHeadlessOrgExport) to
# the help/*.html the Help sidebar actually lists and ships. Needs no
# display -- the export path branches off before InitWindow -- so this is
# equally usable from CI and over ssh. The .html files are checked in
# (CMakeLists.txt installs help/ wholesale), so run this and commit the
# result whenever a help source changes; `mep-help-test` fails when they
# have drifted apart.
help: build-native
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob
    # The Lua API reference is generated from src/lua_env.cpp's own binding
    # table -- ~445 entries is not something to hand-maintain -- so refresh
    # those sources before exporting anything.
    python3 scripts/gen_reference_pages.py
    for src in help/*.org; do
        # _-prefixed sources are not pages: help/_template.org is the
        # starting point a new page is copied from, and would otherwise
        # export to a _template.html the Help sidebar would then list.
        case "$(basename "$src")" in _*) continue ;; esac
        out="${src%.org}.html"
        ./{{native_build_dir}}/mep --export-org "$src" "$out"
        echo "  $src -> $out"
    done

# Check the built-in help workspace: that every help/*.html is exactly what
# its .org source exports today (re-exported and compared, not an mtime
# check -- git does not preserve mtimes), that every page declares the title
# and section the sidebar places it by, that internal links resolve, and how
# much of mep's command/<leader> surface the manual actually mentions.
# Coverage is reported but only enforced with --strict, since the manual is
# being written incrementally (plans/HELP_DOCS_PLAN.md).
help-check *ARGS: build-native
    python3 scripts/check_help.py {{native_build_dir}}/mep {{ARGS}}

# Remove all build output.
clean:
    rm -rf build

# Build and run every *-test binary that needs no display: the pure
# DOM/CSS, workspace-helper and collab tests. Exits non-zero on the first
# failure (each binary aborts on a failed CHECK()).
#
# BUILD_PERFORMANCE_PLAN.md Round 3: every test/smoke/benchmark binary is
# EXCLUDE_FROM_ALL in CMakeLists.txt (so a plain `build-native`/`nix
# build` -- production -- only builds `mep` + its two companion tools,
# not a single test binary), so this recipe has to build the specific
# targets it needs itself rather than getting them for free from
# `build-native`'s own default-`all` build.
test: build-native
    #!/usr/bin/env bash
    set -euo pipefail
    # Runs before the C++ binaries below: `mep-collab-session-test` in that
    # list requires a ws:// URL and exits 2 without one, which aborts the
    # recipe under `set -e` -- a pre-existing failure, but one that would
    # otherwise mean this check never ran at all.
    echo "== check_help"
    python3 scripts/check_help.py {{native_build_dir}}/mep --strict
    targets=(mep-html-doc-test mep-web-ladder-test mep-org-doc-test mep-org-lsp-test mep-python-lsp-test mep-r-lsp-test mep-vterm-test mep-spell-test mep-python-format-test mep-r-format-test mep-cpp-format-test mep-notebook-doc-test mep-workspace-test mep-model3d-doc-test mep-image-procgen-test mep-jpeg-codec-test mep-pdf-object-test mep-pdf-xref-test mep-pdf-crypt-test mep-pdf-filters-test mep-pdf-document-test mep-pdf-outline-test mep-pdf-links-test mep-pdf-annots-test mep-pdf-writer-test mep-rasterizer-test mep-pdf-content-test mep-cff-test mep-type1-test mep-pdf-encodings-test mep-pdf-font-test mep-pdf-text-test mep-mov-container-test mep-collab-crdt-test mep-collab-session-test)
    cmake --build {{native_build_dir}} -j --target "${targets[@]}"
    for t in "${targets[@]}"; do
        if [ -x "{{native_build_dir}}/$t" ]; then
            echo "== $t"
            "./{{native_build_dir}}/$t"
        fi
    done

# CRDT_PERFORMANCE_PLAN.md Phase 1: run the persistent text-editing
# benchmarks (mep-crdt-bench, mep-buffer-bench, mep-lua-frame-hook-bench)
# and print a latest-vs-previous-run comparison. Results accumulate in
# bench_results/history.jsonl (git-tracked) across every invocation --
# this never resets/rotates it, so history builds up over time as the
# user asked. Run from the repo root (bench_results/ is a relative
# path both the bench binaries and the report script resolve against
# the cwd). Builds its own EXCLUDE_FROM_ALL targets explicitly, same
# reasoning as `test` above.
bench: build-native
    #!/usr/bin/env bash
    set -euo pipefail
    cmake --build {{native_build_dir}} -j --target mep-crdt-bench mep-buffer-bench mep-lua-frame-hook-bench
    mkdir -p bench_results
    for t in mep-crdt-bench mep-buffer-bench mep-lua-frame-hook-bench; do
        echo "== $t"
        "./{{native_build_dir}}/$t"
    done
    echo
    python3 tools/bench_report.py

# The tests that drive a real `mep` window: the agent-RPC test (spawns
# mep, needs a display and a working GL driver -- under Xvfb that means a
# GPU-capable/llvmpipe driver; the sandbox WORKSPACES_PLAN.md was written
# in segfaults in raylib's InitWindow) and the MCP server end-to-end test
# (deno, same display requirement since it spawns mep too). Builds its own
# EXCLUDE_FROM_ALL target explicitly, same reasoning as `test` above --
# `mep` itself doesn't need this since it's still part of the default
# `all` build `build-native` already does.
test-gui: build-native
    #!/usr/bin/env bash
    set -euo pipefail
    cmake --build {{native_build_dir}} -j --target mep-agent-rpc-test
    echo "== mep-agent-rpc-test"
    "./{{native_build_dir}}/mep-agent-rpc-test" "./{{native_build_dir}}/mep"
    echo "== mcp/server_test.ts"
    MEP_BINARY="$(realpath {{native_build_dir}}/mep)" deno test --allow-all mcp/server_test.ts

# Static analysis of mep's own C++ (src/*.cpp, src/*.h) -- deliberately
# excludes third_party/ and the build/native/_deps/*-src/ vendored trees
# for all three tools below, same "own code only" scoping .clang-tidy's
# HeaderFilterRegex already uses (we don't own that code and can't act on
# findings inside it). All three read build/native/compile_commands.json,
# so `just build-native` must have run at least once first.

# clang-tidy over every own .cpp/.h, using the project's own curated
# .clang-tidy config (checks list + ExtraArgsBefore, which works around
# clang-tidy's clang driver choking on GCC-only flags baked into
# compile_commands.json by a GCC-compiled build -- see .clang-tidy's own
# comment). clang-analyzer-* is enabled in that checks list, so this also
# *is* mep's clang static analyzer pass -- clang-tidy runs the same
# analyzer engine scan-build would, per-translation-unit, without a
# separate from-scratch build under scan-build's fake-compiler wrapper.
# The file-filter regex run-clang-tidy takes is matched as a substring
# against each compile command's *absolute* path, so a naive 'src/.*\.
# (cpp|h)$' also matches vendored FetchContent sources with their own
# "src" subdirectory on the path (e.g. build/native/_deps/pugixml-src/
# src/pugixml.cpp) -- anchored on the repo's own top-level src/ via
# .* /mep/src/ (no further slashes allowed after it) to rule those out,
# mirroring .clang-tidy's HeaderFilterRegex anchoring for the same reason.
lint-tidy:
    run-clang-tidy -p {{native_build_dir}} -quiet '.*/mep/src/[^/]+\.(cpp|h)$'

# cppcheck in compile_commands.json ("--project") mode, so it sees the
# same include paths/defines the real build uses instead of guessing.
# --enable=warning,style,performance,portability (not "all" -- that also
# turns on "unusedFunction", which is wrong for a project where most
# functions are reached via raylib/Lua callbacks or mep's own dispatch
# tables rather than being called directly, and would be pure noise).
#
# Filters compile_commands.json down to mep's own src/ first
# (scripts/filter_compile_commands.py) so only our own .cpp files are
# used as translation units -- but cppcheck (unlike clang-tidy's
# HeaderFilterRegex) has no concept of "only report diagnostics located
# in these paths": it happily walks into and reports on any header a
# translation unit #includes, own or vendored, tagging each finding with
# that header's own path. Piping through the grep -v below (matched
# against --template's leading {file} field) is the actual "own code
# only" filter; cppcheck's own -i third_party / -i _deps flags looked
# like the right tool for this but are silently a no-op once --project
# is given (verified against cppcheck 2.18.3 -- confirmed by-hand: it
# still analyzed and reported on build/native/_deps/lua-src/**.c with
# -i "$(pwd)/build/native/_deps" passed), so this works around that gap
# rather than depending on a flag that doesn't do what it says here.
#
# --suppress=useStlAlgorithm: manually reviewed a sample of these across
# main.cpp/editor.cpp -- 100% linear scans over small per-frame UI-state
# vectors (panes, tabs, sidebars) where an explicit loop with an early
# continue/break is at least as readable as a find_if/any_of + lambda,
# not a correctness or measurable-perf issue either way. Left enabled
# it's ~150 pure-style findings urging exactly the kind of abstraction-
# for-its-own-sake this codebase avoids elsewhere.
lint-cppcheck:
    #!/usr/bin/env bash
    set -euo pipefail
    filtered="$(mktemp --suffix=.json)"
    trap 'rm -f "$filtered"' EXIT
    python3 scripts/filter_compile_commands.py {{native_build_dir}}/compile_commands.json "$filtered"
    cppcheck --project="$filtered" \
        --enable=warning,style,performance,portability \
        --suppress=useStlAlgorithm \
        --inline-suppr --std=c++17 --language=c++ \
        --template='{file}:{line}:{column}: {severity}: {message} [{id}]' \
        --error-exitcode=0 2>&1 \
        | grep -Ev '/(third_party|build/[^/]+/_deps)/' || true

# include-what-you-use over every own .cpp, via the iwyu_tool.py wrapper
# that ships with IWYU (drives it from compile_commands.json instead of
# needing per-file invocation). IWYU brings its own pinned clang, so its
# analysis is independent of clang-tools' clang-tidy/clangd version --
# which also means, unlike clang-tidy (a libTooling consumer that reuses
# whatever compiler compile_commands.json names, gcc included, via its
# own auto toolchain detection), IWYU's clang has no idea where this
# devShell's libstdc++/glibc/etc headers live: it isn't a "real" GCC
# install at a conventional path, it's a Nix store path only this
# devShell's own $NIX_CFLAGS_COMPILE-equivalent knows about. Reusing that
# exact list (queried from the real `gcc` on PATH, the same compiler
# build/native/compile_commands.json itself was generated with) via
# -isystem, rather than guessing individual store paths, is what makes
# this portable across machines/nix store GC.
# -Wno-unknown-warning-option: same GCC-only-flags-on-a-clang-driver
# mismatch as .clang-tidy's ExtraArgsBefore -- see its comment.
lint-iwyu:
    #!/usr/bin/env bash
    set -euo pipefail
    isystem_flags=()
    while IFS= read -r dir; do
        isystem_flags+=(-isystem "$dir")
    done < <(gcc -E -v -xc++ /dev/null 2>&1 \
        | sed -n '/#include <...> search starts here/,/End of search list/p' \
        | sed '1d;$d' | sed 's/^ //')
    iwyu_tool.py -p {{native_build_dir}} $(find src -maxdepth 1 -name '*.cpp') -- \
        -Wno-unknown-warning-option "${isystem_flags[@]}"

# Run every static analysis tool above.
lint: lint-tidy lint-cppcheck lint-iwyu
