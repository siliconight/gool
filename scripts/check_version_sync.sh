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

# scripts/check_version_sync.sh
#
# v0.79.4: Local version-sync check. Replaces the version-sync CI job
# that shipped briefly in v0.79.2 then got rolled back in v0.79.4 when
# we discovered it (or something near it) was preventing GitHub Actions
# workflows from dispatching.
#
# WHY THIS EXISTS
# ===============
#   plugin.cfg has its own `version=` field that Godot displays in
#   the "Edit a Plugin" dialog. It's easy to bump version.h and
#   CMakeLists for an engine release and forget plugin.cfg — that's
#   exactly what happened from v0.78.0 through v0.79.1 (every release
#   showed "0.1.0" in Godot's plugin dialog because plugin.cfg never
#   got updated). Run this script before any version-bump commit to
#   catch the mismatch.
#
# USAGE
# =====
#   From the repo root:
#       bash scripts/check_version_sync.sh
#
#   Exits 0 if all 5 sources agree, prints the agreed version.
#   Exits 1 with a diff if any mismatch, prints which file is wrong.
#
# THE 5 SOURCES OF TRUTH
# ======================
#   1. include/audio_engine/version.h     (kVersionString)
#   2. CMakeLists.txt                     (project VERSION)
#   3. godot/addons/gool/plugin.cfg                          (version=)
#   4. examples/coop_shooter_template/addons/gool/plugin.cfg (version=)
#   5. examples/voice_chat/addons/gool/plugin.cfg            (version=)
#
# All five must match. The script bumps no files; it only reports.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

VH=$(grep -oP 'kVersionString = "\K[^"]+' include/audio_engine/version.h || echo "MISSING")
CMK=$(grep -oP '^\s*VERSION \K[0-9.]+' CMakeLists.txt | head -1 || echo "MISSING")
PCFG_MAIN=$(grep -oP '^version="\K[^"]+' godot/addons/gool/plugin.cfg || echo "MISSING")
PCFG_COOP=$(grep -oP '^version="\K[^"]+' examples/coop_shooter_template/addons/gool/plugin.cfg || echo "MISSING")
PCFG_VOICE=$(grep -oP '^version="\K[^"]+' examples/voice_chat/addons/gool/plugin.cfg || echo "MISSING")

printf "  %-60s %s\n" "include/audio_engine/version.h:" "$VH"
printf "  %-60s %s\n" "CMakeLists.txt:" "$CMK"
printf "  %-60s %s\n" "godot/addons/gool/plugin.cfg:" "$PCFG_MAIN"
printf "  %-60s %s\n" "examples/coop_shooter_template/addons/gool/plugin.cfg:" "$PCFG_COOP"
printf "  %-60s %s\n" "examples/voice_chat/addons/gool/plugin.cfg:" "$PCFG_VOICE"

if [ "$VH" != "$CMK" ] \
    || [ "$VH" != "$PCFG_MAIN" ] \
    || [ "$VH" != "$PCFG_COOP" ] \
    || [ "$VH" != "$PCFG_VOICE" ]; then
    echo ""
    echo "ERROR: version sync mismatch. Bump ALL 5 sources to the same value."
    echo "       (also remember kVersionFull in version.h needs the same update)"
    exit 1
fi

echo ""
echo "OK: all 5 versions agree at $VH"
exit 0
