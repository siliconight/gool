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

// tests/unit/voice_codec_test.cpp
//
// Two-layer test:
//
//   1. IVoiceCodec contract via StubVoiceCodec; encode/decode round-trip
//      preserves the int16 PCM payload, EmptyPacket / nullptr inputs are
//      rejected, and over-capacity buffers are flagged with
//      AudioResult::BudgetExceeded. No external dependencies.
//
//   2. OpusVoiceCodec availability; when AUDIO_ENGINE_VOICE_OPUS is
//      not defined, OpusVoiceCodec::Create returns a null IVoiceCodec
//      pointer, mirroring the pattern used by the file decoders. When it
//      *is* defined the codec compiles against libopus (real or stub).
//      The test runs only the gated-off-availability check; full encode
//      round-trips against real libopus would require fetching the
//      library, which the build flag opts into separately.

#include "audio_engine/voice_codec.h"
#include "audio_engine/voice/stub_voice_codec.h"
#include "audio_engine/voice/opus_voice_codec.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

void TestStubRoundTrip() {
    constexpr uint32_t kRate    = 48000;
    constexpr uint32_t kCh      = 1;
    constexpr uint32_t kFrames  = 480;     // 10 ms @ 48 kHz mono

    StubVoiceCodec codec(kRate, kCh, kFrames);

    EXPECT(codec.SampleRate() == kRate);
    EXPECT(codec.Channels()   == kCh);
    EXPECT(codec.FrameSize()  == kFrames);

    // Synthesize a deterministic int16 ramp so we can spot any byte-order
    // or frame-count error after the round-trip.
    std::vector<int16_t> pcm(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        pcm[i] = static_cast<int16_t>(((i * 47) & 0xFFFF) - 0x8000);
    }

    std::vector<uint8_t> wire(kFrames * sizeof(int16_t) + 16);
    size_t wireSize = 0;
    EXPECT(codec.Encode(pcm.data(), kFrames, wire.data(), wire.size(), wireSize)
            == AudioResult::Success);
    EXPECT(wireSize == kFrames * sizeof(int16_t));

    VoicePacket pkt;
    pkt.data = wire.data();
    pkt.size = wireSize;

    std::vector<int16_t> recovered(kFrames);
    uint32_t outFrames = 0;
    EXPECT(codec.Decode(pkt, recovered.data(), kFrames, outFrames)
            == AudioResult::Success);
    EXPECT(outFrames == kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) EXPECT(recovered[i] == pcm[i]);
}

void TestStubBudgetAndArgValidation() {
    StubVoiceCodec codec(48000, 1, 480);
    std::vector<int16_t> pcm(480, 0);
    std::vector<uint8_t> wire(100);     // intentionally too small
    size_t wireSize = 0;

    EXPECT(codec.Encode(pcm.data(), 480, wire.data(), wire.size(), wireSize)
            == AudioResult::BudgetExceeded);

    std::vector<uint8_t> bigWire(960 * 2);
    EXPECT(codec.Encode(nullptr, 480, bigWire.data(), bigWire.size(), wireSize)
            == AudioResult::InvalidArgument);

    VoicePacket emptyPkt;     // data=nullptr, size=0
    std::vector<int16_t> outBuf(480);
    uint32_t outFrames = 0;
    EXPECT(codec.Decode(emptyPkt, outBuf.data(), 480, outFrames)
            == AudioResult::InvalidArgument);
}

void TestOpusAvailabilityGating() {
    // Whatever the build mode, OpusVoiceCodec is constructible. In stub
    // mode IsSupported() returns false and Encode/Decode return
    // AudioResult::Unsupported; in gated-on mode the codec works against
    // libopus.
    OpusVoiceCodec::Settings s;
    s.sampleRate     = 48000;
    s.channels       = 1;
    s.frameSize      = 960;
    s.bitrateBps     = 24000;
    s.maxDecoders    = 4;
    s.applicationVoip = true;
    OpusVoiceCodec codec(s);

    EXPECT(codec.SampleRate() == 48000);
    EXPECT(codec.Channels()   == 1);
    EXPECT(codec.FrameSize()  == 960);
    EXPECT(std::string(codec.Name()) == "opus");

#if defined(AUDIO_ENGINE_VOICE_OPUS)
    // Real libopus is linked. IsSupported should be true and encode +
    // decode should round-trip a single frame back to non-zero PCM.
    EXPECT(codec.IsSupported());

    std::vector<int16_t> pcm(960);
    for (uint32_t i = 0; i < 960; ++i) {
        // Mid-frequency sine; enough signal for opus to allocate bits to.
        pcm[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159 * 440.0 * i / 48000.0));
    }
    std::vector<uint8_t> wire(4000);
    size_t wireSize = 0;
    EXPECT(codec.Encode(pcm.data(), 960, wire.data(), wire.size(), wireSize)
            == AudioResult::Success);
    EXPECT(wireSize > 0);
    EXPECT(wireSize < wire.size());

    VoicePacket pkt;
    pkt.playerId = 7;
    pkt.data     = wire.data();
    pkt.size     = wireSize;
    std::vector<int16_t> recovered(960);
    uint32_t outFrames = 0;
    EXPECT(codec.Decode(pkt, recovered.data(), 960, outFrames)
            == AudioResult::Success);
    EXPECT(outFrames == 960);

    // Lossy codec; exact PCM equality is not expected. Test signal energy
    // survived: sum of |sample| across the frame should be in the same
    // ballpark as the input.
    int64_t inEnergy = 0, outEnergy = 0;
    for (uint32_t i = 0; i < 960; ++i) {
        inEnergy  += std::abs(pcm[i]);
        outEnergy += std::abs(recovered[i]);
    }
    // Allow ±50% of input energy. Opus VOIP at 24 kbps preserves overall
    // signal level well within that bound for a clean sine.
    EXPECT(outEnergy > inEnergy / 2);
    EXPECT(outEnergy < inEnergy * 2);
#else
    // Stub mode: codec is constructible but reports unsupported. Encode +
    // Decode must return AudioResult::Unsupported, mirroring the file-
    // decoder behaviour when their build flag is off.
    EXPECT(!codec.IsSupported());

    std::vector<int16_t> pcm(960, 0);
    std::vector<uint8_t> wire(4000);
    size_t wireSize = 0;
    EXPECT(codec.Encode(pcm.data(), 960, wire.data(), wire.size(), wireSize)
            == AudioResult::Unsupported);

    VoicePacket pkt;
    pkt.playerId = 7;
    pkt.data     = wire.data();
    pkt.size     = 8;
    std::vector<int16_t> outBuf(960);
    uint32_t outFrames = 0;
    EXPECT(codec.Decode(pkt, outBuf.data(), 960, outFrames)
            == AudioResult::Unsupported);
#endif
}

} // namespace

int main() {
    std::printf("[voice_codec_test] running...\n");
    TestStubRoundTrip();
    TestStubBudgetAndArgValidation();
    TestOpusAvailabilityGating();
    if (gFails == 0) {
        std::printf("[voice_codec_test] OK\n");
        return 0;
    }
    std::fprintf(stderr, "[voice_codec_test] %d failure(s)\n", gFails);
    return 1;
}
