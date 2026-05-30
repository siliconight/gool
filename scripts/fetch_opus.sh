#!/bin/sh
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

# Fetch libopus source into third_party/opus/. Mirrors the pattern of
# scripts/fetch_decoders.sh and scripts/fetch_miniaudio.sh. Unlike those
# (single-header), libopus is a full source tree, so this clones the
# repository.

set -e

OPUS_REF="${OPUS_REF:-master}"            # override with `OPUS_REF=v1.5.2 ./scripts/fetch_opus.sh`

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${REPO_ROOT}/third_party/opus"

if [ -e "${DEST}/CMakeLists.txt" ]; then
    echo "libopus appears to already be present at ${DEST} (CMakeLists.txt exists)."
    echo "Remove or update it manually if you want a fresh clone."
    exit 0
fi

echo "Cloning xiph/opus (${OPUS_REF}) into ${DEST}..."
mkdir -p "${DEST}"
# Clone directly into DEST. Using --depth 1 keeps the checkout small.
git clone --depth 1 --branch "${OPUS_REF}" \
    https://github.com/xiph/opus.git \
    "${DEST}"

echo "Done."
echo "Configure with -DAUDIO_ENGINE_VOICE_OPUS=ON to enable the codec."
