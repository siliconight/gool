// SPDX-License-Identifier: Apache-2.0
//
// audio_engine/version.h
//
// Compile-time and runtime version information for the gool audio
// engine. Two sources of truth, kept in sync by release procedure:
//
//   * `kVersionMajor` / `kVersionMinor` / `kVersionPatch` here
//   * `project(audio_engine VERSION X.Y.Z)` in the root CMakeLists.txt
//
// Both must match. RELEASING.md walks through the dual-edit step.
//
// The git commit SHA is supplied at compile time via the
// AE_BUILD_COMMIT preprocessor define (set by CMake on configure).
// When the engine is built outside CMake, the SHA falls back to
// "unknown".

#ifndef AUDIO_ENGINE_VERSION_H
#define AUDIO_ENGINE_VERSION_H

namespace audio {

// SemVer triple. Pre-1.0 means the API may evolve in minor bumps;
// breaking changes prompt a major bump once we're past 1.0.
constexpr int  kVersionMajor = 0;
constexpr int  kVersionMinor = 23;
constexpr int  kVersionPatch = 4;

// Stable string form. Update alongside the integer triple.
constexpr const char* kVersionString = "0.23.4";

// Optional pre-release / build-metadata suffix (e.g. "-rc.1", "-dev").
// Empty for stable releases.
constexpr const char* kVersionSuffix = "";

// Full version string including suffix. For "0.2.0" this is "0.2.0";
// for "0.2.0-rc.1" this would be "0.2.0-rc.1".
constexpr const char* kVersionFull = "0.23.4";

// Defined in version.cpp. Reads AE_BUILD_COMMIT (set by CMake) and
// falls back to "unknown" when the engine is built outside CMake.
extern const char* const kBuildCommit;

struct Version {
    int         major;
    int         minor;
    int         patch;
    const char* full;        // "0.2.0" — kVersionFull
    const char* commit;      // 7-char git SHA, or "unknown"
};

// Returns the Version struct. Cheap; the values are compile-time
// constants apart from `commit`, which is a string literal.
Version GetVersion() noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_VERSION_H
