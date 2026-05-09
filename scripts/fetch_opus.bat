@echo off
rem Fetch libopus source into third_party\opus\. Windows counterpart of
rem fetch_opus.sh. Override the ref by setting OPUS_REF in the environment
rem before running.

setlocal

if "%OPUS_REF%"=="" set OPUS_REF=master

set "REPO_ROOT=%~dp0.."
set "DEST=%REPO_ROOT%\third_party\opus"

if exist "%DEST%\CMakeLists.txt" (
    echo libopus appears to already be present at %DEST%.
    echo Remove or update it manually if you want a fresh clone.
    exit /b 0
)

echo Cloning xiph/opus (%OPUS_REF%) into %DEST%...
if not exist "%DEST%" mkdir "%DEST%"
git clone --depth 1 --branch "%OPUS_REF%" https://github.com/xiph/opus.git "%DEST%"
if errorlevel 1 (
    echo git clone failed.
    exit /b 1
)

echo Done.
echo Configure with -DAUDIO_ENGINE_VOICE_OPUS=ON to enable the codec.

endlocal
