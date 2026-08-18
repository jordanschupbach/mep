cmake_minimum_required(VERSION 3.16)
project(mep LANGUAGES CXX)

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

add_executable(mep src/main.cpp)
target_link_libraries(mep PRIVATE raylib)

if(EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    target_link_options(mep PRIVATE
        "-sUSE_GLFW=3"
        "-sASYNCIFY"
        "-sALLOW_MEMORY_GROWTH=1"
        "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
    )
endif()
