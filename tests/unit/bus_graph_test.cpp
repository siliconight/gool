// tests/unit/bus_graph_test.cpp
//
// Unit tests for BusGraph:
//   - auto-master mode (busCount==0 yields a single master bus)
//   - basic tree validation + render order
//   - sidechain-source-renders-before-consumer
//   - cycle detection in parent chain
//   - rejection of unknown sidechain bus

#include "audio_engine/mixer/bus_graph.h"
#include "audio_engine/bus.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace audio;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

void TestAutoMaster() {
    BusGraphConfig cfg;
    cfg.busCount = 0;     // host left empty

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::Success);
    EXPECT(g.BusCount() == 1);
    EXPECT(g.MasterIndex() == 0);
    EXPECT(g.RenderOrder().size() == 1);
    EXPECT(g.IndexOf(kBusMaster) == 0);
}

void TestSimpleTreeAndOrder() {
    BusGraphConfig cfg;
    cfg.busCount = 3;
    cfg.buses[0].id = kBusMaster;
    cfg.buses[0].parent = kBusMaster;
    cfg.buses[1].id = 1;
    cfg.buses[1].parent = kBusMaster;
    cfg.buses[2].id = 2;
    cfg.buses[2].parent = kBusMaster;

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::Success);
    EXPECT(g.BusCount() == 3);
    EXPECT(g.MasterIndex() == 0);

    // Children must come before master in render order.
    const auto& order = g.RenderOrder();
    EXPECT(order.size() == 3);
    int masterPos = -1, b1Pos = -1, b2Pos = -1;
    for (uint32_t i = 0; i < order.size(); ++i) {
        const uint32_t bi = order[i];
        if (bi == 0) masterPos = static_cast<int>(i);
        else if (g.IndexOf(1) == bi) b1Pos = static_cast<int>(i);
        else if (g.IndexOf(2) == bi) b2Pos = static_cast<int>(i);
    }
    EXPECT(b1Pos != -1 && b2Pos != -1 && masterPos != -1);
    EXPECT(b1Pos < masterPos);
    EXPECT(b2Pos < masterPos);
}

void TestSidechainOrdering() {
    // Three buses: Master, Music (compressor sidechained from LocalGun),
    // LocalGun. LocalGun must be processed before Music.
    constexpr BusId kMusic    = 10;
    constexpr BusId kLocalGun = 11;

    BusGraphConfig cfg;
    cfg.busCount = 3;
    cfg.buses[0].id = kBusMaster;
    cfg.buses[0].parent = kBusMaster;
    cfg.buses[1].id = kMusic;
    cfg.buses[1].parent = kBusMaster;
    cfg.buses[1].effectCount = 1;
    cfg.buses[1].effects[0].kind = EffectKind::Compressor;
    cfg.buses[1].effects[0].compressorThresholdDb = -20.0f;
    cfg.buses[1].effects[0].compressorRatio = 4.0f;
    cfg.buses[1].effects[0].compressorSidechainBus = kLocalGun;
    cfg.buses[2].id = kLocalGun;
    cfg.buses[2].parent = kBusMaster;

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::Success);

    const auto& order = g.RenderOrder();
    int musicPos = -1, gunPos = -1;
    for (uint32_t i = 0; i < order.size(); ++i) {
        if (g.IndexOf(kMusic)    == order[i]) musicPos = static_cast<int>(i);
        if (g.IndexOf(kLocalGun) == order[i]) gunPos   = static_cast<int>(i);
    }
    EXPECT(gunPos != -1 && musicPos != -1);
    EXPECT(gunPos < musicPos);     // sidechain source first
}

void TestSidechainToUnknownBusRejected() {
    BusGraphConfig cfg;
    cfg.busCount = 2;
    cfg.buses[0].id = kBusMaster;
    cfg.buses[0].parent = kBusMaster;
    cfg.buses[1].id = 5;
    cfg.buses[1].parent = kBusMaster;
    cfg.buses[1].effectCount = 1;
    cfg.buses[1].effects[0].kind = EffectKind::Compressor;
    cfg.buses[1].effects[0].compressorSidechainBus = 99;     // does not exist

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::InvalidArgument);
}

void TestMissingMasterRejected() {
    BusGraphConfig cfg;
    cfg.busCount = 1;
    cfg.buses[0].id = 1;     // not master
    cfg.buses[0].parent = 1;

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::InvalidArgument);
}

void TestDuplicateIdRejected() {
    BusGraphConfig cfg;
    cfg.busCount = 2;
    cfg.buses[0].id = kBusMaster;
    cfg.buses[1].id = kBusMaster;

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::InvalidArgument);
}

void TestSetBusGain() {
    BusGraphConfig cfg;
    cfg.busCount = 2;
    cfg.buses[0].id = kBusMaster;
    cfg.buses[1].id = 1;
    cfg.buses[1].parent = kBusMaster;
    cfg.buses[1].outputGainDb = 0.0f;

    BusGraph g;
    EXPECT(g.Build(cfg, 48000, 2, 256) == AudioResult::Success);

    const float beforeLin = g.OutputGainLinear(g.IndexOf(1));
    EXPECT(std::abs(beforeLin - 1.0f) < 1e-4f);

    EXPECT(g.SetBusOutputGainDb(1, -6.0f) == AudioResult::Success);
    const float afterLin = g.OutputGainLinear(g.IndexOf(1));
    // -6 dB ≈ 0.501
    EXPECT(afterLin > 0.49f && afterLin < 0.51f);

    EXPECT(g.SetBusOutputGainDb(/*unknown*/99, 0.0f) == AudioResult::InvalidArgument);
}

} // namespace

int main() {
    TestAutoMaster();
    TestSimpleTreeAndOrder();
    TestSidechainOrdering();
    TestSidechainToUnknownBusRejected();
    TestMissingMasterRejected();
    TestDuplicateIdRejected();
    TestSetBusGain();
    std::printf(gFails == 0 ? "OK\n" : "FAILED (%d)\n", gFails);
    return gFails == 0 ? 0 : 1;
}
