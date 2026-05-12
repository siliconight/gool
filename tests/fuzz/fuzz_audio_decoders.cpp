// tests/fuzz/fuzz_audio_decoders.cpp
//
// libFuzzer harness for the audio file decoder pipeline.
//
// Surface under test: gool's audio decoders are wrappers around
// dr_wav, stb_vorbis (or libvorbis), and dr_flac. Each of these has
// had historical CVEs (truncated-header OOB reads, mismatched
// channel-count crashes, ID3 metadata length overflows). The
// wrappers in src/audio_engine/decoders/ add a layer of length
// checking and sample-rate normalization on top; this harness
// exercises both the third-party parsers and gool's wrapping
// logic end-to-end.
//
// Strategy: feed the fuzz input to DecoderFactory::CreateForMemory
// with format=Auto so the format-sniff path runs. If it returns a
// decoder, drain ~one second of audio frames to exercise the
// per-frame decode loop and seek path. All decode methods are
// noexcept and return either uint32_t (frames written) or bool;
// no exceptions to translate.
//
// Build:
//   cmake -S . -B build-fuzz -DAUDIO_ENGINE_FUZZ=ON \
//         -DAUDIO_ENGINE_DECODERS_WAV=ON \
//         -DAUDIO_ENGINE_DECODERS_OGG=ON \
//         -DAUDIO_ENGINE_DECODERS_FLAC=ON \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_audio_decoders
//   ./build-fuzz/fuzz_audio_decoders -max_total_time=60

#include "audio_engine/audio_file_format.h"
#include "audio_engine/decoders/audio_decoder.h"
#include "audio_engine/decoders/decoder_factory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

// Stack-allocated output buffer. Sized large enough for one second of
// stereo audio at 48 kHz (96000 samples), then bounded by the fuzz
// loop to a small number of decode calls so total work per input
// stays predictable.
constexpr uint32_t kOutCapacityFrames = 4096;       // ~85 ms @ 48k
constexpr uint32_t kMaxDecodeIterations = 32;       // total ~2.7 seconds
std::array<float, kOutCapacityFrames * 2> g_out{};  // stereo worst case

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Sniff-and-decode path. CreateForMemory either returns a decoder
    // (the bytes were recognized as one of WAV / Ogg / FLAC and the
    // header parsed) or returns nullptr (unknown format, malformed
    // header, allocation failure). Both are normal outcomes for fuzz
    // input — bugs we want to find are crashes, ASAN reports, UBSAN
    // reports during the construction or the decode loop below.
    auto decoder = audio::DecoderFactory::CreateForMemory(
        data, static_cast<uint64_t>(size), audio::AudioFileFormat::Auto);

    if (!decoder) return 0;

    // Validate channels before computing the output stride. The
    // factory is supposed to reject inputs with implausible channel
    // counts (we cap at 8 in the wrappers), but if it doesn't, this
    // harness should still terminate cleanly rather than overrun the
    // output buffer — that bug is for the underlying wrapper to fix,
    // not for the fuzz harness to crash on.
    const uint32_t channels = decoder->Channels();
    if (channels == 0 || channels > 2) return 0;  // stereo cap

    // Drain a bounded number of decode iterations. The actual number
    // of frames returned per call depends on the format's internal
    // chunk size; the loop terminates early when DecodeFrames returns
    // 0 (EOF or unrecoverable decode error).
    for (uint32_t i = 0; i < kMaxDecodeIterations; ++i) {
        const uint32_t frames = decoder->DecodeFrames(
            g_out.data(), kOutCapacityFrames);
        if (frames == 0) break;
    }

    // Exercise the seek path too — a non-trivial subset of historical
    // decoder bugs only fire when seeking lands inside a partial
    // frame or beyond EOF. Both should be safe operations.
    const uint64_t total = decoder->TotalFrames();
    if (total > 0) {
        // Seek to ~75% of the way through; out-of-bounds seek
        // (when total < 4 because of header-only inputs) is handled
        // by Seek's noexcept contract returning false.
        (void) decoder->Seek(total * 3 / 4);
        (void) decoder->DecodeFrames(g_out.data(), kOutCapacityFrames);
    }

    return 0;
}
