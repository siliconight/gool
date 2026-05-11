// tests/unit/thread_priority_test.cpp
//
// Verifies that SetCurrentThreadAudioPriority returns a structured
// result. The sandbox running this test likely lacks the privileges
// to actually elevate priority (SCHED_FIFO requires CAP_SYS_NICE on
// Linux; THREAD_PRIORITY_TIME_CRITICAL requires no special privilege
// on Windows but might fail in containers; macOS may succeed for
// TIME_CONSTRAINT_POLICY without privilege).
//
// So the test asserts the API contract — a valid result struct with
// a populated platform string and details — not that the priority
// was actually applied. CI runs in privilege-limited contexts and
// would otherwise flake.

#include "audio_engine/thread_priority.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using audio::SetCurrentThreadAudioPriority;
    using audio::AudioThreadKind;

    const auto r1 = SetCurrentThreadAudioPriority(AudioThreadKind::AudioControlThread);

    // Contract: platform must be a non-empty C string from the known set.
    assert(r1.platform != nullptr);
    assert(*r1.platform != '\0');
    const bool knownPlatform =
        (std::strcmp(r1.platform, "linux") == 0)   ||
        (std::strcmp(r1.platform, "macos") == 0)   ||
        (std::strcmp(r1.platform, "windows") == 0) ||
        (std::strcmp(r1.platform, "unsupported") == 0);
    assert(knownPlatform);

    // Details must be populated.
    assert(r1.details != nullptr);
    assert(*r1.details != '\0');

    std::printf("[thread_priority_test]\n");
    std::printf("  platform:      %s\n", r1.platform);
    std::printf("  success:       %s\n", r1.success ? "true" : "false");
    std::printf("  details:       %s\n", r1.details);

    // Idempotent: calling again gives the same shape of result.
    // Successful run stays successful; failed run can stay failed
    // (the API doesn't guarantee retry semantics, unlike
    // LockEngineMemory's exchange flag).
    const auto r2 = SetCurrentThreadAudioPriority(AudioThreadKind::AudioControlThread);
    assert(r2.platform != nullptr);
    assert(std::strcmp(r1.platform, r2.platform) == 0);

    // Both kind values are accepted (no enum validation needed at this
    // layer; the kind is currently informational on all platforms).
    const auto rw = SetCurrentThreadAudioPriority(AudioThreadKind::AudioWorkerThread);
    assert(rw.platform != nullptr);
    assert(std::strcmp(r1.platform, rw.platform) == 0);

    std::printf("[thread_priority_test] PASSED\n");
    return 0;
}
