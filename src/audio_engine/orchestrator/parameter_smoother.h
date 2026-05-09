// audio_engine/orchestrator/parameter_smoother.h
//
// Per-(emitter, parameter) linear smoother. SetEmitterParameter records a
// target value and a smoothing duration; each Update tick advances current
// toward target by deltaSeconds / smoothingSeconds. Designed for runtime
// parameters (gain, pitch, vehicle RPM, etc.) where game-code wants to
// avoid pops from instantaneous changes.
//
// Storage is per-(slot, paramId) and is bounded by maxActiveEmitters and
// the number of distinct parameter IDs the host uses. The implementation
// uses a flat small-vector scan; that is fine at the budgets we ship
// with.

#ifndef AUDIO_ENGINE_ORCHESTRATOR_PARAMETER_SMOOTHER_H
#define AUDIO_ENGINE_ORCHESTRATOR_PARAMETER_SMOOTHER_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "audio_engine/handles.h"
#include "audio_engine/types.h"

namespace audio {

class ParameterSmoother {
public:
    void SetTarget(EmitterHandle handle,
                    AudioParameterId paramId,
                    float target,
                    float smoothingMs) {
        for (auto& e : entries_) {
            if (e.handle == handle && e.paramId == paramId) {
                e.target          = target;
                e.smoothingMs     = std::max(0.0f, smoothingMs);
                e.active          = true;
                return;
            }
        }
        // Allocate by reusing an inactive slot, else push.
        for (auto& e : entries_) {
            if (!e.active) {
                e.handle      = handle;
                e.paramId     = paramId;
                e.current     = target;     // first set: snap to target
                e.target      = target;
                e.smoothingMs = smoothingMs;
                e.active      = true;
                return;
            }
        }
        entries_.push_back({handle, paramId, target, target, smoothingMs, true});
    }

    // Drop all entries for a destroyed emitter.
    void Forget(EmitterHandle handle) {
        for (auto& e : entries_) {
            if (e.handle == handle) {
                e.active = false;
            }
        }
    }

    // Advance all entries toward their targets. Read out via Get().
    void Tick(float deltaSeconds) {
        const float dt = std::max(0.0f, deltaSeconds);
        for (auto& e : entries_) {
            if (!e.active) continue;
            if (e.smoothingMs <= 0.0f || std::abs(e.target - e.current) < 1e-6f) {
                e.current = e.target;
                continue;
            }
            const float step = (e.target - e.current) * std::min(1.0f, dt / (e.smoothingMs * 0.001f));
            e.current += step;
        }
    }

    // Returns the smoothed value for a (handle, paramId), or `fallback` if
    // not registered.
    float Get(EmitterHandle handle,
               AudioParameterId paramId,
               float fallback) const {
        for (const auto& e : entries_) {
            if (e.active && e.handle == handle && e.paramId == paramId) {
                return e.current;
            }
        }
        return fallback;
    }

    void Clear() { entries_.clear(); }

private:
    struct Entry {
        EmitterHandle    handle{};
        AudioParameterId paramId   = 0;
        float            current   = 0.0f;
        float            target    = 0.0f;
        float            smoothingMs = 0.0f;
        bool             active    = false;
    };
    std::vector<Entry> entries_;
};

} // namespace audio

#endif // AUDIO_ENGINE_ORCHESTRATOR_PARAMETER_SMOOTHER_H
