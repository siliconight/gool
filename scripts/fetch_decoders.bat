@echo off
REM Fetch single-header decoder libraries from upstream GitHub:
REM
REM   third_party\dr_libs\dr_wav.h    (mackron/dr_libs)
REM   third_party\dr_libs\dr_flac.h   (mackron/dr_libs)
REM   third_party\stb\stb_vorbis.c    (nothings/stb)
REM
REM Usage:
REM     scripts\fetch_decoders.bat                       (latest from each repo)
REM     scripts\fetch_decoders.bat <drlibsref> <stbref>  (pinned refs)

setlocal

set "SCRIPT_DIR=%~dp0"
set "DR_DIR=%SCRIPT_DIR%..\third_party\dr_libs"
set "STB_DIR=%SCRIPT_DIR%..\third_party\stb"

if "%~1"=="" (set "DRLIBS_REF=master") else (set "DRLIBS_REF=%~1")
if "%~2"=="" (set "STB_REF=master") else (set "STB_REF=%~2")

if not exist "%DR_DIR%"  mkdir "%DR_DIR%"
if not exist "%STB_DIR%" mkdir "%STB_DIR%"

call :fetch "https://raw.githubusercontent.com/mackron/dr_libs/%DRLIBS_REF%/dr_wav.h"  "%DR_DIR%\dr_wav.h"   || goto :failed
call :fetch "https://raw.githubusercontent.com/mackron/dr_libs/%DRLIBS_REF%/dr_flac.h" "%DR_DIR%\dr_flac.h"  || goto :failed
call :fetch "https://raw.githubusercontent.com/nothings/stb/%STB_REF%/stb_vorbis.c"    "%STB_DIR%\stb_vorbis.c" || goto :failed

echo.
echo Downloaded:
echo   dr_wav.h     (ref=%DRLIBS_REF%)  to  %DR_DIR%\dr_wav.h
echo   dr_flac.h    (ref=%DRLIBS_REF%)  to  %DR_DIR%\dr_flac.h
echo   stb_vorbis.c (ref=%STB_REF%)     to  %STB_DIR%\stb_vorbis.c
endlocal
exit /b 0

:fetch
set "URL=%~1"
set "DEST=%~2"
echo Fetching %URL%
where curl >nul 2>&1
if %ERRORLEVEL%==0 (
    curl --fail --show-error --location --output "%DEST%" "%URL%"
    exit /b %ERRORLEVEL%
)
powershell -NoProfile -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%DEST%'"
exit /b %ERRORLEVEL%

:failed
echo Failed to download one or more decoder headers. Please download the
echo files manually from the URLs above into %DR_DIR% and %STB_DIR%.
endlocal
exit /b 1
