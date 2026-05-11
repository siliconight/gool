// audio_engine/util/aligned_float_buffer.h
//
// 64-byte-aligned float buffer for hot-path audio data.
//
// WHY 64 bytes?
//
// Modern x86 cache lines are 64 bytes. Aligning to a cache line:
//   * Guarantees a load never straddles two cache lines (avoids the
//     ~5-cycle penalty on Intel for cross-line loads).
//   * Matches AVX-512 SIMD width (16 floats = 64 bytes).
//   * Is a superset of AVX (32-byte) and SSE (16-byte) alignment, so
//     `vmovaps` and `movaps` both apply.
//
// Without `-march=native` or `-mavx2`, the compiler typically targets
// SSE2 (16-byte) by default, so the alignment benefit is theoretical.
// But this type costs ~0 to use and is correct under any compile flag
// the user might enable later. Future-proof.
//
// WHY A DEDICATED TYPE INSTEAD OF std::vector<float, AlignedAllocator>?
//
//   1. Smaller API surface: we explicitly DON'T want .push_back, .resize,
//      .reserve, etc. on hot-path buffers. Anything that could trigger a
//      reallocation is dangerous in real-time code. This type exposes
//      .assign (init), .size, .data, and .operator[] — that's it.
//   2. Cleaner ownership: no allocator-template noise.
//   3. Easier to read and audit.
//
// MOVE-ONLY: copying a hot-path audio buffer is almost always a bug.
// Compiler enforces; the source code never accidentally clones a 4KB
// bus buffer.

#ifndef AUDIO_ENGINE_UTIL_ALIGNED_FLOAT_BUFFER_H
#define AUDIO_ENGINE_UTIL_ALIGNED_FLOAT_BUFFER_H

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
  #include <malloc.h>      // _aligned_malloc / _aligned_free
#endif

namespace audio::util {

class AlignedFloatBuffer {
public:
    // 64-byte cache-line alignment. See header comment for rationale.
    static constexpr std::size_t kAlignment = 64;

    AlignedFloatBuffer() noexcept = default;

    ~AlignedFloatBuffer() {
        Deallocate();
    }

    // Move-only. Copying a hot-path audio buffer is almost always a bug.
    AlignedFloatBuffer(const AlignedFloatBuffer&)            = delete;
    AlignedFloatBuffer& operator=(const AlignedFloatBuffer&) = delete;

    AlignedFloatBuffer(AlignedFloatBuffer&& other) noexcept
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    AlignedFloatBuffer& operator=(AlignedFloatBuffer&& other) noexcept {
        if (this != &other) {
            Deallocate();
            data_       = other.data_;
            size_       = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // Allocate `count` floats and fill with `value`. Idempotent if called
    // with the same count (reuses existing allocation); otherwise frees
    // and re-allocates. Matches the std::vector::assign(count, value) API
    // we use today.
    //
    // Writing every element (not just allocating) is *load-bearing* for
    // real-time safety: it forces the OS to back every page with real
    // RAM rather than a copy-on-write zero page, eliminating page faults
    // on the first render callback. See audio_mixer.cpp / bus_graph.cpp
    // for the comments that warn future contributors away from changing
    // this to a non-touching reservation.
    void assign(std::size_t count, float value) noexcept {
        if (size_ != count) {
            Deallocate();
            data_ = Allocate(count);
            size_ = data_ ? count : 0;
        }
        if (data_) {
            for (std::size_t i = 0; i < size_; ++i) {
                data_[i] = value;
            }
        }
    }

    std::size_t size() const noexcept { return size_; }
    bool         empty() const noexcept { return size_ == 0; }

    float*       data()       noexcept { return data_; }
    const float* data() const noexcept { return data_; }

    float&       operator[](std::size_t i)       noexcept { return data_[i]; }
    const float& operator[](std::size_t i) const noexcept { return data_[i]; }

    // Iterator support for range-for over the buffer. Rarely needed on
    // hot paths (we usually access by index), but included for parity
    // with the std::vector<float> we're replacing.
    float*       begin()       noexcept { return data_; }
    float*       end()         noexcept { return data_ + size_; }
    const float* begin() const noexcept { return data_; }
    const float* end()   const noexcept { return data_ + size_; }

private:
    static float* Allocate(std::size_t count) noexcept {
        if (count == 0) return nullptr;
        const std::size_t bytes = count * sizeof(float);
        // Round up to a multiple of kAlignment — required by
        // std::aligned_alloc on POSIX. Doesn't hurt on Windows.
        const std::size_t rounded =
            (bytes + kAlignment - 1) & ~(kAlignment - 1);
#if defined(_WIN32)
        void* p = ::_aligned_malloc(rounded, kAlignment);
#else
        void* p = nullptr;
        if (::posix_memalign(&p, kAlignment, rounded) != 0) {
            p = nullptr;
        }
#endif
        return static_cast<float*>(p);
    }

    void Deallocate() noexcept {
        if (!data_) return;
#if defined(_WIN32)
        ::_aligned_free(data_);
#else
        std::free(data_);
#endif
        data_ = nullptr;
        size_ = 0;
    }

    float*      data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_ALIGNED_FLOAT_BUFFER_H
