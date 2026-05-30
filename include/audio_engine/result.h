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

// audio_engine/result.h
//
// Error handling for the public API surface.
//
// We don't take a dependency on C++23 std::expected. Instead:
//   - Result<T>  for fallible operations that produce a value (e.g. handles)
//   - Status     for fallible operations with no value (returned as AudioResult
//                directly when only a code is needed)
//   - AudioResult is the sum of failure codes; success is Success.
//
// Render-thread code does not return Result/Status; it asserts on invariant
// violations and clears a debug "glitch" flag.

#ifndef AUDIO_ENGINE_RESULT_H
#define AUDIO_ENGINE_RESULT_H

#include <cstdint>
#include <utility>
#include <type_traits>

namespace audio {

enum class AudioResult : uint8_t {
    Success = 0,
    InvalidHandle,
    InvalidArgument,
    AssetMissing,
    BackendUnavailable,
    BudgetExceeded,
    NotInitialized,
    AlreadyInitialized,
    QueueFull,
    InternalError,
    Unsupported,    // requested feature compiled out (e.g. an Ogg decoder when AUDIO_ENGINE_DECODERS_OGG=OFF)
    IoError,        // failed to open or read a file
    DecodeError,    // codec rejected the data
    RateLimited,    // replicated event dropped because the per-player token bucket for its category was empty
    PolicyViolation,// replicated event rejected by IReplicationValidator or by AudioReplicationPolicy enforcement
};

// Implemented in result.cpp.
const char* ToString(AudioResult r) noexcept;

// Lightweight Result<T>. T must be default-constructible for the error
// branch's storage. All public-API value types here (handles, ints) satisfy
// this. If a future T does not, use a tagged-union storage; not needed yet.
template <typename T>
class [[nodiscard]] Result {
    static_assert(std::is_default_constructible_v<T>,
                  "Result<T> requires T to be default-constructible");
public:
    // Implicit construction from value or error code is convenient at call
    // sites; both directions are tagged with the boolean state. The implicit
    // conversion is the entire ergonomic point of this expected-shaped type,
    // so cppcheck's noExplicitConstructor warnings are suppressed inline.
    // cppcheck-suppress noExplicitConstructor
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)),
          error_(AudioResult::Success),
          has_value_(true) {}

    // cppcheck-suppress noExplicitConstructor
    Result(AudioResult error) noexcept
        : value_(),
          error_(error),
          has_value_(false) {}

    bool ok() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    AudioResult error() const noexcept { return error_; }

    T&       value() &       { return value_; }
    const T& value() const & { return value_; }
    T&&      value() &&      { return std::move(value_); }

private:
    T           value_;
    AudioResult error_;
    bool        has_value_;
};

} // namespace audio

#endif // AUDIO_ENGINE_RESULT_H
