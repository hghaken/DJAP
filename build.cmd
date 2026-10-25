@echo off
rem Builds DJAP (Release) with the Visual Studio 2026 C++ toolchain.
rem Usage: build.cmd [output dir] [core]   (default: build; "core" = DSP core and unit tests only)
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=build"
set "EXTRA="
if /i "%~2"=="core" set "EXTRA=-DAP_BUILD_APP=OFF"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release %EXTRA% || exit /b 1
cmake --build "%OUT%" || exit /b 1
