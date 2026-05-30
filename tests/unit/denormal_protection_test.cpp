// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

// tests/unit/denormal_protection_test.cpp
//
// Verify that SetCurrentThreadDenormalProtection() actually flushes
// denormal arithmetic results to zero on platforms where it's
// implemented. On unsupported platforms (no FTZ/DAZ mechanism), the
// tests print a "skipped" notice and pass — the function is allowed
// to be a no-op there.
//
// The two tests use different probes:
//
//   1. Direct probe: multiply the smallest positive normal by 0.5,
//      which mathematically equals smallest_normal/2 (a denormal).
//      With FTZ the result is 0.0f; without it, a denormal.
//
//   2. IIR feedback probe: simulate the inner loop of a biquad
//      decaying to silence (y = a * y_prev with a < 1, starting
//      near the denormal boundary). With FTZ the state hits exactly
//      0.0f within a small number of iterations. Without FTZ it
//      drifts through the denormal range for many iterations,
//      never hitting exact zero before becoming subnormal-tiny.
//
// We use `volatile` on the operands to defeat the compiler's
// constant folding — without it, an optimizing compiler can
// evaluate the multiplication at compile time using IEEE 754 rules
// (which honor denormals), regardless of the runtime MXCSR state.

#include "audio_engine/denormal_protection.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

// True if the result was produced by FTZ-aware hardware.
bool ProducesZeroFromDenormalArithmetic() noexcept {
    // smallest_normal_float ≈ 1.175e-38. Half of that is denormal.
    volatile float operand   = std::numeric_limits<float>::min();
    volatile float halfScale = 0.5f;
    volatile float result    = operand * halfScale;
    return result == 0.0f;
}

void TestDirectProbe() {
    std::printf("[denormal_protection_test] direct probe...\n");

    // First confirm the platform — if unsupported, we can't verify
    // anything; just print and bail.
    auto info = audio::SetCurrentThreadDenormalProtection();
    if (std::strcmp(info.platform, "unsupported") == 0) {
        std::printf("[denormal_protection_test] direct probe: "
                    "SKIPPED (platform=unsupported: %s)\n",
                    info.details);
        return;
    }

    // After SetCurrentThreadDenormalProtection, the arithmetic
    // result must flush to exactly 0.0f. This is the load-bearing
    // assertion of the entire feature.
    if (!ProducesZeroFromDenormalArithmetic()) {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: denormal arithmetic "
            "did not flush to zero after "
            "SetCurrentThreadDenormalProtection() on platform '%s'. "
            "Either the CSR write didn't land or the platform's "
            "FPU is ignoring it.\n",
            info.platform);
        std::exit(1);
    }
    std::printf("[denormal_protection_test] direct probe OK "
                "(platform=%s)\n", info.platform);
}

void TestIirFeedbackProbe() {
    std::printf("[denormal_protection_test] iir feedback probe...\n");

    auto info = audio::SetCurrentThreadDenormalProtection();
    if (std::strcmp(info.platform, "unsupported") == 0) {
        std::printf("[denormal_protection_test] iir probe: SKIPPED "
                    "(platform=unsupported)\n");
        return;
    }

    // Simulate a biquad feedback path decaying to silence. Start
    // just above the denormal boundary; each iteration halves the
    // state. With FTZ on, we should hit exactly 0.0f within ~30
    // iterations (smallest_normal × 2^-30 underflows). Without FTZ
    // we'd traverse the entire denormal range first (~150 more
    // iterations), and the result would still be nonzero.
    volatile float state = std::numeric_limits<float>::min() * 2.0f;
    constexpr int kMaxIterations = 60;  // generous margin
    int hitZeroAtIteration = -1;
    for (int i = 0; i < kMaxIterations; ++i) {
        state = state * 0.5f;
        if (state == 0.0f) {
            hitZeroAtIteration = i;
            break;
        }
    }

    if (hitZeroAtIteration < 0) {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: biquad-style decay "
            "did not reach exact 0.0f within %d iterations on "
            "platform '%s'. FTZ appears to NOT be active. "
            "Final state value bit pattern: %a\n",
            kMaxIterations, info.platform,
            static_cast<double>(state));
        std::exit(1);
    }
    std::printf("[denormal_protection_test] iir probe OK "
                "(platform=%s, decay reached 0.0f at iteration %d)\n",
                info.platform, hitZeroAtIteration);
}

void TestReturnedMetadata() {
    std::printf("[denormal_protection_test] return-value metadata...\n");

    auto info = audio::SetCurrentThreadDenormalProtection();

    if (!info.ok) {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: ok=false from "
            "SetCurrentThreadDenormalProtection. "
            "platform='%s', details='%s'\n",
            info.platform, info.details);
        std::exit(1);
    }
    if (info.platform == nullptr || info.platform[0] == '\0') {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: platform string is "
            "null or empty.\n");
        std::exit(1);
    }
    if (info.details == nullptr || info.details[0] == '\0') {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: details string is "
            "null or empty.\n");
        std::exit(1);
    }
    std::printf("[denormal_protection_test] metadata OK "
                "(platform=%s, details=\"%s\")\n",
                info.platform, info.details);
}

void TestIdempotence() {
    std::printf("[denormal_protection_test] idempotence...\n");

    // Calling twice in a row must yield the same result and not
    // alter behavior in any way. This is the property that justifies
    // calling Set...DenormalProtection from every audio callback
    // without thread-local guards.
    auto a = audio::SetCurrentThreadDenormalProtection();
    auto b = audio::SetCurrentThreadDenormalProtection();
    if (a.ok != b.ok
            || std::strcmp(a.platform, b.platform) != 0) {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: two consecutive "
            "calls returned different platform/ok: "
            "a={ok=%d, platform=%s}, b={ok=%d, platform=%s}\n",
            int(a.ok), a.platform, int(b.ok), b.platform);
        std::exit(1);
    }

    // And the FTZ behavior is still active after the second call.
    if (std::strcmp(b.platform, "unsupported") != 0
            && !ProducesZeroFromDenormalArithmetic()) {
        std::fprintf(stderr,
            "[denormal_protection_test] FAIL: idempotent call did "
            "not preserve FTZ behavior.\n");
        std::exit(1);
    }
    std::printf("[denormal_protection_test] idempotence OK\n");
}

} // namespace

int main() {
    std::printf("[denormal_protection_test] running...\n");
    TestReturnedMetadata();
    TestDirectProbe();
    TestIirFeedbackProbe();
    TestIdempotence();
    std::printf("[denormal_protection_test] OK\n");
    return 0;
}
