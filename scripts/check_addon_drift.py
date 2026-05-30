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
check_addon_drift.py — enforce that every example's addons/gool/ is a
perfect mirror of the canonical godot/addons/gool/.

WHY THIS EXISTS
===============

Through v0.81.12, gool shipped four addon copies in the repo:

    godot/addons/gool/                              ← canonical
    examples/coop_shooter_template/addons/gool/     ← copy
    examples/voice_chat/addons/gool/                ← copy
    examples/coop_4p_minimal/addons/gool/           ← copy

Every time a new script was added to canonical, the example copies
were SUPPOSED to be kept in sync. In practice, this was done manually
and the copies fell behind silently over time. By v0.81.12 the
examples had only ~10% of canonical's files (6–16 out of 109),
producing parse errors like "Could not find type 'GoolMixSnapshot'"
when running the example projects.

v0.81.13 ships a one-time sync (cp -a canonical/ → each example/)
and this scanner to enforce parity going forward. CI runs this in
--check mode on every push; if anyone adds a file to canonical
without syncing the examples (or vice versa), CI fails before merge.

MODES
=====

--check (default): Walks every file under canonical. For each one,
                   verifies it exists in each example and matches
                   byte-for-byte. Reports drift, exits non-zero if any.

--fix:             Re-mirrors canonical into each example. Useful for
                   the one-off case where you've intentionally
                   modified canonical and want to propagate.

USAGE
=====

    python3 scripts/check_addon_drift.py             # CI mode
    python3 scripts/check_addon_drift.py --fix       # local resync
"""

from __future__ import annotations

import argparse
import filecmp
import hashlib
import shutil
import sys
from pathlib import Path

# Project layout. All paths relative to the repo root, which is the
# script's parent's parent.
REPO_ROOT = Path(__file__).resolve().parent.parent
CANONICAL = REPO_ROOT / "godot" / "addons" / "gool"
EXAMPLES = [
    REPO_ROOT / "examples" / "coop_shooter_template" / "addons" / "gool",
    REPO_ROOT / "examples" / "voice_chat" / "addons" / "gool",
    REPO_ROOT / "examples" / "coop_4p_minimal" / "addons" / "gool",
]


def _hash_file(path: Path) -> str:
    """SHA-256 of a file's bytes. Used for content comparison reporting."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def _relative_files(root: Path) -> set[Path]:
    """All files under root, as paths relative to root."""
    return {
        p.relative_to(root)
        for p in root.rglob("*")
        if p.is_file()
    }


def check_one_example(example_path: Path) -> tuple[bool, list[str]]:
    """Returns (clean, messages). clean=True means perfect mirror."""
    messages: list[str] = []

    if not example_path.is_dir():
        messages.append(
            f"  MISSING: {example_path.relative_to(REPO_ROOT)} doesn't exist"
        )
        return False, messages

    canonical_files = _relative_files(CANONICAL)
    example_files = _relative_files(example_path)

    # Files in canonical but missing from this example.
    missing = sorted(canonical_files - example_files)
    for rel in missing:
        messages.append(f"  MISSING: {rel}")

    # Files in this example but not in canonical (orphaned).
    orphaned = sorted(example_files - canonical_files)
    for rel in orphaned:
        messages.append(f"  ORPHANED: {rel} (in example but not canonical)")

    # Files in both — compare content.
    shared = canonical_files & example_files
    for rel in sorted(shared):
        cpath = CANONICAL / rel
        epath = example_path / rel
        if not filecmp.cmp(cpath, epath, shallow=False):
            ch = _hash_file(cpath)
            eh = _hash_file(epath)
            messages.append(
                f"  CONTENT DRIFT: {rel} "
                f"(canonical={ch}, example={eh})"
            )

    return len(messages) == 0, messages


def fix_one_example(example_path: Path) -> None:
    """Re-mirror canonical into this example. Destructive but
    deterministic — after this, the example is byte-identical to
    canonical."""
    if example_path.exists():
        shutil.rmtree(example_path)
    shutil.copytree(CANONICAL, example_path)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Enforce parity between canonical addons/gool/ and "
                    "each example's copy."
    )
    ap.add_argument(
        "--fix",
        action="store_true",
        help="Auto-sync canonical → each example. Default is "
             "check-only (CI-friendly, exits non-zero on drift)."
    )
    args = ap.parse_args()

    if not CANONICAL.is_dir():
        print(f"ERROR: canonical addon not found at {CANONICAL}",
              file=sys.stderr)
        return 2

    print(f"Canonical: {CANONICAL.relative_to(REPO_ROOT)} "
          f"({len(_relative_files(CANONICAL))} files)")

    if args.fix:
        # Mirror canonical into each example.
        for example in EXAMPLES:
            rel = example.relative_to(REPO_ROOT)
            print(f"  fixing {rel} ...")
            fix_one_example(example)
        # Verify the fix took.
        print("  re-checking...")

    any_drift = False
    for example in EXAMPLES:
        rel = example.relative_to(REPO_ROOT)
        clean, messages = check_one_example(example)
        if clean:
            print(f"OK: {rel} mirrors canonical exactly.")
        else:
            any_drift = True
            print(f"DRIFT: {rel}")
            for m in messages:
                print(m)

    if any_drift:
        if args.fix:
            # If --fix didn't produce a clean state, something is
            # very wrong — perhaps a file is being written by
            # another process, or canonical has invalid characters.
            print("\nFAIL: drift persists after --fix. Investigate.",
                  file=sys.stderr)
        else:
            print(
                "\nFAIL: example addon copies differ from canonical.\n"
                "Run `python3 scripts/check_addon_drift.py --fix` to "
                "auto-sync, then commit the result.",
                file=sys.stderr
            )
        return 1

    print("\nOK: all example addon copies mirror canonical exactly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
