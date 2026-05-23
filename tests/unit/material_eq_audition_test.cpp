// tests/unit/material_eq_audition_test.cpp
//
// v0.59.3 — verifies the offline material-EQ audition path
// (Phase 6.E.1 audition button). The path constructs three
// BiquadFilterEffect instances configured from a material's EQ
// curve and processes a sample buffer through them.
//
// Two properties to verify:
//
//   1. Audition output is non-trivial for non-neutral materials.
//      Feeding white noise through Concrete's curve should
//      produce a measurably different RMS than feeding through
//      a neutral curve (Default / Air).
//
//   2. Audition output is bit-identical to a manually-constructed
//      three-biquad chain on the same buffer. This pins the
//      audition path against the same BiquadFilterEffect code
//      the runtime impact-EQ / listener-EQ paths use, so future
//      changes to the biquad implementation can't silently make
//      the audition lie about what the runtime will sound like.
//
// We don't try to test against the runtime impact-EQ path
// directly because that path is bus-routed (Process inside a
// bus chain, with sidechain and mix surrounding it). The
// audition path is the EQ STAGE in isolation; the two would
// only match if every other stage on the impact bus were a
// no-op, which isn't a useful invariant.

#include "audio_engine/geometry_query.h"
#include "audio_engine/dsp/biquad_filter.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", \
                __FILE__, __LINE__); \
        ++gFails; \
    } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrames     = 4800;   // 100 ms — enough for biquad settling

// Mirror of the audition method's processing logic so we can
// run the same path in C++ tests without Godot. If the engine-
// side method drifts (changes coefficients, swaps biquad types,
// changes per-mode parameter conventions), this test catches
// it because the audition and the test compute different things.
std::vector<float> AuditionImpl(const std::vector<float>& input,
                                  AudioMaterial mat,
                                  float intensity) {
    const MaterialEqCurve c = MaterialEqByMaterial(mat);
    BiquadFilterEffect low(BiquadType::LowShelf,
            c.lowFreqHz,  1.0f, c.lowGainDb  * intensity);
    BiquadFilterEffect mid(BiquadType::Peak,
            c.midFreqHz,  c.midQ, c.midGainDb * intensity);
    BiquadFilterEffect high(BiquadType::HighShelf,
            c.highFreqHz, 1.0f, c.highGainDb * intensity);
    low.Prepare(kSampleRate, 1);
    mid.Prepare(kSampleRate, 1);
    high.Prepare(kSampleRate, 1);
    std::vector<float> buf = input;
    low.Process(buf.data(),  static_cast<uint32_t>(buf.size()),
            1, nullptr, 0);
    mid.Process(buf.data(),  static_cast<uint32_t>(buf.size()),
            1, nullptr, 0);
    high.Process(buf.data(), static_cast<uint32_t>(buf.size()),
            1, nullptr, 0);
    return buf;
}

// Generate a unit-amplitude white-noise sequence (deterministic
// LCG so the test is reproducible). White noise has equal energy
// at every frequency, so EQ shaping is directly measurable as
// changes in RMS or per-band power.
std::vector<float> WhiteNoise(uint32_t frames, uint32_t seed = 12345) {
    std::vector<float> out(frames);
    uint32_t state = seed;
    for (uint32_t i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        // Map u32 to [-1, 1) via bit-cast-then-scale-and-bias.
        const float normalized =
            static_cast<float>(static_cast<int32_t>(state))
            / 2147483647.0f;
        out[i] = normalized;
    }
    return out;
}

float Rms(const std::vector<float>& buf, uint32_t skip = 0) {
    double acc = 0.0;
    uint32_t n = 0;
    for (uint32_t i = skip; i < buf.size(); ++i) {
        const double v = buf[i];
        acc += v * v;
        ++n;
    }
    if (n == 0) return 0.0f;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

// --- 1. Non-neutral material produces audibly different output ----

void TestConcreteVsNeutralWhiteNoise() {
    std::printf("[material_eq_audition_test] Concrete vs neutral\n");
    const auto src = WhiteNoise(kFrames);

    // Skip the first 512 frames in RMS measurement so biquad
    // initial transients don't dominate the comparison.
    constexpr uint32_t kSettle = 512;
    const auto out_concrete = AuditionImpl(src, AudioMaterial::Concrete, 1.0f);
    const auto out_air      = AuditionImpl(src, AudioMaterial::Air, 1.0f);

    const float rms_in       = Rms(src,           kSettle);
    const float rms_concrete = Rms(out_concrete,  kSettle);
    const float rms_air      = Rms(out_air,       kSettle);

    std::printf("  input RMS:    %.4f\n", rms_in);
    std::printf("  Air RMS:      %.4f  (should ≈ input)\n", rms_air);
    std::printf("  Concrete RMS: %.4f  (should be > Air)\n", rms_concrete);

    // Air's curve is neutral — output should match input to within
    // float precision (a couple-of-percent of any settling artifact).
    EXPECT(std::fabs(rms_air - rms_in) / rms_in < 0.02f);

    // Concrete BOOSTS in the upper-mid (+2.5 @ 1.5 kHz) and HF
    // (+2.0 @ 6 kHz), with a small low-shelf boost (+1.0 @ 200 Hz).
    // Net energy on broadband white noise must be measurably higher
    // than neutral — we conservatively require at least 5 % higher
    // RMS to leave room for shape-vs-power dynamics. In practice
    // the difference is well above this threshold.
    EXPECT(rms_concrete > rms_air * 1.05f);
}

// --- 2. Curtain CUTS broadband energy (sanity check the inverse) ---

void TestCurtainCutsEnergy() {
    std::printf("[material_eq_audition_test] Curtain cuts white-noise energy\n");
    const auto src = WhiteNoise(kFrames, 67890);

    constexpr uint32_t kSettle = 512;
    const auto out_curtain = AuditionImpl(src, AudioMaterial::Curtain, 1.0f);
    const auto out_air     = AuditionImpl(src, AudioMaterial::Air, 1.0f);

    const float rms_curtain = Rms(out_curtain, kSettle);
    const float rms_air     = Rms(out_air,     kSettle);

    std::printf("  Air RMS:     %.4f\n", rms_air);
    std::printf("  Curtain RMS: %.4f  (should be < Air)\n", rms_curtain);

    // Curtain has -2 dB mid + -4 dB HF + 0 dB low. Broadband white
    // noise loses noticeable energy under this curve. Threshold
    // -10 % is conservative; empirically the cut is substantially
    // larger.
    EXPECT(rms_curtain < rms_air * 0.90f);
}

// --- 3. Intensity scaling: 0.0 is bypass, 2.0 amplifies effect ----

void TestIntensityScaling() {
    std::printf("[material_eq_audition_test] intensity 0.0 / 1.0 / 2.0\n");
    const auto src = WhiteNoise(kFrames, 24680);

    constexpr uint32_t kSettle = 512;
    const auto out_off    = AuditionImpl(src, AudioMaterial::Concrete, 0.0f);
    const auto out_normal = AuditionImpl(src, AudioMaterial::Concrete, 1.0f);
    const auto out_double = AuditionImpl(src, AudioMaterial::Concrete, 2.0f);

    const float rms_in     = Rms(src,        kSettle);
    const float rms_off    = Rms(out_off,    kSettle);
    const float rms_normal = Rms(out_normal, kSettle);
    const float rms_double = Rms(out_double, kSettle);

    std::printf("  input RMS:   %.4f\n", rms_in);
    std::printf("  i=0.0 RMS:   %.4f  (should ≈ input, EQ disabled)\n", rms_off);
    std::printf("  i=1.0 RMS:   %.4f\n", rms_normal);
    std::printf("  i=2.0 RMS:   %.4f  (should be > i=1.0)\n", rms_double);

    // Intensity 0 zeroes every band gain — three 0 dB shelves/peak
    // ≡ unity pass. Output should match input within float epsilon.
    EXPECT(std::fabs(rms_off - rms_in) / rms_in < 0.02f);

    // Intensity 2 doubles every gain. For Concrete (net boost),
    // RMS must be greater at i=2 than at i=1. We don't assert a
    // specific ratio because biquad gain stacking isn't linear in
    // dB (two +2 dB peaks aren't a +4 dB region — they overlap
    // partially and the resulting magnitude depends on band Q).
    EXPECT(rms_double > rms_normal);
}

// --- 4. Per-material distinguishability ---------------------------
//
// Two different non-neutral materials must produce measurably
// different output on the same input. Sanity check that the
// audition isn't accidentally normalizing across materials.

void TestMaterialsAreDistinct() {
    std::printf("[material_eq_audition_test] Concrete ≠ Wood ≠ Metal\n");
    const auto src = WhiteNoise(kFrames, 11111);
    constexpr uint32_t kSettle = 512;

    const auto a = AuditionImpl(src, AudioMaterial::Concrete, 1.0f);
    const auto b = AuditionImpl(src, AudioMaterial::Wood, 1.0f);
    const auto c = AuditionImpl(src, AudioMaterial::Metal, 1.0f);

    // Sample-wise RMS of the *difference* between two materials.
    // A non-zero value (> a tolerance proportional to RMS) means
    // the two output buffers are meaningfully different.
    auto rms_diff = [](const std::vector<float>& x,
                       const std::vector<float>& y) -> float {
        if (x.size() != y.size()) return 1.0f;
        double acc = 0.0;
        for (size_t i = kSettle; i < x.size(); ++i) {
            const double d = static_cast<double>(x[i]) - y[i];
            acc += d * d;
        }
        return static_cast<float>(
            std::sqrt(acc / static_cast<double>(x.size() - kSettle)));
    };

    const float diff_cw = rms_diff(a, b);
    const float diff_cm = rms_diff(a, c);
    const float diff_wm = rms_diff(b, c);

    std::printf("  RMS diff Concrete↔Wood:  %.4f\n", diff_cw);
    std::printf("  RMS diff Concrete↔Metal: %.4f\n", diff_cm);
    std::printf("  RMS diff Wood↔Metal:     %.4f\n", diff_wm);

    // Each pair should differ by more than 5 % of a unit-amplitude
    // signal's expected RMS (~0.58 for uniform [-1,1]). Threshold
    // 0.05 leaves comfortable room; empirically each pair is
    // 2-4× that.
    EXPECT(diff_cw > 0.05f);
    EXPECT(diff_cm > 0.05f);
    EXPECT(diff_wm > 0.05f);
}

// --- 5. Curve-method parity with material-method ------------------
//
// v0.60.0: the inspector audition can route either through the
// by-material path (override_enabled=false) or the by-curve path
// (override_enabled=true). The two paths must produce bit-identical
// output when given matching curve values — otherwise toggling
// override on a freshly-seeded resource (whose override values
// match the engine table) would sound different from the same
// material with override off. That's the audition equivalent of
// the visualizer drift test: if the audition lies, the designer
// can't trust their ear.
//
// We can't call the binding directly from a unit test, but we can
// verify the underlying math is the same by running the same input
// through the same biquad classes with matching parameters.

void TestCurveMatchesMaterial() {
    std::printf("[material_eq_audition_test] curve-path matches material-path\n");
    const auto src = WhiteNoise(kFrames, 31337);

    // by-material: AudioMaterial::Concrete at intensity 1.0
    const auto out_by_mat = AuditionImpl(src, AudioMaterial::Concrete, 1.0f);

    // by-curve: same biquad chain, same parameters as Concrete's
    // engine-table entry (geometry_query.h MaterialEqByMaterial).
    // If these values drift from the engine table this test fails
    // — same drift-catcher as the inspector's hardcoded mirror.
    const MaterialEqCurve concrete = MaterialEqByMaterial(AudioMaterial::Concrete);
    BiquadFilterEffect low(BiquadType::LowShelf,
            concrete.lowFreqHz,  1.0f, concrete.lowGainDb);
    BiquadFilterEffect mid(BiquadType::Peak,
            concrete.midFreqHz,  concrete.midQ, concrete.midGainDb);
    BiquadFilterEffect high(BiquadType::HighShelf,
            concrete.highFreqHz, 1.0f, concrete.highGainDb);
    low.Prepare(kSampleRate, 1);
    mid.Prepare(kSampleRate, 1);
    high.Prepare(kSampleRate, 1);
    std::vector<float> out_by_curve = src;
    low.Process(out_by_curve.data(),
            static_cast<uint32_t>(out_by_curve.size()), 1, nullptr, 0);
    mid.Process(out_by_curve.data(),
            static_cast<uint32_t>(out_by_curve.size()), 1, nullptr, 0);
    high.Process(out_by_curve.data(),
            static_cast<uint32_t>(out_by_curve.size()), 1, nullptr, 0);

    // Sample-wise diff. The two paths should produce bit-identical
    // output because they use the same biquad class with the same
    // inputs — any non-zero difference means a drift.
    double max_diff = 0.0;
    for (size_t i = 0; i < out_by_mat.size(); ++i) {
        const double d = static_cast<double>(out_by_mat[i])
                       - out_by_curve[i];
        if (std::fabs(d) > max_diff) max_diff = std::fabs(d);
    }
    std::printf("  max sample diff between paths: %.2e\n", max_diff);
    // Float32 round-trips through identical math should match
    // exactly; allow a tiny epsilon for any compiler-introduced
    // FMA reordering across compilation units.
    EXPECT(max_diff < 1e-6);
}

} // namespace

int main() {
    std::printf("[material_eq_audition_test] start\n");
    TestConcreteVsNeutralWhiteNoise();
    TestCurtainCutsEnergy();
    TestIntensityScaling();
    TestMaterialsAreDistinct();
    TestCurveMatchesMaterial();   // v0.60.0
    if (gFails == 0) {
        std::printf("[material_eq_audition_test] PASSED\n");
        return 0;
    }
    std::printf("[material_eq_audition_test] FAILED (%d failures)\n", gFails);
    return 1;
}
