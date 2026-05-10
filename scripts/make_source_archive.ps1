# scripts\make_source_archive.ps1
#
# Produce a clean source archive of the gool repo, named with the
# project name + version. PowerShell variant of make_source_archive.sh.
#
# Uses tar.exe, which ships with Windows 10 1803+ and Windows 11. If
# tar is not on PATH (older Windows), this script falls back to
# Compress-Archive, which produces a .zip with the same naming
# convention.
#
# Usage:
#     scripts\make_source_archive.ps1
#     scripts\make_source_archive.ps1 -Output C:\path\to\custom.tar.gz

[CmdletBinding()]
param(
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..")).Path

# ----------------------------------------------------------------------
# Read version from version.h.
# ----------------------------------------------------------------------

$VersionHeader = Join-Path $RepoRoot "include\audio_engine\version.h"
if (-not (Test-Path $VersionHeader)) {
    Write-Error "version.h not found at $VersionHeader. Are you running this from a gool checkout?"
    exit 1
}

$VersionLine = Get-Content $VersionHeader |
    Where-Object { $_ -match 'kVersionString\s*=\s*"([^"]+)"' } |
    Select-Object -First 1
if (-not $VersionLine) {
    Write-Error "Failed to parse kVersionString from $VersionHeader"
    exit 1
}
if ($VersionLine -notmatch '"([^"]+)"') {
    Write-Error "Failed to extract version string from line: $VersionLine"
    exit 1
}
$Version = $Matches[1]

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$') {
    Write-Error "Version '$Version' from $VersionHeader doesn't match X.Y.Z[-suffix]"
    exit 1
}

$ArchivePrefix = "gool-$Version"

# ----------------------------------------------------------------------
# Determine output path + format.
# ----------------------------------------------------------------------

$DistDir = Join-Path $RepoRoot "dist"
$UseTar = $true

if ($Output -eq "") {
    if (Get-Command tar -ErrorAction SilentlyContinue) {
        $Output = Join-Path $DistDir "$ArchivePrefix.tar.gz"
    } else {
        # Fall back to .zip via Compress-Archive.
        $Output = Join-Path $DistDir "$ArchivePrefix.zip"
        $UseTar = $false
    }
}

# Detect format from extension if user supplied an output path.
if ($Output -match '\.zip$') { $UseTar = $false }
if ($Output -match '\.tar\.gz$|\.tgz$') { $UseTar = $true }

$OutputDir = Split-Path -Parent $Output
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
}

# ----------------------------------------------------------------------
# Stage the files into a temp directory under the version-stamped name,
# then archive that directory. This is more verbose than tar's
# --transform but works identically with both tar.exe and
# Compress-Archive.
# ----------------------------------------------------------------------

$StagingRoot = Join-Path $env:TEMP "gool-stage-$([System.Guid]::NewGuid().ToString('N'))"
$StagingDir  = Join-Path $StagingRoot $ArchivePrefix
New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null

# Copy everything except the excluded directories / patterns. We use
# robocopy in mirror mode for the bulk-copy because it handles long
# paths and exclusions better than Copy-Item.
$ExcludeDirs = @(
    ".git", "build", "out", "third_party", "dist",
    "cmake-build-debug", "cmake-build-release", "__pycache__"
)
# Globbed build dirs (build-*) need explicit listing.
foreach ($Dir in (Get-ChildItem $RepoRoot -Directory -Filter "build-*" -ErrorAction SilentlyContinue)) {
    $ExcludeDirs += $Dir.Name
}

$ExcludeFiles = @(
    "*.o", "*.obj", "*.a", "*.lib", "*.so", "*.so.*",
    "*.dylib", "*.dll", "*.exe", "*.pdb",
    "*.pyc", "*.pyo",
    ".DS_Store", "Thumbs.db",
    "*.swp", "*.swo"
)

$RoboArgs = @(
    $RepoRoot, $StagingDir, "/E", "/NFL", "/NDL", "/NP", "/NJH", "/NJS"
)
foreach ($D in $ExcludeDirs) { $RoboArgs += "/XD"; $RoboArgs += (Join-Path $RepoRoot $D) }
foreach ($F in $ExcludeFiles) { $RoboArgs += "/XF"; $RoboArgs += $F }

# robocopy returns 0-7 for success (any non-zero in 0-7 just means files
# were copied / mismatched; 8+ is a real error).
& robocopy @RoboArgs | Out-Null
if ($LASTEXITCODE -gt 7) {
    Write-Error "robocopy failed with exit code $LASTEXITCODE"
    Remove-Item $StagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    exit 1
}

# ----------------------------------------------------------------------
# Pack the staged directory.
# ----------------------------------------------------------------------

if ($UseTar) {
    Push-Location $StagingRoot
    try {
        & tar -czf $Output $ArchivePrefix
        if ($LASTEXITCODE -ne 0) {
            Write-Error "tar failed with exit code $LASTEXITCODE"
            exit 1
        }
    } finally {
        Pop-Location
    }
} else {
    if (Test-Path $Output) { Remove-Item $Output -Force }
    Compress-Archive -Path $StagingDir -DestinationPath $Output
}

Remove-Item $StagingRoot -Recurse -Force -ErrorAction SilentlyContinue

# ----------------------------------------------------------------------
# Report.
# ----------------------------------------------------------------------

$SizeBytes = (Get-Item $Output).Length
$SizeKB = [math]::Round($SizeBytes / 1024)

Write-Host ""
Write-Host "Wrote source archive:"
Write-Host "  $Output ($SizeKB KB)"
Write-Host ""
Write-Host "When extracted, produces:"
Write-Host "  $ArchivePrefix\"
Write-Host ""

$Hash = Get-FileHash -Algorithm MD5 $Output
Write-Host "MD5: $($Hash.Hash.ToLower())"
