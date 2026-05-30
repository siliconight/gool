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

// audio_engine/material_eq.h
//
// Public surface for the offline material-EQ audition path.
//
// Two free-standing functions that apply the EQ shape described by a
// MaterialEqCurve (geometry_query.h) to a mono float buffer in place,
// using a LowShelf → Peak → HighShelf biquad chain with RBJ cookbook
// coefficients. This is the same DSP code the runtime impact-EQ and
// listener-EQ paths use — what the audition lets the designer hear is
// what the runtime will play.
//
// These are pure: they read no engine state, drive no audio device,
// allocate nothing, and are safe to call from any thread. The intent
// is to let the editor inspector preview a curve without needing an
// AudioRuntime instance or the /root/Gool autoload to be reachable —
// the audition button must work in the editor's SceneTree, which has
// no game-thread audio context.
//
// History:
//   v0.59.3: audition introduced inside the godot binding, reaching
//            into the engine's private dsp/biquad_filter.h. That
//            broke gdextension CI in v0.60.0 because the binding
//            target didn't have src/ on its include path.
//   v0.61.0: lifted the math into the public engine surface here.
//            The binding becomes a thin marshaler; the binding's
//            include of dsp/biquad_filter.h is gone; src/ stays
//            private to the library.

#ifndef AUDIO_ENGINE_MATERIAL_EQ_H
#define AUDIO_ENGINE_MATERIAL_EQ_H

#include "audio_engine/export.h"
#include "audio_engine/geometry_query.h"  // AudioMaterial, MaterialEqCurve

#include <cstddef>
#include <cstdint>

namespace audio {

// Apply `curve` to a mono float buffer in place.
//
// Chain: LowShelf → Peak → HighShelf. Shelf Q is fixed at 1.0
// (standard "no resonance" value, matching the runtime impact and
// listener chains which don't expose shelf Q in their authoring
// contract). Peak Q comes from `curve.midQ`.
//
// `intensity` scales all three band gains uniformly — the same
// multiplier the runtime applies in its scaled-EQ path. Passing 0.0
// zeroes every band gain and produces a unity passthrough; 1.0 is
// the curve as-authored; values above 1.0 amplify the shaping.
//
// `samples == nullptr` or `numSamples == 0`: no-op. `sampleRate == 0`
// is treated as 48000 (the engine's default and the rate the godot
// inspector's AudioStreamGenerator runs at).
AUDIO_ENGINE_EXPORT
void ProcessBufferThroughMaterialEqCurve(
    float* samples,
    std::size_t numSamples,
    const MaterialEqCurve& curve,
    float intensity = 1.0f,
    std::uint32_t sampleRate = 48000) noexcept;

// Convenience: look up the curve via MaterialEqByMaterial(material),
// then call ProcessBufferThroughMaterialEqCurve.
//
// Out-of-range materials are treated as AudioMaterial::Default
// (neutral curve, near-passthrough audition) — matching how the
// rest of the material API absorbs unknown ints. This is the right
// behavior: no EQ = no coloring, audible as silence-shaped noise
// rather than a mysterious crash.
AUDIO_ENGINE_EXPORT
void ProcessBufferThroughMaterialEq(
    float* samples,
    std::size_t numSamples,
    AudioMaterial material,
    float intensity = 1.0f,
    std::uint32_t sampleRate = 48000) noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_MATERIAL_EQ_H
