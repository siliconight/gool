@echo off
REM Fetch godot-cpp into third_party\godot-cpp\ via shallow git clone.
REM
REM godot-cpp is the C++ binding library for Godot's GDExtension API.
REM See scripts\fetch_godot_cpp.sh for full notes.
REM
REM Usage:
REM     scripts\fetch_godot_cpp.bat            (default branch 4.2)
REM     scripts\fetch_godot_cpp.bat 4.3        (specific branch)
REM     set GODOT_CPP_REF=4.3 ^&^& scripts\fetch_godot_cpp.bat   (via env)
REM
REM Idempotent: if third_party\godot-cpp\.git already exists, the
REM script skips the clone. To re-fetch, delete the directory first.

setlocal

set "SCRIPT_DIR=%~dp0"
set "DEST=%SCRIPT_DIR%..\third_party\godot-cpp"

if not "%~1"=="" (
    set "REF=%~1"
) else if defined GODOT_CPP_REF (
    set "REF=%GODOT_CPP_REF%"
) else (
    set "REF=4.2"
)

if exist "%DEST%\.git" (
    echo godot-cpp already present at %DEST% (has .git directory^).
    echo To re-fetch, delete %DEST% and re-run this script.
    endlocal
    exit /b 0
)

where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: git is not installed. Install it first:                         >&2
    echo   winget install Git.Git                                              >&2
    echo   choco install -y git                                                >&2
    endlocal
    exit /b 1
)

if not exist "%SCRIPT_DIR%..\third_party" mkdir "%SCRIPT_DIR%..\third_party"

echo Cloning godot-cpp (branch=%REF%^) into %DEST%...
git clone --depth 1 --branch "%REF%" https://github.com/godotengine/godot-cpp.git "%DEST%"
if errorlevel 1 goto :failed

echo.
echo Done. godot-cpp is at %DEST%.
echo Next: build it with scons, e.g.
echo   pushd "%DEST%" ^&^& scons platform=windows target=template_release -j%%NUMBER_OF_PROCESSORS%% ^&^& popd
echo Or run scripts\bootstrap.ps1 to do everything in one step.
endlocal
exit /b 0

:failed
echo Failed to clone godot-cpp. Check your network and the branch name (%REF%^).
endlocal
exit /b 1
