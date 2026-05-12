// audio_engine/backend/miniaudio_backend.cpp
//
// The only translation unit that includes miniaudio.h. The MINIAUDIO_IMPL
// define instantiates miniaudio's internals here exactly once; do not define
// it in any other .cpp.
//
// Render-thread contract: miniaudio invokes DataCallback on its own audio
// thread. Inside the callback we call IAudioRenderCallback::OnRender, which
// the engine's mixer implements with no allocation, no locks, no syscalls.
// We do not allocate, lock, or block here either; DataCallback is a pure
// pump from miniaudio's frame buffer to the engine's render path.
//
// To slim build size on platforms where the engine never decodes through
// miniaudio (the engine ships its own asset registry and feeds float PCM
// directly), most of miniaudio's optional subsystems are disabled below.
// Re-enable any that a consumer needs by editing the defines or by setting
// them in their build system before this file is compiled.

// ---- miniaudio configuration ------------------------------------------------
// Disable everything we don't use. The audio engine feeds miniaudio float
// PCM that has already been mixed and spatialized; we only need the device
// I/O subsystem.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
// Implementation define; must appear in exactly one TU project-wide.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio_engine/backend/miniaudio_backend.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace audio {

// ---------------------------------------------------------------------------
// Impl: holds the ma_device, render callback pointer, and negotiated state.
// Defined in the .cpp so the public header doesn't need miniaudio.h.
// ---------------------------------------------------------------------------
struct MiniaudioBackend::Impl {
    ma_device             device{};
    IAudioRenderCallback* callback     = nullptr;
    std::atomic<bool>     running      {false};
    uint32_t              sampleRate   = 48000;
    uint32_t              bufferSize   = 512;
    uint32_t              channels     = 2;
    char                  description[160] = {0};

    // v0.15.0: monotonic count of exceptions caught at the render-thread
    // boundary. The DataCallback is marked noexcept (a violation would
    // terminate the host process), and miniaudio is a C API that cannot
    // propagate C++ exceptions in any case — so we wrap the inner
    // OnRender() call in a catch-all that swaps in silence and increments
    // this counter. Surfaced via RenderCallbackExceptions() for
    // observability; non-zero in steady state means the host's
    // IAudioRenderCallback implementation is throwing and audio frames
    // are being dropped to silence. Atomic because the counter is
    // produced on the render thread and read on the control thread.
    std::atomic<uint64_t> renderCallbackExceptions{0};

    // miniaudio's data callback. Runs on miniaudio's audio thread.
    // Forwards into the engine's render callback. Output is interleaved
    // float32, range [-1, 1], `frameCount` frames across `channels`
    // channels. We zero the buffer if the callback has been cleared (the
    // caller is mid-Stop) so the device doesn't latch garbage.
    //
    // v0.15.0 noexcept boundary: any exception from the engine's
    // OnRender() is caught here, the buffer is zeroed (silence rather
    // than latched garbage), and the exception counter increments. This
    // protects the host process from a std::terminate if the engine's
    // render path ever throws — which the engine itself doesn't, but
    // hostile third-party DSP plugins running inside the mixer's effect
    // chain might. The cost when no exception fires is one untaken
    // branch (the try setup is free at the ABI level on Itanium-style
    // unwinding tables).
    static void DataCallback(ma_device* dev,
                             void*       output,
                             const void* /*input*/,
                             ma_uint32   frameCount) noexcept {
        auto* self = static_cast<Impl*>(dev->pUserData);
        const size_t buf_bytes =
            static_cast<size_t>(frameCount) *
            static_cast<size_t>(dev->playback.channels) *
            sizeof(float);
        if (!self || !self->callback) {
            std::memset(output, 0, buf_bytes);
            return;
        }
        try {
            self->callback->OnRender(
                static_cast<float*>(output),
                static_cast<uint32_t>(frameCount),
                static_cast<uint32_t>(dev->playback.channels));
        } catch (...) {
            // Render thread cannot allocate, lock, or log. Silence is
            // the safe fallback — a glitch is better than terminating
            // the host. Counter is atomic; control thread reads it
            // via RenderCallbackExceptions() and can decide whether
            // to emit a log on the next Update() tick.
            std::memset(output, 0, buf_bytes);
            self->renderCallbackExceptions.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
};

// ---------------------------------------------------------------------------
// MiniaudioBackend
// ---------------------------------------------------------------------------

MiniaudioBackend::MiniaudioBackend() : impl_(std::make_unique<Impl>()) {}

MiniaudioBackend::~MiniaudioBackend() {
    Stop();
}

AudioResult MiniaudioBackend::Start(const AudioBackendConfig& config,
                                     IAudioRenderCallback*     callback) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return AudioResult::AlreadyInitialized;
    }
    if (callback == nullptr) {
        return AudioResult::InvalidArgument;
    }

    // Configure the playback device. We request the format/rate/channels the
    // engine wants; miniaudio may negotiate a different value if the device
    // can't honor the request, in which case we capture what was actually
    // negotiated below.
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = config.channels;
    cfg.sampleRate         = config.sampleRate;
    cfg.periodSizeInFrames = config.bufferSize;
    cfg.pUserData          = impl_.get();
    cfg.dataCallback       = &Impl::DataCallback;

    impl_->callback   = callback;
    impl_->sampleRate = config.sampleRate;
    impl_->bufferSize = config.bufferSize;
    impl_->channels   = config.channels;

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        impl_->callback = nullptr;
        return AudioResult::BackendUnavailable;
    }

    // Capture what miniaudio actually negotiated. The engine should query
    // SampleRate()/Channels() after Start() if it cares.
    impl_->sampleRate = impl_->device.sampleRate;
    impl_->channels   = impl_->device.playback.channels;
    // miniaudio doesn't expose the concrete period size everywhere; keep
    // the requested value as our reported BufferSize.

    // Build a human-readable description for diagnostics.
    const char* backendName = ma_get_backend_name(impl_->device.pContext->backend);
    const char* deviceName  = impl_->device.playback.name[0] != '\0'
                                  ? impl_->device.playback.name
                                  : "default";
    std::snprintf(impl_->description, sizeof(impl_->description),
                  "%.63s / %.95s",
                  backendName ? backendName : "miniaudio",
                  deviceName);

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->callback        = nullptr;
        impl_->description[0]  = '\0';
        return AudioResult::BackendUnavailable;
    }

    impl_->running.store(true, std::memory_order_release);
    return AudioResult::Success;
}

void MiniaudioBackend::Stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // ma_device_uninit stops the device and joins miniaudio's audio thread,
    // so once it returns DataCallback can no longer fire on our user data.
    ma_device_uninit(&impl_->device);
    impl_->callback       = nullptr;
    impl_->description[0] = '\0';
}

uint32_t MiniaudioBackend::SampleRate() const noexcept { return impl_->sampleRate; }
uint32_t MiniaudioBackend::BufferSize() const noexcept { return impl_->bufferSize; }
uint32_t MiniaudioBackend::Channels()   const noexcept { return impl_->channels;   }

const char* MiniaudioBackend::DeviceDescription() const noexcept {
    return impl_->description;
}

uint64_t MiniaudioBackend::RenderCallbackExceptions() const noexcept {
    return impl_->renderCallbackExceptions.load(std::memory_order_relaxed);
}

} // namespace audio
