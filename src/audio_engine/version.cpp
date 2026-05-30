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
