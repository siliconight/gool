// audio_engine/util/json_string_decoder.h
//
// Single-purpose wrapper around nlohmann/json's JSON-string-literal
// unescape pass.
//
// WHY THIS WRAPPER EXISTS
// =======================
//
// Two TUs in the engine need spec-compliant JSON string unescape
// (`\uXXXX`, `\b`, `\f`, etc.) but otherwise have no use for a full
// JSON library: `bus_config_loader.cpp` and `assets/sound_bank.cpp`.
// Both files have hand-rolled recursive-descent parsers that own
// the rest of the JSON grammar — they only need help with the small
// but spec-heavy escape-decoding step inside string literals.
//
// Pre-v0.81.0, both TUs included `<nlohmann/json.hpp>` directly.
// nlohmann is template-heavy (~30K lines of generic code that
// expands per TU). With the strict CI clang-tidy config
// (`cppcoreguidelines-init-variables` in particular), this caused
// per-TU analysis times of:
//   - sound_bank.cpp:        ~1.5 minutes
//   - bus_config_loader.cpp: ~2.5 hours (pathological worst case)
//
// v0.81.0 hides nlohmann behind THIS wrapper. Only
// json_string_decoder.cpp includes `<nlohmann/json.hpp>`; all other
// TUs see a small, template-free header. clang-tidy still pays the
// nlohmann cost — but only on one file, and that file is small enough
// (~30 lines of code) that the cost is bounded at minutes, not hours.
//
// API CONTRACT
// ============
//
// `quoted_literal` must be a COMPLETE JSON string value per RFC 8259:
// it MUST begin with `"` and end with `"`. The bytes in between may
// contain any spec-permitted escapes. The caller is responsible for
// locating those quotes in their source stream — this wrapper does
// not parse JSON structurally, it only decodes a single literal.
//
// On success: `out` receives the decoded string (no surrounding
// quotes, all escapes resolved), returns true.
//
// On failure: `error` receives a human-readable description sourced
// from nlohmann's `parse_error::what()` (which is already informative
// — "invalid string: '\\x' is not a valid escape" etc.), returns
// false. `out` is left in an unspecified state.
//
// SAFETY
// ======
//
// noexcept (we catch every exception nlohmann may throw and convert
// to the bool/error return). Re-entrant. No global state. Safe to
// call from any thread.

#ifndef AUDIO_ENGINE_UTIL_JSON_STRING_DECODER_H
#define AUDIO_ENGINE_UTIL_JSON_STRING_DECODER_H

#include <string>
#include <string_view>

namespace audio::util {

bool DecodeJsonStringLiteral(std::string_view quoted_literal,
                             std::string&     out,
                             std::string&     error) noexcept;

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_JSON_STRING_DECODER_H
