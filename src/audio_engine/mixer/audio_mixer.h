// audio_engine/mixer/audio_mixer.h
//
// AudioMixer implements IAudioRenderCallback. It is the concrete owner of
// per-mix-voice render state: cursors, current gain/pan, source data
// pointers, target-bus routing. The control thread communicates with it
// exclusively through the SPSC command ring; the render thread drains
// commands at the top of each OnRender call before mixing.
//
// Render flow per callback:
//   1. Drain command ring (start/stop voices, parameter updates, bus & effect
//      parameter updates).
//   2. Clear every bus's input buffer (BusGraph::ClearAllInputBuffers).
//   3. Mix every active voice into its target bus's input buffer.
//   4. For each bus in BusGraph::RenderOrder():
//        a. Copy input -> output (effects work in-place on output).
//        b. Run effect chain on output, passing the sidechain buffer when
//           the effect declares a sidechain reference.
//        c. If !silent, sum output * outputGain into parent's input buffer.
//   5. Copy master.output to the device buffer.
//
// The mixer is not allocation-free in its constructor; it is allocation-free
// in OnRender.

#ifndef AUDIO_ENGINE_MIXER_AUDIO_MIXER_H
#define AUDIO_ENGINE_MIXER_AUDIO_MIXER_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio_engine/backend.h"
#include "audio_engine/bus.h"
#include "audio_engine/types.h"
#include "audio_engine/util/spsc_ring.h"
#include "audio_engine/mixer/mixer_command.h"

namespace audio {

namespace util { class PcmRing; class PcmRingF32; }

class BusGraph;

class AudioMixer final : public IAudioRenderCallback {
public:
    AudioMixer(uint32_t maxMixVoices,
               uint32_t outputChannels,
               uint32_t commandRingDepth,
               BusGraph* busGraph,
               uint32_t sampleRate = 48000);

    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    bool PostCommand(const MixerCommand& cmd) noexcept;

    void OnRender(float* output, uint32_t frames, uint32_t channels) noexcept override
        AUDIO_REQUIRES(RenderThread) AUDIO_NO_ALLOC AUDIO_RENDER_PATH;

    uint32_t MaxMixVoices()   const noexcept { return maxVoices_; }
    uint32_t OutputChannels() const noexcept { return outputChannels_; }

    // Diagnostic accessors; read render-thread-owned per-voice state from
    // the control thread or a test. Reads are not synchronised; the values
    // are best-effort snapshots intended for stats and tests, not for any
    // logic that needs precise consistency.
    float DebugPitchTarget(uint32_t slot) const noexcept {
        return (slot < maxVoices_) ? voices_[slot].pitch        : 0.0f;
    }
    float DebugPitchCurrent(uint32_t slot) const noexcept {
        return (slot < maxVoices_) ? voices_[slot].pitchCurrent : 0.0f;
    }

    uint32_t ActiveVoicesApprox() const noexcept {
        return activeApprox_.load(std::memory_order_relaxed);
    }
    uint64_t TotalCallbacks() const noexcept {
        return totalCallbacks_.load(std::memory_order_relaxed);
    }
    uint64_t Underruns() const noexcept {
        return underruns_.load(std::memory_order_relaxed);
    }

private:
    enum class VoiceMode : uint8_t {
        Inactive,
        Sound,
        StreamingSound,
        Voice,
    };

    struct MixVoice {
        VoiceMode mode = VoiceMode::Inactive;

        float gain  = 1.0f;
        float pan   = 0.0f;
        // Pitch is the resampling step used by the Sound path. `pitch` is
        // the *target* set by UpdateParams; `pitchCurrent` is the smoothed
        // running value that the mix loop actually steps the cursor by.
        // Linear per-block ramp from current to target over each render
        // buffer kills clicks from rapid Doppler updates without
        // measurable smearing at typical buffer sizes (~5 ms at 48 kHz/256).
        float pitch        = 1.0f;
        float pitchCurrent = 1.0f;

        BusId targetBus = kInvalidBusId;     // resolved to busIndex by mixer

        // Sound (pinned PCM)
        const float* pcmData     = nullptr;
        uint32_t     pcmFrames   = 0;
        uint32_t     pcmChannels = 1;
        double       cursor      = 0.0;
        bool         looping     = false;

        // StreamingSound (float ring, pre-resampled to engine rate)
        util::PcmRingF32* streamRing     = nullptr;
        uint32_t          streamChannels = 1;

        // Voice (int16 ring from codec)
        util::PcmRing* voiceRing     = nullptr;
        uint32_t       voiceChannels = 1;

        // Per-voice low-pass filter (cookbook biquad). Used for occlusion
        // damping. `lpfAmount` of 0 bypasses the filter at zero cost. Up to
        // 2 channels of state are kept; voices with more channels are
        // downmixed to stereo upstream.
        float lpfAmount = 0.0f;     // current value (driven by UpdateParams)
        float lpfB0 = 1.0f, lpfB1 = 0.0f, lpfB2 = 0.0f;   // numerator
        float lpfA1 = 0.0f, lpfA2 = 0.0f;                 // denominator (a0 normalised)
        float lpfZ1[2]{0.0f, 0.0f};
        float lpfZ2[2]{0.0f, 0.0f};

        // Per-voice send to the conventional reverb bus (kBusReverb). 0
        // means "no send" and skips the extra bus accumulation entirely.
        float reverbSend = 0.0f;

        // Loop-boundary crossfade. 0 = disabled (legacy behavior).
        // > 0 = number of frames to blend at each loop boundary
        // using equal-power curves. The mixer reads from both the
        // tail (cursor in last N frames) and the head (offset
        // within first N frames) when in this region; on wrap, the
        // cursor jumps to N rather than 0.
        uint32_t loopXfadeFrames = 0;

        // Fade state. `fadeDirection` selects the curve and the
        // completion behavior:
        //   0 = no fade
        //   1 = fade-out (gain follows cos(t·π/2), 1 → 0); voice goes
        //       Inactive when remaining hits 0
        //   2 = fade-in  (gain follows sin(t·π/2), 0 → 1); fade state
        //       clears when remaining hits 0; voice keeps playing
        // The cos/sin pair sums to constant power, so a fade-out on
        // one voice paired with a fade-in on another (the music
        // crossfade pattern) has no audible dip at the midpoint.
        // A new StartSound on this slot resets these and preempts
        // the fade.
        uint8_t  fadeDirection       = 0;
        uint32_t fadeRemainingFrames = 0;
        uint32_t fadeTotalFrames     = 0;

        // Binaural (per-ear) state. When `useBinaural` is true the Sound
        // mix path forks off the pan path: the voice's source samples are
        // pushed into both delay lines, each ear reads at its own
        // fractional delay, and per-ear LPF coefficients are applied
        // before per-ear gain. The delay lines are sized once at mixer
        // construction (no allocation on the render thread); typical max
        // ITD is ~32 samples at 48 kHz with the default head radius, so
        // 96 samples gives plenty of headroom.
        bool   useBinaural    = false;
        float  gainL          = 1.0f;
        float  gainR          = 1.0f;
        float  delaySamplesL  = 0.0f;     // target (set by UpdateParams)
        float  delaySamplesR  = 0.0f;
        float  delaySamplesLCurrent = 0.0f;     // ramped per buffer
        float  delaySamplesRCurrent = 0.0f;
        // Per-ear LPF state (when useBinaural). Mirrors lpfB0/lpfA1/etc
        // above but split per ear so the contralateral ear can damp more
        // than the ipsilateral one. Coefficients recomputed only on
        // amount changes; state persists across the change to avoid
        // discontinuity clicks.
        float  lpfAmountL = 0.0f;
        float  lpfAmountR = 0.0f;
        float  lpfBinB0L = 1.0f, lpfBinB1L = 0.0f, lpfBinB2L = 0.0f;
        float  lpfBinA1L = 0.0f, lpfBinA2L = 0.0f;
        float  lpfBinZ1L = 0.0f, lpfBinZ2L = 0.0f;
        float  lpfBinB0R = 1.0f, lpfBinB1R = 0.0f, lpfBinB2R = 0.0f;
        float  lpfBinA1R = 0.0f, lpfBinA2R = 0.0f;
        float  lpfBinZ1R = 0.0f, lpfBinZ2R = 0.0f;
        // Per-ear delay lines. Size set at MixVoice construction, never
        // resized. writePos advances modulo size on every push.
        std::vector<float> delayBufL;
        std::vector<float> delayBufR;
        uint32_t           delayWritePos = 0;
    };

    void DrainCommands() noexcept;
    void MixVoiceIntoBus(MixVoice& v, uint32_t frames, uint32_t channels) noexcept;

    // Copy binaural fields from a MixerCommand into a MixVoice and
    // recompute per-ear LPF coefficients when amounts changed. On Start
    // commands `resetCurrent` is true (delay-current jumps to target;
    // delay-line state cleared). On UpdateParams it's false so the next
    // render ramps from previous-tick values to the new targets.
    static void CopyBinauralFromCmd(MixVoice& v,
                                      const MixerCommand& cmd,
                                      bool resetCurrent,
                                      float sampleRate) noexcept;
    void RunBusGraph(uint32_t frames, uint32_t channels) noexcept;

    // Apply the per-voice biquad LPF to one stereo (or duplicated-mono) sample
    // pair, advancing per-channel state in-place. Defined inline so the hot
    // mix loops can call it without function-call overhead. Caller is
    // responsible for guarding on `v.lpfAmount > 0.001f`.
    static inline void ApplyLpfInPlace(MixVoice& v, float& s0, float& s1) noexcept {
        const float in0 = s0, in1 = s1;
        const float y0 = v.lpfB0 * in0 + v.lpfZ1[0];
        v.lpfZ1[0] = v.lpfB1 * in0 - v.lpfA1 * y0 + v.lpfZ2[0];
        v.lpfZ2[0] = v.lpfB2 * in0 - v.lpfA2 * y0;
        s0 = y0;
        const float y1 = v.lpfB0 * in1 + v.lpfZ1[1];
        v.lpfZ1[1] = v.lpfB1 * in1 - v.lpfA1 * y1 + v.lpfZ2[1];
        v.lpfZ2[1] = v.lpfB2 * in1 - v.lpfA2 * y1;
        s1 = y1;
    }

    // Apply per-frame fade gain using equal-power curves:
    //   fadeDirection == 1 (fade-out): gain = cos(progress · π/2)
    //   fadeDirection == 2 (fade-in):  gain = sin(progress · π/2)
    // where progress = (total - remaining) / total ∈ [0, 1).
    // Returns true if the voice should continue rendering this frame;
    // false only when a fade-out has just completed (caller breaks
    // out of the per-frame loop and picks up Inactive next render).
    static inline bool ApplyFadeIfActive(MixVoice& v, float& s0, float& s1) noexcept {
        if (v.fadeTotalFrames == 0) return true;
        if (v.fadeRemainingFrames == 0) {
            if (v.fadeDirection == 1) {
                v.mode             = VoiceMode::Inactive;
                v.fadeTotalFrames  = 0;
                v.fadeDirection    = 0;
                v.pcmData          = nullptr;
                v.voiceRing        = nullptr;
                v.streamRing       = nullptr;
                return false;
            }
            // Fade-in completed cleanly: gain = 1, clear fade state.
            v.fadeTotalFrames = 0;
            v.fadeDirection   = 0;
            return true;
        }
        // π/2 to four decimals; the mixer is float, this is plenty.
        constexpr float kHalfPi = 1.5707963f;
        const float t = static_cast<float>(v.fadeTotalFrames - v.fadeRemainingFrames)
                      / static_cast<float>(v.fadeTotalFrames);
        const float gain = (v.fadeDirection == 1)
                           ? std::cos(t * kHalfPi)   // out: 1 → 0
                           : std::sin(t * kHalfPi);  // in:  0 → 1
        s0 *= gain;
        s1 *= gain;
        --v.fadeRemainingFrames;
        return true;
    }

    uint32_t maxVoices_;
    uint32_t outputChannels_;
    uint32_t sampleRate_;
    std::vector<MixVoice> voices_;

    util::SpscRing<MixerCommand> commands_;

    BusGraph* busGraph_;          // not owned

    // Cached resolution of kBusReverb in the bus graph, refreshed at the
    // top of each OnRender (cheap: one IndexOf lookup). Equals
    // BusGraph::kInvalidIndex when no reverb bus is configured, in which
    // case voice-level reverb sends are silent no-ops.
    uint32_t reverbBusIndex_ = 0xFFFFFFFFu;     // BusGraph::kInvalidIndex

    // Render-thread-owned scratch.
    std::vector<int16_t> voiceScratchInt16_;

    std::atomic<uint32_t> activeApprox_{0};
    std::atomic<uint64_t> totalCallbacks_{0};
    std::atomic<uint64_t> underruns_{0};
};

} // namespace audio

#endif // AUDIO_ENGINE_MIXER_AUDIO_MIXER_H

