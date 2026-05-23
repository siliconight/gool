// tests/unit/material_eq_audition_test.cpp
//
// Verifies the offline material-EQ audition surface in
// audio_engine/material_eq.h — the same functions the editor
// inspector's audition button uses to preview a material curve
// without going through the bus chain.
//
// History:
//   v0.59.3: audition introduced inside the godot binding,
//            reaching into the engine's private
//            dsp/biquad_filter.h. Test mirrored the binding's
//            hand-rolled biquad chain.
//   v0.61.0: math moved into audio_engine/material_eq.h as
//            ProcessBufferThroughMaterialEqCurve / *Material.
//            This test now calls the public engine surface
//            directly. The binding is a thin marshaler over
//            the same functions, so the audition still reflects
//            what the runtime impact-EQ / listener-EQ paths
//            produce (they all share BiquadFilterEffect under
//            the hood).
//
// Properties verified:
//
//   1. Non-neutral materials produce audibly different output
//      than neutral ones (Concrete vs Air on white noise).
//   2. Curtain CUTS broadband energy (sanity check the inverse).
//   3. Intensity 0.0 ≡ unity passthrough; 2.0 amplifies relative
//      to 1.0.
//   4. Different materials produce measurably different outputs
//      on the same input (no accidental cross-material clamping).
//   5. By-curve and by-material paths are bit-identical when
//      given matching parameters — pins them against drift.
//
// We don't test against the runtime impact-EQ path directly
// because that path is bus-routed (Process inside a bus chain,
// with sidechain and mix surrounding it). The audition path is
// the EQ STAGE in isolation; the two would only match if every
// other stage on the impact bus were a no-op, which isn't a
// useful invariant.

#include "audio_engine/geometry_query.h"
#include "audio_engine/material_eq.h"

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

// Thin wrapper around the engine's by-material audition function.
// Returns a fresh buffer because the engine API is in-place; the
// tests below want to compare against an unmodified `input` buffer
// in the same scope, so we copy here once at the test boundary.
//
// v0.61.0: previously this re-implemented the biquad chain by hand
// (a tautology vs the binding). It now delegates to the canonical
// engine surface. Drift detection is automatic — a change to the
// biquad math is reflected here exactly because there is only one
// implementation.
std::vector<float> AuditionImpl(const std::vector<float>& input,
                                  AudioMaterial mat,
                                  float intensity) {
    std::vector<float> buf = input;
    ProcessBufferThroughMaterialEq(
        buf.data(), buf.size(), mat, intensity, kSampleRate);
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
// The inspector audition can route either through the by-material
// path (override_enabled=false on a GoolAudioMaterial resource) or
// the by-curve path (override_enabled=true with hand-tweaked band
// values). The two paths must produce bit-identical output when
// given matching curve values — otherwise toggling override on a
// freshly-seeded resource (whose override values match the engine
// table) would sound different from the same material with override
// off. If the audition lies, the designer can't trust their ear.
//
// v0.61.0: this test now drives both public engine surfaces
// (ProcessBufferThroughMaterialEq + ProcessBufferThroughMaterialEqCurve)
// directly, with matching parameters. The implementation guarantees
// bit-identity because the by-material function is literally a
// MaterialEqByMaterial lookup followed by the by-curve function —
// any drift between the paths would have to be in
// MaterialEqByMaterial itself, which is a separate concern.

void TestCurveMatchesMaterial() {
    std::printf("[material_eq_audition_test] curve-path matches material-path\n");
    const auto src = WhiteNoise(kFrames, 31337);

    // by-material: AudioMaterial::Concrete at intensity 1.0
    std::vector<float> out_by_mat = src;
    ProcessBufferThroughMaterialEq(
        out_by_mat.data(), out_by_mat.size(),
        AudioMaterial::Concrete, 1.0f, kSampleRate);

    // by-curve: same engine table entry, fetched explicitly.
    // If the binding's value-passing drifts from MaterialEqByMaterial
    // this test fails — same drift-catcher as the inspector's
    // hardcoded mirror.
    const MaterialEqCurve concrete = MaterialEqByMaterial(AudioMaterial::Concrete);
    std::vector<float> out_by_curve = src;
    ProcessBufferThroughMaterialEqCurve(
        out_by_curve.data(), out_by_curve.size(),
        concrete, 1.0f, kSampleRate);

    // Sample-wise diff. The two paths should produce bit-identical
    // output because by-material is implemented as a curve lookup
    // followed by the by-curve function — same biquad math, same
    // parameters, same order.
    double max_diff = 0.0;
    for (size_t i = 0; i < out_by_mat.size(); ++i) {
        const double d = static_cast<double>(out_by_mat[i])
                       - out_by_curve[i];
        if (std::fabs(d) > max_diff) max_diff = std::fabs(d);
    }
    std::printf("  max sample diff between paths: %.2e\n", max_diff);
    // Identical math through identical types: expect exact match.
    // Allow a tiny epsilon only as defense against any compiler-
    // introduced FMA reordering across compilation units.
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
