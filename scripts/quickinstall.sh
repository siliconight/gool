#!/usr/bin/env bash
# scripts/quickinstall.sh
#
# One-line install for the gool Godot addon on Linux / macOS.
# Bash equivalent of scripts/quickinstall.ps1.
#
# DESIGNED INVOCATION:
#
#     # From inside your Godot project directory:
#     curl -sSL https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.sh | bash
#
#     # With arguments:
#     curl -sSL .../quickinstall.sh | bash -s -- --project-path /path/to/project --version v0.11.10
#
# Or if checked out locally:
#     ./scripts/quickinstall.sh --project-path /path/to/project

set -euo pipefail

# ----------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------

PROJECT_PATH="$( pwd )"
VERSION="latest"
REPO="siliconight/gool"

while [ $# -gt 0 ]; do
    case "$1" in
        --project-path)
            PROJECT_PATH="$2"; shift 2 ;;
        --project-path=*)
            PROJECT_PATH="${1#*=}"; shift ;;
        --version)
            VERSION="$2"; shift 2 ;;
        --version=*)
            VERSION="${1#*=}"; shift ;;
        --repo)
            REPO="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^# Or if checked out/p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1 ;;
    esac
done

# ----------------------------------------------------------------------
# Banner
# ----------------------------------------------------------------------

echo
echo "============================================================"
echo "  gool — Godot addon quickinstaller"
echo "============================================================"
echo

# ----------------------------------------------------------------------
# Validate the target is a Godot project
# ----------------------------------------------------------------------

if [ ! -d "${PROJECT_PATH}" ]; then
    echo "ERROR: ${PROJECT_PATH} does not exist." >&2
    echo                                          >&2
    echo "Create your Godot project first, then re-run from inside it:" >&2
    echo "  cd /path/to/my_game"                  >&2
    echo "  curl -sSL https://raw.githubusercontent.com/${REPO}/main/scripts/quickinstall.sh | bash" >&2
    exit 1
fi

if [ ! -f "${PROJECT_PATH}/project.godot" ]; then
    echo "ERROR: ${PROJECT_PATH} has no project.godot file."         >&2
    echo                                                              >&2
    echo "Run this from inside a Godot project, or pass --project-path:" >&2
    echo "  cd /path/to/your_godot_project    # then re-run"          >&2
    exit 1
fi

echo "Target Godot project: ${PROJECT_PATH}"

# ----------------------------------------------------------------------
# Detect platform → archive suffix
# ----------------------------------------------------------------------

case "$( uname -s )" in
    Linux)
        PLATFORM="linux-x86_64"
        ARCHIVE_EXT="tar.gz" ;;
    Darwin)
        # macos-latest GitHub runners are Apple Silicon. Intel Mac users
        # would currently need to build from source via Track B —
        # tracked as a follow-up to add a separate macos-x86_64 entry.
        if [ "$( uname -m )" = "arm64" ]; then
            PLATFORM="macos-arm64"
        else
            echo "ERROR: this addon archive is built for Apple Silicon (arm64)." >&2
            echo "       Your Mac is $( uname -m ) — Intel Mac binaries"          >&2
            echo "       aren't shipped yet. Use Track B (build from source):"   >&2
            echo "       https://github.com/${REPO}/blob/main/SETUP.md"          >&2
            exit 1
        fi
        ARCHIVE_EXT="tar.gz" ;;
    *)
        echo "ERROR: unsupported platform $( uname -s )." >&2
        exit 1 ;;
esac

# ----------------------------------------------------------------------
# Resolve version
# ----------------------------------------------------------------------

if [ "${VERSION}" = "latest" ]; then
    echo "Resolving latest release..."
    if command -v curl >/dev/null 2>&1; then
        VERSION="$( curl -sSL "https://api.github.com/repos/${REPO}/releases/latest" \
                    | grep '"tag_name"' | head -1 \
                    | sed -E 's/.*"tag_name": "([^"]+)".*/\1/' )"
    elif command -v wget >/dev/null 2>&1; then
        VERSION="$( wget -qO- "https://api.github.com/repos/${REPO}/releases/latest" \
                    | grep '"tag_name"' | head -1 \
                    | sed -E 's/.*"tag_name": "([^"]+)".*/\1/' )"
    else
        echo "ERROR: neither curl nor wget is installed." >&2
        exit 1
    fi
    if [ -z "${VERSION}" ]; then
        echo "ERROR: failed to resolve latest release. Pin a version with --version v0.11.10 instead." >&2
        exit 1
    fi
fi

VERSION_NUMBER="${VERSION#v}"
echo "Version: ${VERSION}"

# ----------------------------------------------------------------------
# Download
# ----------------------------------------------------------------------

FILENAME="gool-${VERSION_NUMBER}-godot-addon-${PLATFORM}.${ARCHIVE_EXT}"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${VERSION}/${FILENAME}"
TEMP_DIR="$( mktemp -d )"
TEMP_ARCHIVE="${TEMP_DIR}/${FILENAME}"

echo "Downloading: ${FILENAME}"

if command -v curl >/dev/null 2>&1; then
    if ! curl -sSL "${DOWNLOAD_URL}" -o "${TEMP_ARCHIVE}"; then
        echo "ERROR: download failed."          >&2
        echo "  URL: ${DOWNLOAD_URL}"           >&2
        rm -rf "${TEMP_DIR}"
        exit 1
    fi
else
    if ! wget -q "${DOWNLOAD_URL}" -O "${TEMP_ARCHIVE}"; then
        echo "ERROR: download failed."          >&2
        echo "  URL: ${DOWNLOAD_URL}"           >&2
        rm -rf "${TEMP_DIR}"
        exit 1
    fi
fi

# Sanity-check size — addon archives are ~500 KB. Anything under 50 KB
# is almost certainly a 404 / corrupted download.
SIZE_BYTES="$( stat -c '%s' "${TEMP_ARCHIVE}" 2>/dev/null \
                || stat -f '%z' "${TEMP_ARCHIVE}" )"
if [ "${SIZE_BYTES}" -lt 51200 ]; then
    echo "ERROR: downloaded file is suspiciously small (${SIZE_BYTES} bytes)." >&2
    echo "Likely a 404 or corrupted download. Check:"                          >&2
    echo "  https://github.com/${REPO}/releases/tag/${VERSION}"                >&2
    rm -rf "${TEMP_DIR}"
    exit 1
fi
SIZE_KB=$(( SIZE_BYTES / 1024 ))
echo "Downloaded ${SIZE_KB} KB"

# ----------------------------------------------------------------------
# Extract + locate addons/gool inside the archive
# ----------------------------------------------------------------------

TEMP_EXTRACT="${TEMP_DIR}/extracted"
mkdir -p "${TEMP_EXTRACT}"
( cd "${TEMP_EXTRACT}" && tar -xzf "${TEMP_ARCHIVE}" )

# The archive contains gool-X.Y.Z-godot-addon-PLATFORM/addons/gool/
EXTRACTED_ROOT="$( find "${TEMP_EXTRACT}" -maxdepth 1 -mindepth 1 -type d | head -1 )"
if [ -z "${EXTRACTED_ROOT}" ]; then
    echo "ERROR: extracted archive doesn't contain a top-level directory." >&2
    rm -rf "${TEMP_DIR}"
    exit 1
fi
ADDON_SOURCE="${EXTRACTED_ROOT}/addons/gool"
if [ ! -d "${ADDON_SOURCE}" ]; then
    echo "ERROR: archive structure is unexpected — couldn't find addons/gool inside it." >&2
    echo "  Looked at: ${ADDON_SOURCE}"                                                  >&2
    rm -rf "${TEMP_DIR}"
    exit 1
fi

# ----------------------------------------------------------------------
# Install (replacing existing addons/gool if present)
# ----------------------------------------------------------------------

ADDON_DEST="${PROJECT_PATH}/addons/gool"
if [ -d "${ADDON_DEST}" ]; then
    echo "Replacing existing addons/gool/ ..."
    rm -rf "${ADDON_DEST}"
fi
mkdir -p "${PROJECT_PATH}/addons"
cp -r "${ADDON_SOURCE}" "${ADDON_DEST}"

rm -rf "${TEMP_DIR}"

# ----------------------------------------------------------------------
# Done
# ----------------------------------------------------------------------

cat <<EOF

============================================================
  Installed gool ${VERSION} into ${PROJECT_PATH}/addons/gool/
============================================================

EOF

# ----------------------------------------------------------------------
# v0.78.8: auto-enable the gool EditorPlugin in project.godot
# ----------------------------------------------------------------------
# Same flow as the Windows installers: edit project.godot to add
# res://addons/gool/plugin.cfg to [editor_plugins]/enabled, so the
# user's first open already has the plugin active and the autoloads
# register cleanly. The helper ships with the addon (in
# addons/gool/tools/) so its logic versions with the addon and is
# idempotent — safe to run on a re-install.

ENABLE_SCRIPT="${PROJECT_PATH}/addons/gool/tools/enable_plugin.sh"
if [ -f "${ENABLE_SCRIPT}" ]; then
    echo "Enabling gool plugin in project.godot..."
    chmod +x "${ENABLE_SCRIPT}"
    if bash "${ENABLE_SCRIPT}" "${PROJECT_PATH}"; then
        :
    else
        echo "  [warn] enable_plugin.sh returned an error."
        echo "         You may need to enable the plugin manually in Godot:"
        echo "         Project Settings -> Plugins -> gool -> Enable"
    fi
    echo ""
else
    echo "  [warn] enable_plugin.sh not found at ${ENABLE_SCRIPT}"
    echo "         The plugin will need to be enabled manually:"
    echo "         Project Settings -> Plugins -> gool -> Enable"
    echo ""
fi

# ----------------------------------------------------------------------
# v0.78.6: optional post-install verification
# ----------------------------------------------------------------------
# Look for Godot on PATH. If present, run a headless pass that loads
# the user's project, fires Gool.diagnose(), and exits 0/1. If Godot
# isn't on PATH or the verify pass fails, we surface it — but never
# block on it. The addon is already deployed.
#
# The binary is named 'godot' on most distros, 'godot4' on some, and
# we try both before giving up. Heuristic only; if neither resolves
# we skip with a friendly message rather than fail-noisily.

GODOT_BIN=""
if command -v godot >/dev/null 2>&1; then
    GODOT_BIN="godot"
elif command -v godot4 >/dev/null 2>&1; then
    GODOT_BIN="godot4"
fi

if [ -z "${GODOT_BIN}" ]; then
    echo "[skip] Godot is not on PATH (tried 'godot' and 'godot4') —"
    echo "       skipping post-install verification. That's fine; just"
    echo "       open the project in Godot manually."
    echo ""
else
    echo "Running headless verification (this takes a few seconds)..."
    echo ""
    # --headless / --audio-driver Dummy: no display, no real audio device
    # --quit-after 5: safety net; verify scene quits on its own well before
    # --path: the user's project, where the addon was just deployed
    "${GODOT_BIN}" --headless --audio-driver Dummy --quit-after 5 \
        --path "${PROJECT_PATH}" \
        res://addons/gool/tools/verify_install.tscn
    VERIFY_EXIT=$?

    echo ""
    if [ ${VERIFY_EXIT} -eq 0 ]; then
        echo "[verify] PASSED — install is healthy."
    else
        echo "[verify] FAILED with exit code ${VERIFY_EXIT}."
        echo "         Review the diagnose output above — every fail line"
        echo "         has a hint. The most common cause is the project"
        echo "         not having an autoload entry for Gool yet — open"
        echo "         Project Settings -> Autoload and add"
        echo "         res://addons/gool/runtime_singleton.gd as 'Gool'."
    fi
    echo ""
fi

cat <<EOF
One step left:
  1. Open ${PROJECT_PATH} in Godot 4.2 or later

The gool EditorPlugin is already enabled (see step above), so the
Gool autoload registers on first open. You can call Gool.play_3d()
/ Gool.set_rtpc() from any GDScript. The output panel should show
'[gool] runtime initialized'.

EOF
