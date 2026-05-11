@echo off
:: ============================================================================
:: gool-install.cmd
::
:: One double-click Windows installer for the gool Godot addon.
::
:: HOW TO USE:
::   1. Download this file (right-click > Save link as...)
::   2. Drop it into your Godot project folder (the one with project.godot)
::   3. Double-click it
::
:: WHAT IT DOES:
::   1. Confirms this is a Godot project (looks for project.godot)
::   2. Downloads the latest gool addon archive for Windows from GitHub
::   3. Extracts addons/gool/ into your project
::   4. Shows the one manual step left (open project in Godot, it works)
::
:: DEPENDENCIES:
::   None. The .cmd uses Windows' built-in PowerShell to run scripts/quickinstall.ps1
::   from the gool repo. The addon archive ships gool's runtime as a static-linked
::   DLL — no Visual Studio, no CMake, no godot-cpp needed.
::
:: TROUBLESHOOTING:
::   * "project.godot not found" → you ran the .cmd from outside your Godot project.
::     Move it into the project folder and double-click again.
::   * Windows SmartScreen warning → click "More info" then "Run anyway". This file
::     is unsigned (signing certificates cost money for a free open-source project).
::   * No internet → the .cmd needs to reach GitHub. Offline install isn't supported
::     by this file; use the manual addon download from the Releases page instead.
:: ============================================================================

setlocal enableextensions enabledelayedexpansion
title gool installer

:: Switch CWD to the .cmd's own directory so double-clicking works regardless of
:: how Windows invoked it (shell vs. Explorer can differ).
cd /d "%~dp0"

echo.
echo  ==============================================================
echo    gool one-click installer for Windows
echo    https://github.com/siliconight/gool
echo  ==============================================================
echo.

:: ---------------------------------------------------------------------------
:: Step 1: validate we're in a Godot project
:: ---------------------------------------------------------------------------

if not exist "project.godot" (
    echo  [ ERROR ] project.godot not found in this folder.
    echo.
    echo  This file must live INSIDE your Godot project — the folder
    echo  that contains project.godot. Move gool-install.cmd into that
    echo  folder and double-click it again.
    echo.
    echo  Current folder:
    echo    %CD%
    echo.
    echo  Press any key to close...
    pause >nul
    exit /b 1
)

echo  Godot project found:
echo    %CD%
echo.

:: ---------------------------------------------------------------------------
:: Step 2: run quickinstall.ps1 via iwr ^| iex
:: ---------------------------------------------------------------------------
::
:: We don't ship quickinstall.ps1 alongside this .cmd (so the .cmd stays a
:: single-file install). Instead we pipe the latest version from main into iex.
:: This keeps the .cmd small and always current with bug fixes to the actual
:: installer logic.

echo  Downloading addon and installing...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference = 'Stop'; try { iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1 | iex } catch { Write-Host ''; Write-Host ('  [ ERROR ] ' + $_.Exception.Message) -ForegroundColor Red; exit 1 }"

if errorlevel 1 (
    echo.
    echo  ==============================================================
    echo    Installation failed.
    echo  ==============================================================
    echo.
    echo  See the messages above for what went wrong. Common causes:
    echo    * No internet connection
    echo    * GitHub temporarily unavailable
    echo    * Antivirus blocking the download
    echo.
    echo  Press any key to close...
    pause >nul
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: Step 3: success message + next steps
:: ---------------------------------------------------------------------------

echo.
echo  ==============================================================
echo    Done. gool is installed in addons\gool\
echo  ==============================================================
echo.
echo  NEXT STEPS:
echo.
echo    1. Close Godot if it's currently open with this project
echo    2. Open your project in Godot 4.2 or later
echo    3. The gool C++ classes are immediately available in scripts
echo       and the scene editor — no plugin enabling required
echo       ^(gool is a pure GDExtension, not an EditorPlugin^).
echo.
echo    Quick start:
echo      var runtime = AudioRuntime.new^(^)
echo      runtime.init^(^)
echo.
echo    Full docs: https://github.com/siliconight/gool#readme
echo.
echo  Press any key to close...
pause >nul
endlocal
