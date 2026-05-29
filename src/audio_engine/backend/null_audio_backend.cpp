// audio_engine/backend/null_audio_backend.cpp

#include "audio_engine/backend/null_audio_backend.h"
#include "audio_engine/denormal_protection.h"

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
    // v0.80.25: ensure denormal protection is enabled on this thread.
    // Set once at thread entry; the MXCSR (x86) / FPCR (ARM) state
    // is per-thread and persists for the loop's lifetime. Unlike the
    // miniaudio backend (where we re-set every callback as defense
    // against third-party DSP plugins), this thread is engine-owned
    // end-to-end so a single set is enough.
    //
    // See include/audio_engine/denormal_protection.h for the rationale
    // (IIR feedback paths producing denormals during silence → 10-100x
    // CPU spike).
    (void)SetCurrentThreadDenormalProtection();
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
