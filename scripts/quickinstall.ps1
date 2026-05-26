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
# Helpers
# ----------------------------------------------------------------------

# Test whether a file is currently locked by another process (typically
# Windows file-system semantics — a DLL that's mapped into a running
# .exe can't be deleted or overwritten). Used to detect the most common
# install-time failure: trying to upgrade gool while Godot is open
# with the target project, which keeps gool_godot.dll loaded.
#
# Returns $true if the file exists AND is currently locked.
# Returns $false if the file doesn't exist or can be opened RW.
function Test-FileLocked {
    param([string]$Path)
    if (-not (Test-Path -PathType Leaf $Path)) {
        return $false
    }
    try {
        # Open in ReadWrite mode with FileShare.None — this fails fast
        # if any other process has the file mapped or open. We close
        # immediately if it succeeds; no actual write happens.
        $stream = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $stream.Close()
        return $false
    } catch [System.IO.IOException] {
        return $true
    } catch {
        # Some other error (e.g. permissions on the path itself, not
        # process-level locking). Don't treat as "locked"; let the
        # real install attempt surface a more specific error.
        return $false
    }
}

# Print the "close Godot first" guidance with consistent formatting.
# Used by both the pre-flight check and the install-time catch.
function Write-GodotLockedError {
    param([string]$LockedPath, [string]$OriginalError = "")
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host "  Godot appears to be running with this project open" -ForegroundColor Red
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "The gool GDExtension binary at:"
    Write-Host "  $LockedPath" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "is currently locked, which on Windows means a running"
    Write-Host "Godot editor has it loaded. Windows won't let the installer"
    Write-Host "replace a DLL that's mapped into a running process."
    Write-Host ""
    Write-Host "How to fix:" -ForegroundColor Cyan
    Write-Host "  1. Save your work in Godot if there are unsaved changes"
    Write-Host "  2. Close Godot completely (fully quit the editor, not"
    Write-Host "     just the project window)"
    Write-Host "  3. Wait a few seconds for the process to exit cleanly"
    Write-Host "  4. Re-run gool-install.cmd"
    Write-Host ""
    Write-Host "If you're sure Godot isn't open:" -ForegroundColor Cyan
    Write-Host "  - Check Task Manager for any leftover godot.exe or"
    Write-Host "    Godot_v*.exe processes and end them"
    Write-Host "  - Antivirus software occasionally locks recently-extracted"
    Write-Host "    DLLs while scanning; wait 30 seconds and retry"
    if ($OriginalError -ne "") {
        Write-Host ""
        Write-Host "Original Windows error message:" -ForegroundColor DarkGray
        Write-Host "  $OriginalError" -ForegroundColor DarkGray
    }
    Write-Host ""
}

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
# Pre-flight: is the existing gool DLL locked?
# ----------------------------------------------------------------------
#
# If the target project already has a gool install, and Godot is open
# with that project, the gool_godot.dll is mapped into Godot's process
# and Windows will refuse to overwrite it. We can detect this BEFORE
# downloading anything by trying to open the existing DLL for write
# access. If that fails with IOException, Godot is holding it; bail
# with a clear message instead of doing the download + extract dance
# and failing late.
#
# Skipped silently when there's no existing install (first-time
# install on a fresh project — nothing to be locked).

$ExistingBinary = Join-Path $ProjectPath "addons\gool\bin\gool_godot.dll"
if (Test-FileLocked $ExistingBinary) {
    Write-GodotLockedError -LockedPath $ExistingBinary
    exit 1
}

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
    try {
        Remove-Item $AddonDest -Recurse -Force -ErrorAction Stop
    } catch {
        # Most common failure: Godot was launched between the pre-flight
        # check and now, locking the gool DLL. We catch any failure with
        # "denied" / "in use" / "being used" in the message — Windows
        # phrases this several ways depending on locale and the exact
        # caller. Other unexpected errors fall through to rethrow with
        # the original message preserved.
        $msg = $_.Exception.Message
        if ($msg -match "denied|in use|being used|cannot access|locked") {
            Write-GodotLockedError -LockedPath $ExistingBinary -OriginalError $msg
            Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
            exit 1
        }
        # Not the Godot-locked pattern — surface the original error.
        Write-Host ""
        Write-Host "ERROR: failed to remove existing addons\gool\ directory." -ForegroundColor Red
        Write-Host "  $msg"
        Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
        exit 1
    }
}

New-Item -ItemType Directory -Force -Path (Split-Path $AddonDest -Parent) | Out-Null
try {
    Copy-Item $AddonSource $AddonDest -Recurse -Force -ErrorAction Stop
} catch {
    # Same Godot-locked diagnosis at the copy step — possible if a race
    # between Remove-Item and Copy-Item let Godot grab the DLL again
    # (rare, but worth handling cleanly).
    $msg = $_.Exception.Message
    if ($msg -match "denied|in use|being used|cannot access|locked") {
        Write-GodotLockedError -LockedPath $ExistingBinary -OriginalError $msg
        Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
        exit 1
    }
    Write-Host ""
    Write-Host "ERROR: failed to copy gool addon files into the project." -ForegroundColor Red
    Write-Host "  Destination: $AddonDest"
    Write-Host "  $msg"
    Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

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

# ----------------------------------------------------------------------
# v0.78.8: auto-enable the gool EditorPlugin in project.godot
# ----------------------------------------------------------------------
# Before v0.78.8, install only deployed files. The user then had to
# open the project (got errors), enable the plugin manually in Project
# Settings, restart, then re-open. With this step, project.godot is
# pre-configured so the user's first open is already correct: the
# plugin enables on load, registers the Gool/DialogueDirector/
# MultiplayerBridge autoloads, and scripts compile cleanly.
#
# The enable_plugin.ps1 helper is part of the addon itself (ships in
# addons/gool/tools/) so the logic versions with the addon. It's
# idempotent — safe to run twice, safe to run after a re-install.

$EnablePluginScript = Join-Path $ProjectPath "addons\gool\tools\enable_plugin.ps1"
if (Test-Path $EnablePluginScript) {
    Write-Host "Enabling gool plugin in project.godot..."
    & $EnablePluginScript -ProjectPath $ProjectPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [warn] enable_plugin.ps1 returned $LASTEXITCODE — " -ForegroundColor Yellow `
            -NoNewline
        Write-Host "you may need to enable the plugin manually in Godot." -ForegroundColor Yellow
    }
    Write-Host ""
} else {
    Write-Host "  [warn] enable_plugin.ps1 not found at $EnablePluginScript" -ForegroundColor Yellow
    Write-Host "         The plugin will need to be enabled manually:"
    Write-Host "         Project Settings -> Plugins -> gool -> Enable"
    Write-Host ""
}

# ----------------------------------------------------------------------
# v0.78.6: optional post-install verification
# ----------------------------------------------------------------------
# Look for Godot on PATH. If present, run a headless pass that loads
# the user's project, fires Gool.diagnose(), and exits 0/1. If Godot
# isn't on PATH or the verify pass fails, we surface it — but never
# block on it. The addon is already deployed.

$GodotCmd = Get-Command godot -ErrorAction SilentlyContinue
if ($null -eq $GodotCmd) {
    Write-Host "[skip] Godot is not on PATH — skipping post-install verification." -ForegroundColor Yellow
    Write-Host "       That's fine; just open the project in Godot manually."
    Write-Host ""
} else {
    Write-Host "Running headless verification (this takes a few seconds)..."
    Write-Host ""

    # --headless / Dummy audio driver / --quit-after as a hang safety
    # net. The verify scene quits with its own exit code well before
    # the 5-second timer fires under normal conditions.
    & godot --headless --audio-driver Dummy --quit-after 5 `
        --path $ProjectPath `
        res://addons/gool/tools/verify_install.tscn
    $VerifyExit = $LASTEXITCODE

    Write-Host ""
    if ($VerifyExit -eq 0) {
        Write-Host "[verify] PASSED - install is healthy." -ForegroundColor Green
    } else {
        Write-Host "[verify] FAILED with exit code $VerifyExit." -ForegroundColor Red
        Write-Host "         Review the diagnose output above - every fail line"
        Write-Host "         has a hint. The most common cause is the project"
        Write-Host "         not having an autoload entry for Gool yet - open"
        Write-Host "         Project Settings -> Autoload and add"
        Write-Host "         res://addons/gool/runtime_singleton.gd as 'Gool'."
    }
    Write-Host ""
}

Write-Host "One step left:"
Write-Host "  1. Open $ProjectPath in Godot 4.2 or later"
Write-Host ""
Write-Host "The gool EditorPlugin is already enabled (see step above),"
Write-Host "so the Gool autoload registers on first open. You can call"
Write-Host "Gool.play_3d() / Gool.set_rtpc() from any GDScript. The"
Write-Host "output panel should show '[gool] runtime initialized'."
Write-Host ""
