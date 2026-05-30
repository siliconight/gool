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

// audio_engine/util/json_string_decoder.cpp
//
// THE ONLY translation unit that includes <nlohmann/json.hpp>.
//
// See json_string_decoder.h for the API contract and the rationale
// behind isolating nlohmann to a single TU.

#include "audio_engine/util/json_string_decoder.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace audio::util {

bool DecodeJsonStringLiteral(std::string_view quoted_literal,
                             std::string&     out,
                             std::string&     error) noexcept {
    try {
        // nlohmann::json::parse accepts a complete JSON value. A bare
        // quoted string ("foo") is a valid JSON value per RFC 8259,
        // so we hand it the exact source bytes the caller captured —
        // including the surrounding " characters — and ask for the
        // decoded result as a std::string.
        nlohmann::json j = nlohmann::json::parse(quoted_literal);
        if (!j.is_string()) {
            // Should be unreachable: callers establish that the input
            // is a quoted literal before calling. Belt-and-suspenders
            // for the impossible case (e.g. caller passed "42" — a
            // valid JSON value but not a string).
            error = "internal: captured non-string at string position";
            return false;
        }
        out = j.get<std::string>();
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        // nlohmann's what() already includes a useful description —
        // e.g. "[json.exception.parse_error.101] parse error at line 1,
        // column 5: syntax error while parsing value - invalid string:
        // '\\x' is not a valid escape". Pass it through unmodified;
        // the caller can prefix line number / context as needed.
        error = e.what();
        return false;
    } catch (const std::exception& e) {
        // Any other exception nlohmann or its dependencies might throw
        // (e.g. std::bad_alloc on a pathologically long literal). Wrap
        // with a generic prefix so the caller can tell it wasn't a
        // parse_error.
        error = std::string("string decode failed: ") + e.what();
        return false;
    }
    // Unreachable: every code path above either returns or is inside
    // the try block, and the catch handlers all return. But some
    // compilers/static-analyzers want a terminal statement.
}

} // namespace audio::util
