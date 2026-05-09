#!/usr/bin/env bash
# Fetch miniaudio.h from upstream GitHub into third_party/miniaudio/.
#
# Usage:
#     ./scripts/fetch_miniaudio.sh           # latest from master
#     ./scripts/fetch_miniaudio.sh 0.11.21   # specific tag
#
# Sets the file read-only after download to make accidental edits obvious.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DEST_DIR="${SCRIPT_DIR}/../third_party/miniaudio"
DEST_FILE="${DEST_DIR}/miniaudio.h"

REF="${1:-master}"
URL="https://raw.githubusercontent.com/mackron/miniaudio/${REF}/miniaudio.h"

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
echo "Downloaded miniaudio.h (ref=${REF}) to ${DEST_FILE}"
