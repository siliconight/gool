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

# scripts/check_license_canonical.sh
#
# v0.80.24 (#1): LICENSE drift guard. Fails the build if the project
# LICENSE has been overwritten by anything other than canonical
# Apache 2.0 + the project copyright + the third-party section.
#
# WHY THIS EXISTS
# ===============
#   In May 2026, an MIT-license placeholder template (with the literal
#   string "[YOUR NAME OR ORGANIZATION]" never filled in) overwrote the
#   actual Apache 2.0 LICENSE at some point. Caught during install
#   verification, not by CI. README and fonts/README both correctly
#   stated Apache 2.0; the LICENSE itself was the only wrong artifact.
#   This script catches that specific regression and any structurally
#   similar replacement.
#
# USAGE
# =====
#   From the repo root:
#       bash scripts/check_license_canonical.sh
#
#   Exits 0 if every check passes, with a summary.
#   Exits 1 on the first failed check, with what was expected and why.
#
# WHAT IT CHECKS
# ==============
#   Regression detectors (specific to the May 2026 incident):
#     1. NO "^MIT License" line          — the placeholder header
#     2. NO "YOUR NAME OR ORGANIZATION"  — the placeholder copyright
#
#   Apache 2.0 structural integrity:
#     3. Has the "Apache License" header line
#     4. Has the "Version 2.0, January 2004" subhead
#     5. Has the TERMS AND CONDITIONS heading
#     6. Has "END OF TERMS AND CONDITIONS" — section 9 boundary
#     7. Has the APPENDIX heading — proves the boilerplate template
#        is present
#
#   Project-specific structural:
#     8. Has "Copyright 20YY Brannen Graves" exactly
#     9. Has "Third-party components" section header
#
# What this does NOT check:
#   - Byte-for-byte canonical Apache 2.0 text. A sha256 check is
#     possible but brittle (line-ending normalization, trailing
#     whitespace, etc. would flip the hash). Pattern checks catch
#     the regression we care about; add stricter checks later if
#     subtle drift becomes a real concern.
#   - Source file header coverage. Apache 2.0 recommends per-file
#     license headers in source code. We don't have that yet; the
#     triage flagged it as a separate decision deferred for now.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f LICENSE ]; then
    echo "ERROR: LICENSE file does not exist at repo root."
    exit 1
fi

failed=0
fail() {
    echo "ERROR: $1"
    echo "       $2"
    failed=1
}

# --- Regression detectors ---

if grep -q "^MIT License" LICENSE; then
    fail "LICENSE starts with 'MIT License'." \
         "The MIT placeholder template has overwritten the canonical
       Apache 2.0 text. Restore from a known-good source (e.g.
       https://www.apache.org/licenses/LICENSE-2.0.txt) plus the
       project copyright and third-party section."
fi

if grep -q "YOUR NAME OR ORGANIZATION" LICENSE; then
    fail "LICENSE contains '[YOUR NAME OR ORGANIZATION]'." \
         "This is the unfilled placeholder from the MIT scaffolding
       template. Replace with the canonical Apache 2.0 text and
       the project copyright line."
fi

# --- Apache 2.0 structural integrity ---

if ! grep -qE '^[[:space:]]+Apache License$' LICENSE; then
    fail "Missing 'Apache License' header line." \
         "Expected a line containing only 'Apache License' (centered
       with leading whitespace per Apache's canonical formatting)."
fi

if ! grep -qE '^[[:space:]]+Version 2\.0, January 2004$' LICENSE; then
    fail "Missing 'Version 2.0, January 2004' subhead." \
         "Either Apache 2.0 has been replaced with a different version
       (we use 2.0 specifically) or the line was deleted."
fi

if ! grep -qE 'TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION' LICENSE; then
    fail "Missing 'TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION'." \
         "This heading marks the start of the legal terms section in
       canonical Apache 2.0. Its absence means the license body has
       been removed or replaced."
fi

if ! grep -qE 'END OF TERMS AND CONDITIONS' LICENSE; then
    fail "Missing 'END OF TERMS AND CONDITIONS' marker." \
         "This marks the boundary between section 9 and the appendix.
       Absence means the license body is truncated or corrupted."
fi

if ! grep -qE 'APPENDIX: How to apply the Apache License to your work' LICENSE; then
    fail "Missing 'APPENDIX: How to apply the Apache License to your work' heading." \
         "The appendix is part of canonical Apache 2.0 and contains
       the boilerplate template for per-source-file headers."
fi

# --- Project-specific structural ---

if ! grep -qE '^Copyright 20[0-9]{2} Brannen Graves$' LICENSE; then
    fail "Missing project copyright line." \
         "Expected exactly:
         Copyright 20YY Brannen Graves
       on its own line. Update YY if the copyright year has rolled
       over."
fi

if ! grep -qE '^Third-party components$' LICENSE; then
    fail "Missing 'Third-party components' section header." \
         "The third-party section documents the licenses of vendored
       dependencies (miniaudio, dr_libs, stb, libopus). Absence
       suggests the section was accidentally truncated."
fi

# --- Summary ---

if [ "$failed" -ne 0 ]; then
    echo ""
    echo "FAILED: LICENSE drift detected. See errors above. Run"
    echo "        git diff LICENSE  to see what changed."
    exit 1
fi

# Capture the copyright year for the success message
copyright_line=$(grep -E '^Copyright 20[0-9]{2} Brannen Graves$' LICENSE)
echo "OK: LICENSE is canonical Apache 2.0 + project copyright ($copyright_line) + third-party section."
exit 0
