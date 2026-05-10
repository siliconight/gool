#!/usr/bin/env bash
# scripts/bootstrap.sh
#
# One-command setup for gool on Linux / macOS. Verifies prerequisites,
# fetches single-header dependencies, clones and builds godot-cpp at
# a known-good ref, builds gool's GDExtension binary, and (optionally)
# installs the addon into a target Godot project.
#
# Usage:
#     ./scripts/bootstrap.sh                          # build only
#     ./scripts/bootstrap.sh --install-to ~/MyGame    # build + install
#
# Configuration (env vars):
#     GODOT_CPP_REF      Branch of godot-cpp to clone (default: 4.2)
#     BUILD_TYPE         CMake build type (default: Release)
#     JOBS               Parallel build jobs (default: nproc / sysctl)
#     SKIP_GODOT_CPP     If set, skip the godot-cpp clone+build step
#                          (use when you already have it built elsewhere
#                           and pass GODOT_CPP_PATH=...)
#     GODOT_CPP_PATH     Override godot-cpp location (default:
#                          third_party/godot-cpp)
#
# Idempotent: every step checks if its work is already done before
# repeating it. Re-running this script is cheap.
#
# Verbose mode: set VERBOSE=1 to see every command echoed.

set -euo pipefail

if [ "${VERBOSE:-}" = "1" ]; then
    set -x
fi

# ----------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------

INSTALL_TO=""
while [ $# -gt 0 ]; do
    case "$1" in
        --install-to)
            INSTALL_TO="$2"
            shift 2
            ;;
        --install-to=*)
            INSTALL_TO="${1#*=}"
            shift
            ;;
        -h|--help)
            sed -n '2,/^# Idempotent:/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "See $0 --help"        >&2
            exit 1
            ;;
    esac
done

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/.." && pwd )"

GODOT_CPP_REF="${GODOT_CPP_REF:-4.2}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
GODOT_CPP_PATH="${GODOT_CPP_PATH:-${REPO_ROOT}/third_party/godot-cpp}"

# Detect parallelism — nproc on Linux, sysctl on macOS, fallback to 4.
if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$( nproc )"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$( sysctl -n hw.ncpu 2>/dev/null || echo 4 )"
    else
        JOBS=4
    fi
fi

# Detect platform → SCons platform name.
case "$( uname -s )" in
    Linux)   PLATFORM="linux"   ;;
    Darwin)  PLATFORM="macos"   ;;
    *)
        echo "ERROR: unsupported platform $( uname -s ). bootstrap.sh covers" >&2
        echo "       Linux and macOS. For Windows use scripts/bootstrap.ps1." >&2
        exit 1
        ;;
esac

if [ "${PLATFORM}" = "macos" ]; then
    cat <<EOF

WARNING: macOS builds are currently broken on Apple Clang. The CI
matrix has macOS disabled for the same reason. This script will run,
but expect compile errors. See SETUP.md.

EOF
fi

# ----------------------------------------------------------------------
# Pretty-printing
# ----------------------------------------------------------------------

step() {
    echo
    echo "==> $*"
}

# ----------------------------------------------------------------------
# Step 1: prerequisite checks
# ----------------------------------------------------------------------

step "Step 1/5: checking prerequisites"

require() {
    local cmd="$1"
    local hint="$2"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "ERROR: '${cmd}' is not on PATH." >&2
        echo "       ${hint}"                  >&2
        exit 1
    fi
    printf '  ✓ %s\n' "${cmd}"
}

require git       "Install via your platform's package manager."
require cmake     "https://cmake.org/download/ — or via package manager."
require python3   "Install python3 — needed to bootstrap SCons (godot-cpp's build tool)."
require scons     "pip3 install scons (or: apt install scons / brew install scons)."

# At least one C++ compiler.
if command -v g++       >/dev/null 2>&1; then printf '  ✓ %s\n' "g++ (C++ compiler)"
elif command -v clang++ >/dev/null 2>&1; then printf '  ✓ %s\n' "clang++ (C++ compiler)"
else
    echo "ERROR: no C++ compiler found (g++ or clang++)."     >&2
    echo "       Install build-essential (Linux) or run"      >&2
    echo "       xcode-select --install (macOS)."             >&2
    exit 1
fi

# ----------------------------------------------------------------------
# Step 2: fetch single-header dependencies
# ----------------------------------------------------------------------

step "Step 2/5: fetching single-header dependencies"

if [ -f "${REPO_ROOT}/third_party/miniaudio/miniaudio.h" ]; then
    echo "  ✓ miniaudio.h already present"
else
    bash "${SCRIPT_DIR}/fetch_miniaudio.sh"
fi

if [ -f "${REPO_ROOT}/third_party/dr_libs/dr_wav.h"     ] && \
   [ -f "${REPO_ROOT}/third_party/dr_libs/dr_flac.h"    ] && \
   [ -f "${REPO_ROOT}/third_party/stb/stb_vorbis.c"     ]; then
    echo "  ✓ decoder headers already present"
else
    bash "${SCRIPT_DIR}/fetch_decoders.sh"
fi

# ----------------------------------------------------------------------
# Step 3: clone + build godot-cpp
# ----------------------------------------------------------------------

if [ "${SKIP_GODOT_CPP:-}" = "1" ]; then
    step "Step 3/5: skipping godot-cpp (SKIP_GODOT_CPP=1)"
    if [ ! -d "${GODOT_CPP_PATH}" ]; then
        echo "ERROR: SKIP_GODOT_CPP=1 but GODOT_CPP_PATH=${GODOT_CPP_PATH} doesn't exist." >&2
        exit 1
    fi
else
    step "Step 3/5: cloning + building godot-cpp (branch=${GODOT_CPP_REF})"

    if [ ! -d "${GODOT_CPP_PATH}/.git" ]; then
        bash "${SCRIPT_DIR}/fetch_godot_cpp.sh" "${GODOT_CPP_REF}"
    else
        echo "  ✓ godot-cpp already cloned at ${GODOT_CPP_PATH}"
    fi

    # Look for ANY built libgodot-cpp artifact in bin/. The exact
    # filename varies by platform / arch / build type; one match is
    # enough to confirm the build has run.
    if compgen -G "${GODOT_CPP_PATH}/bin/libgodot-cpp.*.a" >/dev/null; then
        echo "  ✓ godot-cpp already built (libgodot-cpp.*.a present)"
    else
        echo "  building godot-cpp — this takes 5–20 minutes the first time..."
        ( cd "${GODOT_CPP_PATH}" && \
          scons platform="${PLATFORM}" target=template_release "-j${JOBS}" )
    fi
fi

# ----------------------------------------------------------------------
# Step 4: build gool's GDExtension
# ----------------------------------------------------------------------

step "Step 4/5: building gool's GDExtension binding"

BUILD_DIR="${REPO_ROOT}/build-godot"

cmake -S "${REPO_ROOT}/godot" -B "${BUILD_DIR}" \
    -DGODOT_CPP_PATH="${GODOT_CPP_PATH}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON \
    -DAUDIO_ENGINE_DECODERS_WAV=ON \
    -DAUDIO_ENGINE_DECODERS_OGG=ON \
    -DAUDIO_ENGINE_DECODERS_FLAC=ON

cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" "-j${JOBS}"

# Find the produced binary.
case "${PLATFORM}" in
    linux)  BINARY="${BUILD_DIR}/libgool_godot.so"    ;;
    macos)  BINARY="${BUILD_DIR}/libgool_godot.dylib" ;;
esac

if [ ! -f "${BINARY}" ]; then
    echo "ERROR: build completed but binary not at expected path: ${BINARY}" >&2
    echo "       Search the build directory:"                                >&2
    find "${BUILD_DIR}" -name "libgool_godot.*" 2>/dev/null                  >&2
    exit 1
fi

echo
echo "  ✓ built ${BINARY}"

# ----------------------------------------------------------------------
# Step 5: install into Godot project (if requested)
# ----------------------------------------------------------------------

if [ -n "${INSTALL_TO}" ]; then
    step "Step 5/5: installing into Godot project: ${INSTALL_TO}"
    GOOL_BINARY="${BINARY}" bash "${SCRIPT_DIR}/install_addon.sh" "${INSTALL_TO}"
else
    step "Step 5/5: skipped (no --install-to specified)"

    cat <<EOF

To install the addon into a Godot project, run:

  ./scripts/install_addon.sh /path/to/your/project

Or re-run this script with --install-to:

  ./scripts/bootstrap.sh --install-to /path/to/your/project

The built binary is at:
  ${BINARY}

EOF
fi

step "Bootstrap complete."
