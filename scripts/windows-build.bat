@echo off
REM Configure + build mep on Windows with the MSVC toolchain.
REM
REM Everything here is environment setup: vcvars64.bat is what puts cl.exe,
REM the Windows SDK headers and the linker on PATH, and nothing in this
REM build works without having run it first. Ninja (rather than the Visual
REM Studio generator) because the C++20 modules in this tree -- text_diff,
REM path_util, gfx.model_read_util -- need a generator with module
REM dependency scanning.
REM
REM Usage:  scripts\windows-build.bat [build-dir] [build-type]
REM Default build dir is build\win, default type Release.

setlocal

set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build\win"
set "BUILD_TYPE=%~2"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: vswhere.exe not found -- install Visual Studio 2022 or the Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo error: no Visual Studio installation found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

REM Ninja ships inside the VS install; prefer a system one if there is it.
where ninja >nul 2>&1
if errorlevel 1 set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

echo.
echo Built: %BUILD_DIR%\mep.exe
endlocal
