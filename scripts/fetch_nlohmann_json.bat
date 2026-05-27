@echo off
REM Fetch nlohmann/json.hpp from upstream GitHub into third_party\nlohmann\.
REM
REM Why nlohmann/json (v0.80.0): gool previously hand-rolled two JSON
REM parsers (bus_config_loader.cpp, sound_bank.cpp). Both shipped without
REM `\u`, `\b`, or `\f` escape handling — a non-spec-compliant gap that
REM rejected valid JSON. v0.80.0 replaces both with this single-header lib.
REM
REM Usage:
REM     scripts\fetch_nlohmann_json.bat            (pinned default v3.11.3)
REM     scripts\fetch_nlohmann_json.bat v3.11.3    (explicit pin)
REM     scripts\fetch_nlohmann_json.bat develop    (tip of upstream)

setlocal

set "SCRIPT_DIR=%~dp0"
set "DEST_DIR=%SCRIPT_DIR%..\third_party\nlohmann"
set "DEST_FILE=%DEST_DIR%\json.hpp"

if "%~1"=="" (set "REF=v3.11.3") else (set "REF=%~1")
set "URL=https://raw.githubusercontent.com/nlohmann/json/%REF%/single_include/nlohmann/json.hpp"

if not exist "%DEST_DIR%" mkdir "%DEST_DIR%"

echo Fetching %URL%

where curl >nul 2>&1
if %ERRORLEVEL%==0 (
    curl --fail --show-error --location --output "%DEST_FILE%" "%URL%"
    if errorlevel 1 goto :failed
    goto :ok
)

REM PowerShell fallback (available on every modern Windows).
powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%DEST_FILE%'"
if errorlevel 1 goto :failed

:ok
echo Downloaded nlohmann/json.hpp (ref=%REF%) to %DEST_FILE%
endlocal
exit /b 0

:failed
echo Failed to download nlohmann/json.hpp. Please download %URL% manually
echo and save it to %DEST_FILE%.
endlocal
exit /b 1
