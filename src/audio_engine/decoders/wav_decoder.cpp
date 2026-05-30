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

// audio_engine/decoders/wav_decoder.cpp
//
// WavDecoder implementation. The dr_wav single-header library is included
// here and nowhere else; if AUDIO_ENGINE_DECODERS_WAV is undefined we
// emit a stub TU that returns null factories so the link still resolves.
//
// dr_wav itself is included with DR_WAV_IMPLEMENTATION in exactly one
// translation unit (this one). All decoder state lives behind the
// pimpl `State` struct so the header stays clean.

#include "audio_engine/decoders/wav_decoder.h"

#if defined(AUDIO_ENGINE_DECODERS_WAV)

// dr_wav warns aggressively under -Wpedantic / -Wshadow etc; isolate.
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244)
#  pragma warning(disable : 4245)
#  pragma warning(disable : 4267)
#  pragma warning(disable : 4456)
#  pragma warning(disable : 4701)
#  pragma warning(disable : 4703)
#  pragma warning(disable : 4996)
#endif

#define DR_WAV_IMPLEMENTATION
// IMPORTANT: do NOT define DR_WAV_NO_STDIO here. dr_wav.h checks
// `#ifndef DR_WAV_NO_STDIO` to gate the stdio helpers (`drwav_init_file`
// etc.), which means *defining* the macro — to any value, including 0 —
// excludes those helpers. We rely on `drwav_init_file` below, so the
// default (macro undefined → stdio helpers present) is what we want.
#include "dr_wav.h"

#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace audio {

struct WavDecoder::State {
    drwav wav{};
    bool  initialized = false;
};

std::unique_ptr<WavDecoder> WavDecoder::CreateFromFile(const char* path) {
    if (!path) return nullptr;
    auto d   = std::unique_ptr<WavDecoder>(new WavDecoder());
    d->state_ = std::make_unique<State>();
    if (!drwav_init_file(&d->state_->wav, path, nullptr)) return nullptr;
    d->state_->initialized = true;
    d->sampleRate_  = d->state_->wav.sampleRate;
    d->channels_    = d->state_->wav.channels;
    d->totalFrames_ = d->state_->wav.totalPCMFrameCount;
    return d;
}

std::unique_ptr<WavDecoder> WavDecoder::CreateFromMemory(const uint8_t* data,
                                                          uint64_t       size) {
    if (!data || size == 0) return nullptr;
    auto d   = std::unique_ptr<WavDecoder>(new WavDecoder());
    d->state_ = std::make_unique<State>();
    if (!drwav_init_memory(&d->state_->wav, data,
                            static_cast<size_t>(size), nullptr)) return nullptr;
    d->state_->initialized = true;
    d->sampleRate_  = d->state_->wav.sampleRate;
    d->channels_    = d->state_->wav.channels;
    d->totalFrames_ = d->state_->wav.totalPCMFrameCount;
    return d;
}

WavDecoder::~WavDecoder() {
    if (state_ && state_->initialized) drwav_uninit(&state_->wav);
}

uint32_t WavDecoder::DecodeFrames(float* out, uint32_t frames) noexcept {
    if (!state_ || !out || frames == 0) return 0;
    return static_cast<uint32_t>(
        drwav_read_pcm_frames_f32(&state_->wav, frames, out));
}

bool WavDecoder::Seek(uint64_t frame) noexcept {
    if (!state_) return false;
    return drwav_seek_to_pcm_frame(&state_->wav, frame) != 0;
}

} // namespace audio

#else // AUDIO_ENGINE_DECODERS_WAV not defined

namespace audio {

struct WavDecoder::State {};   // unused; needed so unique_ptr<State> destructor links
WavDecoder::~WavDecoder() = default;
std::unique_ptr<WavDecoder> WavDecoder::CreateFromFile(const char*)             { return nullptr; }
std::unique_ptr<WavDecoder> WavDecoder::CreateFromMemory(const uint8_t*, uint64_t) { return nullptr; }
uint32_t WavDecoder::DecodeFrames(float*, uint32_t) noexcept { return 0; }
bool     WavDecoder::Seek(uint64_t) noexcept                  { return false; }

} // namespace audio

#endif
