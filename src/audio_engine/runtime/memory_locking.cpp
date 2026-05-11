// audio_engine/runtime/memory_locking.cpp
//
// Per-platform implementation of LockEngineMemory(). See
// include/audio_engine/memory_locking.h for the contract and rationale.

#include "audio_engine/memory_locking.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
  #include <sys/mman.h>
  #include <errno.h>
#elif defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace audio {

namespace {

// Set on first successful lock; subsequent calls are no-ops.
std::atomic<bool> g_alreadyLocked{false};

// Static buffer for human-readable failure details. We use a static
// (process-lifetime) buffer rather than thread-local so the returned
// pointer is valid even if the caller saves it across threads. Race
// risk: concurrent failures racing to write this buffer could
// interleave; in practice the same EPERM/ERROR_ACCESS_DENIED message
// is what every caller would see, so the race is benign. We accept
// the trade-off for simpler ownership.
char g_detailBuf[256];

#if defined(__linux__)

constexpr const char* kPlatform = "linux";

MemoryLockResult DoLock() noexcept {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        return MemoryLockResult{
            true,
            kPlatform,
            "mlockall(MCL_CURRENT | MCL_FUTURE) succeeded"
        };
    }
    const int err = errno;
    if (err == EPERM) {
        std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "mlockall failed: EPERM (insufficient privilege). "
            "Raise RLIMIT_MEMLOCK via `ulimit -l unlimited` or "
            "grant CAP_IPC_LOCK to the binary.");
    } else if (err == EAGAIN) {
        std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "mlockall failed: EAGAIN (RLIMIT_MEMLOCK exceeded). "
            "Raise the limit or lock less memory.");
    } else if (err == ENOMEM) {
        std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "mlockall failed: ENOMEM (not enough RAM to lock). "
            "Reduce engine memory footprint or add RAM.");
    } else {
        std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "mlockall failed: %s (errno=%d)",
            std::strerror(err), err);
    }
    return MemoryLockResult{ false, kPlatform, g_detailBuf };
}

#elif defined(__APPLE__)

constexpr const char* kPlatform = "macos";

MemoryLockResult DoLock() noexcept {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        return MemoryLockResult{
            true,
            kPlatform,
            "mlockall(MCL_CURRENT | MCL_FUTURE) succeeded. For full "
            "real-time safety also consider setting the audio thread "
            "to TIME_CONSTRAINT_POLICY via thread_policy_set()."
        };
    }
    const int err = errno;
    std::snprintf(g_detailBuf, sizeof(g_detailBuf),
        "mlockall failed: %s (errno=%d). Note: macOS RLIMIT_MEMLOCK "
        "is typically restrictive; consider also setting the audio "
        "thread to TIME_CONSTRAINT_POLICY for paging-resistant "
        "real-time behavior.",
        std::strerror(err), err);
    return MemoryLockResult{ false, kPlatform, g_detailBuf };
}

#elif defined(_WIN32)

constexpr const char* kPlatform = "windows";

MemoryLockResult DoLock() noexcept {
    // Windows doesn't have mlockall. The closest equivalent is to set
    // a minimum working set with QUOTA_LIMITS_HARDWS_MIN_ENABLE so the
    // OS doesn't trim resident pages below that threshold under
    // pressure. 64MB / 1GB bounds are generous for a typical engine.
    HANDLE  proc  = ::GetCurrentProcess();
    SIZE_T  minWs = 64ull  * 1024ull * 1024ull;     // 64 MB
    SIZE_T  maxWs = 1024ull * 1024ull * 1024ull;    // 1 GB
    if (::SetProcessWorkingSetSizeEx(
            proc, minWs, maxWs,
            QUOTA_LIMITS_HARDWS_MIN_ENABLE |
            QUOTA_LIMITS_HARDWS_MAX_DISABLE)) {
        return MemoryLockResult{
            true,
            kPlatform,
            "SetProcessWorkingSetSizeEx with HARDWS_MIN_ENABLE "
            "succeeded (64MB minimum working set pinned)"
        };
    }
    const DWORD err = ::GetLastError();
    std::snprintf(g_detailBuf, sizeof(g_detailBuf),
        "SetProcessWorkingSetSizeEx failed (GetLastError=%lu). "
        "May require SE_INC_WORKING_SET_NAME privilege.",
        static_cast<unsigned long>(err));
    return MemoryLockResult{ false, kPlatform, g_detailBuf };
}

#else

constexpr const char* kPlatform = "unsupported";

MemoryLockResult DoLock() noexcept {
    return MemoryLockResult{
        false,
        kPlatform,
        "Memory locking is not implemented on this platform. "
        "Recompile with platform-specific support or accept the "
        "default OS paging behavior."
    };
}

#endif

} // namespace

MemoryLockResult LockEngineMemory() noexcept {
    // Idempotent. The atomic exchange ensures only the first caller
    // performs the syscall; subsequent callers see `true` and return
    // a no-op result. Concurrent callers all observe the same outcome.
    if (g_alreadyLocked.exchange(true, std::memory_order_acq_rel)) {
        return MemoryLockResult{
            true,
            kPlatform,
            "already locked on a previous call (no-op)"
        };
    }
    MemoryLockResult r = DoLock();
    if (!r.success) {
        // Roll back the flag so the next caller can retry (e.g. after
        // raising RLIMIT_MEMLOCK).
        g_alreadyLocked.store(false, std::memory_order_release);
    }
    return r;
}

} // namespace audio
