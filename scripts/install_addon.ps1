# scripts\install_addon.ps1
#
# Install gool as a Godot addon into a target project. PowerShell
# variant of scripts/install_addon.sh.
#
# Usage:
#     scripts\install_addon.ps1 -TargetProject C:\path\to\my_godot_project
#     scripts\install_addon.ps1 -TargetProject C:\MyGame -Binary C:\custom\gool_godot.dll
#
# The default binary location is build-godot\Release\gool_godot.dll
# (the conventional MSBuild Release output of cmake -S godot).
#
# What it copies (same as install_addon.sh):
#   FROM gool repo                          TO <target>\addons\gool\
#   godot\addons\gool\*.gd                  *.gd
#   godot\addons\gool\plugin.cfg            plugin.cfg
#   godot\addons\gool\prefabs\              prefabs\
#   godot\gool.gdextension                  gool.gdextension
#   <built binary>                          bin\gool_godot.dll

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$TargetProject,

    [string]$Binary = ""
)

$ErrorActionPreference = "Stop"

# Resolve repo root from this script's location.
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..")

# Validate target.
if (-not (Test-Path -PathType Container $TargetProject)) {
    Write-Error "Target directory does not exist: $TargetProject"
    exit 1
}
if (-not (Test-Path (Join-Path $TargetProject "project.godot"))) {
    Write-Error "$TargetProject does not look like a Godot project (no project.godot file)"
    exit 1
}

# Default binary location.
if ($Binary -eq "") {
    $Binary = Join-Path $RepoRoot "build-godot\Release\gool_godot.dll"
}

if (-not (Test-Path -PathType Leaf $Binary)) {
    Write-Error @"
GDExtension binary not found at: $Binary

Build it first with scripts\bootstrap.ps1, or pass an explicit path:
  scripts\install_addon.ps1 -TargetProject $TargetProject -Binary C:\path\to\gool_godot.dll
"@
    exit 1
}

$Dest = Join-Path $TargetProject "addons\gool"
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "bin")     | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Dest "prefabs") | Out-Null

# Copy addon GDScript files.
Write-Host "Copying addon files into $Dest\..."
$Source = Join-Path $RepoRoot "godot\addons\gool"
Copy-Item (Join-Path $Source "runtime_singleton.gd")      $Dest -Force
Copy-Item (Join-Path $Source "audio_relevancy_filter.gd") $Dest -Force
Copy-Item (Join-Path $Source "plugin.gd")                 $Dest -Force
Copy-Item (Join-Path $Source "plugin.cfg")                $Dest -Force
Copy-Item (Join-Path $Source "prefabs\*") (Join-Path $Dest "prefabs") -Recurse -Force

# gdextension manifest lives one level up in the repo source.
Copy-Item (Join-Path $RepoRoot "godot\gool.gdextension")  $Dest -Force

# Copy the built binary.
Write-Host "Copying binary $(Split-Path -Leaf $Binary) into $Dest\bin\..."
Copy-Item $Binary (Join-Path $Dest "bin") -Force

Write-Host @"

Done. gool is installed at:
  $Dest

Next steps:
  1. Open $TargetProject in Godot 4.2 or later.
  2. Project Settings -> Plugins -> gool -> Enable.
  3. Verify the Gool autoload appears in Project Settings -> Autoload.

To verify it loaded, watch the output panel for:
  [gool] runtime initialized

If anything fails, see SETUP.md's Troubleshooting section.
"@
