// tests/unit/version_test.cpp
//
// Validates audio::GetVersion() and the version.h compile-time
// constants. Pinning the version string in the test means a forgotten
// version bump in CMakeLists or a typo in version.h won't slip
// through CI silently.

#include "audio_engine/version.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

int main() {
    std::cout << "[version_test]\n";

    // Compile-time constants
    static_assert(audio::kVersionMajor >= 0);
    static_assert(audio::kVersionMinor >= 0);
    static_assert(audio::kVersionPatch >= 0);
    static_assert(audio::kVersionString != nullptr);
    static_assert(audio::kVersionFull   != nullptr);

    // Runtime accessor returns sane values
    auto v = audio::GetVersion();
    std::cout << "  version: " << v.major << "." << v.minor << "." << v.patch
              << " (full=" << v.full << ", commit=" << v.commit << ")\n";

    // Pinned values for v0.23.12. When bumping, update both this test
    // and the constants in include/audio_engine/version.h plus the
    // project() VERSION in CMakeLists.txt. The triple-edit is
    // documented in RELEASING.md.
    assert(v.major == 0);
    assert(v.minor == 23);
    assert(v.patch == 12);
    assert(std::strcmp(v.full, "0.23.12") == 0);

    // Commit SHA: under CMake builds it should be a 7-char hex
    // string; under raw-g++ builds it falls back to "unknown".
    // Both are valid; just assert non-null and non-empty.
    assert(v.commit != nullptr);
    assert(v.commit[0] != '\0');

    std::cout << "[version_test] PASSED\n";
    return 0;
}
