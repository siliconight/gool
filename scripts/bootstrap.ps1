# scripts\bootstrap.ps1
#
# One-command setup for gool on Windows. PowerShell variant of
# scripts/bootstrap.sh — verifies prerequisites, fetches single-header
# deps, clones + builds godot-cpp, builds gool's GDExtension, and
# (optionally) installs the addon into a target Godot project.
#
# IMPORTANT: this script must be run from the "x64 Native Tools
# Command Prompt for VS 2022" (or equivalent — any shell where
# cl.exe and msbuild are on PATH). A regular PowerShell will fail at
# the cmake build step because MSVC isn't on PATH by default.
#
# Usage:
#     scripts\bootstrap.ps1
#     scripts\bootstrap.ps1 -InstallTo C:\path\to\my_godot_project
#     scripts\bootstrap.ps1 -GodotCppRef 4.3 -BuildType Debug
#
# Idempotent: every step checks if its work is already done.

[CmdletBinding()]
param(
    [string]$InstallTo    = "",
    [string]$GodotCppRef  = "4.2",
    [string]$BuildType    = "Release",
    [string]$GodotCppPath = "",
    [int]   $Jobs         = 0,
    [switch]$SkipGodotCpp
)

$ErrorActionPreference = "Stop"

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if ($GodotCppPath -eq "") {
    $GodotCppPath = Join-Path $RepoRoot "third_party\godot-cpp"
}

if ($Jobs -le 0) {
    $Jobs = $env:NUMBER_OF_PROCESSORS
    if (-not $Jobs) { $Jobs = 4 }
}

$Platform = "windows"

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

function Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Require-Cmd {
    param([string]$Name, [string]$Hint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Write-Host "ERROR: '$Name' is not on PATH." -ForegroundColor Red
        Write-Host "       $Hint"
        exit 1
    }
    Write-Host "  ok  $Name"
}

# ----------------------------------------------------------------------
# Step 1: prerequisite checks
# ----------------------------------------------------------------------

Step "Step 1/5: checking prerequisites"

Require-Cmd "git"     "Install: winget install Git.Git  (or: choco install -y git)"
Require-Cmd "cmake"   "Install: winget install Kitware.CMake  (or: choco install -y cmake)"
Require-Cmd "python"  "Install: winget install Python.Python.3.12  (Python 3.7+ required)"
Require-Cmd "scons"   "Install: pip install scons  (after Python is installed)"
Require-Cmd "cl"      "MSVC compiler not on PATH. Open the 'x64 Native Tools Command Prompt for VS 2022' from the Start menu and re-run this script. Or: winget install Microsoft.VisualStudio.2022.BuildTools  (with the C++ workload)."

# ----------------------------------------------------------------------
# Step 2: fetch single-header dependencies
# ----------------------------------------------------------------------

Step "Step 2/5: fetching single-header dependencies"

$Miniaudio = Join-Path $RepoRoot "third_party\miniaudio\miniaudio.h"
if (Test-Path $Miniaudio) {
    Write-Host "  ok  miniaudio.h already present"
} else {
    & cmd /c (Join-Path $ScriptDir "fetch_miniaudio.bat")
    if ($LASTEXITCODE -ne 0) { Write-Error "fetch_miniaudio.bat failed"; exit 1 }
}

$DrWav  = Join-Path $RepoRoot "third_party\dr_libs\dr_wav.h"
$DrFlac = Join-Path $RepoRoot "third_party\dr_libs\dr_flac.h"
$StbVrb = Join-Path $RepoRoot "third_party\stb\stb_vorbis.c"
if ((Test-Path $DrWav) -and (Test-Path $DrFlac) -and (Test-Path $StbVrb)) {
    Write-Host "  ok  decoder headers already present"
} else {
    & cmd /c (Join-Path $ScriptDir "fetch_decoders.bat")
    if ($LASTEXITCODE -ne 0) { Write-Error "fetch_decoders.bat failed"; exit 1 }
}

# ----------------------------------------------------------------------
# Step 3: clone + build godot-cpp
# ----------------------------------------------------------------------

if ($SkipGodotCpp) {
    Step "Step 3/5: skipping godot-cpp (-SkipGodotCpp)"
    if (-not (Test-Path $GodotCppPath)) {
        Write-Error "-SkipGodotCpp passed but GodotCppPath=$GodotCppPath doesn't exist."
        exit 1
    }
} else {
    Step "Step 3/5: cloning + building godot-cpp (branch=$GodotCppRef)"

    if (-not (Test-Path (Join-Path $GodotCppPath ".git"))) {
        $env:GODOT_CPP_REF = $GodotCppRef
        & cmd /c (Join-Path $ScriptDir "fetch_godot_cpp.bat")
        if ($LASTEXITCODE -ne 0) { Write-Error "fetch_godot_cpp.bat failed"; exit 1 }
    } else {
        Write-Host "  ok  godot-cpp already cloned at $GodotCppPath"
    }

    # Check if already built — any .lib in bin/ counts.
    $Built = Get-ChildItem -Path (Join-Path $GodotCppPath "bin") -Filter "libgodot-cpp*.lib" -ErrorAction SilentlyContinue
    if ($Built) {
        Write-Host "  ok  godot-cpp already built ($(($Built | Select-Object -First 1).Name))"
    } else {
        Write-Host "  building godot-cpp — this takes 5-20 minutes the first time..."
        Push-Location $GodotCppPath
        try {
            & scons platform=$Platform target=template_release "-j$Jobs"
            if ($LASTEXITCODE -ne 0) {
                Write-Error "scons build of godot-cpp failed"
                exit 1
            }
        } finally {
            Pop-Location
        }
    }
}

# ----------------------------------------------------------------------
# Step 4: build gool's GDExtension
# ----------------------------------------------------------------------

Step "Step 4/5: building gool's GDExtension binding"

$BuildDir = Join-Path $RepoRoot "build-godot"

& cmake -S (Join-Path $RepoRoot "godot") -B $BuildDir `
    "-DGODOT_CPP_PATH=$GodotCppPath" `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON" `
    "-DAUDIO_ENGINE_DECODERS_WAV=ON" `
    "-DAUDIO_ENGINE_DECODERS_OGG=ON" `
    "-DAUDIO_ENGINE_DECODERS_FLAC=ON"
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }

& cmake --build $BuildDir --config $BuildType "-j$Jobs"
if ($LASTEXITCODE -ne 0) { Write-Error "cmake build failed"; exit 1 }

# Find the produced .dll. Default MSBuild generator puts it under
# <BuildDir>\<BuildType>\gool_godot.dll. Single-config generators
# (Ninja) put it directly under <BuildDir>\gool_godot.dll.
$BinaryCandidates = @(
    (Join-Path $BuildDir "$BuildType\gool_godot.dll"),
    (Join-Path $BuildDir "gool_godot.dll")
)
$Binary = $null
foreach ($C in $BinaryCandidates) {
    if (Test-Path $C) { $Binary = $C; break }
}

if (-not $Binary) {
    Write-Error @"
Build completed but gool_godot.dll not found at expected paths:
$($BinaryCandidates | ForEach-Object { "  $_" } | Out-String)
Search the build directory manually:
  Get-ChildItem -Recurse -Path $BuildDir -Filter gool_godot.dll
"@
    exit 1
}

Write-Host ""
Write-Host "  ok  built $Binary"

# ----------------------------------------------------------------------
# Step 5: install into Godot project (if requested)
# ----------------------------------------------------------------------

if ($InstallTo -ne "") {
    Step "Step 5/5: installing into Godot project: $InstallTo"
    & (Join-Path $ScriptDir "install_addon.ps1") -TargetProject $InstallTo -Binary $Binary
} else {
    Step "Step 5/5: skipped (no -InstallTo specified)"

    Write-Host @"

To install the addon into a Godot project, run:

  scripts\install_addon.ps1 -TargetProject C:\path\to\your\project

Or re-run this script with -InstallTo:

  scripts\bootstrap.ps1 -InstallTo C:\path\to\your\project

The built binary is at:
  $Binary

"@
}

Step "Bootstrap complete."
