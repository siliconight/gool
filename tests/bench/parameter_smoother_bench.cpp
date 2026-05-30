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

// tests/bench/parameter_smoother_bench.cpp
//
// Microbenchmark for ParameterSmoother under the load patterns the
// runtime actually generates:
//
//   * SetTarget hits an existing entry  — the common case (re-binding
//     a parameter every tick on the same emitter).
//   * SetTarget allocates a new entry   — emitter spawn case.
//   * Get hits an existing entry        — orchestrator step 9 reads
//     four parameters per active emitter per tick (Gain, Pitch,
//     LowPassAmount, ReverbSend).
//   * Tick walks every entry            — once per Update call.
//
// Decision rule: if any single op exceeds ~1 µs at N=256 entries
// (= a maxed-out runtime with multi-target RTPC on every voice),
// or the synthesized "per-tick" cost exceeds 100 µs, we have a
// candidate worth optimizing. Below those thresholds, B1 is noise
// at the budgets we ship with — leave the linear scan alone (Rule 27,
// boring code wins).

#include "audio_engine/orchestrator/parameter_smoother.h"
#include "audio_engine/types.h"
#include "audio_engine/handles.h"

#include "../bench/bench_util.h"

#include <vector>

using namespace audio;
using namespace audio_bench;

namespace {

// Build a smoother pre-populated with N (handle, paramId) entries
// arranged as if N emitters each have 4 active parameters
// (Gain, Pitch, LowPassAmount, ReverbSend).
ParameterSmoother BuildPopulated(size_t emitters) {
    ParameterSmoother sm;
    for (uint32_t i = 0; i < emitters; ++i) {
        EmitterHandle h{i + 1, 1};
        sm.SetTarget(h, AudioParameterIds::Gain,          1.0f, 0.0f);
        sm.SetTarget(h, AudioParameterIds::Pitch,         1.0f, 0.0f);
        sm.SetTarget(h, AudioParameterIds::LowPassAmount, 0.0f, 0.0f);
        sm.SetTarget(h, AudioParameterIds::ReverbSend,    0.0f, 0.0f);
    }
    return sm;
}

} // namespace

int main() {
    BenchSuite suite{"parameter_smoother"};

    // ---- SetTarget hits existing entry ----
    // Realistic runtime pattern: every tick, the RTPC evaluator calls
    // SetTarget on already-bound (emitter, param) pairs. This is the
    // first linear scan in SetTarget.
    for (size_t emitters : {16u, 64u, 256u, 1024u}) {
        auto sm = BuildPopulated(emitters);
        // Touch the LAST entry — worst-case linear scan depth.
        EmitterHandle hLast{static_cast<uint32_t>(emitters), 1};
        char label[64];
        std::snprintf(label, sizeof(label),
                       "SetTarget hit-existing (last)  N=%zu", emitters);
        suite.Run(label, 100000, [&] {
            sm.SetTarget(hLast, AudioParameterIds::Gain, 0.5f, 0.0f);
        });
    }

    // ---- SetTarget hits MIDDLE entry ----
    // Average-case linear scan depth.
    for (size_t emitters : {64u, 256u, 1024u}) {
        auto sm = BuildPopulated(emitters);
        EmitterHandle hMid{static_cast<uint32_t>(emitters / 2), 1};
        char label[64];
        std::snprintf(label, sizeof(label),
                       "SetTarget hit-existing (mid)   N=%zu", emitters);
        suite.Run(label, 100000, [&] {
            sm.SetTarget(hMid, AudioParameterIds::Gain, 0.5f, 0.0f);
        });
    }

    // ---- Get hits existing entry ----
    // Orchestrator step 9 hot path. 4 Get() calls per active emitter
    // per tick. Same linear scan inside the function.
    for (size_t emitters : {16u, 64u, 256u, 1024u}) {
        auto sm = BuildPopulated(emitters);
        EmitterHandle hLast{static_cast<uint32_t>(emitters), 1};
        char label[64];
        std::snprintf(label, sizeof(label),
                       "Get hit-existing (last)        N=%zu", emitters);
        volatile float sink = 0.0f;  // anti-DCE
        suite.Run(label, 100000, [&] {
            sink = sm.Get(hLast, AudioParameterIds::Gain, 1.0f);
            DoNotOptimize(sink);
        });
    }

    // ---- Tick (full walk) ----
    // Once per Update — but always walks every entry. Linear cost in N.
    for (size_t emitters : {16u, 64u, 256u, 1024u}) {
        auto sm = BuildPopulated(emitters);
        char label[64];
        std::snprintf(label, sizeof(label),
                       "Tick (full walk)               N=%zu", emitters);
        suite.Run(label, 10000, [&] {
            sm.Tick(1.0f / 60.0f);
        });
    }

    // ---- Synthetic "per-Update" cost ----
    // What does step 9 + RTPC eval cost the smoother PER TICK at
    // various emitter counts? Each tick = N SetTarget calls (RTPC
    // eval, 1 per emitter binding) + 4*N Get calls (step 9
    // user-gain/pitch/lpf/reverb) + 1 Tick call.
    for (size_t emitters : {16u, 64u, 256u}) {
        auto sm = BuildPopulated(emitters);
        std::vector<EmitterHandle> handles;
        handles.reserve(emitters);
        for (uint32_t i = 0; i < emitters; ++i) handles.push_back({i + 1, 1});
        char label[64];
        std::snprintf(label, sizeof(label),
                       "synthesized per-tick           N=%zu", emitters);
        volatile float sink = 0.0f;
        suite.Run(label, 1000, [&] {
            // RTPC eval: 1 SetTarget per emitter binding (volume).
            for (auto h : handles) {
                sm.SetTarget(h, AudioParameterIds::Gain, 0.5f, 0.0f);
            }
            // Step 9: 4 Get calls per emitter.
            for (auto h : handles) {
                sink += sm.Get(h, AudioParameterIds::Gain,          1.0f);
                sink += sm.Get(h, AudioParameterIds::Pitch,         1.0f);
                sink += sm.Get(h, AudioParameterIds::LowPassAmount, 0.0f);
                sink += sm.Get(h, AudioParameterIds::ReverbSend,    0.0f);
            }
            DoNotOptimize(sink);
            // 1 smoother Tick.
            sm.Tick(1.0f / 60.0f);
        });
    }

    return suite.Summary();
}
