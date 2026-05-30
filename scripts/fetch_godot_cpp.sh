#!/usr/bin/env bash
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

# Fetch godot-cpp into third_party/godot-cpp/ via shallow git clone.
#
# godot-cpp is the C++ binding library for Godot's GDExtension API.
# It's ~100MB and tracks Godot's release cadence by branch — we don't
# vendor it because the bytes change every Godot release. Mirrors the
# pattern of scripts/fetch_opus.sh (which clones xiph/opus the same way).
#
# Usage:
#     ./scripts/fetch_godot_cpp.sh           # default branch (4.2 — matches
#                                             # compatibility_minimum in the
#                                             # gdextension manifest)
#     ./scripts/fetch_godot_cpp.sh 4.3       # specific branch
#     GODOT_CPP_REF=4.3 ./scripts/fetch_godot_cpp.sh   # via env var
#
# Idempotent: if third_party/godot-cpp/.git already exists, the script
# skips the clone (and prints a note). To re-fetch, delete the
# directory manually first.
#
# After fetching, you still need to BUILD godot-cpp before the gool
# GDExtension can link against it. scripts/bootstrap.sh does both;
# this script only handles the fetch step.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DEST="${SCRIPT_DIR}/../third_party/godot-cpp"

# Precedence: positional arg > env var > default
if [ "${1:-}" != "" ]; then
    REF="$1"
elif [ "${GODOT_CPP_REF:-}" != "" ]; then
    REF="${GODOT_CPP_REF}"
else
    REF="4.2"
fi

if [ -d "${DEST}/.git" ]; then
    echo "godot-cpp already present at ${DEST} (has .git directory)."
    echo "To re-fetch, delete ${DEST} and re-run this script."
    exit 0
fi

if [ -d "${DEST}" ]; then
    # Directory exists but isn't a git repo — likely a previous clone
    # was interrupted, or someone manually unpacked an archive. Don't
    # silently proceed (git clone would fail with "destination path
    # already exists" and a less clear error). Make the user choose.
    echo "ERROR: ${DEST} exists but is not a git checkout (no .git directory)." >&2
    echo                                                                        >&2
    echo "This usually means a previous clone was interrupted. Resolve by:"     >&2
    echo "  rm -rf ${DEST}"                                                     >&2
    echo "and re-running this script."                                          >&2
    exit 1
fi

if ! command -v git >/dev/null 2>&1; then
    echo "ERROR: git is not installed. Install via your package manager:" >&2
    echo "  Ubuntu/Debian: sudo apt install git"                            >&2
    echo "  Fedora:        sudo dnf install git"                            >&2
    echo "  Arch:          sudo pacman -S git"                              >&2
    echo "  macOS:         xcode-select --install"                          >&2
    exit 1
fi

mkdir -p "$( dirname "${DEST}" )"
echo "Cloning godot-cpp (branch=${REF}) into ${DEST}..."
git clone --depth 1 --branch "${REF}" \
    https://github.com/godotengine/godot-cpp.git \
    "${DEST}"

echo
echo "Done. godot-cpp is at ${DEST}."
echo "Next: build it with scons, e.g."
echo "  (cd ${DEST} && scons platform=linux target=template_release -j\$(nproc))"
echo "Or run scripts/bootstrap.sh to do everything in one step."
