// tests/unit/compressor_test.cpp
//
// Sanity-checks CompressorEffect at the DSP level, independent of the bus
// graph / mixer / runtime. Drives the effect directly with synthesized
// signals and checks that:
//
//   1. With no sidechain and a quiet input, gain reduction stays at 0 dB.
//   2. With self-sidechain on a loud input, the envelope rises above
//      threshold and the compressor reduces gain.
//   3. With explicit sidechain (loud) on a quiet main input, the main
//      output is attenuated even though the input never crossed threshold;
//      this is the ducking case.

#include "audio_engine/dsp/compressor.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels   = 1;

std::vector<float> ConstantBuffer(uint32_t frames, float amplitude) {
    return std::vector<float>(static_cast<size_t>(frames) * kChannels, amplitude);
}

void TestNoSidechainQuietSignal() {
    // -40 dB threshold means anything below ~0.01 amplitude won't trigger.
    CompressorEffect c(/*thr*/-40.0f, /*ratio*/8.0f, /*attack*/5.0f,
                        /*release*/100.0f, /*makeup*/0.0f, kInvalidBusId);
    c.Prepare(kSampleRate, kChannels);

    auto buf = ConstantBuffer(2048, 0.001f);     // -60 dB, well below threshold
    c.Process(buf.data(), 2048, kChannels, nullptr, 0);

    // No reduction expected.
    EXPECT(c.CurrentReductionDb() < 0.5f);
}

void TestSelfSidechainLoudSignal() {
    // Threshold -20 dB, ratio 8:1. A 0.5-amplitude (-6 dB) signal is well
    // above threshold; should produce substantial reduction.
    CompressorEffect c(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, kInvalidBusId);
    c.Prepare(kSampleRate, kChannels);

    auto buf = ConstantBuffer(static_cast<uint32_t>(kSampleRate * 0.1), 0.5f);
    c.Process(buf.data(), static_cast<uint32_t>(buf.size()), kChannels, nullptr, 0);

    // Envelope should have risen above threshold; reduction > 5 dB.
    EXPECT(c.CurrentEnvelopeDb() > -20.0f);
    EXPECT(c.CurrentReductionDb() > 5.0f);

    // Output should be attenuated relative to input (0.5).
    const float lastSample = buf.back();
    EXPECT(std::abs(lastSample) < 0.5f);
}

void TestSidechainDucksQuietMain() {
    // Main signal is quiet (-30 dB). Sidechain is loud (0 dB). Threshold is
    // -20 dB so the main alone wouldn't compress; the sidechain pushes the
    // envelope above threshold and the main gets ducked.
    CompressorEffect c(-20.0f, 8.0f, 5.0f, 100.0f, 0.0f, /*sc bus stub*/42);
    c.Prepare(kSampleRate, kChannels);

    const uint32_t frames = static_cast<uint32_t>(kSampleRate * 0.1);
    auto main = ConstantBuffer(frames, 0.0316f);     // -30 dB approx
    auto side = ConstantBuffer(frames, 1.0f);         // 0 dB

    const float mainBefore = std::abs(main.front());
    c.Process(main.data(), frames, kChannels, side.data(), kChannels);
    const float mainAfter = std::abs(main.back());

    EXPECT(c.CurrentEnvelopeDb() > -1.0f);     // sidechain drives envelope to ~0 dB
    EXPECT(c.CurrentReductionDb() > 10.0f);    // ratio 8:1 from 20 dB above threshold
    EXPECT(mainAfter < mainBefore * 0.5f);     // main visibly attenuated
}

void TestSidechainReleaseRecovers() {
    // After a loud sidechain pulse ends, the envelope releases and gain
    // reduction should approach 0 over the release period.
    CompressorEffect c(-20.0f, 8.0f, 5.0f, 50.0f, 0.0f, 42);
    c.Prepare(kSampleRate, kChannels);

    const uint32_t pulseFrames = static_cast<uint32_t>(kSampleRate * 0.05);
    const uint32_t restFrames  = static_cast<uint32_t>(kSampleRate * 0.5);

    auto mainPulse = ConstantBuffer(pulseFrames, 0.1f);
    auto sidePulse = ConstantBuffer(pulseFrames, 1.0f);
    c.Process(mainPulse.data(), pulseFrames, kChannels, sidePulse.data(), kChannels);
    const float reductionDuring = c.CurrentReductionDb();

    auto mainRest = ConstantBuffer(restFrames, 0.1f);
    auto sideRest = ConstantBuffer(restFrames, 0.0f);
    c.Process(mainRest.data(), restFrames, kChannels, sideRest.data(), kChannels);
    const float reductionAfter = c.CurrentReductionDb();

    EXPECT(reductionDuring > 5.0f);
    EXPECT(reductionAfter < 1.0f);
}

} // namespace

int main() {
    TestNoSidechainQuietSignal();
    TestSelfSidechainLoudSignal();
    TestSidechainDucksQuietMain();
    TestSidechainReleaseRecovers();
    std::printf(gFails == 0 ? "OK\n" : "FAILED (%d)\n", gFails);
    return gFails == 0 ? 0 : 1;
}
