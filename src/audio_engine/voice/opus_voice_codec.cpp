// audio_engine/voice/opus_voice_codec.cpp
//
// Implementation of the public OpusVoiceCodec contract from
// include/audio_engine/voice/opus_voice_codec.h. Conditionally compiled:
// when AUDIO_ENGINE_VOICE_OPUS is undefined the State struct still
// constructs (so hosts can create the codec object unconditionally) but
// IsSupported() returns false and Encode/Decode return
// AudioResult::Unsupported.
//
// State per player
// ----------------
// Opus decoders carry several frames of state (PLC + crossfades). A
// single decoder shared across all incoming streams would corrupt
// itself the moment two players' packets interleaved. We allocate
// `Settings.maxDecoders` decoder slots up front, bind a slot to an
// AudioPlayerId on first packet, and evict the least-recently-used slot
// when the pool is full.

#include "audio_engine/voice/opus_voice_codec.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#if defined(AUDIO_ENGINE_VOICE_OPUS)
#include <opus.h>
#endif

namespace audio {

struct OpusVoiceCodec::State {
    Settings settings;
    bool     supported = false;

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    OpusEncoder* encoder = nullptr;

    struct DecoderSlot {
        AudioPlayerId playerId    = kInvalidPlayerId;
        OpusDecoder*  decoder     = nullptr;
        uint64_t      lastUsedTick = 0;
    };
    std::vector<DecoderSlot> decoderSlots;
    uint64_t                 tickCounter = 0;

    // Look up or assign a decoder slot for `playerId`. LRU eviction when
    // the pool is full. Returns nullptr only if pool size is zero.
    OpusDecoder* AcquireDecoder(AudioPlayerId playerId) noexcept {
        ++tickCounter;
        // Hit?
        for (auto& s : decoderSlots) {
            if (s.playerId == playerId && s.decoder) {
                s.lastUsedTick = tickCounter;
                return s.decoder;
            }
        }
        // Miss; find LRU slot.
        if (decoderSlots.empty()) return nullptr;
        size_t   lruIdx  = 0;
        uint64_t lruTick = decoderSlots[0].lastUsedTick;
        for (size_t i = 1; i < decoderSlots.size(); ++i) {
            if (decoderSlots[i].lastUsedTick < lruTick) {
                lruTick = decoderSlots[i].lastUsedTick;
                lruIdx  = i;
            }
        }
        auto& s = decoderSlots[lruIdx];
        if (s.decoder) {
            // Reset state for the new player so we don't crossfade out of
            // the evicted player's last frame.
            opus_decoder_ctl(s.decoder, OPUS_RESET_STATE);
        }
        s.playerId     = playerId;
        s.lastUsedTick = tickCounter;
        return s.decoder;
    }
#endif
};

OpusVoiceCodec::OpusVoiceCodec() : OpusVoiceCodec(Settings{}) {}

OpusVoiceCodec::OpusVoiceCodec(const Settings& settings)
    : state_(std::make_unique<State>()) {
    state_->settings = settings;

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    // Validate config; libopus is strict and we want a clean fail rather
    // than carrying half-initialised state.
    auto rateOk = [](uint32_t r) {
        return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000;
    };
    if (!rateOk(settings.sampleRate))                  return;
    if (settings.channels != 1 && settings.channels != 2) return;
    if (settings.frameSize == 0)                       return;
    if (settings.maxDecoders == 0)                     return;

    int err = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(
        static_cast<opus_int32>(settings.sampleRate),
        static_cast<int>(settings.channels),
        settings.applicationVoip ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO,
        &err);
    if (!enc || err != OPUS_OK) {
        if (enc) opus_encoder_destroy(enc);
        return;
    }
    // VOIP-tuned settings. FEC + 5% expected packet loss pairs with the
    // engine's jitter buffer late-discard behaviour on the network seam.
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(settings.bitrateBps));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(
        settings.applicationVoip ? OPUS_SIGNAL_VOICE : OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));

    state_->encoder = enc;

    // Allocate the decoder pool. On any failure tear down everything we
    // allocated and leave the codec in stub mode (IsSupported() == false).
    state_->decoderSlots.resize(settings.maxDecoders);
    for (auto& s : state_->decoderSlots) {
        int derr = OPUS_OK;
        s.decoder = opus_decoder_create(
            static_cast<opus_int32>(settings.sampleRate),
            static_cast<int>(settings.channels),
            &derr);
        if (!s.decoder || derr != OPUS_OK) {
            for (auto& cleanup : state_->decoderSlots) {
                if (cleanup.decoder) {
                    opus_decoder_destroy(cleanup.decoder);
                    cleanup.decoder = nullptr;
                }
            }
            opus_encoder_destroy(state_->encoder);
            state_->encoder = nullptr;
            return;
        }
    }

    state_->supported = true;
#else
    // Stub mode; codec is constructible but IsSupported() returns false.
    (void)settings;
#endif
}

// NOLINTNEXTLINE(modernize-use-equals-default) — body is non-empty
// when AUDIO_ENGINE_VOICE_OPUS is defined; clang-tidy only sees the
// preprocessor-stripped form during static analysis builds.
OpusVoiceCodec::~OpusVoiceCodec() {
#if defined(AUDIO_ENGINE_VOICE_OPUS)
    if (state_->encoder) opus_encoder_destroy(state_->encoder);
    for (auto& s : state_->decoderSlots) {
        if (s.decoder) opus_decoder_destroy(s.decoder);
    }
#endif
}

bool OpusVoiceCodec::IsSupported() const noexcept {
    return state_->supported;
}

uint32_t OpusVoiceCodec::FrameSize()  const noexcept { return state_->settings.frameSize; }
uint32_t OpusVoiceCodec::SampleRate() const noexcept { return state_->settings.sampleRate; }
uint32_t OpusVoiceCodec::Channels()   const noexcept { return state_->settings.channels; }

AudioResult OpusVoiceCodec::Decode(const VoicePacket& packet,
                                    int16_t*           output,
                                    uint32_t           outputCapacityFrames,
                                    uint32_t&          outFrames) noexcept {
    outFrames = 0;
    if (!output || outputCapacityFrames == 0) return AudioResult::InvalidArgument;
    if (!state_->supported)                    return AudioResult::Unsupported;

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    OpusDecoder* dec = state_->AcquireDecoder(packet.playerId);
    if (!dec) return AudioResult::InternalError;

    // libopus convention: data == nullptr requests one frame of packet-
    // loss concealment, which the network-seam jitter buffer can use when
    // a sequence number is missing.
    const unsigned char* data = packet.data;
    const opus_int32     len  = (data != nullptr)
        ? static_cast<opus_int32>(packet.size)
        : 0;

    const int written = opus_decode(
        dec,
        data,
        len,
        output,
        static_cast<int>(outputCapacityFrames),
        /*decode_fec*/ 0);
    if (written < 0) return AudioResult::DecodeError;

    outFrames = static_cast<uint32_t>(written);
    return AudioResult::Success;
#else
    (void)packet;
    return AudioResult::Unsupported;
#endif
}

AudioResult OpusVoiceCodec::DecodeLost(AudioPlayerId playerId,
                                         int16_t*      output,
                                         uint32_t      outputCapacityFrames,
                                         uint32_t&     outFrames) noexcept {
    outFrames = 0;
    if (!output || outputCapacityFrames == 0) return AudioResult::InvalidArgument;
    if (!state_->supported)                    return AudioResult::Unsupported;

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    // PLC: ask libopus to extrapolate one frame's worth of samples from
    // its decoder state. The convention is data=NULL, len=0; libopus
    // generates a plausible signal that crossfades cleanly with the
    // next real packet decoded for this player. Critical that we pass
    // the SAME decoder this player is bound to — calling against a
    // shared decoder would corrupt other players' PLC state.
    OpusDecoder* dec = state_->AcquireDecoder(playerId);
    if (!dec) return AudioResult::InternalError;

    const int written = opus_decode(
        dec,
        nullptr,
        0,
        output,
        static_cast<int>(outputCapacityFrames),
        /*decode_fec*/ 0);
    if (written < 0) return AudioResult::DecodeError;

    outFrames = static_cast<uint32_t>(written);
    return AudioResult::Success;
#else
    (void)playerId;
    return AudioResult::Unsupported;
#endif
}

AudioResult OpusVoiceCodec::Encode(const int16_t* input,
                                    uint32_t       frameCount,
                                    uint8_t*       outBytes,
                                    size_t         outCapacity,
                                    size_t&        outSize) noexcept {
    outSize = 0;
    if (!input || !outBytes)                   return AudioResult::InvalidArgument;
    if (!state_->supported)                    return AudioResult::Unsupported;
    if (frameCount != state_->settings.frameSize) return AudioResult::InvalidArgument;
    if (outCapacity == 0)                       return AudioResult::BudgetExceeded;

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    const opus_int32 written = opus_encode(
        state_->encoder,
        input,
        static_cast<int>(frameCount),
        outBytes,
        static_cast<opus_int32>(outCapacity));
    if (written < 0) return AudioResult::InternalError;
    if (static_cast<size_t>(written) > outCapacity) return AudioResult::BudgetExceeded;

    outSize = static_cast<size_t>(written);
    return AudioResult::Success;
#else
    (void)input;
    (void)outBytes;
    (void)outCapacity;
    return AudioResult::Unsupported;
#endif
}

} // namespace audio
