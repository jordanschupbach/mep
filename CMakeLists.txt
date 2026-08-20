cmake_minimum_required(VERSION 3.16)
project(mep LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

if(EMSCRIPTEN)
    set(PLATFORM "Web" CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(raylib)

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
target_include_directories(lua PUBLIC "${lua_SOURCE_DIR}/src")

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

add_executable(mep
    src/main.cpp
    src/editor.cpp
    src/lua_env.cpp
    src/job.cpp
    src/vterm.cpp
    src/treesitter.cpp
)
target_include_directories(mep PRIVATE src)
target_link_libraries(mep PRIVATE raylib lua tree_sitter ${TS_GRAMMAR_LIBS})
if(NOT EMSCRIPTEN AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(mep PRIVATE Threads::Threads)
    # dlopen/dlsym for the dynamic-grammar-loading path (treesitter.cpp).
    # Emscripten/wasm has no filesystem access to arbitrary shared
    # libraries, so that whole path is compiled out there instead of
    # linked against a (nonexistent) libdl.
    target_link_libraries(mep PRIVATE ${CMAKE_DL_LIBS})
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
