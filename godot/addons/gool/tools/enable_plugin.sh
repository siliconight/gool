#!/usr/bin/env bash
# addons/gool/tools/enable_plugin.sh
#
# v0.78.8 — Idempotent project.godot editor (bash port).
#
# Mirror of enable_plugin.ps1. Adds res://addons/gool/plugin.cfg to
# the [editor_plugins]/enabled PackedStringArray. Five cases handled
# identically to the PowerShell version; logic was prototyped in
# Python before translation to both shells.
#
# Usage:  ./enable_plugin.sh <project-path>
#
# Returns:  0 on success (including no-op when already enabled)
#           1 if project.godot is missing or unreadable
set -u

if [ $# -lt 1 ]; then
    echo "  usage: $0 <project-path>" >&2
    exit 1
fi

PROJECT_PATH="$1"
PLUGIN_PATH="res://addons/gool/plugin.cfg"
PROJECT_GODOT="${PROJECT_PATH}/project.godot"

if [ ! -f "${PROJECT_GODOT}" ]; then
    echo "  [skip] project.godot not found at ${PROJECT_GODOT}"
    exit 1
fi

# Idempotency check — Case 5. Use grep to find any enabled= line
# containing our plugin path. -F for fixed string (no regex escaping
# pain), -q for silent. We grep the WHOLE file rather than parsing
# the array structure because if "addons/gool/plugin.cfg" appears
# anywhere in any PackedStringArray, the plugin is enabled.
if grep -qF "addons/gool/plugin.cfg" "${PROJECT_GODOT}"; then
    echo "  [ok] gool plugin already enabled in project.godot (no change)"
    exit 0
fi

# Read whole file into a variable for awk-based mutation. project.godot
# files are small (kilobytes), so this is fine.
CONTENT="$(cat "${PROJECT_GODOT}")"

# Determine which case we're in by structural grep:
#   - has_section: [editor_plugins] header present somewhere
#   - has_enabled: enabled=PackedStringArray(...) line present
HAS_SECTION=0
HAS_ENABLED=0
if echo "${CONTENT}" | grep -qE '^\[editor_plugins\]'; then
    HAS_SECTION=1
fi
if echo "${CONTENT}" | grep -qE '^enabled\s*=\s*PackedStringArray\('; then
    HAS_ENABLED=1
fi

if [ "${HAS_ENABLED}" = "1" ]; then
    # Cases 3 or 4: enabled= line exists.
    # Use sed to capture the array's current contents.
    EXISTING="$(echo "${CONTENT}" \
        | sed -nE 's/^enabled\s*=\s*PackedStringArray\((.*)\)\s*$/\1/p' \
        | head -n1)"
    # Trim whitespace
    EXISTING_TRIMMED="$(echo "${EXISTING}" | sed 's/^\s*//;s/\s*$//')"

    if [ -z "${EXISTING_TRIMMED}" ]; then
        # Case 3: empty array. Replace whole line.
        NEW_LINE="enabled=PackedStringArray(\"${PLUGIN_PATH}\")"
        # Use perl for the in-place replacement; sed's behavior on
        # multi-line matches varies across BSD/GNU and we want
        # portable code. Perl is universally present.
        perl -i -pe 's|^enabled\s*=\s*PackedStringArray\(\s*\)\s*$|'"${NEW_LINE}"'|' \
            "${PROJECT_GODOT}"
        echo "  [ok] gool plugin added to [editor_plugins] (was empty)"
    else
        # Case 4: populated array. Append our entry.
        NEW_LINE="enabled=PackedStringArray(${EXISTING_TRIMMED}, \"${PLUGIN_PATH}\")"
        perl -i -pe 's|^enabled\s*=\s*PackedStringArray\([^)]+\)\s*$|'"${NEW_LINE}"'|' \
            "${PROJECT_GODOT}"
        echo "  [ok] gool plugin appended to existing [editor_plugins] list"
    fi
elif [ "${HAS_SECTION}" = "1" ]; then
    # Case 2: section exists but no enabled= line. Insert one right
    # after the section header.
    perl -i -pe 's|^\[editor_plugins\]\s*$|[editor_plugins]\nenabled=PackedStringArray("'"${PLUGIN_PATH}"'")|' \
        "${PROJECT_GODOT}"
    echo "  [ok] gool plugin added to existing [editor_plugins] section"
else
    # Case 1: no section. Append at end. Ensure file ends with newline
    # before adding our block.
    LAST_CHAR="$(tail -c1 "${PROJECT_GODOT}")"
    if [ "${LAST_CHAR}" != "" ]; then
        # File doesn't end with newline. Add one.
        echo "" >> "${PROJECT_GODOT}"
    fi
    cat >> "${PROJECT_GODOT}" <<EOF

[editor_plugins]

enabled=PackedStringArray("${PLUGIN_PATH}")
EOF
    echo "  [ok] gool plugin enabled (created [editor_plugins] section)"
fi

exit 0
