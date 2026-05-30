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

// examples/telemetry/main.cpp
//
// Demonstrates wiring all three built-in telemetry sinks side-by-side.
// Real games typically use one — usually Prometheus for ops dashboards
// or Ring for in-game debug overlays — but they're independent, so
// you can run a Ring sink for live debugging and a Prometheus sink
// for ops simultaneously.
//
// To run: ./telemetry_demo
//
// Output:
//   * stdout: a stream of one-line JSON metrics every 100 ms
//   * a Prometheus snapshot dumped after the simulated 1.5 seconds
//   * the last few ring-buffer samples for in-process inspection

#include "audio_engine/audio_runtime.h"
#include "audio_engine/config.h"
#include "audio_engine/telemetry.h"

#include <cstdio>
#include <iostream>
#include <memory>

int main() {
    // Three sinks — only one should be set on the runtime at a time
    // (the runtime takes a single sink pointer). To run two
    // concurrently, write a host-side fan-out sink that calls into
    // both. We pick the Prometheus one for the runtime and exercise
    // the others through manual invocations to keep the demo
    // self-contained.
    audio::JsonLinesTelemetrySink   jsonSink(stdout);
    audio::PrometheusTelemetrySink  promSink("gool_demo");
    audio::RingTelemetrySink        ringSink(/*capacity*/ 16);

    audio::AudioConfig cfg;
    cfg.sampleRate          = 48000;
    cfg.bufferSize          = 256;
    cfg.outputMode          = audio::AudioOutputMode::Stereo;
    cfg.telemetryIntervalMs = 100;  // 10 Hz

    audio::AudioRuntimeDependencies deps;
    deps.telemetrySink = &promSink;  // wire Prometheus into the runtime

    audio::AudioRuntime rt;
    auto rc = rt.Initialize(cfg, std::move(deps));
    if (rc != audio::AudioResult::Success) {
        std::fprintf(stderr, "Initialize failed\n");
        return 1;
    }

    // Simulate 1.5 seconds of game time. The runtime calls
    // promSink.OnRuntimeStats() ~15 times. We manually fan into
    // jsonSink and ringSink each tick to show what they'd produce.
    for (int frame = 0; frame < 90; ++frame) {
        rt.Update(1.0f / 60.0f);

        // Mirror the runtime's stats into the demo sinks at the same
        // cadence the runtime uses internally (every 6 frames at 60 Hz
        // ≈ 100 ms).
        if (frame % 6 == 0) {
            const auto stats = rt.GetStats();
            const audio::RuntimeStatsSample sample{
                static_cast<audio::TimestampMs>(frame * 1000 / 60),
                stats};
            jsonSink.OnRuntimeStats(sample);
            ringSink.OnRuntimeStats(sample);
        }
    }

    // Show the Prometheus snapshot: this is what a /metrics endpoint
    // would return. Truncated to first ~600 chars for the demo.
    std::cout << "\n--- Prometheus snapshot (HTTP /metrics body) ---\n";
    const std::string snap = promSink.Snapshot();
    std::cout << snap.substr(0, 600);
    if (snap.size() > 600) std::cout << "\n... (" << (snap.size() - 600)
                                       << " more bytes)\n";

    // Show the in-memory ring's last 5 samples for debug-overlay use.
    std::cout << "\n--- Ring buffer (last 5 of " << ringSink.Size()
              << " samples) ---\n";
    auto buf = ringSink.Snapshot();
    const size_t lastN = std::min<size_t>(5, buf.size());
    for (size_t i = buf.size() - lastN; i < buf.size(); ++i) {
        std::cout << "  ts=" << buf[i].timestampMs << " ms"
                  << "  active_emitters=" << buf[i].stats.activeEmitters
                  << "  events_drained=" << buf[i].stats.eventsDrainedLastTick
                  << "  underruns=" << buf[i].stats.renderUnderruns
                  << "\n";
    }

    rt.Shutdown();
    return 0;
}
