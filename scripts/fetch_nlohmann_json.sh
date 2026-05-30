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

# Fetch nlohmann/json.hpp from upstream GitHub into third_party/nlohmann/.
#
# Why nlohmann/json (v0.80.0): gool previously hand-rolled two JSON
# parsers (bus_config_loader.cpp, sound_bank.cpp). Both shipped without
# `\u`, `\b`, or `\f` escape handling — a non-spec-compliant gap that
# rejected valid JSON containing Unicode escapes. The v0.80.0 fix
# replaces both parsers with this battle-tested single-header library.
# See CHANGELOG.md and docs/json_loader.md for the full rationale.
#
# Usage:
#     ./scripts/fetch_nlohmann_json.sh           # pinned default (v3.11.3)
#     ./scripts/fetch_nlohmann_json.sh v3.11.3   # explicit pin
#     ./scripts/fetch_nlohmann_json.sh develop   # tip of upstream (NOT for releases)
#
# Sets the file read-only after download to make accidental edits obvious.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DEST_DIR="${SCRIPT_DIR}/../third_party/nlohmann"
DEST_FILE="${DEST_DIR}/json.hpp"

# Default to a specific tagged release. nlohmann/json's single-header
# format is stable across versions; pinning to a tag means the build
# is reproducible regardless of when this script is run.
REF="${1:-v3.11.3}"
URL="https://raw.githubusercontent.com/nlohmann/json/${REF}/single_include/nlohmann/json.hpp"

mkdir -p "${DEST_DIR}"

if command -v curl >/dev/null 2>&1; then
    echo "Fetching ${URL}"
    curl --fail --show-error --location --output "${DEST_FILE}" "${URL}"
elif command -v wget >/dev/null 2>&1; then
    echo "Fetching ${URL}"
    wget --quiet --show-progress -O "${DEST_FILE}" "${URL}"
else
    echo "Neither curl nor wget is available. Please install one or download" >&2
    echo "  ${URL}" >&2
    echo "manually to ${DEST_FILE}" >&2
    exit 1
fi

chmod a-w "${DEST_FILE}" || true
echo "Downloaded nlohmann/json.hpp (ref=${REF}) to ${DEST_FILE}"
