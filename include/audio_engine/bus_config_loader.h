// audio_engine/bus_config_loader.h
//
// JSON → BusGraphConfig parser. Builds a complete bus graph
// (including sidechain references resolved by bus name) plus a
// category-routing map from a JSON document, suitable for handing
// directly to AudioRuntime::Initialize via the AudioConfig.busGraph
// field.
//
// Why this exists: the C++ side of the engine has always supported
// multi-tier sidechain ducking via the BusGraphConfig struct (see
// examples/multi_tier_ducking). Hosts using the engine through the
// GDExtension binding could not access that capability — the binding
// only exposed sample_rate and buffer_size. This loader is the
// translation layer that lets a Godot project ship its bus topology
// in a JSON file (read on init) and get the full multi-tier ducking
// behavior with no engine code changes.
//
// Usage from C++:
//
//     auto result = audio::BusConfigLoader::ParseFromJson(jsonText);
//     if (!result.ok) { handle(result.error, result.errorLine); return; }
//     audio::AudioConfig cfg;
//     cfg.busGraph = result.busGraph;       // categoryMap is nested inside
//     runtime.Initialize(cfg, std::move(deps));
//
// Schema (all fields optional unless noted):
//
//     {
//       "buses": [
//         {
//           "name":     "Master",                // REQUIRED, unique
//           "parent":   "Master",                // bus name or "Master"
//                                                //   for the root; defaults
//                                                //   to "Master"
//           "gain_db":  0.0,
//           "silent":   false,                   // sidechain-source-only
//           "effects": [
//             { "kind": "compressor",
//               "threshold_db": -30.0,
//               "ratio":         8.0,
//               "attack_ms":     5.0,
//               "release_ms":  250.0,
//               "makeup_db":     0.0,
//               "sidechain_bus": "LocalSfx" },   // resolved by name
//             { "kind": "saturation",
//               "drive": 1.5, "mix": 0.15,
//               "output_gain": 0.85, "bias": 0.0 }
//           ]
//         },
//         …
//       ],
//       "category_routing": {                    // optional
//         "music":    "Music",
//         "sfx":      "LocalSfx",
//         "voice":    "Voice",
//         "ambience": "Ambient",
//         "ui":       "Master",
//         "dialogue": "Voice"
//       }
//     }
//
// Effect kinds and their fields match EffectConfig in bus.h. See
// the parser implementation for the exact field-name list per kind.

#ifndef AUDIO_ENGINE_BUS_CONFIG_LOADER_H
#define AUDIO_ENGINE_BUS_CONFIG_LOADER_H

#include "audio_engine/bus.h"
#include "audio_engine/config.h"

#include <optional>
#include <string>
#include <string_view>

namespace audio::BusConfigLoader {

struct ParseResult {
    bool             ok          = false;
    BusGraphConfig   busGraph{};   // categoryMap is nested inside
    // v0.73.0: optional top-level "budget" block. When std::nullopt
    // (absent from JSON), AudioRuntimeBudget defaults apply. When
    // present, hosts should copy this into AudioConfig::budget
    // before calling Initialize. Backward-compatible — pre-v0.73.0
    // configs without a budget block produce nullopt here and the
    // hardcoded defaults (128 active emitters, etc.) stay in effect.
    std::optional<AudioRuntimeBudget> budget;
    std::string      error;        // human-readable; populated when ok = false
    int              errorLine    = 0;
};

// Parse a JSON document into a BusGraphConfig (with categoryMap
// populated as part of the graph). On success, returns ok=true with
// the structure populated.
// On any error (malformed JSON, unknown effect kind, unresolved bus
// reference, duplicate bus name, exceeded BusGraphConfig::kMaxBuses
// or kMaxEffectsPerBus), returns ok=false with `error` and
// `errorLine` populated.
//
// The returned BusGraphConfig is a self-contained value type; copy
// it into AudioConfig::busGraph before calling Initialize.
ParseResult ParseFromJson(std::string_view json);

} // namespace audio::BusConfigLoader

#endif // AUDIO_ENGINE_BUS_CONFIG_LOADER_H
