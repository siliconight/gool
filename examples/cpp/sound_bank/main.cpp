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

// examples/sound_bank/main.cpp
//
// Minimal demonstration of the sound bank asset pipeline. Shows:
//
//   1. Pre-registering procedural PCM under known names (so the
//      example doesn't need an on-disk asset directory or a built-in
//      WAV decoder).
//   2. Loading a JSON sound bank with a defaults block, three
//      sounds, and a `random_no_repeat` group.
//   3. Resolving a group name to a member id and submitting a
//      play-at-location event with that id.
//   4. Hot reloading a modified bank and observing the change.
//
// This example uses the NullAudioBackend so it runs anywhere with no
// audio device. In a real game, replace the backend with miniaudio.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/sound_bank.h"
#include "audio_engine/events.h"
#include "audio_engine/config.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<float> Tone(float freq, uint32_t numFrames, uint32_t sampleRate) {
    std::vector<float> out(numFrames);
    for (uint32_t i = 0; i < numFrames; ++i) {
        out[i] = 0.4f * std::sin(2.0f * 3.14159265f * freq *
                                  static_cast<float>(i) /
                                  static_cast<float>(sampleRate));
    }
    return out;
}

} // namespace

int main() {
    constexpr uint32_t kSampleRate = 48000;

    audio::AudioRuntime runtime;
    {
        audio::AudioConfig cfg;
        cfg.sampleRate = kSampleRate;
        cfg.bufferSize = 512;
        cfg.outputMode = audio::AudioOutputMode::Stereo;
        audio::AudioRuntimeDependencies deps;  // null backend by default
        if (runtime.Initialize(cfg, std::move(deps)) != audio::AudioResult::Success) {
            std::fprintf(stderr, "runtime initialize failed\n");
            return 1;
        }
    }

    // --- Step 1: pre-register procedural PCM under three names.
    // In a real game these would be on-disk WAV/OGG/FLAC files
    // referenced by a "file" field in the JSON.
    const std::vector<std::pair<std::string, float>> entries = {
        {"footstep.grass.01",  440.0f},
        {"footstep.grass.02",  494.0f},
        {"footstep.grass.03",  523.0f},
    };
    for (const auto& [name, freq] : entries) {
        const auto id = audio::HashSoundName(name);
        const auto pcm = Tone(freq, kSampleRate / 5, kSampleRate); // 200 ms
        runtime.RegisterPcmSound(
            id,
            std::span<const float>(pcm.data(), pcm.size()),
            kSampleRate, 1u);
    }

    // --- Step 2: load a JSON sound bank that references those names.
    // The bank registers SoundDefinitions for them and builds the
    // random_no_repeat group from the three members.
    const std::string json = R"({
        "version": 1,
        "defaults": {
            "category": "SFX",
            "priority": "Normal",
            "spatialized": true,
            "attenuation": { "min": 1.0, "max": 25.0, "falloff": "Logarithmic" }
        },
        "sounds": [
            { "name": "footstep.grass.01" },
            { "name": "footstep.grass.02" },
            { "name": "footstep.grass.03" }
        ],
        "groups": [
            {
                "name": "footstep.grass",
                "policy": "random_no_repeat",
                "members": [
                    "footstep.grass.01",
                    "footstep.grass.02",
                    "footstep.grass.03"
                ]
            }
        ]
    })";

    audio::SoundBank bank;
    auto r = bank.LoadFromJsonString(runtime, json);
    if (!r.success) {
        std::fprintf(stderr,
                      "bank load failed at line %d: %s\n",
                      r.errorLine, r.errorMessage.c_str());
        return 1;
    }
    std::printf("loaded: %u sounds, %u groups\n",
                 r.soundsLoaded, r.groupsLoaded);

    // --- Step 3: trigger eight footsteps. Each Find("footstep.grass")
    // applies the random_no_repeat policy, returning a different
    // member id from the previous call.
    audio::Vec3 stepPos{0.0f, 0.0f, 1.0f};
    std::printf("\neight footstep triggers:\n");
    for (int i = 0; i < 8; ++i) {
        const auto id = bank.Find("footstep.grass");
        runtime.SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(id, stepPos));

        // Identify which variant fired by comparing against the known
        // hashes (in real game code you wouldn't bother; you just
        // trust the bank to pick).
        const char* which = "?";
        if      (id == audio::HashSoundName("footstep.grass.01")) which = "01";
        else if (id == audio::HashSoundName("footstep.grass.02")) which = "02";
        else if (id == audio::HashSoundName("footstep.grass.03")) which = "03";
        std::printf("  step %d -> footstep.grass.%s\n", i + 1, which);

        runtime.OnTickAdvanced(static_cast<audio::SimulationTick>(i + 1),
                                 static_cast<audio::TimestampMs>((i + 1) * 50));
    }

    // --- Step 4: hot reload with an edited JSON. Change the policy
    // from random_no_repeat to sequential, demonstrate that the same
    // ids now arrive in fixed order.
    const std::string json2 = R"({
        "sounds": [
            { "name": "footstep.grass.01" },
            { "name": "footstep.grass.02" },
            { "name": "footstep.grass.03" }
        ],
        "groups": [
            {
                "name": "footstep.grass",
                "policy": "sequential",
                "members": ["footstep.grass.01",
                             "footstep.grass.02",
                             "footstep.grass.03"]
            }
        ]
    })";
    auto r2 = bank.LoadFromJsonString(runtime, json2);
    if (!r2.success) {
        std::fprintf(stderr, "reload failed: %s\n", r2.errorMessage.c_str());
        return 1;
    }
    std::printf("\nafter hot reload (policy=sequential), six triggers:\n");
    for (int i = 0; i < 6; ++i) {
        const auto id = bank.Find("footstep.grass");
        const char* which = "?";
        if      (id == audio::HashSoundName("footstep.grass.01")) which = "01";
        else if (id == audio::HashSoundName("footstep.grass.02")) which = "02";
        else if (id == audio::HashSoundName("footstep.grass.03")) which = "03";
        std::printf("  step %d -> footstep.grass.%s\n", i + 1, which);
    }

    runtime.Shutdown();
    return 0;
}
