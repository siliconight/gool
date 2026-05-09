#!/bin/sh
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
