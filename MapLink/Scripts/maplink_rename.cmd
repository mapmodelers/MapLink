@echo off
setlocal EnableDelayedExpansion

:: ===================================================================
::  MapLink Rename Tool — Launcher
:: ===================================================================
::
::  Usage:
::    maplink_rename.cmd scan
::    maplink_rename.cmd scan --blend "C:\...\file.blend"
::    maplink_rename.cmd execute --dry-run
::    maplink_rename.cmd execute
::    maplink_rename.cmd execute --plan other.json --dry-run
::
::  First arg = mode. Everything else forwarded to the Python script.
:: ===================================================================

set "SCRIPT_DIR=%~dp0"
set "PY_SCRIPT=%SCRIPT_DIR%maplink_rename.py"

if not exist "%PY_SCRIPT%" (
    echo [MapLink] ERROR: Cannot find maplink_rename.py
    echo [MapLink] Expected: %PY_SCRIPT%
    pause
    exit /b 1
)

:: --- Locate Blender ---
set "BLENDER_EXE="

if defined MAPLINK_BLENDER_EXE (
    if exist "%MAPLINK_BLENDER_EXE%" (
        set "BLENDER_EXE=%MAPLINK_BLENDER_EXE%"
        goto :found
    )
)

for %%V in (5.0 4.4 4.3 4.2 4.1 4.0 3.6) do (
    if exist "C:\Program Files\Blender Foundation\Blender %%V\blender.exe" (
        set "BLENDER_EXE=C:\Program Files\Blender Foundation\Blender %%V\blender.exe"
        goto :found
    )
)

where blender >nul 2>&1 && for /f "delims=" %%i in ('where blender') do (
    set "BLENDER_EXE=%%i"
    goto :found
)

echo [MapLink] ERROR: Blender not found.
pause
exit /b 1

:found
echo [MapLink] Blender: %BLENDER_EXE%
echo [MapLink] Script:  %PY_SCRIPT%

:: --- Get mode (first arg), default to scan ---
set "MODE=%~1"
if "%MODE%"=="" set "MODE=scan"

:: --- Collect remaining args after the mode ---
set "EXTRA="
shift
:argloop
if "%~1"=="" goto :run
set "EXTRA=!EXTRA! %1"
shift
goto :argloop

:run
echo [MapLink] Mode: %MODE%
echo [MapLink] Args:%EXTRA%
echo.

"%BLENDER_EXE%" --background --python "%PY_SCRIPT%" -- --mode %MODE%%EXTRA%

echo.
echo [MapLink] Exit code: %errorlevel%
pause
