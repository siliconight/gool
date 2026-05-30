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

# scripts/apply_apache_headers.py
#
# v0.81.2: apply (or verify) the Apache 2.0 copyright header on every
# first-party source file in the gool repository.
#
# This script is the single source of truth for:
#   - Which files need the header (FILE_PATTERNS).
#   - What the header text is (HEADER_TEXT).
#   - Which comment style applies per file extension (COMMENT_STYLES).
#   - Where existing first-party code lives versus third-party code
#     (vendored libraries in third_party/ are NOT touched; they carry
#     their own licenses).
#
# WHY THIS EXISTS
# ===============
#
# Apache 2.0 recommends that each source file carry a 13-line
# boilerplate notice citing the project's copyright and the license
# under which the file is distributed. The LICENSE file in the repo
# root is legally sufficient on its own, but per-file headers:
#
#   - Survive being copied out of the repository (e.g. someone
#     grabbing a single .cpp to learn from it).
#   - Make the licensing clear to anyone reading any individual file.
#   - Are the convention for serious open-source projects in this
#     space (every major audio middleware / Apache-licensed C++
#     library does this).
#
# Pre-v0.81.2, gool didn't have per-file headers. The LICENSE file
# was correct (canonical Apache 2.0 + project copyright, drift-
# guarded by check_license_canonical.sh as of v0.80.24), but the
# per-file convention was missing. v0.81.2 closes that gap.
#
# USAGE
# =====
#
#   Apply headers (idempotent — files that already have the header
#   are left alone):
#
#       python3 scripts/apply_apache_headers.py --apply
#
#   CI / pre-commit verification (no writes; exits 1 if any matched
#   file is missing the header):
#
#       python3 scripts/apply_apache_headers.py --check
#
# DESIGN
# ======
#
#   - File selection: glob patterns rooted at the repository root.
#     Each pattern names the directory tree to walk and the
#     extensions to match. Adding a new source tree means adding one
#     entry to FILE_PATTERNS.
#
#   - Header detection: a file is considered to "already have the
#     header" if the first 30 lines (a generous window covering
#     shebangs, preceding comments, etc.) contain the literal
#     string "Copyright 2026 Brannen Graves". This is a tight
#     enough match that false positives are vanishingly unlikely
#     and false negatives basically can't happen (the year in the
#     copyright doesn't change mid-file). If the year rolls over,
#     update HEADER_TEXT and CHECK_MARKER together.
#
#   - Insertion point: at file start by default. For shebang files
#     (first line starts with "#!"), inserted at line 2 to preserve
#     the shebang.
#
#   - Comment style: derived from file extension. C++ uses "//",
#     shell/Python/GDScript/CMake use "#". Add new styles to
#     COMMENT_STYLES.

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Header content — the canonical Apache 2.0 boilerplate from the appendix
# of the license itself, with the project's copyright filled in. Lines
# are stored without comment markers; per-extension prefixes are applied
# at write time.
# ---------------------------------------------------------------------------
HEADER_LINES = [
    "Copyright 2026 Brannen Graves",
    "",
    'Licensed under the Apache License, Version 2.0 (the "License");',
    "you may not use this file except in compliance with the License.",
    "You may obtain a copy of the License at",
    "",
    "    http://www.apache.org/licenses/LICENSE-2.0",
    "",
    "Unless required by applicable law or agreed to in writing, software",
    'distributed under the License is distributed on an "AS IS" BASIS,',
    "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or",
    "implied. See the License for the specific language governing permissions",
    "and limitations under the License.",
]

# String we grep for to decide if a file already has the header. Kept
# verbatim from HEADER_LINES[0] so the two can't drift.
CHECK_MARKER = "Copyright 2026 Brannen Graves"


# ---------------------------------------------------------------------------
# Comment styles per file extension. Add new extensions here.
# ---------------------------------------------------------------------------
COMMENT_STYLES: dict[str, str] = {
    # C / C++ family
    ".cpp": "//",
    ".cc":  "//",
    ".c":   "//",
    ".h":   "//",
    ".hpp": "//",
    # Hash-comment family
    ".gd":  "#",   # GDScript
    ".py":  "#",
    ".sh":  "#",
    ".bash": "#",
}


# ---------------------------------------------------------------------------
# File selection. Each entry is (relative-directory, file-extensions).
# The script recursively walks the directory and selects files whose
# extension is in the set. Directories not listed here are NOT scanned;
# this is the include-list, not a filter-from-everything approach.
#
# Notably EXCLUDED:
#   - third_party/        — vendored libraries with their own licenses
#   - build*/             — generated build trees
#   - .git/               — git internals
#   - examples/<template>/ scripts outside addons/ — those become the
#     end user's game code; we don't want our copyright on it
#   - CMakeLists.txt      — Apache doesn't recommend headers on build
#     configuration files; they're treated like project metadata
#   - LICENSE, README*, CHANGELOG, *.md — documentation, not source
#   - plugin.cfg, *.json, *.yml, *.toml — configuration files
# ---------------------------------------------------------------------------
FILE_PATTERNS: list[tuple[str, set[str]]] = [
    ("include/audio_engine",                          {".h", ".hpp"}),
    ("src/audio_engine",                              {".cpp", ".h", ".hpp"}),
    ("tests",                                         {".cpp", ".h"}),
    ("tools",                                         {".cpp", ".h"}),
    ("examples/cpp",                                  {".cpp", ".h"}),
    ("godot/addons/gool",                             {".gd"}),
    # Addon copies that ship inside example template projects. Same
    # code as godot/addons/gool/ but maintained as separate files;
    # they're still our code and should carry our header.
    ("examples/coop_shooter_template/addons/gool",    {".gd"}),
    ("examples/voice_chat/addons/gool",               {".gd"}),
    # Build / CI scripts. The .py here covers
    # scripts/check_addon_autoload_safety.py and any future Python
    # scripts under scripts/.
    ("scripts",                                       {".sh", ".py"}),
]


def comment_style_for(path: Path) -> str | None:
    """Return the comment prefix for `path` based on extension, or
    None if the extension is not in COMMENT_STYLES."""
    return COMMENT_STYLES.get(path.suffix)


def format_header(comment_prefix: str) -> str:
    """Render the header with the given comment prefix.

    Lines with content become `<prefix> <text>`; blank header lines
    become just `<prefix>` (the trailing space is trimmed to avoid
    creating trailing-whitespace lines, which most editors flag).
    """
    lines = []
    for text in HEADER_LINES:
        if text:
            lines.append(f"{comment_prefix} {text}")
        else:
            lines.append(comment_prefix)
    # One blank separator line after the header, before whatever
    # content follows.
    lines.append("")
    return "\n".join(lines) + "\n"


def file_already_has_header(path: Path) -> bool:
    """Detect whether `path` already has the Apache header.

    Reads the first 30 lines (covers shebangs + multi-line prefaces)
    and returns True if CHECK_MARKER appears anywhere in that window.
    """
    try:
        with path.open("r", encoding="utf-8") as f:
            head = "".join(line for line, _ in zip(f, range(30)))
    except (OSError, UnicodeDecodeError):
        # Unreadable file — treat as "has header" so we don't try to
        # modify it. apply mode will silently skip; check mode will
        # also pass (binary/unreadable files aren't our problem).
        return True
    return CHECK_MARKER in head


def insert_header(path: Path, comment_prefix: str) -> None:
    """Add the Apache header to `path` at its top, preserving shebang
    if present. No-op if the header is already present (defensive;
    callers should check first)."""
    if file_already_has_header(path):
        return

    header_block = format_header(comment_prefix)

    with path.open("r", encoding="utf-8") as f:
        original = f.read()

    # Shebang preservation: if the file begins with "#!", emit the
    # shebang line as-is, then the header, then the rest.
    if original.startswith("#!"):
        first_newline = original.find("\n")
        if first_newline == -1:
            # File is a single shebang line with no newline; append
            # newline, header, done.
            new_content = original + "\n" + header_block
        else:
            shebang = original[: first_newline + 1]
            rest = original[first_newline + 1 :]
            new_content = shebang + header_block + rest
    else:
        new_content = header_block + original

    with path.open("w", encoding="utf-8") as f:
        f.write(new_content)


def iter_target_files(repo_root: Path):
    """Yield (Path, comment_prefix) for every file under FILE_PATTERNS
    whose extension is registered. Skips files whose comment style is
    unknown (defensive; shouldn't happen given the FILE_PATTERNS
    extension restrictions)."""
    for rel_dir, extensions in FILE_PATTERNS:
        root = repo_root / rel_dir
        if not root.exists():
            # Directory not in this checkout (e.g. examples shipped
            # only in release tarballs). Skip silently rather than
            # erroring; the inclusion list is meant to be a superset
            # of what any given checkout has.
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix not in extensions:
                continue
            prefix = comment_style_for(path)
            if prefix is None:
                continue
            yield path, prefix


def cmd_apply(repo_root: Path) -> int:
    """Apply the header to every matched file that doesn't already
    have one. Prints what was modified. Returns 0."""
    modified = 0
    skipped = 0
    for path, prefix in iter_target_files(repo_root):
        if file_already_has_header(path):
            skipped += 1
            continue
        insert_header(path, prefix)
        modified += 1
        rel = path.relative_to(repo_root)
        print(f"  added header: {rel}")
    total = modified + skipped
    print()
    print(f"Result: {modified} files modified, {skipped} already had "
          f"the header ({total} files checked).")
    return 0


def cmd_check(repo_root: Path) -> int:
    """Verify every matched file has the header. Prints any files
    that don't. Returns 0 on full coverage, 1 on any miss."""
    missing: list[Path] = []
    total = 0
    for path, _prefix in iter_target_files(repo_root):
        total += 1
        if not file_already_has_header(path):
            missing.append(path)

    if missing:
        print("ERROR: the following files are missing the Apache 2.0 header:")
        for path in missing:
            print(f"  {path.relative_to(repo_root)}")
        print()
        print(f"To fix:  python3 scripts/apply_apache_headers.py --apply")
        return 1

    print(f"OK: all {total} matched first-party source files carry the "
          f"Apache 2.0 header.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply or verify the Apache 2.0 source-file "
                    "header across first-party gool sources."
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--apply", action="store_true",
        help="Insert the header in any matched file that doesn't "
             "already have it. Idempotent."
    )
    mode.add_argument(
        "--check", action="store_true",
        help="Read-only. Exits 1 if any matched file is missing "
             "the header. CI-friendly."
    )
    args = parser.parse_args()

    # Repository root: one level up from this script.
    repo_root = Path(__file__).resolve().parent.parent

    if args.apply:
        return cmd_apply(repo_root)
    else:
        return cmd_check(repo_root)


if __name__ == "__main__":
    sys.exit(main())
