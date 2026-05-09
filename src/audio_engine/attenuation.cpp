// audio_engine/attenuation.cpp

#include "audio_engine/attenuation.h"

#include <algorithm>
#include <cmath>

namespace audio {

float ComputeAttenuationGain(const AttenuationSettings& s, float distance) noexcept {
    const float minD   = std::max(0.0f, s.minDistance);
    const float maxD   = std::max(minD + 1e-3f, s.maxDistance);
    const float floor  = std::clamp(s.volumeFloor, 0.0f, 1.0f);
    const float d      = std::max(0.0f, distance);

    if (d <= minD) return 1.0f;
    if (d >= maxD) return floor;

    // Normalized progression from minD to maxD: t in [0, 1].
    const float t = (d - minD) / (maxD - minD);

    float gain = 1.0f - t;  // Defensive default == Linear; covered switch below.
    switch (s.falloffModel) {
        case FalloffModel::Linear:
            gain = 1.0f - t;
            break;
        case FalloffModel::Logarithmic: {
            // Smooth log-style falloff. -6 dB at half-distance from source.
            // Simple approximation using minD/d so it matches inverse falloff
            // intuition while staying numerically tame.
            gain = minD / d;
            // Squash to [0,1]; minD/d is already at most 1 when d >= minD.
            break;
        }
        case FalloffModel::InverseSquare: {
            const float r = minD / d;
            gain = r * r;
            break;
        }
        case FalloffModel::CustomCurve:
            // Until the host-supplied curve API exists, fall back to the
            // logarithmic shape.
            gain = minD / d;
            break;
    }

    // Blend toward floor as we approach maxD so falloff hits floor exactly
    // at maxD instead of asymptoting elsewhere.
    gain = std::lerp(floor, gain, 1.0f - t);
    return std::clamp(gain, floor, 1.0f);
}

} // namespace audio
