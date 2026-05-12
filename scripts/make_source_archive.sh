#!/usr/bin/env bash
# scripts/make_source_archive.sh
#
# Produce a clean source archive of the gool repo, named with the
# project name + version so adopters get a clearly labeled artifact:
#
#   gool-X.Y.Z.tar.gz                 ← archive filename
#   └── gool-X.Y.Z/                   ← top-level directory inside
#       ├── src/
#       ├── include/
#       ├── godot/
#       ├── scripts/
#       ├── README.md
#       ├── ...
#
# The version is read from include/audio_engine/version.h's
# kVersionString constant — single source of truth, kept in sync
# with CMakeLists.txt and the version_test by the release procedure.
#
# Usage:
#     ./scripts/make_source_archive.sh                     # → ./dist/gool-X.Y.Z.tar.gz
#     ./scripts/make_source_archive.sh path/to/output.tgz  # → custom output path
#     ./scripts/make_source_archive.sh -                   # → stdout
#
# What's INCLUDED:
#   src/ include/ godot/ examples/ tests/ scripts/ docs/
#   .github/ CMakeLists.txt
#   README.md SETUP.md RELEASING.md CHANGELOG.md LICENSE
#   Top-level dotfiles (.gitignore, .clang-format if present)
#
# What's EXCLUDED:
#   build/ build-*/ out/ cmake-build-*/  (CMake/IDE build dirs)
#   third_party/                         (fetched on demand by scripts/fetch_*)
#   dist/                                (other source archives)
#   .git/                                (use `git archive` directly if you want repo state)
#   *.o *.a *.so *.dylib *.dll *.lib     (build artifacts)
#   __pycache__/ *.pyc                   (Python cache)
#
# This mirrors the convention used by release.yml's compiled
# artifacts (`gool-X.Y.Z-PLATFORM.tar.gz/zip`) and the addon archive
# (`gool-X.Y.Z-godot-addon-PLATFORM.tar.gz/zip`). The plain
# version-only suffix unambiguously means "source tree at vX.Y.Z."

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/.." && pwd )"

# ----------------------------------------------------------------------
# Read version from version.h. Brittle if the format changes; the
# release procedure (see RELEASING.md) keeps version.h, CMakeLists.txt,
# and the version test in lockstep, so this is the canonical source.
# ----------------------------------------------------------------------

VERSION_HEADER="${REPO_ROOT}/include/audio_engine/version.h"
if [ ! -f "${VERSION_HEADER}" ]; then
    echo "ERROR: version.h not found at ${VERSION_HEADER}" >&2
    echo "       Are you running this from a gool checkout?"  >&2
    exit 1
fi

VERSION="$( grep -E '^[[:space:]]*constexpr[[:space:]]+const[[:space:]]+char\*[[:space:]]+kVersionString' \
            "${VERSION_HEADER}" \
            | sed -E 's/.*"([^"]+)".*/\1/' )"

if [ -z "${VERSION}" ]; then
    echo "ERROR: failed to parse kVersionString from ${VERSION_HEADER}" >&2
    exit 1
fi

# Sanity-check format (X.Y.Z, optionally with pre-release suffix).
if ! echo "${VERSION}" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$'; then
    echo "ERROR: version '${VERSION}' from ${VERSION_HEADER} doesn't match X.Y.Z[-suffix]" >&2
    exit 1
fi

ARCHIVE_PREFIX="gool-${VERSION}"

# ----------------------------------------------------------------------
# Determine output path.
# ----------------------------------------------------------------------

if [ $# -gt 1 ]; then
    echo "Usage: $0 [output_path | -]" >&2
    exit 1
fi

OUTPUT_PATH="${1:-${REPO_ROOT}/dist/${ARCHIVE_PREFIX}.tar.gz}"

if [ "${OUTPUT_PATH}" != "-" ]; then
    OUTPUT_DIR="$( dirname "${OUTPUT_PATH}" )"
    mkdir -p "${OUTPUT_DIR}"
fi

# ----------------------------------------------------------------------
# Build the tar.
#
# Two bugs lived in the previous "tar . --transform" approach (fixed in
# v0.20.2):
#
#   1. GNU tar's `--transform` is documented to skip the root-directory
#      entry, so when packing `.` from the repo root the bare `./` entry
#      at the head of the archive was emitted unrewritten. Every other
#      member (`./src/...`) was correctly rewritten to `gool-X.Y.Z/...`,
#      but the lone `./` slipped through. Windows extractors that show
#      the top-level entries of a .tar.gz then displayed something like
#      "./" alongside "gool-X.Y.Z/", which looked like the archive was
#      missing its version prefix.
#
#   2. `tar -z` invokes gzip in a way that embeds the absolute output
#      path in the gzip header's "original name" field. When the .gz is
#      opened on Windows, tools that read that field saw a Linux-shaped
#      absolute path (/mnt/.../gool-X.Y.Z.tar) and rendered it weirdly.
#
# Both problems disappear when we (a) stage the source into a named
# directory and tar that as an explicit member — same approach the
# Windows .ps1 script uses; and (b) drive gzip via
# --use-compress-program with the -n flag, which tells gzip to omit the
# original-name and timestamp fields from its header.
#
# Excludes are listed explicitly rather than relying on .gitignore
# because (a) this script needs to work in non-git checkouts and
# (b) we want excludes here to track the deliberate ship-set, not
# the broader local-noise set in .gitignore.
# ----------------------------------------------------------------------

EXCLUDES=(
    --exclude='./.git'
    --exclude='./.git/*'
    --exclude='./build'
    --exclude='./build/*'
    --exclude='./build-*'
    --exclude='./build-*/*'
    --exclude='./out'
    --exclude='./out/*'
    --exclude='./cmake-build-*'
    --exclude='./third_party'
    --exclude='./third_party/*'
    --exclude='./dist'
    --exclude='./dist/*'
    --exclude='*.o'
    --exclude='*.obj'
    --exclude='*.a'
    --exclude='*.lib'
    --exclude='*.so'
    --exclude='*.so.*'
    --exclude='*.dylib'
    --exclude='*.dll'
    --exclude='*.exe'
    --exclude='*.pdb'
    --exclude='__pycache__'
    --exclude='*.pyc'
    --exclude='*.pyo'
    --exclude='.DS_Store'
    --exclude='Thumbs.db'
    --exclude='*.swp'
    --exclude='*.swo'
)

# Stage into ${STAGING_ROOT}/gool-X.Y.Z/ via a tar | tar pipe so the
# EXCLUDES list applies during copy. Pure cp/rsync would either ignore
# excludes (cp -a) or require a separate dependency (rsync); the
# tar | tar idiom is portable and doesn't pull anything new in.
STAGING_ROOT="$( mktemp -d -t gool-stage-XXXXXXXX )"
STAGING_DIR="${STAGING_ROOT}/${ARCHIVE_PREFIX}"
mkdir -p "${STAGING_DIR}"

# Always clean up the staging dir on exit (success or failure).
trap 'rm -rf "${STAGING_ROOT}"' EXIT

cd "${REPO_ROOT}"
tar "${EXCLUDES[@]}" --owner=0 --group=0 -cf - . \
    | tar -xf - -C "${STAGING_DIR}"

# Pack the staged directory by its explicit name. No `--transform`
# needed — the on-disk layout is already the layout we want inside
# the archive. `--use-compress-program='gzip -n'` strips the embedded
# original-name and mtime from the gzip header.
if [ "${OUTPUT_PATH}" = "-" ]; then
    cd "${STAGING_ROOT}"
    tar --use-compress-program='gzip -n' \
        --owner=0 --group=0 \
        -cf - "${ARCHIVE_PREFIX}"
else
    cd "${STAGING_ROOT}"
    tar --use-compress-program='gzip -n' \
        --owner=0 --group=0 \
        -cf "${OUTPUT_PATH}" "${ARCHIVE_PREFIX}"

    # Report what we wrote.
    SIZE_BYTES="$( stat -c '%s' "${OUTPUT_PATH}" 2>/dev/null \
                    || stat -f '%z' "${OUTPUT_PATH}" )"
    SIZE_KB=$(( SIZE_BYTES / 1024 ))

    echo "Wrote source archive:"
    echo "  ${OUTPUT_PATH} (${SIZE_KB} KB)"
    echo
    echo "When extracted, produces:"
    echo "  ${ARCHIVE_PREFIX}/"
    echo
    if command -v md5sum >/dev/null 2>&1; then
        echo "MD5: $( md5sum "${OUTPUT_PATH}" | awk '{print $1}' )"
    elif command -v md5 >/dev/null 2>&1; then
        echo "MD5: $( md5 -q "${OUTPUT_PATH}" )"
    fi
fi
