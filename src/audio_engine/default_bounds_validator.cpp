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

// SPDX-License-Identifier: Apache-2.0
#include "audio_engine/default_bounds_validator.h"

#include <cmath>

namespace audio {

namespace {

inline bool Vec3IsFinite(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline float Vec3Magnitude(const Vec3& v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

bool DefaultBoundsValidator::ShouldAccept(const AudioEvent& event,
                                            AudioPlayerId /*playerId*/) noexcept {
    // Vec3 finite check covers position, forward, velocity in one
    // pass. NaN/Inf in any of these poisons downstream math.
    if (!Vec3IsFinite(event.position) ||
        !Vec3IsFinite(event.forward)  ||
        !Vec3IsFinite(event.velocity)) {
        cNonFiniteVec3_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Magnitude bounds for position. forward is a unit vector by
    // contract — we still cap it loosely under maxPositionMagnitude
    // to catch a host that forgot to normalize, but the practical
    // attack vector here is the position field.
    const float posMag = Vec3Magnitude(event.position);
    if (posMag > cfg_.maxPositionMagnitude) {
        cExtremePos_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const float velMag = Vec3Magnitude(event.velocity);
    if (velMag > cfg_.maxVelocityMagnitude) {
        cExtremeVel_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Parameter fields: NaN/Inf reject first, then magnitude.
    if (!std::isfinite(event.parameterValue) ||
        !std::isfinite(event.parameterSmoothingMs)) {
        cNonFiniteParam_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (std::abs(event.parameterValue) > cfg_.maxAbsParameterValue ||
        event.parameterSmoothingMs < 0.0f ||
        event.parameterSmoothingMs > cfg_.maxAbsParameterSmoothingMs) {
        cExtremeParam_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Optional soundId existence check via host-supplied lookup.
    if (cfg_.soundIdIsKnown && event.soundId != kInvalidSoundId &&
        !cfg_.soundIdIsKnown(event.soundId)) {
        cUnknownSound_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

DefaultBoundsValidator::Stats DefaultBoundsValidator::GetStats() const noexcept {
    Stats s;
    s.rejectedNonFiniteVec3   = cNonFiniteVec3_.load(std::memory_order_relaxed);
    s.rejectedExtremePosition = cExtremePos_.load(std::memory_order_relaxed);
    s.rejectedExtremeVelocity = cExtremeVel_.load(std::memory_order_relaxed);
    s.rejectedNonFiniteParam  = cNonFiniteParam_.load(std::memory_order_relaxed);
    s.rejectedExtremeParam    = cExtremeParam_.load(std::memory_order_relaxed);
    s.rejectedUnknownSound    = cUnknownSound_.load(std::memory_order_relaxed);
    return s;
}

} // namespace audio
