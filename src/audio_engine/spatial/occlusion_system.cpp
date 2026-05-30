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

// audio_engine/spatial/occlusion_system.cpp

#include "audio_engine/spatial/occlusion_system.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {
constexpr float kSmoothingTimeSeconds = 0.15f;
}

OcclusionSystem::OcclusionSystem(uint32_t maxEmitters,
                                   uint32_t maxRaycastsPerFrame)
    : maxEmitters_(maxEmitters),
      maxRaycastsPerFrame_(maxRaycastsPerFrame),
      targetAbsorption_(static_cast<size_t>(maxEmitters) + 1, 0.0f),
      targetDamping_(static_cast<size_t>(maxEmitters) + 1, 0.0f) {}

void OcclusionSystem::Reset() {
    std::fill(targetAbsorption_.begin(), targetAbsorption_.end(), 0.0f);
    std::fill(targetDamping_.begin(),    targetDamping_.end(),    0.0f);
    scheduleCursor_     = 1;
    lastFrameRaycasts_  = 0;
}

void OcclusionSystem::SetIntensity(float intensity) noexcept {
    // Clamp to a wide-but-bounded range. Below 0 doesn't make
    // physical sense; above ~3 starts to flatten everything at the
    // [0,1] post-clamp ceiling regardless of material (every wall
    // becomes "wall").
    intensity_ = std::clamp(intensity, 0.0f, 3.0f);
}

void OcclusionSystem::Update(const Vec3&    listenerPos,
                              const Vec3*    emitterPositions,
                              const uint8_t* slotOccupied,
                              float*         absorptionOut,
                              float*         dampingOut,
                              float          deltaSeconds) {
    if (!emitterPositions || !slotOccupied || !absorptionOut || !dampingOut) return;

    // 1. Raycast a budgeted slice this frame. ResolveOcclusion turns
    //    the host's hit (which may carry a material preset, explicit
    //    absorption + damping, or just legacy materialAbsorption) into
    //    a (absorption, damping) pair. Both pass through directly:
    //    the old `0.5 + 0.5*absorption` shaping that floored every
    //    blocked hit at 50% reduction was a single-knob workaround
    //    and no longer makes sense once materials specify absorption
    //    independently. Glass-blocked hits should now produce the
    //    light reduction the host requested, not a 50% floor.
    uint32_t casts = 0;
    if (geometry_ && maxRaycastsPerFrame_ > 0 && maxEmitters_ > 0) {
        const uint32_t budget = std::min(maxRaycastsPerFrame_, maxEmitters_);
        for (uint32_t i = 0; i < budget; ++i) {
            const uint32_t slot = scheduleCursor_;
            scheduleCursor_     = (scheduleCursor_ % maxEmitters_) + 1;

            if (!slotOccupied[slot]) continue;

            AudioOcclusionHit hit{};
            const bool blocked = geometry_->RaycastAudioOcclusion(
                listenerPos, emitterPositions[slot], hit);
            if (blocked) {
                float abs = 0.0f, dmp = 0.0f;
                ResolveOcclusion(hit, abs, dmp);
                // Apply global intensity multiplier, then clamp to
                // [0,1]. Materials with strong defaults (e.g.
                // Concrete at 0.90 absorption) saturate first as
                // intensity rises above 1; weaker materials retain
                // headroom for designers to push harder.
                targetAbsorption_[slot] = std::clamp(abs * intensity_, 0.0f, 1.0f);
                targetDamping_[slot]    = std::clamp(dmp * intensity_, 0.0f, 1.0f);
            } else {
                targetAbsorption_[slot] = 0.0f;
                targetDamping_[slot]    = 0.0f;
            }
            ++casts;
        }
    }
    lastFrameRaycasts_ = casts;

    // 2. Smooth current toward target on every slot. Frame-rate independent
    //    exponential smoothing.
    const float dt    = std::max(0.0f, deltaSeconds);
    const float alpha = kSmoothingTimeSeconds > 1e-4f
        ? 1.0f - std::exp(-dt / kSmoothingTimeSeconds)
        : 1.0f;

    for (uint32_t slot = 1; slot <= maxEmitters_; ++slot) {
        if (!slotOccupied[slot]) {
            absorptionOut[slot]      = 0.0f;
            dampingOut[slot]         = 0.0f;
            targetAbsorption_[slot]  = 0.0f;
            targetDamping_[slot]     = 0.0f;
            continue;
        }
        absorptionOut[slot] = std::lerp(absorptionOut[slot],
                                          targetAbsorption_[slot], alpha);
        dampingOut[slot]    = std::lerp(dampingOut[slot],
                                          targetDamping_[slot],    alpha);
    }
}

} // namespace audio
