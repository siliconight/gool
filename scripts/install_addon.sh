#!/usr/bin/env bash
# Install gool as a Godot addon into a target project. Copies the
# addon's GDScript files (runtime singleton, prefabs, plugin) AND
# the built GDExtension binary, into the target project's
# addons/gool/ directory.
#
# Usage:
#     ./scripts/install_addon.sh <godot_project_path>
#     ./scripts/install_addon.sh ~/MyGame
#
# Or override the binary location:
#     GOOL_BINARY=/path/to/libgool_godot.so \
#         ./scripts/install_addon.sh ~/MyGame
#
# What it copies:
#   FROM gool repo                          TO <target>/addons/gool/
#   --------------------------------------  -----------------------
#   godot/addons/gool/*.gd                  *.gd
#   godot/addons/gool/plugin.cfg            plugin.cfg
#   godot/addons/gool/prefabs/              prefabs/
#   godot/gool.gdextension                  gool.gdextension
#   <built binary>                          bin/<binary>
#
# The script auto-detects the platform and looks for the matching
# binary at the conventional build output location:
#   Linux:   build-godot/libgool_godot.so
#   macOS:   build-godot/libgool_godot.dylib
#   Windows: build-godot/Release/gool_godot.dll
#
# Override with $GOOL_BINARY if your build is elsewhere.

set -euo pipefail

if [ $# -lt 1 ]; then
    cat <<EOF >&2
Usage: $0 <godot_project_path>

Installs the gool addon (GDScript files + GDExtension binary) into
a target Godot project. The target project must contain a
project.godot file at the given path.

Examples:
  $0 ~/MyGame
  $0 ~/games/coop_shooter

Override the binary location with GOOL_BINARY=/path/to/libgool_godot.so.
EOF
    exit 1
fi

TARGET_PROJECT="$1"
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="${SCRIPT_DIR}/.."

# Validate target.
if [ ! -d "${TARGET_PROJECT}" ]; then
    echo "ERROR: target directory does not exist: ${TARGET_PROJECT}" >&2
    exit 1
fi
if [ ! -f "${TARGET_PROJECT}/project.godot" ]; then
    echo "ERROR: ${TARGET_PROJECT} does not look like a Godot project" >&2
    echo "       (no project.godot file)"                              >&2
    exit 1
fi

# Detect platform → expected binary path.
case "$( uname -s )" in
    Linux)   DEFAULT_BINARY="${REPO_ROOT}/build-godot/libgool_godot.so"          ;;
    Darwin)  DEFAULT_BINARY="${REPO_ROOT}/build-godot/libgool_godot.dylib"       ;;
    *)       DEFAULT_BINARY="${REPO_ROOT}/build-godot/libgool_godot.so"          ;;  # best guess
esac

BINARY="${GOOL_BINARY:-${DEFAULT_BINARY}}"

if [ ! -f "${BINARY}" ]; then
    echo "ERROR: GDExtension binary not found at: ${BINARY}"             >&2
    echo                                                                  >&2
    echo "Build it first with scripts/bootstrap.sh, or pass an explicit" >&2
    echo "path via GOOL_BINARY:"                                          >&2
    echo "  GOOL_BINARY=/path/to/libgool_godot.so $0 ${TARGET_PROJECT}"  >&2
    exit 1
fi

DEST="${TARGET_PROJECT}/addons/gool"
mkdir -p "${DEST}/bin"
mkdir -p "${DEST}/prefabs"

# Copy addon GDScript files.
echo "Copying addon files into ${DEST}/..."
cp "${REPO_ROOT}/godot/addons/gool/runtime_singleton.gd"     "${DEST}/"
cp "${REPO_ROOT}/godot/addons/gool/audio_relevancy_filter.gd" "${DEST}/"
cp "${REPO_ROOT}/godot/addons/gool/plugin.gd"                 "${DEST}/"
cp "${REPO_ROOT}/godot/addons/gool/plugin.cfg"                "${DEST}/"
cp -r "${REPO_ROOT}/godot/addons/gool/prefabs/."              "${DEST}/prefabs/"

# Copy the gdextension manifest (lives one level up in the repo).
cp "${REPO_ROOT}/godot/gool.gdextension"                      "${DEST}/"

# Copy the built binary into bin/.
echo "Copying binary $( basename "${BINARY}" ) into ${DEST}/bin/..."
cp "${BINARY}" "${DEST}/bin/"

cat <<EOF

Done. gool is installed at:
  ${DEST}

Next steps:
  1. Open ${TARGET_PROJECT} in Godot 4.2 or later.
  2. Project Settings → Plugins → gool → Enable.
  3. Verify the Gool autoload appears in Project Settings → Autoload.

To verify it loaded, watch the output panel for:
  [gool] runtime initialized

If anything fails, see SETUP.md's Troubleshooting section.
EOF
