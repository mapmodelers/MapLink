@echo off
REM ===========================================================================
REM MapLink — batch FBX export
REM ===========================================================================
REM Pre-populates the FBX/analysis cache for all BP_/LI_/SM_ .blend files under
REM the given roots, so the first UE import doesn't have to launch Blender for
REM each one.  Run this once after installing the v1.4.0 addon, or whenever
REM you want to refresh many files at once.
REM
REM Usage:
REM   maplink_batch_export.cmd  ROOT1  [ROOT2  ROOT3 ...]
REM
REM Example:
REM   maplink_batch_export.cmd ^
REM     C:\Projects\MyMap\Blender\BP ^
REM     C:\Projects\MyMap\Blender\LI ^
REM     C:\Projects\MyMap\Blender\SM
REM
REM Environment overrides:
REM   MAPLINK_BLENDER_EXE  Path to blender.exe (default: standard install path)
REM   MAPLINK_PLUGIN_DIR   Path to plugin root (default: parent of this script's dir)
REM ===========================================================================

setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ----- Resolve plugin dir -----
if defined MAPLINK_PLUGIN_DIR (
    set "PLUGIN_DIR=%MAPLINK_PLUGIN_DIR%"
) else (
    for %%I in ("%SCRIPT_DIR%\..") do set "PLUGIN_DIR=%%~fI"
)

REM ----- Resolve blender exe -----
if defined MAPLINK_BLENDER_EXE (
    set "BLENDER_EXE=%MAPLINK_BLENDER_EXE%"
) else (
    set "BLENDER_EXE=C:\Program Files\Blender Foundation\Blender 5.0\blender.exe"
)

if not exist "%BLENDER_EXE%" (
    echo ERROR: Blender exe not found at:
    echo   %BLENDER_EXE%
    echo Set MAPLINK_BLENDER_EXE to the correct path, or install Blender to
    echo the default location.
    exit /b 1
)

if "%~1"=="" (
    echo Usage: maplink_batch_export.cmd ROOT1 [ROOT2 ROOT3 ...]
    echo.
    echo Plugin:  %PLUGIN_DIR%
    echo Blender: %BLENDER_EXE%
    exit /b 1
)

"%BLENDER_EXE%" --background --factory-startup --python "%SCRIPT_DIR%\maplink_batch_export.py" -- --plugin-dir "%PLUGIN_DIR%" --blender-exe "%BLENDER_EXE%" %*

endlocal
