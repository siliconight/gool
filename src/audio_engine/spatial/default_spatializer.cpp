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

// audio_engine/spatial/default_spatializer.cpp

#include "audio_engine/spatial/default_spatializer.h"
#include "audio_engine/attenuation.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

inline Vec3 Sub(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float Length(const Vec3& v) noexcept {
    return std::sqrt(Dot(v, v));
}

inline Vec3 Normalize(const Vec3& v, float fallbackX = 0.0f) noexcept {
    const float len = Length(v);
    if (len < 1e-6f) return Vec3{fallbackX, 0.0f, 0.0f};
    const float inv = 1.0f / len;
    return Vec3{v.x * inv, v.y * inv, v.z * inv};
}

} // namespace

SpatialParams DefaultSpatializer::Calculate(
    const SpatialEmitterView&     emitter,
    const SpatialListenerView&    listener,
    const SpatialEnvironmentState& environment) const noexcept {
    SpatialParams out;

    if (!emitter.spatialized) {
        // 2D source: no positional shaping.
        out.gain          = 1.0f;
        out.pan           = 0.0f;
        out.lowPassAmount = 0.0f;
        out.reverbSend    = environment.globalReverbSend;
        return out;
    }

    const Vec3 toEmitter = Sub(emitter.position, listener.position);
    const float distance = Length(toEmitter);

    // Distance attenuation
    AttenuationSettings att;
    att.minDistance  = emitter.minDistance;
    att.maxDistance  = emitter.maxDistance;
    att.volumeFloor  = emitter.volumeFloor;
    att.falloffModel = emitter.falloffModel;
    out.gain = ComputeAttenuationGain(att, distance);

    // Apply occlusion: split absorption (volume cue, applied to gain)
    // from damping (HF cue, applied to the per-voice LPF). When the
    // host's occlusion system fills both fields independently,
    // absorption and damping curves are exposed separately so a
    // curtain (modest level cut, heavy HF rolloff) sounds different
    // from concrete (strong both) or glass (mild both).
    const float absorption = std::clamp(emitter.occlusionAmount,  0.0f, 1.0f);
    const float damping    = std::clamp(emitter.occlusionDamping, 0.0f, 1.0f);
    out.gain *= std::lerp(1.0f, 0.35f, absorption);          // up to 65% gain reduction
    const float occLpfAmount = std::lerp(0.0f, 0.7f, damping);

    // Air absorption: highs roll off with distance, on top of any occlusion.
    // Combined via max() rather than addition; both mechanisms damp the
    // same biquad, so cascading would over-attenuate. The result is "the
    // dominant cause of muffle wins". Both saturate at 1.0.
    float airLpfAmount = 0.0f;
    if (environment.airAbsorptionPerMeter > 0.0f) {
        airLpfAmount = std::clamp(distance * environment.airAbsorptionPerMeter,
                                    0.0f, 1.0f);
    }
    out.lowPassAmount = std::max(occLpfAmount, airLpfAmount);

    // Stereo pan: project the emitter direction onto the listener's right
    // axis. right = forward x up (right-handed); we don't assume one
    // convention here; the host's forward+up implicitly defines it.
    const Vec3 fwdN = Normalize(listener.forward, 0.0f);
    const Vec3 upN  = Normalize(listener.up, 0.0f);
    const Vec3 right = Cross(fwdN, upN);

    const Vec3 toEmitterN = (distance > 1e-6f)
        ? Vec3{toEmitter.x / distance, toEmitter.y / distance, toEmitter.z / distance}
        : Vec3{0.0f, 0.0f, 0.0f};

    out.pan = std::clamp(Dot(toEmitterN, right), -1.0f, 1.0f);

    // Doppler-style pitch shift: simplistic radial-velocity model.
    if (environment.dopplerEnabled && distance > 1e-3f) {
        const Vec3 relVel{emitter.velocity.x - listener.velocity.x,
                           emitter.velocity.y - listener.velocity.y,
                           emitter.velocity.z - listener.velocity.z};
        const float radial = Dot(relVel, toEmitterN);     // +ve = receding
        const float c      = std::max(50.0f, environment.speedOfSound);
        // Clamp radial to (-0.9c, +inf) before applying the Doppler formula:
        // beyond −c the textbook ratio c/(c+v) goes singular then negative,
        // which our outer clamp would silently bend toward 0.5 (the wrong
        // direction for a fast-approach case). Capping at -0.9c keeps the
        // ratio well-defined and lets the outer clamp pin to 2.0 cleanly.
        const float radialClamped = std::max(radial, -0.9f * c);
        out.pitch = std::clamp(c / (c + radialClamped), 0.5f, 2.0f);
    } else {
        out.pitch = 1.0f;
    }

    out.reverbSend = environment.globalReverbSend;
    return out;
}

} // namespace audio
