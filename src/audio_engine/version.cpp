// SPDX-License-Identifier: Apache-2.0
#include "audio_engine/version.h"

namespace audio {

// AE_BUILD_COMMIT is supplied by CMake at configure time as a
// quoted string macro (e.g. -DAE_BUILD_COMMIT="1290502"). When the
// engine is built outside CMake (raw g++ invocation, custom build
// systems), the macro is undefined and we fall back to "unknown".
#ifndef AE_BUILD_COMMIT
#define AE_BUILD_COMMIT "unknown"
#endif

const char* const kBuildCommit = AE_BUILD_COMMIT;

Version GetVersion() noexcept {
    return Version{
        kVersionMajor,
        kVersionMinor,
        kVersionPatch,
        kVersionFull,
        kBuildCommit,
    };
}

} // namespace audio
