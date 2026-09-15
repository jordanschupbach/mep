cmake_minimum_required(VERSION 3.28)
project(mep LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# BUILD_PERFORMANCE_PLAN.md Round 2 Phase D: Clang is this project's
# default compiler (see flake.nix's `mepStdenv`, switched from GCC
# specifically to get real, working C++20 modules -- GCC 15's own module
# support was verified broken here). Clang on Linux still depends on a
# GCC installation for libstdc++'s headers/runtime (no libc++ toolchain
# is set up for this project), and ordinary compilation auto-discovers
# that fine -- but `clang-scan-deps` (the tool CMake's own C++ module
# dependency scanning shells out to) does NOT inherit the same implicit
# search paths, in a plain `nix develop` shell *or* one built on
# `clangStdenv` (both verified live: a trivial one-module toy project
# failed dependency scanning with "'string' file not found" either way).
# Reuses the exact same discovery trick justfile's lint-iwyu recipe
# already established for the same "Clang needs GCC's own headers
# spelled out" problem (`gcc -E -v -xc++ /dev/null`, parsed for the
# `#include <...> search starts here` block) -- applied globally via
# add_compile_options rather than per-recipe since every target's C++
# module scanning needs it, not just one lint tool's own invocation.
# A no-op when the compiler isn't Clang (GCC needs none of this).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    execute_process(
        COMMAND gcc -E -v -xc++ /dev/null
        ERROR_VARIABLE MEP_GCC_VERBOSE_OUTPUT
        OUTPUT_QUIET
    )
    if(MEP_GCC_VERBOSE_OUTPUT MATCHES "#include <\\.\\.\\.> search starts here:\n(.*)\nEnd of search list")
        string(STRIP "${CMAKE_MATCH_1}" MEP_GCC_ISYSTEM_BLOCK)
        string(REPLACE "\n" ";" MEP_GCC_ISYSTEM_DIRS "${MEP_GCC_ISYSTEM_BLOCK}")
        foreach(MEP_GCC_ISYSTEM_DIR ${MEP_GCC_ISYSTEM_DIRS})
            string(STRIP "${MEP_GCC_ISYSTEM_DIR}" MEP_GCC_ISYSTEM_DIR)
            if(MEP_GCC_ISYSTEM_DIR)
                add_compile_options("-isystem${MEP_GCC_ISYSTEM_DIR}")
            endif()
        endforeach()
    endif()
endif()

# ccache (BUILD_PERFORMANCE_PLAN.md): auto-detected, not opt-in -- every
# invocation path (the justfile, `nix build`'s mepPackage, a manual
# `cmake -S -B` a dev runs by hand) picks this up automatically with no
# per-caller wiring, and it's a silent no-op if ccache isn't installed
# (`find_program` just returns NOTFOUND). Real payoff here isn't just
# "rebuild a file identically" (unrelated single-file touches rarely
# regenerate a byte-identical preprocessed TU) -- this codebase's own
# CMakeLists.txt already compiles several .cpp files (model3d_doc.cpp,
# wav_doc.cpp, deflate.cpp, the *_codec.cpp files, ...) into *multiple*
# separate targets (mep_core, several of the mep-*-test binaries, the
# gfx-native smoke tests) with identical flags -- ccache turns every one
# of those repeat compiles, across the *same* build directory and across
# a from-scratch `rm -rf build && rebuild`, into a cache hit.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Faster linker (BUILD_PERFORMANCE_PLAN.md Round 2): both mold and lld were
# tried; `auto` prefers **lld**, not mold, based on real measurements in
# this project/environment, not the other way around as originally assumed
# -- relinking `mep` (Debug, i.e. with debug info, the more linker-bound
# case) after touching main.cpp: bfd ~1.1s, mold ~0.59-0.60s, lld ~0.17-
# 0.19s, reproduced across 3 back-to-back runs each. mold is usually the
# fastest option for large parallel link jobs, but its fixed per-invocation
# overhead doesn't pay for itself at this project's link-job size in this
# particular sandboxed environment, where it ended up ~3.3x *slower* than
# lld and barely faster than plain bfd. mold stays fully wired up and
# selectable (-DMEP_LINKER=mold) since it's what was asked for first and
# may well win on a different machine/link job size -- just not the auto
# default here. Auto-detected and silently skipped if neither is installed
# (same pattern as ccache above -- every invocation path benefits with no
# per-caller wiring), and the check_cxx_compiler_flag calls below do a real
# trivial link, not just a flag-spelling check, so a mold/lld binary that's
# on PATH but not actually wired up to this compiler (e.g. too old to
# recognize -fuse-ld=lld) falls back to the default linker instead of
# breaking every link in the project. Linux-only: mepPackage/the devShell
# only provision either linker there.
set(MEP_LINKER "auto" CACHE STRING "Linker to use for mep's own build: auto, lld, mold, or default")
set_property(CACHE MEP_LINKER PROPERTY STRINGS auto lld mold default)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MEP_LINKER STREQUAL "default")
    include(CheckCXXCompilerFlag)
    find_program(MOLD_PROGRAM mold)
    find_program(LLD_PROGRAM ld.lld)
    set(MEP_LINKER_FLAG "")
    if(LLD_PROGRAM AND MEP_LINKER MATCHES "^(auto|lld)$")
        check_cxx_compiler_flag("-fuse-ld=lld" MEP_HAVE_LLD_LINK)
        if(MEP_HAVE_LLD_LINK)
            set(MEP_LINKER_FLAG "-fuse-ld=lld")
        endif()
    endif()
    if(NOT MEP_LINKER_FLAG AND MOLD_PROGRAM AND MEP_LINKER MATCHES "^(auto|mold)$")
        check_cxx_compiler_flag("-fuse-ld=mold" MEP_HAVE_MOLD_LINK)
        if(MEP_HAVE_MOLD_LINK)
            set(MEP_LINKER_FLAG "-fuse-ld=mold")
        endif()
    endif()
    if(MEP_LINKER_FLAG)
        add_link_options(${MEP_LINKER_FLAG})
        # -fuse-ld bypasses Nix's own ld wrapper script -- the one that
        # normally post-processes every link, adding a -rpath for each -L
        # directory a library actually got resolved from (glibc, libgcc,
        # libstdc++, X11, GL, ALSA, OpenSSL, ...) -- so under a Nix
        # devShell/mepPackage build, a binary linked via -fuse-ld=lld/mold
        # builds fine but then fails at *runtime* with e.g.
        # "libstdc++.so.6: cannot open shared object file" or
        # "libX11.so.6: cannot open shared object file" for whichever
        # libraries that particular target happens to need (found live via
        # real `just test` failures -- first mep-html-doc-test needing
        # libstdc++, then mep-model3d-doc-test needing libX11, i.e. this is
        # a whole class of breakage, not a single missing library).
        # $NIX_LDFLAGS already carries the exact list of every devShell
        # package's lib/ directory as -L flags (that's how the normal
        # wrapper would have known what to rpath in the first place) -- add
        # all of them back as explicit rpaths rather than hand-picking
        # individual libraries one failure at a time. A no-op outside Nix
        # (the env var is unset there, so this block never runs) and
        # harmless even when over-broad (an unused rpath entry costs
        # nothing at runtime).
        set(MEP_RPATH_ARGS "")
        if(DEFINED ENV{NIX_LDFLAGS})
            separate_arguments(MEP_NIX_LDFLAGS_LIST NATIVE_COMMAND "$ENV{NIX_LDFLAGS}")
            foreach(MEP_LDFLAG ${MEP_NIX_LDFLAGS_LIST})
                if(MEP_LDFLAG MATCHES "^-L(.+)$")
                    list(APPEND MEP_RPATH_ARGS "-Wl,-rpath,${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
        # $NIX_LDFLAGS's -L list above is the devShell's own buildInputs
        # (X11, GL, ALSA, OpenSSL, ...) -- it does NOT include the
        # compiler's own runtime (libstdc++/libgcc_s/libc), which live in a
        # separate store path the cc-wrapper injects on every invocation
        # regardless of buildInputs. Ask the compiler itself where each one
        # actually resolves, rather than guessing the store path -- always
        # via `gcc`, not `${CMAKE_CXX_COMPILER}`: with Clang as the actual
        # compiler (BUILD_PERFORMANCE_PLAN.md Round 2 Phase D), `clang++
        # -print-file-name=libstdc++.so` doesn't resolve to a real path at
        # all (Clang still finds libstdc++'s *headers* fine via the
        # -isystem block above, and even links successfully via its own
        # separate default library search -- just doesn't know its on-disk
        # location for -print-file-name), so asking Clang specifically for
        # this leaves the exact same rpath gap this block exists to close.
        # `gcc` is always the one that actually owns these files here,
        # Clang or not.
        find_program(MEP_GCC_FOR_RPATH gcc)
        foreach(MEP_RUNTIME_LIB libstdc++.so libgcc_s.so.1 libc.so.6)
            execute_process(
                COMMAND ${MEP_GCC_FOR_RPATH} -print-file-name=${MEP_RUNTIME_LIB}
                OUTPUT_VARIABLE MEP_RUNTIME_LIB_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(MEP_RUNTIME_LIB_PATH AND NOT MEP_RUNTIME_LIB_PATH STREQUAL MEP_RUNTIME_LIB)
                get_filename_component(MEP_RUNTIME_LIB_DIR "${MEP_RUNTIME_LIB_PATH}" DIRECTORY)
                list(APPEND MEP_RPATH_ARGS "-Wl,-rpath,${MEP_RUNTIME_LIB_DIR}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES MEP_RPATH_ARGS)
        add_link_options(${MEP_RPATH_ARGS})
        message(STATUS "mep: linking with ${MEP_LINKER_FLAG}")
    elseif(NOT MEP_LINKER STREQUAL "auto")
        message(WARNING "mep: MEP_LINKER=${MEP_LINKER} requested but not usable with this compiler/toolchain; falling back to the default linker")
    endif()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# Strict warnings for mep's OWN targets only (mep_core, mep, mep-collabd,
# the collab/agent-rpc test binaries, mep-amalgam) -- deliberately NOT
# applied to vendored/fetched third-party targets (lua,
# tree_sitter, the ts_* grammar libs) since we don't own
# that code and can't fix warnings inside it. Applied via
# target_compile_options(... PRIVATE ${MEP_STRICT_FLAGS}) on each of our
# own targets below, not injected globally through CMAKE_CXX_FLAGS.
set(MEP_STRICT_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
)
# -Wnull-dereference deliberately dropped (BUILD_PERFORMANCE_PLAN.md Round 2
# Phase D, the C++17->C++20 bump for modules): GCC 15 produces real,
# reproducible false positives from it under -std=gnu++20 -O3 specifically
# -- confirmed twice, in unrelated code (a hand-rolled LZW decoder in
# gif_codec.cpp, and libstdc++'s own vector<string>::assign in
# office_doc.cpp), both flagging a plain container destructor several
# levels deep through inlining, with no pointer/null anywhere near the
# actual logic. Root cause looks like C++20 making much of <string>/
# <vector> constexpr, which changes the shape of what gets inlined enough
# to trip up -Wnull-dereference's analysis; identical code was clean under
# C++17. Given it already surfaced in two unrelated files on the very
# first C++20 build, pragma-suppressing it file-by-file as more turn up
# isn't a good tradeoff against the actual point of this phase (modules) --
# dropped project-wide instead of chased one file at a time.

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND MEP_STRICT_FLAGS
        -Wuseless-cast
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
    )
endif()
option(MEP_WARNINGS_AS_ERRORS "Treat mep's own strict compiler warnings as errors" ON)
if(MEP_WARNINGS_AS_ERRORS)
    list(APPEND MEP_STRICT_FLAGS -Werror)
endif()

# Optional AddressSanitizer + UndefinedBehaviorSanitizer build: configure
# with -DCMAKE_BUILD_TYPE=Sanitize. Same strict-warnings scoping applies
# (own targets only); sanitizer instrumentation is also own-targets-only
# since re-instrumenting fetched third-party static libs would require
# rebuilding them from source under the sanitizer flags too, which
# FetchContent's prebuilt/cached artifacts here don't do.
set(CMAKE_CXX_FLAGS_SANITIZE "-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_SANITIZE "-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_SANITIZE "-fsanitize=address,undefined" CACHE STRING "" FORCE)
set(MEP_SANITIZE_FLAGS "")
if(CMAKE_BUILD_TYPE STREQUAL "Sanitize")
    set(MEP_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
endif()

# Lua has no official CMake build, so it's fetched as a plain tarball and
# built as a static library by hand: every src/*.c except the two files
# that provide the standalone `lua`/`luac` executables' own main().
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)

file(GLOB LUA_SOURCES "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c")
add_library(lua STATIC ${LUA_SOURCES})
# SYSTEM (not just PUBLIC): makes every consumer's compiler see this as
# -isystem rather than -I, so mep's own MEP_STRICT_FLAGS/-Werror don't fire
# on Lua's own vendored headers pulled into our .cpp files (e.g.
# lauxlib.h's old-style casts) -- a real, if minor, pre-existing gap found
# under Clang (BUILD_PERFORMANCE_PLAN.md Round 2 Phase D): GCC happened not
# to warn on the exact same code, Clang's -Wold-style-cast did.
target_include_directories(lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")

# Tree-sitter runtime + a small core set of grammars: vendored locally
# under third_party/ (source files only -- parser.c, an optional
# external scanner, and the grammar's own copy of tree_sitter/parser.h;
# see third_party/README.md) rather than fetched via CMake FetchContent
# at configure time -- a from-scratch `build/` directory doing a
# sequential git clone per grammar didn't scale even at a moderate
# count, and once, catastrophically, didn't scale at all: tree-sitter-
# ocaml's examples/ directory vendors several huge, entirely unrelated
# upstream projects (js_of_ocaml, dune, ...) as git submodules, and a
# plain recursive clone pulled gigabytes of them in just to reach the
# handful of files under src/ that actually matter. Vendoring only those
# files makes every build fully offline and instant to configure.
#
# Deliberately a *small, curated* set, not "as many as possible": C,
# C++, Lua (mep's own scripting/config language), Python, JavaScript,
# Markdown (+ its separate inline-formatting grammar), Org, and R.
# Every other language is loaded dynamically at runtime instead (see
# src/treesitter.cpp's LoadDynamicLanguage/TS_DYNAMIC_LANGUAGES) from a
# compiled `.so` the user already has -- via a Nix devShell (flake.nix
# demonstrates this: `pkgs.tree-sitter.withPlugins`, exported as
# MEP_TS_PARSER_PATH), the tree-sitter CLI's own build cache, an
# nvim-treesitter install, or mep's own ~/.config/mep/parsers/ -- rather
# than compiled into mep itself. That dynamic path is native-only
# (dlopen has no equivalent for the Emscripten/wasm build, which only
# ever has these 8 grammars available).
set(TS_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/third_party/tree-sitter")
add_library(tree_sitter STATIC "${TS_RUNTIME_DIR}/src/lib.c")
target_include_directories(tree_sitter
    PUBLIC "${TS_RUNTIME_DIR}/include"
    PRIVATE "${TS_RUNTIME_DIR}/src"
)

# Builds one grammar library from third_party/grammars/<name>/src.
function(ts_build name)
    set(base "${CMAKE_SOURCE_DIR}/third_party/grammars/${name}/src")
    set(grammar_src "${base}/parser.c")
    if(EXISTS "${base}/scanner.c")
        list(APPEND grammar_src "${base}/scanner.c")
    elseif(EXISTS "${base}/scanner.cc")
        list(APPEND grammar_src "${base}/scanner.cc")
    endif()
    add_library(ts_${name} STATIC ${grammar_src})
    target_include_directories(ts_${name} PRIVATE "${base}")
endfunction()

set(TS_GRAMMAR_NAMES c cpp lua python javascript markdown markdown_inline org r)
set(TS_GRAMMAR_LIBS "")
foreach(gname ${TS_GRAMMAR_NAMES})
    ts_build(${gname})
    list(APPEND TS_GRAMMAR_LIBS ts_${gname})
endforeach()

# The in-house OpenGL gfx:: backend: mep_core's rendering/windowing/input/
# audio/model-import layer, and (below) four standalone smoke-test
# executables that exercise it independently of the full app.
#
# Windowing/input/GL-context creation is platform-specific and selected
# here: native desktop links gfx/backend_native.cpp, a hand-written X11/
# GLX implementation (mep owns this dependency directly -- no GLFW at
# all, see GLFW_REMOVAL_PLAN.md); Emscripten links
# gfx/backend_native_emscripten_glfw.cpp instead, which still goes
# through a GLFW-shaped API because `-sUSE_GLFW=3 -sUSE_WEBGL2=1
# -sFULL_ES3=1` are emcc's own link-time shims over the browser's canvas/
# input/WebGL2 APIs -- GLFW_REMOVAL_PLAN.md's own Phase 5 explicitly
# keeps that shim out of scope (there's no in-house version of a
# browser's own event model worth writing). Neither needs
# find_package/target_link_libraries for its windowing pieces specifically
# (X11/OpenGL are found below for the native case; Emscripten's shim needs
# neither). Declared before mep_core below since mep_core links these
# sources in directly.
set(MEP_GFX_NATIVE_SOURCES
    src/gfx/gl_loader.cpp
    src/gfx/gl_shader.cpp
    src/gfx/backend_native_audio.cpp
    src/gfx/backend_native_renderer2d.cpp
    src/gfx/backend_native_renderer3d.cpp
    src/gfx/backend_native_model_obj.cpp
    src/gfx/backend_native_model_iqm.cpp
    src/gfx/backend_native_model_vox.cpp
    src/gfx/backend_native_model_m3d.cpp
    src/gfx/backend_native_model_gltf.cpp
    src/gfx/backend_native_text.cpp
    src/gfx/truetype.cpp
    src/gfx/rasterizer.cpp
    src/gfx/backend.cpp
)
if(EMSCRIPTEN)
    list(APPEND MEP_GFX_NATIVE_SOURCES src/gfx/backend_native_emscripten_glfw.cpp)
else()
    list(APPEND MEP_GFX_NATIVE_SOURCES src/gfx/backend_native.cpp)
    find_package(OpenGL REQUIRED)
    # Xi: not called directly by any mep code (agent_ui_input.h's own
    # comment explains why XInput2 was deliberately not used for
    # synthetic scroll), but CMake's FindX11 module models Xtst's
    # IMPORTED target as linking Xi (libXtst.so really is linked against
    # libXi.so.6 on this distro) -- a real transitive dependency, not
    # just CMake bookkeeping, so it must be requested too or
    # X11::Xtst's own target fails to configure.
    find_package(X11 REQUIRED COMPONENTS Xtst Xi)
endif()

# Every logical module (editor, lua_env, job, agent_rpc, vterm, treesitter,
# regex, image_doc, pdf_doc, office_doc/office_odt, formula, sheet_doc/
# sheet_xlsx/sheet_ods, html_doc, js_engine, doc_export, org_doc) is its own
# translation unit, built here as a static library so touching one module
# only recompiles that module + relinks, instead of the whole application.
# main.cpp itself stays a separate translation unit (the driver) linked
# against this library. See tools/amalgamate.py / the `mep-amalgam` target
# below for an optional single-translation-unit build of the same sources.
add_library(mep_core STATIC
    src/vterm.cpp
    src/regex.cpp
    src/treesitter.cpp
    src/image_doc.cpp
    src/image_procgen.cpp
    src/model3d_doc.cpp
    src/model3d_blend_import.cpp
    src/wav_doc.cpp
    src/svg_doc.cpp
    src/pdf_doc.cpp
    # PDFIUM_REMOVAL_PLAN.md Phase 13: pdf_doc.cpp's own backend, swapped
    # from PDFium to this in-house parser/interpreter/rasterizer stack
    # (Phases 2-12). gfx/truetype.cpp and gfx/rasterizer.cpp are NOT
    # listed again here -- they're already part of MEP_GFX_NATIVE_SOURCES
    # below (mep's own native UI text rendering uses the same TrueType
    # engine Phase 9 extracted these from); gfx/cff.cpp and jpeg_codec.cpp
    # are the only ones of this stack shared with nothing else already in
    # this list (deflate.cpp already is, for zip_archive.cpp/png_codec.cpp).
    src/pdf_object.cpp
    src/pdf_xref.cpp
    src/pdf_crypt.cpp
    src/pdf_filters.cpp
    src/pdf_document.cpp
    src/pdf_content.cpp
    src/pdf_encodings.cpp
    src/pdf_font.cpp
    src/pdf_text.cpp
    src/pdf_outline.cpp
    src/pdf_links.cpp
    src/gfx/cff.cpp
    src/formula.cpp
    src/html_doc.cpp
    src/job.cpp
    src/org_doc.cpp
    src/text_diff.cpp
    src/agent_rpc.cpp
    src/lua_env.cpp
    src/xml_doc.cpp
    src/deflate.cpp
    src/zip_archive.cpp
    src/png_codec.cpp
    src/jpeg_codec.cpp
    src/bmp_codec.cpp
    src/gif_codec.cpp
    src/image_codec.cpp
    src/mov_container.cpp
    src/office_doc.cpp
    src/office_odt.cpp
    src/doc_export.cpp
    src/sheet_doc.cpp
    src/sheet_xlsx.cpp
    src/sheet_ods.cpp
    src/js_engine.cpp
    src/editor.cpp
    src/workspace_git.cpp
    ${MEP_GFX_NATIVE_SOURCES}
)
# C++20 module (BUILD_PERFORMANCE_PLAN.md Round 2 Phase D): the interface
# unit (text_diff.cppm, `export module mep.diff;`) goes in its own FILE_SET
# so CMake knows up front what module it provides and can propagate that to
# anything linking mep_core (PUBLIC -- `mep`'s own target_sources(mep
# PRIVATE ... src/collab_session.cpp) importing mep.diff needs this, since
# collab_session.cpp isn't compiled as part of mep_core itself). The
# implementation unit (text_diff.cpp, `module mep.diff;`, no `export`)
# stays a plain source above -- only interface units need their own
# FILE_SET entry; Ninja's own module dependency scanning discovers
# implementation units' module membership from their content.
target_sources(mep_core PUBLIC FILE_SET CXX_MODULES FILES src/text_diff.cppm src/path_util.cppm src/gfx/model_read_util.cppm)
target_include_directories(mep_core PUBLIC src)
target_compile_options(mep_core PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep_core PRIVATE ${MEP_SANITIZE_FLAGS})

# Precompiled headers (BUILD_PERFORMANCE_PLAN.md Round 2 Phase B): this
# exact list -- <memory>/<vector>/<functional>/<fstream>/<cmath>/<iterator>/
# ... -- came from a one-off ClangBuildAnalyzer profiling run (`-ftime-trace`
# over a Clang build of this same mep_core target; Clang-only, this
# project's actual default compiler stays GCC, see MEP_LINKER/CMAKE_CXX_
# COMPILER above), which found these as the most expensive-to-parse
# standard headers across mep_core's ~49 translation units -- together
# roughly a quarter of total frontend parse time. Deliberately does NOT
# include editor.h, despite it costing even more per-include (747ms avg,
# ClangBuildAnalyzer's own "Expensive headers" report): it's only included
# by 4 of mep_core's ~49 files, so forcing every other TU to carry it would
# likely cost more than it saves, and it's large/macro-heavy enough
# (9k+ lines) that verifying it's actually safe to force-include into files
# that never asked for it wasn't attempted this round -- see this phase's
# own notes for the numbers this traded off against.
set(MEP_PCH_HEADERS
    <string>
    <vector>
    <memory>
    <functional>
    <unordered_map>
    <map>
    <algorithm>
    <fstream>
    <sstream>
    <cmath>
    <iterator>
)
target_precompile_headers(mep_core PRIVATE ${MEP_PCH_HEADERS})

# Unity/jumbo build (BUILD_PERFORMANCE_PLAN.md Round 2 Phase C): CMake's
# own native UNITY_BUILD, batching mep_core's ~49 translation units into
# groups so each batch is compiled (and its headers parsed) once instead
# of once per file -- a real clean-build win, but it comes straight out of
# incremental-rebuild speed (touching any one file now forces recompiling
# its whole batch, not just itself), the same tradeoff Round 1 above
# weighed and initially skipped. Opt-in (`-DMEP_UNITY_BUILD=ON`, its own
# `build-native-unity` build dir via the justfile -- never build-native/
# build-native-dev, which stay the ones `just test`/day-to-day iteration
# use) rather than the default, so nobody's ordinary edit/rebuild loop
# pays this cost. mep_core specifically (not `mep`, whose only two source
# files are already effectively one "batch"): it's the target with by far
# the most translation units to batch.
option(MEP_UNITY_BUILD "Batch mep_core's translation units for a faster clean build at the cost of incremental-rebuild speed" OFF)
set(MEP_UNITY_BATCH_SIZE "8" CACHE STRING "Files per unity batch when MEP_UNITY_BUILD is ON -- smaller preserves more build parallelism, larger shares more header-parse cost")
if(MEP_UNITY_BUILD)
    set_target_properties(mep_core PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE ${MEP_UNITY_BATCH_SIZE})
endif()
target_link_libraries(mep_core PUBLIC lua tree_sitter ${TS_GRAMMAR_LIBS})
if(NOT EMSCRIPTEN)
    # X11::X11: gfx/backend_native.cpp's own X11/GLX window/input/context
    # implementation (GLFW_REMOVAL_PLAN.md) -- GLX itself is part of
    # libGL/OpenGL::GL (Mesa exports both from the same shared object), so
    # no separate GLX find_package/target is needed.
    target_link_libraries(mep_core PUBLIC OpenGL::GL X11::X11)
endif()
if(NOT EMSCRIPTEN AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(mep_core PUBLIC Threads::Threads)
    # dlopen/dlsym for the dynamic-grammar-loading path (treesitter.cpp).
    # Emscripten/wasm has no filesystem access to arbitrary shared
    # libraries, so that whole path is compiled out there instead of
    # linked against a (nonexistent) libdl.
    target_link_libraries(mep_core PUBLIC ${CMAKE_DL_LIBS})
endif()
if(NOT EMSCRIPTEN AND NOT WIN32 AND NOT APPLE)
    # In-house ALSA audio backend (gfx/backend_native_audio.cpp, see
    # MINIAUDIO_REMOVAL_PLAN.md) -- Linux only, matching every other
    # Linux-first gfx:: native-backend subsystem.
    find_package(ALSA REQUIRED)
    target_link_libraries(mep_core PUBLIC ALSA::ALSA)
endif()

# Defines one gfx-native smoke-test executable (${main_src} plus the
# shared MEP_GFX_NATIVE_SOURCES above), wired for whichever of native
# desktop or Emscripten this configure is targeting.
function(mep_add_gfx_native_smoke name main_src)
    # src/wav_doc.cpp: gfx/backend_native_audio.cpp's own WAV decode
    # (see MINIAUDIO_REMOVAL_PLAN.md). src/deflate.cpp: gfx/backend_native_
    # model_m3d.cpp's own zlib-wrapped inflate for M3D's compressed body
    # (see MINIZ_REMOVAL_PLAN.md). src/{png,jpeg,bmp,gif,image}_codec.cpp:
    # gfx/backend_native_renderer2d.cpp's/backend_native_model_obj.cpp's/
    # backend_native_model_gltf.cpp's image decode/encode (see
    # STB_IMAGE_REMOVAL_PLAN.md). None of these are part of
    # MEP_GFX_NATIVE_SOURCES itself since mep_core already lists them all
    # separately (each is also used outside the gfx:: backend -- wav_doc.cpp
    # by html_doc.cpp's media playback, deflate.cpp by office_doc.cpp/
    # doc_export.cpp's ZIP support, image_codec.cpp by image_doc.cpp's
    # image-editor document model) and listing them twice there would
    # double-compile them.
    # EXCLUDE_FROM_ALL (BUILD_PERFORMANCE_PLAN.md Round 3): a smoke test,
    # not something `mep` needs -- see that plan's own note on the `tests`
    # aggregate target these are collected into below, right after all
    # three of these get created.
    add_executable(${name} EXCLUDE_FROM_ALL ${main_src} ${MEP_GFX_NATIVE_SOURCES} src/wav_doc.cpp src/deflate.cpp
        src/png_codec.cpp src/jpeg_codec.cpp src/bmp_codec.cpp src/gif_codec.cpp src/image_codec.cpp)
    # This target compiles the gfx model loaders (part of
    # MEP_GFX_NATIVE_SOURCES) directly rather than linking mep_core, so
    # (like mep-collab-session-test/mep.diff above) it needs its own copy
    # of mep.gfx.model_read_util's interface FILE_SET.
    target_sources(${name} PUBLIC FILE_SET CXX_MODULES FILES src/gfx/model_read_util.cppm)
    target_include_directories(${name} PRIVATE src)
    target_compile_options(${name} PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    if(EMSCRIPTEN)
        target_link_options(${name} PRIVATE "-sUSE_GLFW=3" "-sUSE_WEBGL2=1" "-sFULL_ES3=1" "-sALLOW_MEMORY_GROWTH=1")
    else()
        target_link_options(${name} PRIVATE ${MEP_SANITIZE_FLAGS})
        target_link_libraries(${name} PRIVATE OpenGL::GL X11::X11)
        if(NOT WIN32 AND NOT APPLE)
            target_link_libraries(${name} PRIVATE Threads::Threads m ALSA::ALSA)
        endif()
    endif()
endfunction()

# (The original B1 walking-skeleton smoke test, mep-gfx-native-smoke,
# opened a window via raw GLFW calls directly -- separately from and
# before gfx::backend_native.cpp's own window/context creation existed --
# to verify the windowing/GL-context foundation in isolation. Retired
# once GLFW was removed (GLFW_REMOVAL_PLAN.md): windowing/context
# creation now lives only inside backend_native.cpp itself, so there's no
# longer a lower-level "raw" path to test separately, and
# mep-gfx-native-integration-smoke below already verifies the real
# gfx::InitWindow path end to end.)

# Integration smoke test: exercises the actual gfx:: facade (2D
# primitives, textures, text) against the native backend, not raw GL.
mep_add_gfx_native_smoke(mep-gfx-native-integration-smoke src/gfx/native_gfx_smoke_main.cpp)

# Audio lifecycle smoke test (device init, load, play/pause/resume,
# IsSoundPlaying, volume, unload) -- see gfx/native_audio_smoke_main.cpp.
mep_add_gfx_native_smoke(mep-gfx-native-audio-smoke src/gfx/native_audio_smoke_main.cpp)

# Model-import smoke test (Stage B8): loads a real model file and
# renders it, see gfx/native_model_smoke_main.cpp.
mep_add_gfx_native_smoke(mep-gfx-native-model-smoke src/gfx/native_model_smoke_main.cpp)

# BUILD_PERFORMANCE_PLAN.md Round 3: a production build only needs `mep`
# itself plus its two companion tools (mep-mcp, mep-collabd, both real
# deliverables users run directly -- not test infrastructure). Every test/
# smoke/benchmark binary below is marked EXCLUDE_FROM_ALL so a plain
# `cmake --build .`/`nix build` (the default `all` target) skips all of
# them, and collected into this one `tests` target instead so `just test`/
# `bench`/`test-gui` can build exactly what each of them needs in one
# `cmake --build . --target tests` (or an explicit smaller target list)
# rather than relying on `all` to have built everything as a side effect.
add_custom_target(tests)
add_dependencies(tests mep-gfx-native-integration-smoke mep-gfx-native-audio-smoke mep-gfx-native-model-smoke)

add_executable(mep
    src/main.cpp
    src/agent_ui_input.cpp
)
target_include_directories(mep PRIVATE src)
target_compile_options(mep PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep PRIVATE ${MEP_SANITIZE_FLAGS})
target_link_libraries(mep PRIVATE mep_core)
# Same PCH as mep_core (defined above) -- main.cpp is the single largest
# translation unit in the whole project (~27k lines) and uses this same
# set of standard headers throughout.
target_precompile_headers(mep PRIVATE ${MEP_PCH_HEADERS})
if(EMSCRIPTEN)
    target_sources(mep PRIVATE src/collab_crdt.cpp src/collab_web_session.cpp)
endif()
if(NOT EMSCRIPTEN AND NOT WIN32 AND NOT APPLE)
    # agent_ui_input.cpp's synthetic-input backend: XTest for injecting
    # mouse/keyboard events (the agent-control socket's "ui.*" methods,
    # main.cpp). X11::X11 already comes in transitively via mep_core's own
    # PUBLIC link above; listed again here for clarity since this file
    # uses it directly too.
    target_link_libraries(mep PRIVATE X11::Xtst X11::X11)
endif()

# Dependency-free MCP bridge (mcp/server.ts's replacement -- see
# src/mcp_bridge.cpp's own top comment): a plain external client of
# mep's agent socket, same as mcp/mep_client.ts or `socat`, so it needs
# none of mep_core -- just json.h/persist.h/rpc_framing.h and
# POSIX sockets. Skipped on Emscripten/Windows for the same reason
# agent_rpc.h's socket support itself is (see that header's own comment).
if(NOT EMSCRIPTEN AND NOT WIN32)
    add_executable(mep-mcp src/mcp_bridge.cpp)
    target_include_directories(mep-mcp PRIVATE src)
    target_compile_options(mep-mcp PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-mcp PRIVATE ${MEP_SANITIZE_FLAGS})
endif()

# Pure DOM/CSS test: intentionally links only html_doc.cpp, so selector and
# cascade behavior can be verified without opening a window.
add_executable(mep-html-doc-test EXCLUDE_FROM_ALL src/html_doc_test.cpp src/html_doc.cpp src/js_engine.cpp src/svg_doc.cpp src/wav_doc.cpp)
target_include_directories(mep-html-doc-test PRIVATE src)
target_compile_options(mep-html-doc-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-html-doc-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-html-doc-test)

# Org outline helpers (org_doc.cpp is dependency-free): the Todo-panel
# <-> TODO.org sync (OrgTodoListItems/OrgTodoListApply), same windowless
# model as mep-html-doc-test above.
add_executable(mep-org-doc-test EXCLUDE_FROM_ALL src/org_doc_test.cpp src/org_doc.cpp)
target_include_directories(mep-org-doc-test PRIVATE src)
target_compile_options(mep-org-doc-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-org-doc-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-org-doc-test)

# CHESS_SET_BENCHMARK_PLAN.md Phase 3: procedural texture generation
# (value noise/fBm, gradients, wood, marble, box blur) -- pure
# std::vector<unsigned char> buffer code, no GL context needed, so this
# links only image_procgen.cpp + png_codec.cpp (to dump sample PNGs for
# visual inspection) + deflate.cpp (png_codec's zlib dependency), same
# windowless model as mep-html-doc-test above.
add_executable(mep-image-procgen-test EXCLUDE_FROM_ALL src/image_procgen_test.cpp src/image_procgen.cpp src/png_codec.cpp src/deflate.cpp)
target_include_directories(mep-image-procgen-test PRIVATE src)
target_compile_options(mep-image-procgen-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-image-procgen-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-image-procgen-test)

# ANIMATION_VIDEO_PLAN.md Phase 2: jpeg_codec.h's new Encode(), round-
# tripped through its own pre-existing Decode(). Pure std::string/
# std::vector<unsigned char> buffer code, no GL context needed -- same
# windowless model as mep-image-procgen-test above. Links only
# jpeg_codec.cpp itself (Encode/Decode have no dependency on the rest of
# the image codec stack).
add_executable(mep-jpeg-codec-test EXCLUDE_FROM_ALL src/jpeg_encoder_test.cpp src/jpeg_codec.cpp)
target_include_directories(mep-jpeg-codec-test PRIVATE src)
target_compile_options(mep-jpeg-codec-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-jpeg-codec-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-jpeg-codec-test)

# PDFIUM_REMOVAL_PLAN.md Phase 2: pdf_object.h/.cpp's object-model
# parser/tokenizer, standalone -- no xref/document context needed yet.
# Same windowless, pure-buffer-parsing shape as mep-jpeg-codec-test above.
add_executable(mep-pdf-object-test EXCLUDE_FROM_ALL src/pdf_object_test.cpp src/pdf_object.cpp)
target_include_directories(mep-pdf-object-test PRIVATE src)
target_compile_options(mep-pdf-object-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-object-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-object-test)

# PDFIUM_REMOVAL_PLAN.md Phase 3: pdf_xref.h/.cpp's cross-reference
# table/trailer parser + recovery, and pdf_filters.h/.cpp's FlateDecode/
# Predictor (landed early in Phase 3 -- see pdf_filters.h's own doc
# comment). Needs deflate.cpp (FlateDecode's own dependency, and used
# by this test to build synthetic compressed fixtures), pdf_object.cpp,
# and jpeg_codec.cpp (pdf_filters.cpp's DCTDecode, added in Phase 4,
# pulls this in transitively even though this test doesn't exercise it
# directly). Same windowless, pure-buffer-parsing shape as
# mep-pdf-object-test above.
add_executable(mep-pdf-xref-test EXCLUDE_FROM_ALL src/pdf_xref_test.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-xref-test PRIVATE src)
target_compile_options(mep-pdf-xref-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-xref-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-xref-test)

# PDFIUM_REMOVAL_PLAN.md Phase 12: pdf_crypt.h/.cpp's hand-rolled MD5/
# RC4/AES-128 primitives and Standard Security Handler key derivation --
# deliberately dependency-free (see pdf_crypt.h's own "Scoping decision
# -- no OpenSSL"), so this needs nothing beyond pdf_object.cpp (the
# Object type SetupStandardSecurityHandler reads /Filter/V/R/O/U/P/
# Length/CF from). Pure computational module otherwise, same windowless
# shape as mep-pdf-object-test above.
add_executable(mep-pdf-crypt-test EXCLUDE_FROM_ALL src/pdf_crypt_test.cpp src/pdf_crypt.cpp src/pdf_object.cpp)
target_include_directories(mep-pdf-crypt-test PRIVATE src)
target_compile_options(mep-pdf-crypt-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-crypt-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-crypt-test)

# PDFIUM_REMOVAL_PLAN.md Phase 4: pdf_filters.h/.cpp's remaining stream
# filters (ASCIIHex/ASCII85/RunLength/LZW/DCT) and the /Filter-array
# chain dispatcher -- FlateDecode+Predictor already covered by
# mep-pdf-xref-test in Phase 3. Needs jpeg_codec.cpp (DCTDecode's own
# dependency, and jpeg::Encode to build this test's synthetic JPEG) and
# deflate.cpp (FlateDecode, exercised via DecodeStream's chain test).
add_executable(mep-pdf-filters-test EXCLUDE_FROM_ALL src/pdf_filters_test.cpp src/pdf_filters.cpp src/pdf_object.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-filters-test PRIVATE src)
target_compile_options(mep-pdf-filters-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-filters-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-filters-test)

# PDFIUM_REMOVAL_PLAN.md Phase 5: pdf_document.h/.cpp's page-tree
# flattening (inheritance of /Resources/MediaBox/Rotate, cycle
# protection). Needs pdf_xref.cpp/pdf_filters.cpp/pdf_object.cpp/
# deflate.cpp/jpeg_codec.cpp transitively (pdf_document.cpp resolves
# objects through pdf_xref.h, same as mep-pdf-xref-test above).
add_executable(mep-pdf-document-test EXCLUDE_FROM_ALL src/pdf_document_test.cpp src/pdf_document.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-document-test PRIVATE src)
target_compile_options(mep-pdf-document-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-document-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-document-test)

# pdf_outline.h/.cpp's PDF /Outlines (bookmarks) tree reader -- the
# Structure sidebar's PDF-buffer support, an independent successor to
# PDFIUM_REMOVAL_PLAN.md's own 14 phases. Same dependency set as
# mep-pdf-document-test above (pdf_outline.cpp only needs pdf_document.h
# for the page-index lookup, no rendering/font/text-extraction stack).
add_executable(mep-pdf-outline-test EXCLUDE_FROM_ALL src/pdf_outline_test.cpp src/pdf_outline.cpp src/pdf_document.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-outline-test PRIVATE src)
target_compile_options(mep-pdf-outline-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-outline-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-outline-test)

# pdf_links.h/.cpp's PDF Link-annotation reader -- the hint system's PDF
# hyperlink targets (HINT_SYSTEM.md). Same dependency set as
# mep-pdf-outline-test (whose ResolveDestPage it reuses directly) plus
# pdf_outline.cpp itself; no rendering/font/text-extraction stack needed
# (pdf_links.cpp keeps its own tiny page-to-device transform rather than
# linking pdf_content.cpp for it -- see that file's own comment).
add_executable(mep-pdf-links-test EXCLUDE_FROM_ALL src/pdf_links_test.cpp src/pdf_links.cpp src/pdf_outline.cpp src/pdf_document.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-links-test PRIVATE src)
target_compile_options(mep-pdf-links-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-links-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-links-test)

# PDFIUM_REMOVAL_PLAN.md Phase 6: gfx/rasterizer.h/.cpp's even-odd fill
# rule and cubic Bezier flattening -- the two additions on top of the
# pure extraction from truetype.cpp (verified separately via live
# glyph-rendering screenshots, since glyph rendering itself only
# exercises the nonzero rule + quadratic flattening). Pure computational
# module, no other dependencies.
add_executable(mep-rasterizer-test EXCLUDE_FROM_ALL src/gfx/rasterizer_test.cpp src/gfx/rasterizer.cpp)
target_include_directories(mep-rasterizer-test PRIVATE src)
target_compile_options(mep-rasterizer-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-rasterizer-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-rasterizer-test)

# PDFIUM_REMOVAL_PLAN.md Phase 7 (graphics state, paths, clipping,
# color, Form XObjects) + Phase 10 (pdf_font.cpp's text-showing
# operators, added to this same target rather than a separate one since
# text rendering can only be verified meaningfully through the full
# RenderContentStream pipeline -- a font's glyph bitmap in isolation,
# covered by mep-pdf-font-test, doesn't prove it draws in the right
# place). Needs the full pdf_* + gfx/cff.cpp + gfx/truetype.cpp +
# gfx/rasterizer.cpp + deflate.cpp/jpeg_codec.cpp stack.
add_executable(mep-pdf-content-test EXCLUDE_FROM_ALL src/pdf_content_test.cpp src/pdf_content.cpp src/pdf_document.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/pdf_font.cpp src/pdf_encodings.cpp src/gfx/cff.cpp src/gfx/truetype.cpp src/gfx/rasterizer.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-content-test PRIVATE src)
target_compile_options(mep-pdf-content-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-content-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-content-test)

# PDFIUM_REMOVAL_PLAN.md Phase 11: pdf_text.h/.cpp's document-wide
# Search/MatchRectsForPage, built on pdf_content.cpp's
# ExtractContentStreamText -- same "standalone test target only" pattern,
# not yet wired into mep_core/the live app (Phase 13's job).
add_executable(mep-pdf-text-test EXCLUDE_FROM_ALL src/pdf_text_test.cpp src/pdf_text.cpp src/pdf_content.cpp src/pdf_document.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/pdf_font.cpp src/pdf_encodings.cpp src/gfx/cff.cpp src/gfx/truetype.cpp src/gfx/rasterizer.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-text-test PRIVATE src)
target_compile_options(mep-pdf-text-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-text-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-text-test)

# PDFIUM_REMOVAL_PLAN.md Phase 9: gfx/cff.h/.cpp's CFF/Type2-charstring
# outline engine -- not yet wired into mep_core/the live app (that's
# Phase 13's swap-in job), same "standalone test target only" pattern
# every other pdf_*.cpp module has followed throughout this plan. Needs
# rasterizer.cpp for the shared scanline fill.
add_executable(mep-cff-test EXCLUDE_FROM_ALL src/gfx/cff_test.cpp src/gfx/cff.cpp src/gfx/rasterizer.cpp)
target_include_directories(mep-cff-test PRIVATE src)
target_compile_options(mep-cff-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-cff-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-cff-test)

# PDFIUM_REMOVAL_PLAN.md Phase 10: pdf_encodings.h/.cpp's Standard/
# WinAnsi/MacRoman encoding tables, AGL-subset name->Unicode resolver,
# and CFF standard-strings table. Pure static-data module, no other
# dependencies.
add_executable(mep-pdf-encodings-test EXCLUDE_FROM_ALL src/pdf_encodings_test.cpp src/pdf_encodings.cpp)
target_include_directories(mep-pdf-encodings-test PRIVATE src)
target_compile_options(mep-pdf-encodings-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-encodings-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-encodings-test)

# PDFIUM_REMOVAL_PLAN.md Phase 10: pdf_font.h/.cpp's font-dict wiring
# (simple + composite, standard-14 substitution). Needs the full pdf_*
# + gfx/cff.cpp + gfx/truetype.cpp + gfx/rasterizer.cpp + deflate.cpp/
# jpeg_codec.cpp stack (font-dict resolution walks Resources/Font,
# decodes embedded FontFile2/3 streams, and falls back to mep's own
# vendored Liberation fonts via office_font_data*.h).
add_executable(mep-pdf-font-test EXCLUDE_FROM_ALL src/pdf_font_test.cpp src/pdf_font.cpp src/pdf_encodings.cpp src/pdf_xref.cpp src/pdf_crypt.cpp src/pdf_filters.cpp src/pdf_object.cpp src/gfx/cff.cpp src/gfx/truetype.cpp src/gfx/rasterizer.cpp src/deflate.cpp src/jpeg_codec.cpp)
target_include_directories(mep-pdf-font-test PRIVATE src)
target_compile_options(mep-pdf-font-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-pdf-font-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-pdf-font-test)

# ANIMATION_VIDEO_PLAN.md Phase 3: mov_container.h's WriteMovFile/
# OpenMovFile/ReadMovFrameJpeg, round-tripped against jpeg_codec.h-
# encoded synthetic frames. Same windowless, pure-file-I/O shape as
# mep-jpeg-codec-test above.
add_executable(mep-mov-container-test EXCLUDE_FROM_ALL src/mov_container_test.cpp src/mov_container.cpp src/jpeg_codec.cpp)
target_include_directories(mep-mov-container-test PRIVATE src)
target_compile_options(mep-mov-container-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
target_link_options(mep-mov-container-test PRIVATE ${MEP_SANITIZE_FLAGS})
add_dependencies(tests mep-mov-container-test)

# WORKSPACES_PLAN.md Phase 12: pure-helper test (worktree dir derivation,
# porcelain parsing, name validation, session JSON round trip). Links only
# workspace_git.cpp, same windowless model as mep-html-doc-test above.
if(NOT EMSCRIPTEN)
    add_executable(mep-workspace-test EXCLUDE_FROM_ALL src/workspace_test.cpp src/workspace_git.cpp)
    target_include_directories(mep-workspace-test PRIVATE src)
    target_compile_options(mep-workspace-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-workspace-test PRIVATE ${MEP_SANITIZE_FLAGS})
    add_dependencies(tests mep-workspace-test)
endif()

# MODEL3D.md: covers Scene bookkeeping and the glTF exporter -- the half of
# model3d_doc.cpp that's pure CPU logic. Deliberately does NOT exercise
# LoadModel3DFile/AddPrimitiveToScene (both need a live GL context via
# gfx::UploadMesh, the one thing this windowless binary avoids) or open a
# window -- but still links the full native gfx:: backend (not just
# gfx/backend.cpp) since model3d_doc.cpp references its symbols even in
# the functions this test never calls, and calls gfx::SetBackends() at
# startup so the pure-CPU calls it does exercise (GenImageColor/
# ExportImage/LoadImage/ImageFormat/UnloadImage, none of which touch GL --
# see backend_native_renderer2d.cpp) have a backend installed. Native-only,
# matching mep-workspace-test's own gating above (the feature itself is
# native-only regardless, see MODEL3D_PLAN.md).
if(NOT EMSCRIPTEN)
    add_executable(mep-model3d-doc-test EXCLUDE_FROM_ALL src/model3d_doc_test.cpp src/model3d_doc.cpp src/wav_doc.cpp src/deflate.cpp
        src/png_codec.cpp src/jpeg_codec.cpp src/bmp_codec.cpp src/gif_codec.cpp src/image_codec.cpp
        ${MEP_GFX_NATIVE_SOURCES})
    # Own copy of mep.gfx.model_read_util's FILE_SET -- same reasoning as
    # mep_add_gfx_native_smoke's targets above.
    target_sources(mep-model3d-doc-test PUBLIC FILE_SET CXX_MODULES FILES src/gfx/model_read_util.cppm)
    target_include_directories(mep-model3d-doc-test PRIVATE src)
    target_link_libraries(mep-model3d-doc-test PRIVATE OpenGL::GL X11::X11)
    if(NOT WIN32 AND NOT APPLE)
        target_link_libraries(mep-model3d-doc-test PRIVATE Threads::Threads m ALSA::ALSA)
    endif()
    target_compile_options(mep-model3d-doc-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-model3d-doc-test PRIVATE ${MEP_SANITIZE_FLAGS})
    add_dependencies(tests mep-model3d-doc-test)
endif()

# Collaboration's WebSocket handshake needs SHA-1.  OpenSSL is the one
# deliberate native-only dependency permitted for secure transport support;
# the framing, session protocol, and relay remain mep's own C++ code.
if(NOT EMSCRIPTEN)
    find_package(OpenSSL REQUIRED)
    target_sources(mep PRIVATE src/collab_crdt.cpp src/collab_session.cpp src/collab_websocket.cpp)
    target_link_libraries(mep PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_executable(mep-collabd src/collab_relay.cpp src/collab_websocket.cpp)
    target_include_directories(mep-collabd PRIVATE src)
    target_compile_options(mep-collabd PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-collabd PRIVATE ${MEP_SANITIZE_FLAGS})
    target_link_libraries(mep-collabd PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    if(NOT WIN32)
        target_link_libraries(mep-collabd PRIVATE Threads::Threads)
    else()
        target_link_libraries(mep-collabd PRIVATE ws2_32)
    endif()
    add_executable(mep-collab-crdt-test EXCLUDE_FROM_ALL src/collab_crdt_test.cpp src/collab_crdt.cpp)
    target_include_directories(mep-collab-crdt-test PRIVATE src)
    target_compile_options(mep-collab-crdt-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-collab-crdt-test PRIVATE ${MEP_SANITIZE_FLAGS})
    add_dependencies(tests mep-collab-crdt-test)
    # CRDT_PERFORMANCE_PLAN.md Phase 1: persistent benchmarks (`just
    # bench`), not part of `just test` -- these write timed results to
    # bench_results/history.jsonl rather than asserting pass/fail.
    add_executable(mep-crdt-bench EXCLUDE_FROM_ALL src/text_crdt_bench.cpp src/collab_crdt.cpp)
    target_include_directories(mep-crdt-bench PRIVATE src)
    target_compile_options(mep-crdt-bench PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-crdt-bench PRIVATE ${MEP_SANITIZE_FLAGS})
    add_dependencies(tests mep-crdt-bench)
    add_executable(mep-buffer-bench EXCLUDE_FROM_ALL src/text_buffer_bench.cpp)
    target_include_directories(mep-buffer-bench PRIVATE src)
    target_compile_options(mep-buffer-bench PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-buffer-bench PRIVATE ${MEP_SANITIZE_FLAGS})
    add_dependencies(tests mep-buffer-bench)
    # Links only against `lua` (not mep_core/editor.cpp) -- see the bench
    # file's own top comment for why a bare lua_State is enough to measure
    # the mep.on_frame dispatch cost this guards (FIX_PERFORMANCE_ISSUES).
    add_executable(mep-lua-frame-hook-bench EXCLUDE_FROM_ALL src/lua_frame_hook_bench.cpp)
    target_include_directories(mep-lua-frame-hook-bench PRIVATE src)
    target_compile_options(mep-lua-frame-hook-bench PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-lua-frame-hook-bench PRIVATE ${MEP_SANITIZE_FLAGS})
    target_link_libraries(mep-lua-frame-hook-bench PRIVATE lua)
    add_dependencies(tests mep-lua-frame-hook-bench)
    add_executable(mep-collab-session-test EXCLUDE_FROM_ALL src/collab_session_test.cpp src/collab_session.cpp src/collab_crdt.cpp src/collab_websocket.cpp src/text_diff.cpp)
    # This target deliberately doesn't link mep_core (see collab_session.cpp's
    # own decoupling comment), so it needs its own copy of mep.diff's
    # interface FILE_SET rather than inheriting mep_core's.
    target_sources(mep-collab-session-test PUBLIC FILE_SET CXX_MODULES FILES src/text_diff.cppm)
    target_include_directories(mep-collab-session-test PRIVATE src)
    target_compile_options(mep-collab-session-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
    target_link_options(mep-collab-session-test PRIVATE ${MEP_SANITIZE_FLAGS})
    target_link_libraries(mep-collab-session-test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_dependencies(tests mep-collab-session-test)
    if(NOT WIN32)
        target_link_libraries(mep-collab-session-test PRIVATE Threads::Threads)
    else()
        target_link_libraries(mep-collab-session-test PRIVATE ws2_32)
    endif()
    if(NOT WIN32)
        # Unlike the collab tests above, this spawns the real native `mep`
        # binary (path given as argv[1]) and needs a live display -- run by
        # hand (`./mep-agent-rpc-test ./mep`), not wired into an automated
        # ctest suite, matching how the collab tests here aren't either.
        add_executable(mep-agent-rpc-test EXCLUDE_FROM_ALL src/agent_rpc_test.cpp)
        target_include_directories(mep-agent-rpc-test PRIVATE src)
        target_compile_options(mep-agent-rpc-test PRIVATE ${MEP_STRICT_FLAGS} ${MEP_SANITIZE_FLAGS})
        target_link_options(mep-agent-rpc-test PRIVATE ${MEP_SANITIZE_FLAGS})
        target_link_libraries(mep-agent-rpc-test PRIVATE Threads::Threads)
        add_dependencies(tests mep-agent-rpc-test)
    endif()
endif()

# `mep-amalgam`: the exact same application as `mep`, but built from one
# generated translation unit (tools/amalgamate.py concatenates every
# module + the collaboration subsystem + the embedded font/query data,
# in dependency order, with a handful of same-named-but-unrelated symbols
# disambiguated -- see that script's own comment) instead of mep_core +
# main.cpp as separate translation units. Not part of the default `all`
# target -- opt in with `cmake --build . --target mep-amalgam` when the
# single-file build is actually wanted; every other target already gets
# mep_core's fast incremental-compile benefit.
if(NOT EMSCRIPTEN)
    find_program(MEP_PYTHON3 NAMES python3 python)
    if(NOT MEP_PYTHON3)
        message(WARNING "python3 not found -- the mep-amalgam target will not be available")
    else()
        set(MEP_AMALGAM_SRC "${CMAKE_BINARY_DIR}/generated/mep_amalgam.cpp")
        file(GLOB MEP_AMALGAM_DEPS "${CMAKE_SOURCE_DIR}/src/*.h" "${CMAKE_SOURCE_DIR}/src/*.cpp")
        add_custom_command(
            OUTPUT "${MEP_AMALGAM_SRC}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
            COMMAND "${MEP_PYTHON3}" "${CMAKE_SOURCE_DIR}/tools/amalgamate.py" "${CMAKE_SOURCE_DIR}/src" "${MEP_AMALGAM_SRC}"
            DEPENDS "${CMAKE_SOURCE_DIR}/tools/amalgamate.py" ${MEP_AMALGAM_DEPS}
            COMMENT "Generating single-file amalgamated build (tools/amalgamate.py)"
            VERBATIM
        )
        # A plain source file (not mep_core, which would duplicate-define every
        # symbol the generated file already embeds): the same external
        # libraries mep_core links against, plus OpenSSL for the collaboration
        # subsystem's SHA-1, since that's folded into the generated file too.
        add_executable(mep-amalgam EXCLUDE_FROM_ALL "${MEP_AMALGAM_SRC}")
        target_include_directories(mep-amalgam PRIVATE src)
        target_link_libraries(mep-amalgam PRIVATE OpenGL::GL X11::X11 lua tree_sitter ${TS_GRAMMAR_LIBS} OpenSSL::SSL OpenSSL::Crypto)
        if(NOT WIN32)
            target_link_libraries(mep-amalgam PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
        endif()
        if(NOT WIN32 AND NOT APPLE)
            target_link_libraries(mep-amalgam PRIVATE ALSA::ALSA)
        endif()
        if(WIN32)
            target_link_libraries(mep-amalgam PRIVATE ws2_32)
        endif()
    endif()
endif()

if(EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    target_link_options(mep PRIVATE
        "-sUSE_GLFW=3"
        # No -sASYNCIFY: src/editor.cpp's file-bridge calls use synchronous
        # XHR (blocking the JS main thread directly) rather than an
        # awaited fetch(), specifically to avoid linking Asyncify at all --
        # see the comment above mep_js_read_file in editor.cpp for the
        # severe memory leak that caused (Asyncify instruments the entire
        # reachable call graph, including the per-frame Lua polling loop,
        # not just the functions that actually need to suspend).
        "-sALLOW_MEMORY_GROWTH=1"
        # Embeds the .wasm as base64 inside mep.js so the launcher can open
        # the build with a plain file:// URL instead of running a local
        # HTTP server (see launcher/serve.ts for why that mattered).
        "-sSINGLE_FILE=1"
        "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
    )
endif()
