// tests/unit/aligned_float_buffer_test.cpp
//
// Verifies AlignedFloatBuffer's contract:
//   * data() pointer is 64-byte aligned
//   * assign() writes every element (page-touching)
//   * size() reports correctly
//   * Move semantics work (move-only type)
//   * Copy operations are deleted (compile-time check, asserted via
//     std::is_copy_constructible_v == false)
//   * Re-assign with different sizes reallocates correctly

#include "audio_engine/util/aligned_float_buffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>

namespace {

bool IsAligned64(const void* p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) % 64u) == 0u;
}

} // namespace

int main() {
    using audio::util::AlignedFloatBuffer;

    // --- Type traits ---------------------------------------------------
    static_assert(!std::is_copy_constructible_v<AlignedFloatBuffer>,
                  "AlignedFloatBuffer must be move-only (copy disabled)");
    static_assert(!std::is_copy_assignable_v<AlignedFloatBuffer>,
                  "AlignedFloatBuffer must be move-only (copy disabled)");
    static_assert(std::is_move_constructible_v<AlignedFloatBuffer>,
                  "AlignedFloatBuffer must be move-constructible");
    static_assert(std::is_move_assignable_v<AlignedFloatBuffer>,
                  "AlignedFloatBuffer must be move-assignable");
    static_assert(std::is_nothrow_move_constructible_v<AlignedFloatBuffer>,
                  "Move constructor must be noexcept (vector<MixVoice> "
                  "needs this for strong exception guarantee on growth)");

    // --- Default-constructed buffer ------------------------------------
    AlignedFloatBuffer buf;
    assert(buf.size() == 0);
    assert(buf.empty());
    assert(buf.data() == nullptr);

    // --- Allocate + fill -----------------------------------------------
    const std::size_t kFrames = 2048;   // 8 KB; spans multiple cache lines.
    buf.assign(kFrames, 0.5f);
    assert(buf.size() == kFrames);
    assert(buf.data() != nullptr);
    assert(IsAligned64(buf.data()));    // The core alignment guarantee.

    // Every element written (page-touching contract).
    for (std::size_t i = 0; i < kFrames; ++i) {
        assert(buf[i] == 0.5f);
    }

    // --- Re-assign with same size (no reallocation expected) -----------
    float* dataBefore = buf.data();
    buf.assign(kFrames, 1.0f);
    assert(buf.size() == kFrames);
    assert(buf.data() == dataBefore);   // Allocation was reused.
    for (std::size_t i = 0; i < kFrames; ++i) {
        assert(buf[i] == 1.0f);
    }

    // --- Re-assign with different size (reallocation expected) ---------
    buf.assign(kFrames * 2, -1.0f);
    assert(buf.size() == kFrames * 2);
    assert(IsAligned64(buf.data()));
    for (std::size_t i = 0; i < kFrames * 2; ++i) {
        assert(buf[i] == -1.0f);
    }

    // --- Move constructor ----------------------------------------------
    AlignedFloatBuffer moved(std::move(buf));
    assert(moved.size() == kFrames * 2);
    assert(IsAligned64(moved.data()));
    assert(moved[0] == -1.0f);
    assert(buf.size() == 0);            // Source is empty after move.
    assert(buf.data() == nullptr);

    // --- Move assignment -----------------------------------------------
    AlignedFloatBuffer dst;
    dst.assign(64, 9.0f);
    dst = std::move(moved);
    assert(dst.size() == kFrames * 2);
    assert(IsAligned64(dst.data()));
    assert(dst[0] == -1.0f);
    assert(moved.size() == 0);

    // --- Iterator support (for completeness) ----------------------------
    AlignedFloatBuffer ranged;
    ranged.assign(16, 7.0f);
    float sum = 0.0f;
    for (float x : ranged) sum += x;
    assert(sum == 16.0f * 7.0f);

    // --- Small allocation still aligned ---------------------------------
    AlignedFloatBuffer tiny;
    tiny.assign(1, 42.0f);
    assert(IsAligned64(tiny.data()));
    assert(tiny[0] == 42.0f);

    std::printf("[aligned_float_buffer_test]\n");
    std::printf("  alignment guarantee:    %zu bytes (kAlignment)\n",
                AlignedFloatBuffer::kAlignment);
    std::printf("  re-assign same size:    no realloc (verified)\n");
    std::printf("  re-assign larger:       realloc, still aligned\n");
    std::printf("  move semantics:         ok (move-only enforced)\n");
    std::printf("[aligned_float_buffer_test] PASSED\n");
    return 0;
}
