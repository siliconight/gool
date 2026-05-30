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

// tests/unit/material_eq_curve_test.cpp
//
// Verifies the v0.33.0 (Phase 6.A) per-material EQ curve table.
// These are pure data — no DSP involved — so the tests are
// structural rather than acoustic:
//
//   1. Every material returns a curve.
//   2. Curves are within sane physical ranges (frequencies in
//      [20, 20000], gains in [-12, +12] dB, Q in [0.1, 10]).
//   3. Air and Default are neutral (all gains 0 dB) and the
//      MaterialEqIsNeutral helper detects that.
//   4. The "loud" materials (Concrete, Metal) are NOT neutral —
//      they boost something, somewhere.
//   5. The "soft" materials (Curtain, Foliage) are NOT neutral —
//      they cut something, somewhere.
//   6. Materials are perceptually distinct from each other —
//      Concrete and Wood don't accidentally have the same curve,
//      etc.
//
// Audible quality of the curves is judged in-game; this test just
// catches accidental table corruption and ensures the design
// constraints survive future refactors.

#include "audio_engine/geometry_query.h"

#include <cmath>
#include <cstdio>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

bool InRange(float v, float lo, float hi) {
    return v >= lo && v <= hi;
}

bool CurveInPhysicalRange(const MaterialEqCurve& c) {
    // Sanity bounds: frequencies in human audible range, gains
    // within ±12 dB (this is "tasteful EQ" territory; if a curve
    // wants more it should signal intent to the reader).
    return InRange(c.lowFreqHz,  20.0f, 20000.0f)
        && InRange(c.midFreqHz,  20.0f, 20000.0f)
        && InRange(c.highFreqHz, 20.0f, 20000.0f)
        && InRange(c.lowGainDb,  -12.0f, 12.0f)
        && InRange(c.midGainDb,  -12.0f, 12.0f)
        && InRange(c.highGainDb, -12.0f, 12.0f)
        && InRange(c.midQ,        0.1f,  10.0f);
}

bool CurvesDifferent(const MaterialEqCurve& a, const MaterialEqCurve& b) {
    // Any single parameter differing by > 0.1 (dB or Hz) counts.
    // Two materials shouldn't be byte-identical.
    constexpr float kEps = 0.1f;
    return std::fabs(a.lowGainDb  - b.lowGainDb)  > kEps
        || std::fabs(a.lowFreqHz  - b.lowFreqHz)  > kEps
        || std::fabs(a.midGainDb  - b.midGainDb)  > kEps
        || std::fabs(a.midFreqHz  - b.midFreqHz)  > kEps
        || std::fabs(a.midQ       - b.midQ)       > kEps
        || std::fabs(a.highGainDb - b.highGainDb) > kEps
        || std::fabs(a.highFreqHz - b.highFreqHz) > kEps;
}

void TestEveryMaterialReturnsSomeCurve() {
    std::printf("[material_eq_curve_test] every material -> some curve\n");
    for (uint8_t i = 0; i < kAudioMaterialCount; ++i) {
        const auto m = static_cast<AudioMaterial>(i);
        const auto c = MaterialEqByMaterial(m);
        EXPECT(CurveInPhysicalRange(c));
    }
}

void TestAirAndDefaultAreNeutral() {
    std::printf("[material_eq_curve_test] Air + Default neutrality\n");
    const auto air     = MaterialEqByMaterial(AudioMaterial::Air);
    const auto def     = MaterialEqByMaterial(AudioMaterial::Default);
    EXPECT(MaterialEqIsNeutral(air));
    EXPECT(MaterialEqIsNeutral(def));
    // Out-of-range inputs fall through to Default and should
    // therefore also be neutral. (Tested via the binding layer in
    // GDScript; here we only check via the inline function.)
}

void TestLoudMaterialsBoostSomewhere() {
    std::printf("[material_eq_curve_test] Concrete + Metal NOT neutral\n");
    const auto concrete = MaterialEqByMaterial(AudioMaterial::Concrete);
    const auto metal    = MaterialEqByMaterial(AudioMaterial::Metal);

    EXPECT(!MaterialEqIsNeutral(concrete));
    EXPECT(!MaterialEqIsNeutral(metal));
    // Specifically: both should have a positive mid_gain (the
    // upper-mid "bite" that defines hard materials' character).
    EXPECT(concrete.midGainDb > 0.5f);
    EXPECT(metal.midGainDb    > 0.5f);
    std::printf("  concrete mid: %+.1f dB @ %.0f Hz (Q=%.2f)\n",
                concrete.midGainDb, concrete.midFreqHz, concrete.midQ);
    std::printf("  metal    mid: %+.1f dB @ %.0f Hz (Q=%.2f)\n",
                metal.midGainDb,    metal.midFreqHz,    metal.midQ);
}

void TestSoftMaterialsCutSomewhere() {
    std::printf("[material_eq_curve_test] Curtain + Foliage NOT neutral\n");
    const auto curtain = MaterialEqByMaterial(AudioMaterial::Curtain);
    const auto foliage = MaterialEqByMaterial(AudioMaterial::Foliage);

    EXPECT(!MaterialEqIsNeutral(curtain));
    EXPECT(!MaterialEqIsNeutral(foliage));
    // Both should cut the high band — that's the perceptual
    // signature of "soft" materials.
    EXPECT(curtain.highGainDb < -0.5f);
    EXPECT(foliage.highGainDb < -0.5f);
    std::printf("  curtain high: %+.1f dB @ %.0f Hz\n",
                curtain.highGainDb, curtain.highFreqHz);
    std::printf("  foliage high: %+.1f dB @ %.0f Hz\n",
                foliage.highGainDb, foliage.highFreqHz);
}

void TestMaterialsAreDistinctFromEachOther() {
    std::printf("[material_eq_curve_test] non-Default materials distinct\n");
    // Spot-check the materials a designer would most likely care
    // to distinguish: Glass / Wood / Concrete / Metal / Curtain /
    // Foliage / Meat / Cardboard / Rubber / Liquid. (Air and
    // Default are intentionally identical because both should be
    // neutral.)
    const AudioMaterial mats[] = {
        AudioMaterial::Glass, AudioMaterial::Wood,
        AudioMaterial::Drywall, AudioMaterial::Concrete,
        AudioMaterial::Metal, AudioMaterial::Curtain,
        AudioMaterial::Foliage, AudioMaterial::Meat,
        AudioMaterial::Cardboard, AudioMaterial::Rubber,
        AudioMaterial::Liquid,
    };
    constexpr size_t kN = sizeof(mats) / sizeof(mats[0]);
    for (size_t i = 0; i < kN; ++i) {
        for (size_t j = i + 1; j < kN; ++j) {
            const auto a = MaterialEqByMaterial(mats[i]);
            const auto b = MaterialEqByMaterial(mats[j]);
            EXPECT(CurvesDifferent(a, b));
        }
    }
}

} // namespace

int main() {
    std::printf("[material_eq_curve_test] running...\n");
    TestEveryMaterialReturnsSomeCurve();
    TestAirAndDefaultAreNeutral();
    TestLoudMaterialsBoostSomewhere();
    TestSoftMaterialsCutSomewhere();
    TestMaterialsAreDistinctFromEachOther();
    if (gFails == 0) {
        std::printf("[material_eq_curve_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[material_eq_curve_test] %d failure(s)\n", gFails);
    return 1;
}
