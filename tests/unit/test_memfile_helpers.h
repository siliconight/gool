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

// tests/unit/test_memfile_helpers.h
//
// Cross-platform replacement for POSIX `open_memstream`, which is not
// available on Windows MSVC. Used by telemetry_test.cpp and
// logging_test.cpp to capture a sink's FILE* output into a string.
//
// USAGE:
//
//     #include "test_memfile_helpers.h"
//
//     FILE* mem = test_helpers::OpenMemFile();
//     assert(mem != nullptr);
//     // ... write to mem with normal stdio (fprintf etc.) ...
//     const std::string content = test_helpers::ReadAndClose(mem);
//     // content now holds everything written; mem has been closed.
//
// Implementation note: std::tmpfile() is portable to Windows and the
// resulting file is automatically deleted when closed (or at program
// exit). For unit-test scratch capture this matches open_memstream's
// "ephemeral memory buffer" semantics close enough — the only
// difference is that the bytes briefly transit disk, which is fine
// for tests.

#ifndef AUDIO_ENGINE_TEST_MEMFILE_HELPERS_H
#define AUDIO_ENGINE_TEST_MEMFILE_HELPERS_H

#include <cstdio>
#include <string>

namespace test_helpers {

inline FILE* OpenMemFile() noexcept {
    return std::tmpfile();
}

inline std::string ReadAndClose(FILE* f) {
    std::string out;
    if (!f) return out;
    std::fflush(f);
    std::rewind(f);
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

} // namespace test_helpers

#endif // AUDIO_ENGINE_TEST_MEMFILE_HELPERS_H
