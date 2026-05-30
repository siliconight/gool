#!/usr/bin/env python3
# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

"""
check_scene_references.py — walk every .tscn and .tres file in the
canonical addon, extract all ext_resource paths, and verify each
referenced file exists on disk.

WHY THIS EXISTS
===============

v0.81.15 discovered a bug class the existing scanners missed:
`godot/addons/gool/templates/quickstart_3d.tscn` referenced
`res://addons/gool/templates/test_beep.wav`, but the .wav file
had been silently excluded from git by a blanket `*.wav` rule
in .gitignore. The file existed in every dev's local working
copy (it was created when the addon was authored), but git
never tracked it, so:

  - It never shipped in release archives (CI checkouts didn't
    have it)
  - Users installing via quickinstall.ps1 got a broken templates
    directory
  - quickstart_3d.tscn fired "Load failed: missing dependencies"
    the moment Godot tried to load it
  - This had been broken across many releases without anyone
    noticing, because the addon's developers all had local copies

None of the existing scanners caught this:

  - addon-drift scanner: comparing git-tracked files only; both
    canonical and examples saw the .wav as nonexistent (because
    git ignored it).
  - apache-headers: skips binary files anyway.
  - autoload-safety: scans .gd only, not .tscn references.

This scanner catches the general case: any scene file in the
addon that references an asset that isn't on disk fails. Run
in CI on every push; catches new instances of the bug class
before they ship.

WHAT IT CHECKS
==============

For each .tscn / .tres file under godot/addons/gool/:
  - Find every [ext_resource ... path="res://..."] line
  - Convert the res:// path to a filesystem path relative to repo root
  - Verify the target file exists on disk
  - Report any missing references with file:line:reference

Currently checks the canonical addon only. The v0.81.13
addon-drift scanner ensures example addons mirror canonical, so
if canonical's scenes are clean, examples' will be too.

USAGE
=====

    python3 scripts/check_scene_references.py     # CI mode
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CANONICAL = REPO_ROOT / "godot" / "addons" / "gool"

# For canonical addon's .tscn/.tres files, the implicit Godot project
# root is REPO_ROOT/godot/ (where gool.gdextension lives), NOT
# REPO_ROOT itself. The scenes reference paths like
#     res://addons/gool/templates/test_beep.wav
# which Godot would resolve relative to a project.godot. Canonical
# doesn't have its own project.godot, so we treat REPO_ROOT/godot/
# as that implicit root.
CANONICAL_PROJECT_ROOT = REPO_ROOT / "godot"

# Godot's ext_resource line shape (Godot 4 .tscn/.tres syntax):
#
#   [ext_resource type="AudioStream" path="res://path/to/file.wav" id="..."]
#
# Group 1 captures the res:// path. Tolerates variable attribute
# ordering and additional attributes; the path is always quoted.
_EXT_RESOURCE_RE = re.compile(
    r'\[ext_resource[^\]]*\bpath="(res://[^"]+)"'
)


def _scene_files(root: Path) -> list[Path]:
    """All .tscn / .tres files under root."""
    out: list[Path] = []
    for suffix in ("*.tscn", "*.tres"):
        out.extend(root.rglob(suffix))
    return sorted(out)


def _resolve_res_path(res_path: str) -> Path:
    """Convert a 'res://path/to/file' string to an absolute filesystem
    path. For canonical addon scenes, res:// resolves under
    CANONICAL_PROJECT_ROOT (REPO_ROOT/godot/), not the repo root."""
    if not res_path.startswith("res://"):
        # Shouldn't happen given the regex, but be defensive.
        return CANONICAL_PROJECT_ROOT / res_path
    return CANONICAL_PROJECT_ROOT / res_path[len("res://"):]


def check_scene_file(path: Path) -> list[tuple[int, str]]:
    """Returns list of (line_number, res_path) for missing references."""
    missing: list[tuple[int, str]] = []
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError) as e:
        # Binary or unreadable — skip with a soft warning.
        print(f"  warn: couldn't read {path.relative_to(REPO_ROOT)}: {e}",
              file=sys.stderr)
        return missing

    for line_num, line in enumerate(text.splitlines(), start=1):
        m = _EXT_RESOURCE_RE.search(line)
        if m is None:
            continue
        res_path = m.group(1)
        fs_path = _resolve_res_path(res_path)
        if not fs_path.exists():
            missing.append((line_num, res_path))
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Verify every ext_resource in addon scenes "
                    "resolves to an existing file on disk."
    )
    ap.parse_args()  # No flags currently; --check is the only mode.

    if not CANONICAL.is_dir():
        print(f"ERROR: canonical addon not found at {CANONICAL}",
              file=sys.stderr)
        return 2

    scenes = _scene_files(CANONICAL)
    print(f"Scanning {len(scenes)} scene/resource files under "
          f"{CANONICAL.relative_to(REPO_ROOT)}...")

    total_refs = 0
    total_missing = 0
    failing_files: list[Path] = []

    for path in scenes:
        missing = check_scene_file(path)
        # Count references in this file for the summary
        try:
            text = path.read_text(encoding="utf-8")
            total_refs += len(_EXT_RESOURCE_RE.findall(text))
        except (UnicodeDecodeError, OSError):
            pass
        if missing:
            failing_files.append(path)
            rel = path.relative_to(REPO_ROOT)
            print(f"\nFAIL: {rel}")
            for line_num, res_path in missing:
                print(f"  line {line_num}: ext_resource references "
                      f"'{res_path}' which doesn't exist on disk")
                # Translate to filesystem path for the user
                fs_path = _resolve_res_path(res_path)
                print(f"           (expected at "
                      f"{fs_path.relative_to(REPO_ROOT)})")
            total_missing += len(missing)

    if failing_files:
        print(f"\nFAIL: {len(failing_files)} scene file(s) reference "
              f"{total_missing} missing asset(s).")
        print("This usually means an asset file was deleted, renamed, "
              "or never tracked by git in the first place.")
        print("If the file exists locally but isn't tracked, check "
              ".gitignore — see v0.81.15 for the test_beep.wav "
              "precedent.")
        return 1

    print(f"OK: {total_refs} ext_resource references across "
          f"{len(scenes)} scene/resource files all resolve.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
