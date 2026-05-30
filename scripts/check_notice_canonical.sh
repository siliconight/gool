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

# scripts/check_notice_canonical.sh
#
# v0.81.3: NOTICE drift guard. Fails the build if the project NOTICE
# file has been modified away from its canonical form.
#
# WHY THIS EXISTS
# ===============
#   Apache 2.0 Section 4(d) gives the NOTICE file special legal
#   weight: any derivative work distributed in source or binary form
#   MUST include a readable copy of the attribution notices from the
#   original NOTICE file. This makes NOTICE the strongest attribution
#   mechanism Apache 2.0 provides — stronger than the LICENSE file
#   alone, stronger than per-file headers (which a determined
#   infringer could try to argue were stripped accidentally).
#
#   Because forks legally must copy NOTICE verbatim, the content
#   needs to be:
#     (1) Stable. Every change to NOTICE creates work for every
#         downstream fork.
#     (2) Correct. A typo or wrong year in NOTICE propagates to
#         every fork that complies with the license.
#     (3) Tamper-evident. A regression that strips the project
#         attribution should be caught by CI, not by a future
#         lawyer.
#
#   This guard handles (3). The drift detection is intentionally
#   strict: any change to project name, copyright owner, or source
#   URL fails CI immediately.
#
# USAGE
# =====
#   From the repo root:
#       bash scripts/check_notice_canonical.sh
#
#   Exits 0 if every check passes, with a summary.
#   Exits 1 on the first failed check, with what was expected and why.
#
# WHAT IT CHECKS
# ==============
#   Regression detectors:
#     1. NO "MIT License" header (defense in depth — the same
#        threat model that motivated the LICENSE drift guard)
#     2. NO "YOUR NAME OR ORGANIZATION" placeholder
#
#   Project-specific structural:
#     3. "gool" project name on its own line near the top
#     4. "Copyright 20YY Brannen Graves" exactly
#     5. Source URL "https://github.com/siliconight/gool" present
#
# What this does NOT check:
#   - Byte-for-byte canonical form. A future patch may legitimately
#     add a one-line description change or fold in third-party
#     acknowledgments. The pattern checks above are strict enough
#     to catch attribution stripping but loose enough to allow
#     legitimate evolution.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f NOTICE ]; then
    echo "ERROR: NOTICE file does not exist at repo root."
    echo "       Apache 2.0 Section 4(d) requires forks to copy NOTICE"
    echo "       verbatim. Without one, downstream consumers don't"
    echo "       have to preserve any attribution beyond LICENSE."
    exit 1
fi

failed=0
fail() {
    echo "ERROR: $1"
    echo "       $2"
    failed=1
}

# --- Regression detectors ---

if grep -q "^MIT License" NOTICE; then
    fail "NOTICE starts with 'MIT License'." \
         "The MIT placeholder template has overwritten the project
       NOTICE content. Restore the canonical Apache 2.0 NOTICE."
fi

if grep -q "YOUR NAME OR ORGANIZATION" NOTICE; then
    fail "NOTICE contains '[YOUR NAME OR ORGANIZATION]'." \
         "This is the unfilled placeholder from the MIT scaffolding
       template. Replace with the canonical project NOTICE."
fi

# --- Project-specific structural ---

if ! grep -qE '^gool$' NOTICE; then
    fail "Missing 'gool' project name on its own line." \
         "Apache convention puts the project name as the first
       non-blank line of NOTICE. Expected exactly:
         gool
       on its own line near the top."
fi

if ! grep -qE '^Copyright 20[0-9]{2} Brannen Graves$' NOTICE; then
    fail "Missing project copyright line." \
         "Expected exactly:
         Copyright 20YY Brannen Graves
       on its own line. Update YY if the copyright year has rolled
       over (and remember every fork has to copy this verbatim)."
fi

if ! grep -qF 'https://github.com/siliconight/gool' NOTICE; then
    fail "Missing source URL." \
         "The NOTICE should include 'https://github.com/siliconight/gool'
       so downstream consumers can trace the project to its
       authoritative source."
fi

# --- Summary ---

if [ "$failed" -ne 0 ]; then
    echo ""
    echo "FAILED: NOTICE drift detected. See errors above. Run"
    echo "        git diff NOTICE  to see what changed."
    exit 1
fi

copyright_line=$(grep -E '^Copyright 20[0-9]{2} Brannen Graves$' NOTICE)
echo "OK: NOTICE is canonical (${copyright_line}, source URL present, project name 'gool')."
exit 0
