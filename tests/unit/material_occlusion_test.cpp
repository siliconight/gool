// tests/unit/material_occlusion_test.cpp
//
// Verifies that AudioMaterial presets (and the AudioOcclusionHit
// absorption/damping split) produce distinguishable spatial signatures.
// Curtain, concrete, and glass should each give different combinations
// of gain reduction and LPF amount through the same OcclusionSystem +
// DefaultSpatializer pipeline.

#include "audio_engine/geometry_query.h"
#include "audio_engine/spatializer.h"
#include "audio_engine/spatial/default_spatializer.h"
#include "audio_engine/spatial/occlusion_system.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

class MaterialQuery final : public IAudioGeometryQuery {
public:
    explicit MaterialQuery(AudioMaterial m) : material_(m) {}
    bool RaycastAudioOcclusion(const Vec3&, const Vec3&,
                                AudioOcclusionHit& hit) noexcept override {
        hit.hit       = true;
        hit.distance  = 1.0f;
        hit.material  = material_;
        return true;
    }
private:
    AudioMaterial material_;
};

struct Profile {
    float gain;
    float lpf;
};

Profile ProfileForMaterial(AudioMaterial m) {
    constexpr uint32_t kMaxEmitters = 4;
    OcclusionSystem occ(kMaxEmitters, /*raycastsPerFrame*/ kMaxEmitters);
    MaterialQuery   q(m);
    occ.SetGeometryQuery(&q);

    std::vector<Vec3>    pos(kMaxEmitters + 1);
    std::vector<uint8_t> occupied(kMaxEmitters + 1, 0);
    std::vector<float>   absorption(kMaxEmitters + 1, 0.0f);
    std::vector<float>   damping(kMaxEmitters + 1, 0.0f);
    occupied[1] = 1;
    pos[1]      = {5.0f, 0.0f, 0.0f};

    // Run several ticks so the smoother converges.
    for (int i = 0; i < 30; ++i) {
        occ.Update({0.0f, 0.0f, 0.0f}, pos.data(), occupied.data(),
                    absorption.data(), damping.data(), /*dt*/ 0.025f);
    }

    DefaultSpatializer sp;
    SpatialEmitterView e;
    e.position         = pos[1];
    e.spatialized      = true;
    e.minDistance      = 1.0f;
    e.maxDistance      = 100.0f;
    e.volumeFloor      = 1.0f;     // disable distance falloff for clean compare
    e.occlusionAmount  = absorption[1];
    e.occlusionDamping = damping[1];

    SpatialListenerView l;
    l.position = {0.0f, 0.0f, 0.0f};
    l.forward  = {0.0f, 0.0f, -1.0f};
    l.up       = {0.0f, 1.0f, 0.0f};
    SpatialEnvironmentState env;

    const SpatialParams r = sp.Calculate(e, l, env);
    return {r.gain, r.lowPassAmount};
}

void TestMaterialsAreDistinct() {
    const auto glass    = ProfileForMaterial(AudioMaterial::Glass);
    const auto curtain  = ProfileForMaterial(AudioMaterial::Curtain);
    const auto concrete = ProfileForMaterial(AudioMaterial::Concrete);

    std::printf("  glass    gain=%.3f lpf=%.3f\n", glass.gain,    glass.lpf);
    std::printf("  curtain  gain=%.3f lpf=%.3f\n", curtain.gain,  curtain.lpf);
    std::printf("  concrete gain=%.3f lpf=%.3f\n", concrete.gain, concrete.lpf);

    // Glass: light on both axes (audible through, slight HF cut).
    EXPECT(glass.gain  > 0.85f);
    EXPECT(glass.lpf   < 0.10f);

    // Curtain: modest gain reduction, heavy damping (the diagnostic
    // case for the split — damping high while absorption is low).
    EXPECT(curtain.gain > 0.75f);
    EXPECT(curtain.lpf  > 0.40f);

    // Concrete: strong on both axes.
    EXPECT(concrete.gain < 0.65f);
    EXPECT(concrete.lpf  > 0.40f);

    // Cross-comparisons: curtain damping >> glass damping;
    // concrete absorption > curtain absorption.
    EXPECT(curtain.lpf  > glass.lpf  * 2.0f);
    EXPECT(concrete.gain < curtain.gain - 0.05f);
}

void TestLegacyMaterialAbsorptionStillWorks() {
    // A host that only sets the legacy `materialAbsorption` field
    // should get behavior identical to pre-split: ResolveOcclusion
    // mirrors the value to both absorption and damping.
    constexpr uint32_t kMaxEmitters = 4;
    OcclusionSystem occ(kMaxEmitters, kMaxEmitters);

    class LegacyQuery final : public IAudioGeometryQuery {
    public:
        bool RaycastAudioOcclusion(const Vec3&, const Vec3&,
                                    AudioOcclusionHit& hit) noexcept override {
            hit.hit                = true;
            hit.materialAbsorption = 0.8f;
            // material left as Default, damping left at sentinel -1.
            return true;
        }
    } q;
    occ.SetGeometryQuery(&q);

    std::vector<Vec3>    pos(kMaxEmitters + 1);
    std::vector<uint8_t> occupied(kMaxEmitters + 1, 0);
    std::vector<float>   absorption(kMaxEmitters + 1, 0.0f);
    std::vector<float>   damping(kMaxEmitters + 1, 0.0f);
    occupied[1] = 1;
    pos[1]      = {5.0f, 0.0f, 0.0f};

    for (int i = 0; i < 30; ++i) {
        occ.Update({0.0f, 0.0f, 0.0f}, pos.data(), occupied.data(),
                    absorption.data(), damping.data(), 0.025f);
    }
    // Mirror property: absorption and damping should converge to the
    // same value because ResolveOcclusion mirrors materialAbsorption
    // to both fields when material is Default and damping < 0. With
    // the floor removed, both equal the input (0.8) directly.
    EXPECT(std::fabs(absorption[1] - 0.8f) < 0.02f);
    EXPECT(std::fabs(damping[1]    - 0.8f) < 0.02f);
}

} // namespace

int main() {
    std::printf("[material_occlusion_test] running...\n");
    TestMaterialsAreDistinct();
    TestLegacyMaterialAbsorptionStillWorks();
    if (gFails == 0) {
        std::printf("[material_occlusion_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[material_occlusion_test] %d failure(s)\n", gFails);
    return 1;
}
