# addons/gool/tools/enable_plugin.ps1
#
# v0.78.8 — Idempotent project.godot editor.
#
# Adds res://addons/gool/plugin.cfg to the [editor_plugins]/enabled
# PackedStringArray in a user's project.godot file. Run by the install
# scripts (gool-install.cmd, quickinstall.ps1) immediately after the
# addon files are deployed, so the user's first project open already
# has the EditorPlugin active — which is what registers the Gool /
# DialogueDirector / MultiplayerBridge autoloads. Without this step,
# the user has to: open the project (gets errors), enable the plugin
# in Project Settings, restart Godot. With this step, first open is
# already in the working state.
#
# Handles five cases:
#   1. project.godot has no [editor_plugins] section          -> create it
#   2. Section exists, no enabled= line                       -> add it
#   3. Section exists, enabled=PackedStringArray()  (empty)   -> populate
#   4. Section exists, enabled=PackedStringArray("other_plugin") -> append
#   5. Section exists, our plugin already in the array        -> no-op
#
# Test coverage for these cases lives in the Python prototype that
# generated this script's logic (see CHANGELOG v0.78.8). The behavior
# was verified against six fixture project.godot files before the
# translation; the PowerShell regex semantics match Python's for the
# patterns used here.

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$ProjectPath
)

$ErrorActionPreference = "Stop"

$PluginPath = "res://addons/gool/plugin.cfg"
$ProjectGodot = Join-Path $ProjectPath "project.godot"

if (-not (Test-Path $ProjectGodot)) {
    Write-Host "  [skip] project.godot not found at $ProjectGodot" -ForegroundColor Yellow
    exit 1
}

# Read as a single string so multiline regex works predictably.
$Content = Get-Content -Raw -Path $ProjectGodot

# Case 5 (idempotency): if our plugin path appears inside any
# PackedStringArray after `enabled=`, do nothing. Match is loose
# enough to handle "gool/plugin.cfg" appearing among other entries.
if ($Content -match 'enabled\s*=\s*PackedStringArray\([^)]*addons/gool/plugin\.cfg[^)]*\)') {
    Write-Host "  [ok] gool plugin already enabled in project.godot (no change)" -ForegroundColor Green
    exit 0
}

# Cases 3+4 (section + enabled line exist): branch on empty vs populated.
$EnabledLineRegex = 'enabled\s*=\s*PackedStringArray\(([^)]*)\)'
if ($Content -match $EnabledLineRegex) {
    $ExistingEntries = $Matches[1].Trim()
    if ($ExistingEntries -eq "") {
        # Case 3: empty array. Replace with our entry.
        $NewLine = "enabled=PackedStringArray(`"$PluginPath`")"
        $Content = $Content -replace $EnabledLineRegex, $NewLine
        Write-Host "  [ok] gool plugin added to [editor_plugins] (was empty)" -ForegroundColor Green
    } else {
        # Case 4: append to existing array. Escape quotes inside the
        # replacement string carefully; PowerShell's -replace operator
        # treats $1, $2, etc. as backreferences, so we don't use them
        # here and rebuild the line by hand.
        $NewLine = "enabled=PackedStringArray($ExistingEntries, `"$PluginPath`")"
        $Content = $Content -replace $EnabledLineRegex, $NewLine
        Write-Host "  [ok] gool plugin appended to existing [editor_plugins] list" -ForegroundColor Green
    }
} elseif ($Content -match '(?m)^\[editor_plugins\]') {
    # Case 2: section exists but no enabled= line. Insert one right
    # after the section header. ${1} is the header-with-newline match.
    $Content = $Content -replace `
        '(?m)^(\[editor_plugins\]\r?\n)', `
        "`${1}enabled=PackedStringArray(`"$PluginPath`")`n"
    Write-Host "  [ok] gool plugin added to existing [editor_plugins] section" -ForegroundColor Green
} else {
    # Case 1: no section. Append at end. Ensure file ends with newline
    # before adding our block so we don't run-on into prior content.
    if (-not $Content.EndsWith("`n")) { $Content += "`n" }
    $Content += "`n[editor_plugins]`n`nenabled=PackedStringArray(`"$PluginPath`")`n"
    Write-Host "  [ok] gool plugin enabled (created [editor_plugins] section)" -ForegroundColor Green
}

# Write back. -NoNewline preserves whatever trailing-newline shape
# the user's file had (we manage that explicitly above).
Set-Content -Path $ProjectGodot -Value $Content -NoNewline
exit 0
