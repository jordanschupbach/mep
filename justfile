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
