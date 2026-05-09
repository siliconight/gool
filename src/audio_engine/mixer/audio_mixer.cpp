// audio_engine/mixer/audio_mixer.cpp

#include "audio_engine/mixer/audio_mixer.h"

#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/dsp/dsp_effect.h"
#include "audio_engine/util/pcm_ring.h"
#include "audio_engine/util/pcm_ring_f32.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio {

namespace {

// Equal-power pan: input pan in [-1, +1], output left/right gains.
void EqualPowerPan(float pan, float& outL, float& outR) noexcept {
    const float p = std::clamp(pan * 0.5f + 0.5f, 0.0f, 1.0f);
    constexpr float kPi2 = 1.5707963267948966f;
    const float angle    = p * kPi2;
    outL = std::cos(angle);
    outR = std::sin(angle);
}

// Map a 0..1 occlusion / damping amount to a biquad LPF cutoff in Hz.
// Exponential curve so the muffle ramps in fast; at amount=0.5 the
// cutoff is already ~3.3 kHz, at the spatializer's 0.7 cap it's ~1.7 kHz.
//   0.0 → 22 kHz  (effectively bypass)
//   0.5 →  3.3 kHz
//   0.7 →  1.7 kHz
//   1.0 →  0.5 kHz
inline float LpfCutoffHzFromAmount(float amount) noexcept {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    // 22000 * 0.022727^amount.
    return 22000.0f * std::pow(0.022727f, a);
}

// Compute biquad-LPF cookbook coefficients (Robert Bristow-Johnson) for the
// given cutoff and Q at the given sample rate, normalised by a0. Q=0.707
// gives a maximally-flat (Butterworth) magnitude response at the corner.
inline void ComputeLpfCoeffs(float cutoffHz, float sampleRate, float Q,
                              float& b0, float& b1, float& b2,
                              float& a1, float& a2) noexcept {
    const float ny     = 0.45f * sampleRate;
    const float fc     = std::clamp(cutoffHz, 20.0f, ny);
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float w0     = kTwoPi * fc / sampleRate;
    const float cosw0  = std::cos(w0);
    const float sinw0  = std::sin(w0);
    const float alpha  = sinw0 / (2.0f * std::max(0.05f, Q));

    const float a0 = 1.0f + alpha;
    const float inv = 1.0f / a0;
    b0 = ((1.0f - cosw0) * 0.5f) * inv;
    b1 = (1.0f - cosw0)           * inv;
    b2 = ((1.0f - cosw0) * 0.5f) * inv;
    a1 = (-2.0f * cosw0)         * inv;
    a2 = (1.0f - alpha)           * inv;
}

// Map a 0..1 lowPassAmount to a biquad cutoff frequency. Mirrors the
// LpfCutoffHzFromAmount helper used on the non-binaural path so both
// curves match perceptually: amount=0 keeps highs intact; amount~1 cuts
// down to ~500 Hz.
inline float LpfCutoffFromAmountBin(float amount) noexcept {
    constexpr float kMaxHz = 22000.0f;
    constexpr float kBase  = 0.022727f;     // 22000 * 0.022727 ≈ 500 Hz at amount=1
    return kMaxHz * std::pow(kBase, std::clamp(amount, 0.0f, 1.0f));
}

} // namespace

// static
void AudioMixer::CopyBinauralFromCmd(MixVoice& v,
                                       const MixerCommand& cmd,
                                       bool resetCurrent,
                                       float sampleRate) noexcept {
    v.useBinaural   = cmd.useBinaural;
    v.gainL         = cmd.gainL;
    v.gainR         = cmd.gainR;
    v.delaySamplesL = cmd.delaySamplesL;
    v.delaySamplesR = cmd.delaySamplesR;
    if (resetCurrent) {
        v.delaySamplesLCurrent = cmd.delaySamplesL;
        v.delaySamplesRCurrent = cmd.delaySamplesR;
        // Reset delay-line state too; otherwise the new voice would
        // briefly pull stale samples from the previous voice that lived
        // in this slot.
        for (auto& s : v.delayBufL) s = 0.0f;
        for (auto& s : v.delayBufR) s = 0.0f;
        v.delayWritePos = 0;
        v.lpfBinZ1L = v.lpfBinZ2L = 0.0f;
        v.lpfBinZ1R = v.lpfBinZ2R = 0.0f;
    }
    // Recompute per-ear LPF coefficients only when the amount changes
    // meaningfully (state is preserved across the change to avoid clicks).
    if (std::fabs(cmd.lpfAmountL - v.lpfAmountL) > 0.001f) {
        v.lpfAmountL = cmd.lpfAmountL;
        if (v.lpfAmountL > 0.001f) {
            ComputeLpfCoeffs(LpfCutoffFromAmountBin(v.lpfAmountL),
                              sampleRate, 0.7071f,
                              v.lpfBinB0L, v.lpfBinB1L, v.lpfBinB2L,
                              v.lpfBinA1L, v.lpfBinA2L);
        }
    }
    if (std::fabs(cmd.lpfAmountR - v.lpfAmountR) > 0.001f) {
        v.lpfAmountR = cmd.lpfAmountR;
        if (v.lpfAmountR > 0.001f) {
            ComputeLpfCoeffs(LpfCutoffFromAmountBin(v.lpfAmountR),
                              sampleRate, 0.7071f,
                              v.lpfBinB0R, v.lpfBinB1R, v.lpfBinB2R,
                              v.lpfBinA1R, v.lpfBinA2R);
        }
    }
}

AudioMixer::AudioMixer(uint32_t maxMixVoices,
                       uint32_t outputChannels,
                       uint32_t commandRingDepth,
                       BusGraph* busGraph,
                       uint32_t sampleRate)
    : maxVoices_(maxMixVoices),
      outputChannels_(outputChannels),
      sampleRate_(sampleRate),
      voices_(maxMixVoices),
      commands_(commandRingDepth),
      busGraph_(busGraph),
      voiceScratchInt16_(static_cast<size_t>(2048) * 2)
{
    // Per-voice binaural delay-line buffers. Sized for the worst-case
    // ITD plus headroom at sample rates up to 96 kHz: ITD_max at 48 kHz
    // is ~32 samples, so 192 samples covers up to 96 kHz with margin.
    // Allocated here once per voice; never resized after Initialize.
    constexpr uint32_t kBinauralDelayCapacity = 192;
    for (auto& v : voices_) {
        v.delayBufL.assign(kBinauralDelayCapacity, 0.0f);
        v.delayBufR.assign(kBinauralDelayCapacity, 0.0f);
    }
}

bool AudioMixer::PostCommand(const MixerCommand& cmd) noexcept {
    return commands_.Push(cmd);
}

void AudioMixer::DrainCommands() noexcept {
    MixerCommand cmd;
    while (commands_.Pop(cmd)) {
        switch (cmd.kind) {
            case MixerCommandKind::StartSound: {
                if (cmd.mixSlot >= maxVoices_) break;
                MixVoice& v   = voices_[cmd.mixSlot];
                v.mode        = VoiceMode::Sound;
                if (cmd.fadeInMs > 0.0f) {
                    const uint32_t f = static_cast<uint32_t>(
                        (cmd.fadeInMs / 1000.0f) * static_cast<float>(sampleRate_));
                    v.fadeRemainingFrames = f;
                    v.fadeTotalFrames     = f;
                    v.fadeDirection       = 2;
                } else {
                    v.fadeRemainingFrames = 0;
                    v.fadeTotalFrames     = 0;
                    v.fadeDirection       = 0;
                }
                v.gain        = cmd.gain;
                v.pan         = cmd.pan;
                v.pitch       = cmd.pitch;
                v.pitchCurrent = cmd.pitch;     // start ramp at target; no click on Start
                v.targetBus   = cmd.targetBus;
                v.pcmData     = cmd.pcmData;
                v.pcmFrames   = cmd.pcmFrames;
                v.pcmChannels = cmd.pcmChannels;
                v.cursor      = 0.0;
                v.looping     = cmd.looping;
                v.loopXfadeFrames = (cmd.looping && cmd.loopXfadeFrames * 2 < cmd.pcmFrames)
                                      ? cmd.loopXfadeFrames : 0u;
                v.voiceRing   = nullptr;
                v.streamRing  = nullptr;
                // Reset LPF state. Recompute coefficients only if the
                // voice starts already-occluded (uncommon, but possible).
                v.lpfAmount   = cmd.lowPassAmount;
                v.reverbSend  = cmd.reverbSend;
                CopyBinauralFromCmd(v, cmd, /*resetCurrent*/true, static_cast<float>(sampleRate_));
                v.lpfZ1[0] = v.lpfZ1[1] = 0.0f;
                v.lpfZ2[0] = v.lpfZ2[1] = 0.0f;
                if (v.lpfAmount > 0.001f) {
                    ComputeLpfCoeffs(LpfCutoffHzFromAmount(v.lpfAmount),
                                     static_cast<float>(sampleRate_), 0.7071f,
                                     v.lpfB0, v.lpfB1, v.lpfB2, v.lpfA1, v.lpfA2);
                }
            } break;
            case MixerCommandKind::StartStreamingSound: {
                if (cmd.mixSlot >= maxVoices_) break;
                MixVoice& v      = voices_[cmd.mixSlot];
                v.mode           = VoiceMode::StreamingSound;
                if (cmd.fadeInMs > 0.0f) {
                    const uint32_t f = static_cast<uint32_t>(
                        (cmd.fadeInMs / 1000.0f) * static_cast<float>(sampleRate_));
                    v.fadeRemainingFrames = f;
                    v.fadeTotalFrames     = f;
                    v.fadeDirection       = 2;
                } else {
                    v.fadeRemainingFrames = 0;
                    v.fadeTotalFrames     = 0;
                    v.fadeDirection       = 0;
                }
                v.gain           = cmd.gain;
                v.pan            = cmd.pan;
                v.pitch          = 1.0f;       // streaming sources are pre-resampled; pitch unused
                v.pitchCurrent   = 1.0f;
                v.targetBus      = cmd.targetBus;
                v.pcmData        = nullptr;
                v.pcmFrames      = 0;
                v.pcmChannels    = 0;
                v.cursor         = 0.0;
                v.looping        = cmd.looping;
                v.loopXfadeFrames = 0u;   // streaming sources have no fixed buffer
                v.voiceRing      = nullptr;
                v.streamRing     = cmd.streamRing;
                v.streamChannels = cmd.streamChannels;
                v.lpfAmount      = cmd.lowPassAmount;
                v.reverbSend     = cmd.reverbSend;
                CopyBinauralFromCmd(v, cmd, /*resetCurrent*/true, static_cast<float>(sampleRate_));
                v.lpfZ1[0] = v.lpfZ1[1] = 0.0f;
                v.lpfZ2[0] = v.lpfZ2[1] = 0.0f;
                if (v.lpfAmount > 0.001f) {
                    ComputeLpfCoeffs(LpfCutoffHzFromAmount(v.lpfAmount),
                                     static_cast<float>(sampleRate_), 0.7071f,
                                     v.lpfB0, v.lpfB1, v.lpfB2, v.lpfA1, v.lpfA2);
                }
            } break;
            case MixerCommandKind::StartVoice: {
                if (cmd.mixSlot >= maxVoices_) break;
                MixVoice& v     = voices_[cmd.mixSlot];
                v.mode          = VoiceMode::Voice;
                v.fadeRemainingFrames = 0;
                v.fadeTotalFrames     = 0;
                v.fadeDirection       = 0;
                v.gain          = cmd.gain;
                v.pan           = cmd.pan;
                v.pitch         = 1.0f;
                v.pitchCurrent  = 1.0f;
                v.targetBus     = cmd.targetBus;
                v.pcmData       = nullptr;
                v.pcmFrames     = 0;
                v.pcmChannels   = 0;
                v.cursor        = 0.0;
                v.looping       = false;
                v.loopXfadeFrames = 0u;
                v.voiceRing     = cmd.voiceRing;
                v.voiceChannels = cmd.voiceChannels;
                v.streamRing    = nullptr;
                v.lpfAmount     = cmd.lowPassAmount;
                v.reverbSend    = cmd.reverbSend;
                CopyBinauralFromCmd(v, cmd, /*resetCurrent*/true, static_cast<float>(sampleRate_));
                v.lpfZ1[0] = v.lpfZ1[1] = 0.0f;
                v.lpfZ2[0] = v.lpfZ2[1] = 0.0f;
                if (v.lpfAmount > 0.001f) {
                    ComputeLpfCoeffs(LpfCutoffHzFromAmount(v.lpfAmount),
                                     static_cast<float>(sampleRate_), 0.7071f,
                                     v.lpfB0, v.lpfB1, v.lpfB2, v.lpfA1, v.lpfA2);
                }
            } break;
            case MixerCommandKind::UpdateParams: {
                if (cmd.mixSlot >= maxVoices_) break;
                MixVoice& v = voices_[cmd.mixSlot];
                if (v.mode != VoiceMode::Inactive) {
                    v.gain       = cmd.gain;
                    v.pan        = cmd.pan;
                    v.pitch      = cmd.pitch;
                    v.reverbSend = cmd.reverbSend;
                    CopyBinauralFromCmd(v, cmd, /*resetCurrent*/false, static_cast<float>(sampleRate_));
                    // Recompute LPF coeffs only when the amount actually
                    // changed by a perceptible step. Filter STATE is
                    // preserved across param updates so there are no
                    // clicks; only coefficients are swapped.
                    if (std::fabs(cmd.lowPassAmount - v.lpfAmount) > 0.001f) {
                        v.lpfAmount = cmd.lowPassAmount;
                        if (v.lpfAmount > 0.001f) {
                            ComputeLpfCoeffs(LpfCutoffHzFromAmount(v.lpfAmount),
                                             static_cast<float>(sampleRate_), 0.7071f,
                                             v.lpfB0, v.lpfB1, v.lpfB2, v.lpfA1, v.lpfA2);
                        }
                    }
                }
            } break;
            case MixerCommandKind::Stop: {
                if (cmd.mixSlot >= maxVoices_) break;
                MixVoice& v  = voices_[cmd.mixSlot];
                if (v.mode == VoiceMode::Inactive) break;
                if (cmd.fadeOutMs > 0.0f) {
                    const uint32_t fadeFrames = static_cast<uint32_t>(
                        (cmd.fadeOutMs / 1000.0f) * static_cast<float>(sampleRate_));
                    v.fadeRemainingFrames = fadeFrames;
                    v.fadeTotalFrames     = fadeFrames;
                    v.fadeDirection       = 1;
                } else {
                    v.mode       = VoiceMode::Inactive;
                    v.pcmData    = nullptr;
                    v.voiceRing  = nullptr;
                    v.streamRing = nullptr;
                    v.fadeRemainingFrames = 0;
                    v.fadeTotalFrames     = 0;
                    v.fadeDirection       = 0;
                }
            } break;
            case MixerCommandKind::SetBusGain: {
                if (busGraph_) busGraph_->SetBusOutputGainDb(cmd.busId, cmd.paramValue);
            } break;
            case MixerCommandKind::SetEffectParameter: {
                if (!busGraph_) break;
                const uint32_t bi = busGraph_->IndexOf(cmd.busId);
                if (bi == BusGraph::kInvalidIndex) break;
                IDspEffect* fx = busGraph_->EffectAt(bi, cmd.effectIndex);
                if (fx) fx->OnParameter(cmd.paramId, cmd.paramValue);
            } break;
        }
    }
}

void AudioMixer::MixVoiceIntoBus(MixVoice& v, uint32_t frames, uint32_t channels) noexcept {
    if (!busGraph_) return;

    // Resolve target bus index. If the voice carries kInvalidBusId (host
    // didn't set it), fall back to master.
    uint32_t busIdx = (v.targetBus == kInvalidBusId)
        ? busGraph_->MasterIndex()
        : busGraph_->IndexOf(v.targetBus);
    if (busIdx == BusGraph::kInvalidIndex) busIdx = busGraph_->MasterIndex();

    float* dst = busGraph_->InputBuffer(busIdx);

    // Reverb send: optional. If a reverb bus is present and the voice has
    // a non-zero send level, we'll additionally accumulate the post-LPF,
    // post-pan signal into that bus's input buffer scaled by reverbSend.
    // No allocation, no branching cost when reverbSend is zero.
    float* reverbDst = nullptr;
    if (v.reverbSend > 0.001f && reverbBusIndex_ != BusGraph::kInvalidIndex) {
        reverbDst = busGraph_->InputBuffer(reverbBusIndex_);
    }

    float gL = v.gain, gR = v.gain;
    if (channels == 2) {
        float panL = 1.0f, panR = 1.0f;
        EqualPowerPan(v.pan, panL, panR);
        gL *= panL;
        gR *= panR;
    }
    const float sendL = gL * v.reverbSend;
    const float sendR = gR * v.reverbSend;

    if (v.mode == VoiceMode::Sound) {
        const float* src = v.pcmData;
        if (!src || v.pcmFrames == 0) {
            v.mode = VoiceMode::Inactive;
            return;
        }
        const uint32_t srcChans = v.pcmChannels;
        // Per-block linear pitch ramp: interpolate from pitchCurrent toward
        // the target `pitch` over this buffer. dStep = 0 in steady state
        // (current already equals target), so the cost is one extra add/mul
        // per frame compared to the constant-pitch case.
        double         curStep  = static_cast<double>(v.pitchCurrent);
        const double   tgtStep  = static_cast<double>(v.pitch);
        const double   dStep    = (frames > 0)
            ? (tgtStep - curStep) / static_cast<double>(frames)
            : 0.0;
        double         cursor   = v.cursor;
        const double   frameMax = static_cast<double>(v.pcmFrames);
        const bool     useLpf   = (v.lpfAmount > 0.001f);
        const uint32_t loopXf   = v.loopXfadeFrames;
        const bool     useLoopXf = (loopXf > 0u);
        const double   wrapTo   = useLoopXf ? static_cast<double>(loopXf) : 0.0;
        const double   loopSpan = frameMax - wrapTo;          // > 0 (validated at Start)

        for (uint32_t f = 0; f < frames; ++f) {
            if (cursor >= frameMax) {
                if (v.looping) {
                    // With loop crossfade, jump to loopXf rather than 0:
                    // the first loopXf head samples were already mixed in
                    // during the tail's crossfade region. Without
                    // crossfade, behavior is unchanged (wrapTo = 0).
                    cursor = wrapTo + std::fmod(cursor - wrapTo, loopSpan);
                } else {
                    v.mode = VoiceMode::Inactive;
                    break;
                }
            }
            // Linear interpolation between two consecutive source frames.
            // With pitch=1 the fractional part is zero on every step so the
            // interpolation collapses to the nearest sample; with pitch != 1
            // (Doppler shift, design-intent pitch) it removes the aliasing
            // that nearest-neighbour resampling would otherwise produce.
            const uint32_t idx0 = static_cast<uint32_t>(cursor);
            const float    frac = static_cast<float>(cursor - static_cast<double>(idx0));
            uint32_t idx1 = idx0 + 1;
            if (idx1 >= v.pcmFrames) {
                // For loops, the next-sample lookup wraps to the post-loop
                // landing point: loopXf for crossfade-enabled, 0 otherwise.
                idx1 = v.looping ? (useLoopXf ? loopXf : 0u) : idx0;
            }

            const float s0a = src[idx0 * srcChans];
            const float s0b = src[idx1 * srcChans];
            float s0 = s0a + (s0b - s0a) * frac;

            float s1;
            if (srcChans >= 2) {
                const float s1a = src[idx0 * srcChans + 1];
                const float s1b = src[idx1 * srcChans + 1];
                s1 = s1a + (s1b - s1a) * frac;
            } else {
                s1 = s0;
            }

            // Loop-boundary equal-power crossfade. When the cursor is in
            // the last loopXf frames of the buffer, the tail samples
            // (just read above) fade out via cos(t·π/2) while the head
            // samples (read fresh from the start of the buffer) fade in
            // via sin(t·π/2). cos² + sin² = 1, so a perfectly-looping
            // asset (head ≡ tail) reproduces unchanged; an imperfect
            // loop transitions smoothly without click.
            if (useLoopXf && idx0 + loopXf >= v.pcmFrames) {
                const uint32_t headOffset = idx0 - (v.pcmFrames - loopXf); // 0..loopXf-1
                const uint32_t hidx0 = headOffset;
                const uint32_t hidx1 = (hidx0 + 1u < v.pcmFrames) ? hidx0 + 1u : hidx0;

                const float h0a = src[hidx0 * srcChans];
                const float h0b = src[hidx1 * srcChans];
                const float h0  = h0a + (h0b - h0a) * frac;
                float h1;
                if (srcChans >= 2) {
                    const float h1a = src[hidx0 * srcChans + 1];
                    const float h1b = src[hidx1 * srcChans + 1];
                    h1 = h1a + (h1b - h1a) * frac;
                } else {
                    h1 = h0;
                }

                constexpr float kHalfPi = 1.5707963f;
                const float t     = static_cast<float>(headOffset)
                                    / static_cast<float>(loopXf);
                const float gTail = std::cos(t * kHalfPi);
                const float gHead = std::sin(t * kHalfPi);
                s0 = s0 * gTail + h0 * gHead;
                s1 = s1 * gTail + h1 * gHead;
            }

            if (useLpf) ApplyLpfInPlace(v, s0, s1);
            if (!ApplyFadeIfActive(v, s0, s1)) break;

            if (v.useBinaural) {
                // Mono input; downmix any stereo source so both ears
                // process the same signal through their independent
                // delay + per-ear LPF chains. The dry signal we write
                // here is what the spatializer's directional cues
                // shape, identical to the conceptual model where the
                // emitter is a point source.
                const float monoIn = (srcChans >= 2) ? 0.5f * (s0 + s1) : s0;

                const uint32_t N = static_cast<uint32_t>(v.delayBufL.size());
                v.delayBufL[v.delayWritePos] = monoIn;
                v.delayBufR[v.delayWritePos] = monoIn;
                const uint32_t writeIdx = v.delayWritePos;
                v.delayWritePos = (writeIdx + 1u) % N;

                // Linear-interp read at the current per-ear delay. The
                // current delay ramps from previous-tick value toward
                // the new target across this buffer, avoiding clicks
                // when the listener turns.
                const float dStepL = (frames > 0)
                    ? (v.delaySamplesL - v.delaySamplesLCurrent) / static_cast<float>(frames)
                    : 0.0f;
                const float dStepR = (frames > 0)
                    ? (v.delaySamplesR - v.delaySamplesRCurrent) / static_cast<float>(frames)
                    : 0.0f;
                v.delaySamplesLCurrent += dStepL;
                v.delaySamplesRCurrent += dStepR;

                auto readDelayed = [&](const std::vector<float>& buf, float delaySamps) -> float {
                    const float clamped = std::clamp(delaySamps, 0.0f,
                                                       static_cast<float>(N) - 2.0f);
                    const uint32_t intPart  = static_cast<uint32_t>(clamped);
                    const float    fracPart = clamped - static_cast<float>(intPart);
                    const uint32_t i0 = (writeIdx + N - intPart)         % N;
                    const uint32_t i1 = (writeIdx + N - intPart - 1u)    % N;
                    return buf[i0] * (1.0f - fracPart) + buf[i1] * fracPart;
                };
                float earL = readDelayed(v.delayBufL, v.delaySamplesLCurrent);
                float earR = readDelayed(v.delayBufR, v.delaySamplesRCurrent);

                // Per-ear LPF (head shadow). Coefficients were computed
                // in CopyBinauralFromCmd; here we run the biquad state.
                if (v.lpfAmountL > 0.001f) {
                    const float in = earL;
                    earL = v.lpfBinB0L * in + v.lpfBinZ1L;
                    v.lpfBinZ1L = v.lpfBinB1L * in - v.lpfBinA1L * earL + v.lpfBinZ2L;
                    v.lpfBinZ2L = v.lpfBinB2L * in - v.lpfBinA2L * earL;
                }
                if (v.lpfAmountR > 0.001f) {
                    const float in = earR;
                    earR = v.lpfBinB0R * in + v.lpfBinZ1R;
                    v.lpfBinZ1R = v.lpfBinB1R * in - v.lpfBinA1R * earR + v.lpfBinZ2R;
                    v.lpfBinZ2R = v.lpfBinB2R * in - v.lpfBinA2R * earR;
                }

                const float outL = earL * v.gain * v.gainL;
                const float outR = earR * v.gain * v.gainR;
                if (channels >= 1) dst[f * channels + 0] += outL;
                if (channels >= 2) dst[f * channels + 1] += outR;
                if (reverbDst) {
                    // Reverb send pre-pan: average the ears so the wet
                    // path doesn't double-spatialize via the reverb
                    // tail. v.reverbSend already factored into sendL/R
                    // outside this loop, but those were derived from
                    // gL/gR (pan path); recompute here from the binaural
                    // gains.
                    const float monoSend = 0.5f * (outL + outR) * v.reverbSend;
                    if (channels >= 1) reverbDst[f * channels + 0] += monoSend;
                    if (channels >= 2) reverbDst[f * channels + 1] += monoSend;
                }
            } else {
                if (channels >= 1) dst[f * channels + 0] += s0 * gL;
                if (channels >= 2) dst[f * channels + 1] += s1 * gR;
                if (reverbDst) {
                    if (channels >= 1) reverbDst[f * channels + 0] += s0 * sendL;
                    if (channels >= 2) reverbDst[f * channels + 1] += s1 * sendR;
                }
            }
            cursor  += curStep;
            curStep += dStep;
        }
        v.cursor       = cursor;
        // Land exactly on target; avoids drift from float imprecision
        // when the loop completed normally. If the loop early-broke (voice
        // went Inactive), pitchCurrent doesn't matter.
        v.pitchCurrent = static_cast<float>(tgtStep);
    } else if (v.mode == VoiceMode::StreamingSound) {
        util::PcmRingF32* ring = v.streamRing;
        if (!ring) {
            v.mode = VoiceMode::Inactive;
            return;
        }
        // Float scratch reused from voiceScratchInt16_'s allocation budget
        // would be type-unsafe; allocate this scratch on first use of
        // streaming. For now, mix in small chunks using a stack-bounded
        // loop. Pop directly into a small stack buffer to avoid heap.
        constexpr uint32_t kChunkFrames = 256;
        const uint32_t sChans = v.streamChannels;
        const bool     useLpf = (v.lpfAmount > 0.001f);
        float chunk[kChunkFrames * 2];   // up to 2 channels
        uint32_t produced = 0;
        while (produced < frames) {
            const uint32_t want = std::min(kChunkFrames, frames - produced);
            const uint32_t got  = ring->Pop(chunk, want);
            if (got == 0) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
                break;     // ring empty: leave the rest as silence
            }
            for (uint32_t f = 0; f < got; ++f) {
                float s0 = chunk[f * sChans];
                float s1 = (sChans >= 2) ? chunk[f * sChans + 1] : s0;
                if (useLpf) ApplyLpfInPlace(v, s0, s1);
                if (!ApplyFadeIfActive(v, s0, s1)) break;
                const uint32_t out = produced + f;
                if (channels >= 1) dst[out * channels + 0] += s0 * gL;
                if (channels >= 2) dst[out * channels + 1] += s1 * gR;
                if (reverbDst) {
                    if (channels >= 1) reverbDst[out * channels + 0] += s0 * sendL;
                    if (channels >= 2) reverbDst[out * channels + 1] += s1 * sendR;
                }
            }
            produced += got;
            if (got < want) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    } else if (v.mode == VoiceMode::Voice) {
        util::PcmRing* ring = v.voiceRing;
        if (!ring) {
            v.mode = VoiceMode::Inactive;
            return;
        }
        const uint32_t vChans  = v.voiceChannels;
        const uint32_t maxSamp = static_cast<uint32_t>(voiceScratchInt16_.size());
        const uint32_t want    = std::min(frames, maxSamp / std::max(1u, vChans));
        const uint32_t got     = ring->Pop(voiceScratchInt16_.data(), want);
        if (got < want) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        constexpr float k = 1.0f / 32768.0f;
        const bool useLpf = (v.lpfAmount > 0.001f);
        for (uint32_t f = 0; f < got; ++f) {
            float s0 = static_cast<float>(voiceScratchInt16_[f * vChans]) * k;
            float s1 = (vChans >= 2)
                ? static_cast<float>(voiceScratchInt16_[f * vChans + 1]) * k
                : s0;
            if (useLpf) ApplyLpfInPlace(v, s0, s1);
            if (!ApplyFadeIfActive(v, s0, s1)) break;
            if (channels >= 1) dst[f * channels + 0] += s0 * gL;
            if (channels >= 2) dst[f * channels + 1] += s1 * gR;
            if (reverbDst) {
                if (channels >= 1) reverbDst[f * channels + 0] += s0 * sendL;
                if (channels >= 2) reverbDst[f * channels + 1] += s1 * sendR;
            }
        }
        // Frames beyond `got` produce silence (input already cleared).
    }
}

void AudioMixer::RunBusGraph(uint32_t frames, uint32_t channels) noexcept {
    if (!busGraph_) return;

    const auto& order = busGraph_->RenderOrder();
    const uint32_t total = frames * channels;

    for (uint32_t busIdx : order) {
        float*       output = busGraph_->OutputBuffer(busIdx);
        const float* input  = busGraph_->InputBuffer(busIdx);

        // Stage 1: copy input -> output. Effects then process output in place.
        std::memcpy(output, input, sizeof(float) * total);

        // Stage 2: run the effect chain. Each effect can read its sidechain
        // bus's *output* buffer (which is already filled because of topo
        // ordering).
        const uint32_t fxCount = busGraph_->EffectCount(busIdx);
        for (uint32_t e = 0; e < fxCount; ++e) {
            IDspEffect* fx = busGraph_->EffectAt(busIdx, e);
            if (!fx) continue;
            const uint32_t scIdx = busGraph_->SidechainSourceIndex(busIdx, e);
            const float*   scBuf = (scIdx != BusGraph::kInvalidIndex)
                ? busGraph_->OutputBufferConst(scIdx)
                : nullptr;
            const uint32_t scCh  = (scBuf != nullptr) ? channels : 0;
            fx->Process(output, frames, channels, scBuf, scCh);
        }

        // Stage 3: route to parent. Silent buses keep their output for
        // sidechain consumers but do not contribute to the audible mix.
        if (busGraph_->IsSilent(busIdx)) continue;
        const uint32_t parent = busGraph_->ParentIndex(busIdx);
        if (parent == BusGraph::kInvalidIndex) continue;     // master; no parent
        const float g = busGraph_->OutputGainLinear(busIdx);
        float* parentIn = busGraph_->InputBuffer(parent);
        if (std::abs(g - 1.0f) < 1e-6f) {
            for (uint32_t i = 0; i < total; ++i) parentIn[i] += output[i];
        } else {
            for (uint32_t i = 0; i < total; ++i) parentIn[i] += output[i] * g;
        }
    }
}

void AudioMixer::OnRender(float* output, uint32_t frames, uint32_t channels) noexcept {
    totalCallbacks_.fetch_add(1, std::memory_order_relaxed);

    DrainCommands();

    // Bus-graph fast path requires a graph; fall back to silent output if
    // the runtime hasn't wired one in. (Should never happen in production;
    // AudioRuntimeImpl always builds at least the auto-master graph.)
    if (!busGraph_ || busGraph_->BusCount() == 0) {
        std::memset(output, 0, sizeof(float) * frames * channels);
        return;
    }

    busGraph_->ClearAllInputBuffers(frames, channels);

    // Refresh the reverb-bus index this render. Cheap (one map lookup).
    // Stays at kInvalidIndex when no kBusReverb bus is in the graph, in
    // which case MixVoiceIntoBus skips reverb sends entirely.
    reverbBusIndex_ = busGraph_->IndexOf(kBusReverb);

    uint32_t active = 0;
    for (uint32_t i = 0; i < maxVoices_; ++i) {
        MixVoice& v = voices_[i];
        if (v.mode == VoiceMode::Inactive) continue;
        ++active;
        MixVoiceIntoBus(v, frames, channels);
    }
    activeApprox_.store(active, std::memory_order_relaxed);

    RunBusGraph(frames, channels);

    // Master's output -> device. Apply master's output gain at this point
    // since master has no parent to sum into.
    const uint32_t masterIdx = busGraph_->MasterIndex();
    const float*   master    = busGraph_->OutputBufferConst(masterIdx);
    const float    masterG   = busGraph_->OutputGainLinear(masterIdx);
    const uint32_t total     = frames * channels;
    if (std::abs(masterG - 1.0f) < 1e-6f) {
        std::memcpy(output, master, sizeof(float) * total);
    } else {
        for (uint32_t i = 0; i < total; ++i) output[i] = master[i] * masterG;
    }
}

} // namespace audio
