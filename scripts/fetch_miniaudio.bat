@echo off
REM Fetch miniaudio.h from upstream GitHub into third_party\miniaudio\.
REM
REM Usage:
REM     scripts\fetch_miniaudio.bat            (latest from master)
REM     scripts\fetch_miniaudio.bat 0.11.21    (specific tag)

setlocal

set "SCRIPT_DIR=%~dp0"
set "DEST_DIR=%SCRIPT_DIR%..\third_party\miniaudio"
set "DEST_FILE=%DEST_DIR%\miniaudio.h"

if "%~1"=="" (set "REF=master") else (set "REF=%~1")
set "URL=https://raw.githubusercontent.com/mackron/miniaudio/%REF%/miniaudio.h"

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
echo Downloaded miniaudio.h (ref=%REF%) to %DEST_FILE%
endlocal
exit /b 0

:failed
echo Failed to download miniaudio.h. Please download %URL% manually
echo and save it to %DEST_FILE%.
endlocal
exit /b 1
