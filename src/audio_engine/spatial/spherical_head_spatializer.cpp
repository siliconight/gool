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

// audio_engine/spatial/spherical_head_spatializer.cpp

#include "audio_engine/spatial/spherical_head_spatializer.h"
#include "audio_engine/attenuation.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

constexpr float kPi = 3.14159265358979323846f;

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

// Woodworth's spherical-head ITD formula. Returns the inter-aural time
// delay in seconds for an azimuth `theta` (radians from the median
// plane: 0 = ahead, +π/2 = full right, -π/2 = full left, ±π = behind).
//   ITD = (a/c) * (sin θ + θ)        for |θ| ≤ π/2
//   ITD = (a/c) * (sin θ + π - θ)    for |θ| > π/2
// The second branch reflects the contralateral path wrapping over the
// top of the sphere; without it, the formula would predict zero delay
// at θ = ±π, which is wrong (front/back symmetry exists for delay only
// at θ = 0 and θ = π).
inline float WoodworthItdSeconds(float theta, float headRadius, float speedOfSound) noexcept {
    const float absT  = std::fabs(theta);
    const float a_c   = headRadius / speedOfSound;
    if (absT <= kPi * 0.5f) {
        return a_c * (std::sin(absT) + absT);
    }
    return a_c * (std::sin(absT) + (kPi - absT));
}

} // namespace

SpatialParams SphericalHeadSpatializer::Calculate(
    const SpatialEmitterView&      emitter,
    const SpatialListenerView&     listener,
    const SpatialEnvironmentState& environment) const noexcept {
    SpatialParams out;

    if (!emitter.spatialized) {
        out.gain          = 1.0f;
        out.pan           = 0.0f;
        out.lowPassAmount = 0.0f;
        out.reverbSend    = environment.globalReverbSend;
        out.useBinaural   = false;     // 2D sources stay on the pan path
        return out;
    }

    const Vec3  toEmitter = Sub(emitter.position, listener.position);
    const float distance  = Length(toEmitter);

    // ---- Non-directional fields: identical to DefaultSpatializer -------
    AttenuationSettings att;
    att.minDistance  = emitter.minDistance;
    att.maxDistance  = emitter.maxDistance;
    att.volumeFloor  = emitter.volumeFloor;
    att.falloffModel = emitter.falloffModel;
    const float distGain = ComputeAttenuationGain(att, distance);

    const float absorption   = std::clamp(emitter.occlusionAmount,  0.0f, 1.0f);
    const float damping      = std::clamp(emitter.occlusionDamping, 0.0f, 1.0f);
    const float occGain      = std::lerp(1.0f, 0.35f, absorption);
    const float occLpfAmount = std::lerp(0.0f, 0.7f, damping);

    float airLpfAmount = 0.0f;
    if (environment.airAbsorptionPerMeter > 0.0f) {
        airLpfAmount = std::clamp(distance * environment.airAbsorptionPerMeter,
                                    0.0f, 1.0f);
    }
    const float baseLpfAmount = std::max(occLpfAmount, airLpfAmount);

    out.gain        = distGain * occGain;
    out.reverbSend  = environment.globalReverbSend;

    // ---- Directional fields: ITD + head-shadow ILD ---------------------
    const Vec3 fwdN  = Normalize(listener.forward, 0.0f);
    const Vec3 upN   = Normalize(listener.up,      0.0f);
    const Vec3 right = Cross(fwdN, upN);

    const Vec3 toEmitterN = (distance > 1e-6f)
        ? Vec3{toEmitter.x / distance, toEmitter.y / distance, toEmitter.z / distance}
        : Vec3{0.0f, 0.0f, -1.0f};

    // Azimuth in the horizontal plane. atan2(rightComponent, forwardComponent)
    // gives 0 in front, +π/2 to the right, ±π behind, -π/2 to the left.
    const float fwdComp   = Dot(toEmitterN, fwdN);
    const float rightComp = Dot(toEmitterN, right);
    const float azimuth   = std::atan2(rightComp, fwdComp);

    // Pan field is set for backward compatibility with code paths that
    // still read it, but the mixer ignores it when useBinaural=true.
    out.pan = std::clamp(rightComp, -1.0f, 1.0f);

    // ITD: the ipsilateral ear hears the sound first, the contralateral
    // ear hears it `itd` seconds later. Sign of azimuth picks which ear.
    const float c       = std::max(50.0f, environment.speedOfSound);
    const float itdSecs = WoodworthItdSeconds(azimuth, settings_.headRadiusMeters, c);

    // Convert to fractional samples at the engine's output rate. The
    // mixer is told the sample rate via Prepare, but the spatializer
    // doesn't have it directly; we report seconds-as-samples assuming
    // 48 kHz, which the mixer scales to its true rate. (The mixer's
    // delay line stores in samples relative to its own rate.)
    //
    // To keep the spatializer rate-agnostic, we instead store the delay
    // in seconds-equivalent and let the mixer scale. The simplest way:
    // multiply by a fixed reference rate the mixer also uses. The
    // engine standardises on 48 kHz internally; both DefaultSpatializer
    // and the mixer assume that. So:
    constexpr float kRefRate    = 48000.0f;
    const float     itdSamples  = itdSecs * kRefRate;

    if (azimuth >= 0.0f) {
        out.delaySamplesL = itdSamples;
        out.delaySamplesR = 0.0f;
    } else {
        out.delaySamplesL = 0.0f;
        out.delaySamplesR = itdSamples;
    }

    // ILD: head shadow on the contralateral ear. We model it as a
    // smooth ramp keyed on |azimuth|, peaking at full lateral. At
    // 0° azimuth (ahead) and ±180° (behind) the shadow is symmetric
    // and contributes nothing; this is the cone-of-confusion limit
    // we accept until pinna data arrives.
    //
    // The sin² mapping gives a smooth profile that peaks at ±π/2 and
    // returns to zero at 0 and ±π, matching the physical fact that
    // ILD is largest when one ear faces the source while the other
    // is in the geometric shadow.
    const float lateralFactor = std::sin(azimuth);     // -1 (left) .. +1 (right)
    const float shadowMag     = lateralFactor * lateralFactor;     // 0..1

    float gainL = 1.0f, gainR = 1.0f;
    float shadowLpfL = 0.0f, shadowLpfR = 0.0f;

    if (lateralFactor > 0.0f) {
        // Source is on the right side; left ear is in shadow.
        gainL      = std::lerp(1.0f, settings_.minShadowGain,    shadowMag);
        shadowLpfL = settings_.maxShadowLpfAmount * shadowMag;
    } else if (lateralFactor < 0.0f) {
        gainR      = std::lerp(1.0f, settings_.minShadowGain,    shadowMag);
        shadowLpfR = settings_.maxShadowLpfAmount * shadowMag;
    }

    out.useBinaural = true;
    out.gainL       = gainL;
    out.gainR       = gainR;
    // Per-ear LPF: the existing occlusion + air-absorption contribution
    // applies to both ears equally (it's an environmental cue, not a
    // directional one); the head-shadow contribution stacks on top of
    // that on the contralateral ear via max() the same way the other
    // sources combine.
    out.lpfAmountL  = std::max(baseLpfAmount, shadowLpfL);
    out.lpfAmountR  = std::max(baseLpfAmount, shadowLpfR);
    out.lowPassAmount = baseLpfAmount;     // legacy field, kept consistent

    // Doppler — same logic as DefaultSpatializer.
    if (environment.dopplerEnabled && distance > 1e-3f) {
        const Vec3 relVel{emitter.velocity.x - listener.velocity.x,
                           emitter.velocity.y - listener.velocity.y,
                           emitter.velocity.z - listener.velocity.z};
        const float radial        = Dot(relVel, toEmitterN);
        const float radialClamped = std::max(radial, -0.9f * c);
        out.pitch = std::clamp(c / (c + radialClamped), 0.5f, 2.0f);
    } else {
        out.pitch = 1.0f;
    }

    return out;
}

} // namespace audio
