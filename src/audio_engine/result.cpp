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
    }
    return "Unknown";
}

} // namespace audio
