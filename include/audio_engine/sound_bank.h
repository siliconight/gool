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

#ifndef AUDIO_ENGINE_SOUND_BANK_H
#define AUDIO_ENGINE_SOUND_BANK_H

// Sound bank: data-driven asset pipeline for the engine.
//
// What this is, in one paragraph: a runtime loader that reads a JSON
// file describing the game's sounds (file path, category, attenuation,
// bus, priority, etc.), assigns stable hashed IDs to each entry,
// registers every entry with an AudioRuntime, and provides a
// name-to-id lookup the host code uses at play time. Designers author
// the JSON in their normal text editor; programmers reference sounds
// by stable string names (`weapon.ak47.shot`) instead of magic
// numbers; sound groups (`footstep.concrete` resolves to one of N
// concrete-footstep variations) are supported as a first-class
// concept.
//
// Why hashing names instead of sequential IDs: hot-reload. A stable
// hash means an emitter holding `weapon.ak47.shot`'s id keeps working
// after a reload that re-registers the same name. Sequential IDs
// would shuffle on every reload and invalidate every in-flight
// reference.
//
// Schema (full reference in docs/asset_pipeline.md):
//
//   {
//     "version": 1,
//     "defaults": {
//       "category":         "SFX",        // SFX|Voice|Music|Ambience|UI|Dialogue
//       "priority":         "Normal",     // Lowest|Low|Normal|High|Critical
//       "bus":              "master",     // resolved via BusResolver
//       "spatialized":      true,
//       "looping":          false,
//       "occlusionEnabled": true,
//       "replication":      "LocalOnly",  // LocalOnly|OwnerOnly|RemoteRelevant|
//                                         //   Global|ServerAuthoritative|Predicted
//       "attenuation": {
//         "min":     1.0,                 // distance below which gain == 1.0
//         "max":    50.0,                 // distance at/beyond which gain == floor
//         "floor":   0.0,                 // gain past max
//         "falloff": "Logarithmic"        // Linear|Logarithmic|InverseSquare
//       }
//     },
//     "sounds": [
//       { "name": "weapon.ak47.shot",  "file": "sfx/ak47.wav",  "priority": "High" },
//       { "name": "footstep.grass.01", "file": "sfx/grass1.wav" },
//       { "name": "test.beep" },                  // no `file`: see note below
//       ...
//     ],
//
// The `file` field is optional. Entries without a `file` are assumed
// to have audio data pre-registered with the runtime under the same
// hashed id (`HashSoundName(name)`) — useful for procedural sounds,
// debug tones, or runtime-generated audio. The bank still registers
// the SoundDefinition in that case so the entry behaves like any
// other for play/eviction/spatialization.
//     "groups": [
//       { "name":    "footstep.grass",
//         "policy":  "random_no_repeat", // random | random_no_repeat |
//                                        //   sequential | by_material
//         "members": ["footstep.grass.01", "footstep.grass.02", "footstep.grass.03"] }
//     ]
//   }
//
// The `by_material` policy uses a different "members" shape — a
// dict of arrays keyed by AudioMaterial name strings:
//
//     "groups": [
//       { "name":   "bullet_impact",
//         "policy": "by_material",
//         "members_by_material": {
//           "Concrete": ["impact.concrete.01", "impact.concrete.02"],
//           "Wood":     ["impact.wood.01"],
//           "Default":  ["impact.generic.01"]
//         } }
//     ]
//
// At play time, the host code calls Find(name, material) with the
// material the projectile hit; the bank picks one variant at random
// from that material's bucket. If the material has no bucket but
// a "Default" bucket exists, Default is used as the fallback. If
// neither matches, Find returns kInvalidSoundId — designers can
// choose to handle that silently or play a fallback themselves.
//
// Threading: Load* and Reload run on whatever thread the host calls
// them on (typically the loading thread or game thread). Find() is
// thread-safe-for-read after load completes — internally it does an
// atomic-snapshot lookup against an immutable map plus, for groups,
// an atomic counter for the selection policy. Find() may be called
// concurrently with itself; do NOT call it concurrently with Load or
// Reload. Find() does not allocate.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "audio_engine/bus.h"             // BusId, kInvalidBusId
#include "audio_engine/geometry_query.h"  // AudioMaterial (for Find overload)
#include "audio_engine/types.h"           // AudioSoundId, kInvalidSoundId, enums

namespace audio {

class AudioRuntime;

// Outcome of a load or reload operation. Inspect `success` first;
// `errorMessage` is human-readable and `errorLine` (1-based) points
// at the offending line in the JSON if available.
struct SoundBankLoadResult {
    bool        success      = false;
    uint32_t    soundsLoaded = 0;
    uint32_t    groupsLoaded = 0;
    std::string errorMessage;
    int         errorLine    = 0;     // 0 = unknown / not applicable
};

// Optional callbacks/configuration for Load*.
struct SoundBankLoadOptions {
    // Resolve a bus name (e.g. "master", "music") to a runtime BusId.
    // The default resolver returns kBusMaster for "master" and
    // kInvalidBusId for everything else. Host games configure their
    // bus graph at Initialize() and inject a resolver here that maps
    // their bus names to the BusIds they got back from Initialize().
    std::function<BusId(std::string_view name)> busResolver;

    // Read the bytes for a file referenced by a sound entry. The
    // default reads from disk relative to the JSON file's directory
    // (or the current directory if the JSON came from a string).
    // Override for asset packs, encrypted content, Steam Workshop,
    // etc.
    //
    // Return false on failure. The bank reports a load error
    // identifying which sound entry's file failed.
    std::function<bool(std::string_view path,
                       std::vector<uint8_t>& outBytes)> fileLoader;

    // Validate that every group member references a real sound, that
    // no name is defined twice, and that bus references resolve.
    // Default true. Disable only for test-loading partial banks.
    bool validateReferences = true;
};

// Hash a sound name into an AudioSoundId via FNV-1a (32-bit). This
// is the same hash the bank uses internally; exposed so host code
// that wants to bypass Find() (in extremely hot inner loops) can
// hash at compile time. Collisions are detected and reported at
// load time, so as long as your bank loads cleanly, the hashes are
// safe.
constexpr AudioSoundId HashSoundName(std::string_view name) noexcept {
    uint32_t h = 2166136261u;
    for (char c : name) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    // Reserve 0 (kInvalidSoundId) by remapping to 1.
    return h == 0u ? 1u : h;
}

class SoundBank {
public:
    SoundBank();
    ~SoundBank();

    SoundBank(const SoundBank&)            = delete;
    SoundBank& operator=(const SoundBank&) = delete;
    SoundBank(SoundBank&&) noexcept;
    SoundBank& operator=(SoundBank&&) noexcept;

    // Load a JSON sound bank from disk, register every sound and its
    // SoundDefinition with the runtime. Idempotent: subsequent calls
    // re-register, useful for hot-reload (call Reload() instead in
    // most cases). The path is also remembered for Reload().
    SoundBankLoadResult LoadFromJsonFile(AudioRuntime&               runtime,
                                          std::string_view            jsonPath,
                                          const SoundBankLoadOptions& opts = {});

    // Same, parsing JSON from an in-memory string. Reload() re-parses
    // the same string if this entry point was used.
    SoundBankLoadResult LoadFromJsonString(AudioRuntime&               runtime,
                                            std::string_view            json,
                                            const SoundBankLoadOptions& opts = {});

    // Re-run the most recent load against the same runtime. Returns a
    // failure result if no Load* has been called yet.
    SoundBankLoadResult Reload(AudioRuntime& runtime);

    // Resolve a name to an AudioSoundId. For sound names: returns the
    // hashed id if the name was loaded, kInvalidSoundId otherwise.
    // For group names: applies the group's selection policy and
    // returns the chosen member's id. Threadsafe-for-read after
    // load completes. No allocation.
    AudioSoundId Find(std::string_view name) const noexcept;

    // Material-aware overload. For `by_material` groups, picks a
    // random variant from the bucket matching `material`; if that
    // bucket is empty or missing, falls back to the `Default`
    // bucket; if both are empty, returns kInvalidSoundId. For
    // any other policy (random, random_no_repeat, sequential),
    // the material is ignored and behavior is identical to the
    // single-argument overload. For sound names (not groups),
    // material is also ignored. Safe to call with any AudioMaterial.
    AudioSoundId Find(std::string_view name, AudioMaterial material) const noexcept;

    // Was this name loaded as either a sound or a group?
    bool Contains(std::string_view name) const noexcept;

    uint32_t SoundCount() const noexcept;
    uint32_t GroupCount() const noexcept;

    // Clear all internal state. Does NOT unregister anything from the
    // runtime; the runtime keeps any sounds previously registered.
    void Clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace audio

#endif // AUDIO_ENGINE_SOUND_BANK_H
