// audio_engine/backend/null_audio_backend.cpp

#include "audio_engine/backend/null_audio_backend.h"

#include <chrono>

namespace audio {

NullAudioBackend::~NullAudioBackend() {
    Stop();
}

AudioResult NullAudioBackend::Start(const AudioBackendConfig& config,
                                      IAudioRenderCallback*    callback) {
    if (running_.load(std::memory_order_acquire)) {
        return AudioResult::AlreadyInitialized;
    }
    if (!callback) return AudioResult::InvalidArgument;
    if (config.sampleRate == 0 || config.bufferSize == 0 || config.channels == 0) {
        return AudioResult::InvalidArgument;
    }

    config_   = config;
    callback_ = callback;
    scratch_.assign(static_cast<size_t>(config.bufferSize) * config.channels, 0.0f);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { this->RenderLoop(); });
    return AudioResult::Success;
}

void NullAudioBackend::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
    callback_ = nullptr;
}

void NullAudioBackend::RenderLoop() {
    using clock = std::chrono::steady_clock;

    const auto periodNs = std::chrono::nanoseconds{
        static_cast<int64_t>(1e9 * static_cast<double>(config_.bufferSize)
                              / static_cast<double>(config_.sampleRate))
    };
    auto next = clock::now() + periodNs;

    while (running_.load(std::memory_order_acquire)) {
        if (callback_) {
            callback_->OnRender(scratch_.data(), config_.bufferSize, config_.channels);
        }
        std::this_thread::sleep_until(next);
        next += periodNs;
        // Drift correction: if we fell well behind, resync.
        const auto now = clock::now();
        if (next + periodNs < now) {
            next = now + periodNs;
        }
    }
}

} // namespace audio
