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

# pugixml (src/office_doc.cpp's DOCX/ODT XML parsing): has its own CMake
# build exporting a `pugixml` interface target (static by default, see
# its CMakeLists.txt), so this is a plain FetchContent -- no by-hand
# static-lib wiring needed, unlike lua/miniz below.
FetchContent_Declare(
    pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz
    URL_HASH SHA256=4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(pugixml)

# miniz (src/office_doc.cpp's DOCX/ODT ZIP container reading + writing --
# both formats are ZIP archives of XML parts): like lua, has no CMake
# build of its own, fetched as a tarball and built by hand. Unlike the
# modular source tree in miniz's git repo, its GitHub Release asset is a
# single pre-amalgamated miniz.c/miniz.h pair (confirmed by inspecting the
# downloaded archive) -- as easy to vendor as a single-file stb-style
# library, no glob needed.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)
add_library(miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(miniz PUBLIC "${miniz_SOURCE_DIR}")

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

# PDFium (src/pdf_doc.cpp's PDF-viewer backend): PDFium itself is BSD-3,
# and every third-party library it bundles (freetype/lcms/openjpeg/zlib/
# libpng/libjpeg-turbo/abseil/icu/simdutf/fast_float/llvm-libc) is also
# permissively licensed -- see third_party_licenses/pdfium-LICENSE.txt.
# There's no CMake build for PDFium itself (upstream uses Google's own
# GN/ninja toolchain, not CMake), so it's fetched here as a prebuilt
# shared-library release from github.com/bblanchon/pdfium-binaries (MIT-
# licensed packaging of Google's own binary builds) rather than vendored
# as source, the same "prebuilt binary, not a source vendor" tradeoff
# raylib/lua deliberately avoid elsewhere in this file. Not available for
# the Emscripten/wasm build -- see the #if defined(__EMSCRIPTEN__) stub in
# pdf_doc.cpp.
if(NOT EMSCRIPTEN)
    set(PDFIUM_RELEASE "chromium/8009")
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
            set(PDFIUM_ASSET "pdfium-mac-arm64.tgz")
            set(PDFIUM_HASH "SHA256=b1f2f17c7432a9942514dda5094ee9822c743bdfd07e7187725efbd34fde941f")
        else()
            set(PDFIUM_ASSET "pdfium-mac-x64.tgz")
            set(PDFIUM_HASH "SHA256=e0afb6905e97438a9159aaa5b6c4fe188d97a7e3be05dc4782422c3ca7e1ce1a")
        endif()
    elseif(WIN32)
        # Only x86_64 Windows is pinned here; pdfium-binaries also
        # publishes win-x86/win-arm64 if that's ever needed.
        set(PDFIUM_ASSET "pdfium-win-x64.tgz")
        set(PDFIUM_HASH "SHA256=c78a8cd51b48abafcb266d868e401afefda1d189aa01ebbe743dc1d144e06031")
    else()
        # Only x86_64 glibc Linux is pinned/verified here (this repo's own
        # dev/build environment); other Linux targets (arm64, musl) would
        # need their own branch + hash from the same release's assets.
        set(PDFIUM_ASSET "pdfium-linux-x64.tgz")
        set(PDFIUM_HASH "SHA256=be513e8021a5bf8eb2116e00d78c3bacb82c5a02b3785156ae14fe5e33084385")
    endif()

    FetchContent_Declare(
        pdfium
        URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_RELEASE}/${PDFIUM_ASSET}"
        URL_HASH "${PDFIUM_HASH}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(pdfium)

    add_library(pdfium SHARED IMPORTED)
    if(WIN32)
        set_target_properties(pdfium PROPERTIES
            IMPORTED_LOCATION "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
            IMPORTED_IMPLIB "${pdfium_SOURCE_DIR}/lib/pdfium.dll.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
        )
        set(PDFIUM_RUNTIME_FILE "${pdfium_SOURCE_DIR}/bin/pdfium.dll")
    elseif(APPLE)
        set_target_properties(pdfium PROPERTIES
            IMPORTED_LOCATION "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
        )
        set(PDFIUM_RUNTIME_FILE "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
    else()
        set_target_properties(pdfium PROPERTIES
            IMPORTED_LOCATION "${pdfium_SOURCE_DIR}/lib/libpdfium.so"
            INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
        )
        set(PDFIUM_RUNTIME_FILE "${pdfium_SOURCE_DIR}/lib/libpdfium.so")
    endif()
endif()

add_executable(mep
    src/main.cpp
    src/editor.cpp
    src/lua_env.cpp
    src/job.cpp
    src/vterm.cpp
    src/treesitter.cpp
    src/regex.cpp
    src/image_doc.cpp
    src/pdf_doc.cpp
    src/office_doc.cpp
    src/office_odt.cpp
    src/formula.cpp
    src/sheet_doc.cpp
)
target_include_directories(mep PRIVATE src)
target_link_libraries(mep PRIVATE raylib lua tree_sitter ${TS_GRAMMAR_LIBS} pugixml miniz)
if(NOT EMSCRIPTEN AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(mep PRIVATE Threads::Threads)
    # dlopen/dlsym for the dynamic-grammar-loading path (treesitter.cpp).
    # Emscripten/wasm has no filesystem access to arbitrary shared
    # libraries, so that whole path is compiled out there instead of
    # linked against a (nonexistent) libdl.
    target_link_libraries(mep PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NOT EMSCRIPTEN)
    target_link_libraries(mep PRIVATE pdfium)
    # PDFium ships as a shared library, not something that can be linked
    # statically into a single self-contained binary -- copy it next to
    # the built executable and point mep's own rpath at its own directory
    # ($ORIGIN/@loader_path) so a plain `./mep` launch finds it without
    # needing LD_LIBRARY_PATH/DYLD_LIBRARY_PATH set, matching how an
    # end-user launch would work.
    if(WIN32)
        add_custom_command(TARGET mep POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME_FILE}" "$<TARGET_FILE_DIR:mep>"
        )
    else()
        if(APPLE)
            set_target_properties(mep PROPERTIES BUILD_RPATH "@loader_path")
        else()
            set_target_properties(mep PROPERTIES BUILD_RPATH "$ORIGIN")
        endif()
        add_custom_command(TARGET mep POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME_FILE}" "$<TARGET_FILE_DIR:mep>"
        )
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
