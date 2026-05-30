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

// tests/fuzz/fuzz_opus_voice.cpp
//
// libFuzzer harness for the Opus voice-packet decode path.
//
// Surface under test: voice chat packets arrive from the network as
// untrusted byte payloads. The engine's receive path runs them
// through OpusVoiceCodec::Decode, which in turn hands them to
// libopus's opus_decode. libopus has been audited and is generally
// solid, but the wrapping logic in OpusVoiceCodec (range checks on
// packet size, player ID validation, per-player decoder lookup,
// output frame counting) is gool's own — and that's where bugs
// would live. This harness exercises both layers end-to-end.
//
// The fuzz input is interpreted as the OPUS PAYLOAD of a single
// voice packet from a single (synthetic) player; the harness wraps
// it in a VoicePacket and calls Decode against a long-lived codec
// instance. Per-player decoder state is reused across iterations
// to also exercise the codec's resync / packet-loss-concealment
// paths when the fuzz input changes radically between calls.
//
// Build:
//   cmake -S . -B build-fuzz -DAUDIO_ENGINE_FUZZ=ON \
//         -DAUDIO_ENGINE_VOICE_OPUS=ON \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_opus_voice
//   ./build-fuzz/fuzz_opus_voice -max_total_time=60

#include "audio_engine/voice_codec.h"
#include "audio_engine/voice/opus_voice_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

// Output buffer sized for one 60ms frame at 48 kHz (2880 samples).
// Opus packets above 60ms are non-standard; the codec will reject
// them before writing to this buffer, so we don't need slack.
constexpr uint32_t kOutCapacity = 2880;
std::array<int16_t, kOutCapacity> g_out{};

// Long-lived codec instance. Constructed lazily on first call so
// that initialization failures (e.g., libopus not linked) show up
// at fuzz-start rather than at static-init time. Settings default
// to 48 kHz mono 20ms-frame VoIP — the gool standard.
audio::OpusVoiceCodec& Codec() {
    static audio::OpusVoiceCodec instance;
    return instance;
}

// Counter to vary the playerId across iterations. This stresses
// the codec's per-player decoder cache (gool maintains up to
// `maxDecoders` parallel Opus decoder states; rotating playerIds
// exercises eviction and re-init paths).
uint16_t g_player_seed = 0;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Reject implausibly large inputs early. The Opus spec allows
    // packets up to 1275 bytes; anything larger is malformed by
    // construction. We still let the codec see slightly oversize
    // inputs (up to 4 KB) so its rejection path is exercised, but
    // multi-MB inputs would just waste fuzz cycles.
    if (size > 4096) return 0;

    auto& codec = Codec();
    if (!codec.IsSupported()) {
        // Build was configured without AUDIO_ENGINE_VOICE_OPUS=ON.
        // Nothing to fuzz; libFuzzer will treat every input as
        // uninteresting and exit quickly.
        return 0;
    }

    // Build a VoicePacket pointing at the fuzz bytes. The codec
    // copies what it needs; the lifetime of `data` only has to
    // outlive the Decode call, which it does.
    audio::VoicePacket packet;
    packet.playerId = static_cast<audio::AudioPlayerId>(
        (g_player_seed++ % 8) + 1);  // cycle 1..8
    packet.sequenceNumber = static_cast<uint16_t>(g_player_seed);
    packet.timestampMs    = static_cast<audio::TimestampMs>(g_player_seed * 20);
    packet.data           = data;
    packet.size           = size;

    uint32_t outFrames = 0;
    (void) codec.Decode(packet, g_out.data(), kOutCapacity, outFrames);

    // Also exercise the packet-loss-concealment path. Opus's PLC is
    // its own piece of code and has had bugs around state carry-over
    // when invoked between truly received packets. Calling it after
    // a real Decode is a realistic sequence.
    (void) codec.DecodeLost(packet.playerId, g_out.data(),
                              kOutCapacity, outFrames);

    return 0;
}
