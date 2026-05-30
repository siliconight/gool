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

// audio_engine/result.cpp

#include "audio_engine/result.h"

namespace audio {

const char* ToString(AudioResult r) noexcept {
    switch (r) {
        case AudioResult::Success:            return "Success";
        case AudioResult::InvalidHandle:      return "InvalidHandle";
        case AudioResult::InvalidArgument:    return "InvalidArgument";
        case AudioResult::AssetMissing:       return "AssetMissing";
        case AudioResult::BackendUnavailable: return "BackendUnavailable";
        case AudioResult::BudgetExceeded:     return "BudgetExceeded";
        case AudioResult::NotInitialized:     return "NotInitialized";
        case AudioResult::AlreadyInitialized: return "AlreadyInitialized";
        case AudioResult::QueueFull:          return "QueueFull";
        case AudioResult::InternalError:      return "InternalError";
        case AudioResult::Unsupported:        return "Unsupported";
        case AudioResult::IoError:            return "IoError";
        case AudioResult::DecodeError:        return "DecodeError";
        case AudioResult::RateLimited:        return "RateLimited";
        case AudioResult::PolicyViolation:    return "PolicyViolation";
    }
    return "Unknown";
}

} // namespace audio
