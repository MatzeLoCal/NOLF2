@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  NOLF2 modern Windows build  (Jupiter EX engine, GPL)
REM
REM  Usage:
REM    build.bat                 -> Release, Win32  (recommended)
REM    build.bat Debug           -> Debug,   Win32
REM    build.bat Release x64     -> Release, x64    (see notes below)
REM    build.bat clean           -> delete the build folder
REM
REM  Prerequisites:
REM    * Visual Studio 2022 with "Desktop development with C++"
REM    * CMake 3.5+ (standalone, or the one bundled with VS2022)
REM    * DirectX SDK (June 2010) installed  -> sets DXSDK_DIR
REM    * Retail NOLF2 assets (.rez) to actually run the result
REM ============================================================

set "GENERATOR=Visual Studio 17 2022"
set "BUILDDIR=build"
set "CONFIG=Release"
set "ARCH=Win32"

if /I "%~1"=="clean" (
    if exist "%BUILDDIR%" (
        echo Removing %BUILDDIR% ...
        rmdir /S /Q "%BUILDDIR%"
    )
    echo Clean done.
    exit /b 0
)

if not "%~1"=="" set "CONFIG=%~1"
if not "%~2"=="" set "ARCH=%~2"

echo ============================================================
echo  Generator : %GENERATOR%
echo  Arch      : %ARCH%
echo  Config    : %CONFIG%
echo ============================================================

REM ---- DirectX SDK check (the #1 cause of build failure) ----
if not defined DXSDK_DIR (
    echo.
    echo ERROR: DXSDK_DIR is not set.
    echo Install the Microsoft DirectX SDK ^(June 2010^), then open a NEW
    echo command prompt so DXSDK_DIR is visible. It provides d3dx9,
    echo DirectSound8 and the DirectShow base classes this engine needs.
    echo If the installer fails with error S1023, uninstall the
    echo "Visual C++ 2010 Redistributable" packages, install the SDK,
    echo then reinstall the redistributables.
    exit /b 1
)
echo  DirectX SDK: %DXSDK_DIR%

REM ---- locate cmake (PATH first, then VS2022's bundled copy) ----
set "CMAKE="
where cmake >nul 2>nul && set "CMAKE=cmake"
if not defined CMAKE (
    for %%E in (Community Professional Enterprise BuildTools) do (
        set "CAND=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if exist "!CAND!" set "CMAKE=!CAND!"
    )
)
if not defined CMAKE (
    echo.
    echo ERROR: cmake not found on PATH or under Visual Studio 2022.
    echo Install CMake, or VS2022 with the "C++ CMake tools" component.
    exit /b 1
)
echo  CMake      : %CMAKE%

REM ---- a Debug build needs the engine's DEBUG define too ----
set "EXTRA="
if /I "%CONFIG%"=="Debug" set "EXTRA=-DDEBUG=ON"

echo.
echo === Configuring ===
"%CMAKE%" -S . -B "%BUILDDIR%" -G "%GENERATOR%" -A %ARCH% %EXTRA%
if errorlevel 1 (
    echo.
    echo Configure FAILED.
    echo If you see a CMake policy/version error, re-run after adding:
    echo     -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    exit /b 1
)

echo.
echo === Building %CONFIG% ^(%ARCH%^) ===
"%CMAKE%" --build "%BUILDDIR%" --config %CONFIG%
if errorlevel 1 (
    echo.
    echo Build FAILED. Scroll up for the first error.
    echo Tip: build Release before Debug ^(Debug trips secure-CRT/STL checks^),
    echo and prefer Win32 over x64 ^(the engine is 32-bit-pointer-bound^).
    exit /b 1
)

echo.
echo ============================================================
echo  BUILD SUCCEEDED
echo  Outputs are under "%BUILDDIR%\" (the engine .exe plus the
echo  game modules: cshell.dll, object.lto, cres.dll, ...).
echo  Copy them into a retail NOLF2 install to run.
echo ============================================================
endlocal
