# scripts/quickinstall.ps1
#
# One-line install for the gool Godot addon. Downloads the latest
# release's prebuilt addon archive, extracts addons/gool/ into your
# Godot project. No C++ compiler, no CMake, no SCons, no godot-cpp.
#
# DESIGNED INVOCATION (Windows PowerShell):
#
#     # Run from inside your Godot project directory (the one with
#     # project.godot). One line, no checkout required:
#     iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1 | iex
#
# OR with arguments:
#
#     # Specify project path explicitly:
#     & ([scriptblock]::Create((iwr -useb "https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1").Content)) -ProjectPath C:\path\to\my_game
#
#     # Pin a specific version:
#     & ([scriptblock]::Create((iwr -useb "...").Content)) -Version v0.11.10
#
# Or if you've checked out the repo locally:
#
#     scripts\quickinstall.ps1 -ProjectPath C:\path\to\my_game
#
# What it does:
#   1. Verifies the target is a Godot project (has project.godot)
#   2. Resolves the latest release tag via the GitHub API
#   3. Downloads gool-X.Y.Z-godot-addon-windows-x86_64.zip
#   4. Extracts addons/gool/ into the project
#   5. Prints the one manual step left: enable the plugin in Godot

[CmdletBinding()]
param(
    [string]$ProjectPath = (Get-Location).Path,
    [string]$Version = "latest",
    [string]$Repo = "siliconight/gool"
)

$ErrorActionPreference = "Stop"

# ----------------------------------------------------------------------
# Banner
# ----------------------------------------------------------------------

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  gool — Godot addon quickinstaller (Windows)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# ----------------------------------------------------------------------
# Validate the target is a Godot project
# ----------------------------------------------------------------------

if (-not (Test-Path -PathType Container $ProjectPath)) {
    Write-Host "ERROR: $ProjectPath does not exist." -ForegroundColor Red
    Write-Host ""
    Write-Host "Create your Godot project first, then re-run from inside it:"
    Write-Host "  cd C:\path\to\my_game"
    Write-Host "  iwr -useb https://raw.githubusercontent.com/$Repo/main/scripts/quickinstall.ps1 | iex"
    exit 1
}

if (-not (Test-Path (Join-Path $ProjectPath "project.godot"))) {
    Write-Host "ERROR: $ProjectPath has no project.godot file." -ForegroundColor Red
    Write-Host ""
    Write-Host "Run this from inside a Godot project's directory, or pass -ProjectPath:"
    Write-Host "  cd C:\path\to\your_godot_project    # then re-run"
    Write-Host ""
    Write-Host "Or, if you don't have a project yet, create one in Godot first:"
    Write-Host "  open Godot -> New Project -> set folder to C:\path\to\my_game"
    exit 1
}

Write-Host "Target Godot project: $ProjectPath" -ForegroundColor Green

# ----------------------------------------------------------------------
# Resolve version
# ----------------------------------------------------------------------

if ($Version -eq "latest") {
    Write-Host "Resolving latest release..."
    try {
        $Release = Invoke-RestMethod -UseBasicParsing `
                                     -Uri "https://api.github.com/repos/$Repo/releases/latest"
        $Version = $Release.tag_name
    } catch {
        Write-Host "ERROR: failed to query the GitHub API." -ForegroundColor Red
        Write-Host "  $_"
        Write-Host ""
        Write-Host "Possible causes:"
        Write-Host "  - No network connection"
        Write-Host "  - GitHub API rate limit (60 unauthenticated req/hour). Try again in an hour."
        Write-Host "  - The repo $Repo doesn't exist or has no releases yet."
        Write-Host ""
        Write-Host "Workaround: pin a specific version with -Version v0.11.10."
        exit 1
    }
}

$VersionNumber = $Version -replace '^v', ''
Write-Host "Version: $Version" -ForegroundColor Green

# ----------------------------------------------------------------------
# Construct download URL
# ----------------------------------------------------------------------

# Default Windows runtime platform. Override via -Platform if you're on
# something exotic (e.g. ARM Windows, when we ship that).
$Platform = "windows-x86_64"
$Filename = "gool-$VersionNumber-godot-addon-$Platform.zip"
$DownloadUrl = "https://github.com/$Repo/releases/download/$Version/$Filename"

Write-Host "Downloading: $Filename"

# ----------------------------------------------------------------------
# Download to temp
# ----------------------------------------------------------------------

$TempDir = Join-Path $env:TEMP "gool-quickinstall-$([System.Guid]::NewGuid().ToString('N'))"
$TempZip = Join-Path $TempDir $Filename
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

try {
    $ProgressPreference = 'SilentlyContinue'  # speeds up Invoke-WebRequest dramatically
    Invoke-WebRequest -UseBasicParsing -Uri $DownloadUrl -OutFile $TempZip
} catch {
    Write-Host "ERROR: download failed." -ForegroundColor Red
    Write-Host "  URL:    $DownloadUrl"
    Write-Host "  Error:  $_"
    Write-Host ""
    Write-Host "Possible causes:"
    Write-Host "  - The release for $Version doesn't have an addon archive for $Platform yet."
    Write-Host "    Check https://github.com/$Repo/releases/tag/$Version manually."
    Write-Host "  - Network blocked / proxy required."
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# Sanity-check the download — addon archives are typically ~500 KB.
# Anything smaller than 50 KB is almost certainly a 404 page or a
# corrupted download.
$Size = (Get-Item $TempZip).Length
if ($Size -lt 50KB) {
    Write-Host "ERROR: downloaded file is suspiciously small ($Size bytes)." -ForegroundColor Red
    Write-Host "Likely the release exists but the addon archive doesn't, or the download was interrupted."
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}
$SizeMB = [math]::Round($Size / 1MB, 2)
Write-Host "Downloaded $SizeMB MB" -ForegroundColor Green

# ----------------------------------------------------------------------
# Extract + locate addons/gool inside the archive
# ----------------------------------------------------------------------

$TempExtract = Join-Path $TempDir "extracted"
Expand-Archive -Path $TempZip -DestinationPath $TempExtract -Force

# The archive contains gool-X.Y.Z-godot-addon-PLATFORM/addons/gool/
$ExtractedRoot = Get-ChildItem $TempExtract -Directory | Select-Object -First 1
if (-not $ExtractedRoot) {
    Write-Host "ERROR: extracted archive doesn't contain a top-level directory." -ForegroundColor Red
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}
$AddonSource = Join-Path $ExtractedRoot.FullName "addons\gool"
if (-not (Test-Path -PathType Container $AddonSource)) {
    Write-Host "ERROR: archive structure is unexpected — couldn't find addons\gool inside it." -ForegroundColor Red
    Write-Host "  Looked at: $AddonSource"
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# ----------------------------------------------------------------------
# Install (replacing existing addons/gool/ if present)
# ----------------------------------------------------------------------

$AddonDest = Join-Path $ProjectPath "addons\gool"
if (Test-Path $AddonDest) {
    Write-Host "Replacing existing addons\gool\ ..." -ForegroundColor Yellow
    Remove-Item $AddonDest -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Split-Path $AddonDest -Parent) | Out-Null
Copy-Item $AddonSource $AddonDest -Recurse -Force

# Cleanup
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

# ----------------------------------------------------------------------
# Done — print the one manual step left
# ----------------------------------------------------------------------

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  Installed gool $Version into $ProjectPath\addons\gool\" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "One step left:"
Write-Host "  1. Open $ProjectPath in Godot 4.2 or later"
Write-Host "  2. Project Settings -> Plugins -> gool -> Enable"
Write-Host ""
Write-Host "After enabling, the Gool autoload appears in Project Settings ->"
Write-Host "Autoload, and you can call Gool.play_3d() / Gool.set_rtpc() from"
Write-Host "any GDScript. The output panel should show '[gool] runtime initialized'."
Write-Host ""
