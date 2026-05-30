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

# Fetch single-header decoder libraries from upstream GitHub:
#
#   third_party/dr_libs/dr_wav.h    (mackron/dr_libs)
#   third_party/dr_libs/dr_flac.h   (mackron/dr_libs)
#   third_party/stb/stb_vorbis.c    (nothings/stb)
#
# Usage:
#     ./scripts/fetch_decoders.sh                  # latest from each repo's master
#     ./scripts/fetch_decoders.sh <drlibs> <stb>   # pinned refs
#
# Sets the files read-only after download to make accidental edits obvious.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DR_DIR="${SCRIPT_DIR}/../third_party/dr_libs"
STB_DIR="${SCRIPT_DIR}/../third_party/stb"

DRLIBS_REF="${1:-master}"
STB_REF="${2:-master}"

mkdir -p "${DR_DIR}" "${STB_DIR}"

fetch() {
    local url="$1"
    local dest="$2"
    if command -v curl >/dev/null 2>&1; then
        echo "Fetching ${url}"
        curl --fail --show-error --location --output "${dest}" "${url}"
    elif command -v wget >/dev/null 2>&1; then
        echo "Fetching ${url}"
        wget --quiet --show-progress -O "${dest}" "${url}"
    else
        echo "Neither curl nor wget is available. Please install one or download" >&2
        echo "  ${url}" >&2
        echo "manually to ${dest}" >&2
        exit 1
    fi
    chmod a-w "${dest}" || true
}

fetch "https://raw.githubusercontent.com/mackron/dr_libs/${DRLIBS_REF}/dr_wav.h"  "${DR_DIR}/dr_wav.h"
fetch "https://raw.githubusercontent.com/mackron/dr_libs/${DRLIBS_REF}/dr_flac.h" "${DR_DIR}/dr_flac.h"
fetch "https://raw.githubusercontent.com/nothings/stb/${STB_REF}/stb_vorbis.c"    "${STB_DIR}/stb_vorbis.c"

echo
echo "Downloaded:"
echo "  dr_wav.h     (ref=${DRLIBS_REF}) → ${DR_DIR}/dr_wav.h"
echo "  dr_flac.h    (ref=${DRLIBS_REF}) → ${DR_DIR}/dr_flac.h"
echo "  stb_vorbis.c (ref=${STB_REF})    → ${STB_DIR}/stb_vorbis.c"
