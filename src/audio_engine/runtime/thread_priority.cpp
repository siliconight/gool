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

// audio_engine/runtime/thread_priority.cpp
//
// Per-platform implementation of SetCurrentThreadAudioPriority(). See
// include/audio_engine/thread_priority.h for the contract.

#include "audio_engine/thread_priority.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
  #include <pthread.h>
  #include <sched.h>
  #include <cerrno>
#endif

#if defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <mach/thread_act.h>
  #include <mach/thread_policy.h>
#endif

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace audio {

namespace {

// Process-lifetime buffer for human-readable failure details. See
// memory_locking.cpp for the rationale; same trade-off applies here.
char g_detailBuf[256];

#if defined(__linux__)

constexpr const char* kPlatform = "linux";

// Conservative SCHED_FIFO priority. Well below kernel threads (50+)
// but above all normal threads (default 0). Going higher risks
// kernel starvation; lower defeats the purpose.
constexpr int kFifoPriority = 5;

ThreadPriorityResult ApplyLinux(AudioThreadKind /*kind*/) noexcept {
    sched_param param{};
    param.sched_priority = kFifoPriority;
    const int rc = ::pthread_setschedparam(
        ::pthread_self(), SCHED_FIFO, &param);
    if (rc == 0) {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "SCHED_FIFO priority=%d applied", kFifoPriority);
        return ThreadPriorityResult{ true, kPlatform, g_detailBuf };
    }
    // pthread_setschedparam returns errno-style codes directly, not
    // -1 with errno set.
    if (rc == EPERM) {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "pthread_setschedparam(SCHED_FIFO) failed: EPERM "
            "(insufficient privilege). Grant CAP_SYS_NICE to the "
            "binary or raise RLIMIT_RTPRIO via `ulimit -r 99` / "
            "pam_limits.so.");
    } else {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "pthread_setschedparam(SCHED_FIFO) failed: %s (rc=%d)",
            std::strerror(rc), rc);
    }
    return ThreadPriorityResult{ false, kPlatform, g_detailBuf };
}

#elif defined(__APPLE__)

constexpr const char* kPlatform = "macos";

ThreadPriorityResult ApplyMacOS(AudioThreadKind /*kind*/) noexcept {
    // Convert wall-clock nanoseconds to mach absolute-time ticks for
    // this machine. Required because thread_time_constraint_policy_data_t
    // is in mach ticks, not nanoseconds.
    mach_timebase_info_data_t tb{};
    if (::mach_timebase_info(&tb) != KERN_SUCCESS || tb.denom == 0) {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "mach_timebase_info failed; cannot compute "
            "TIME_CONSTRAINT_POLICY ticks");
        return ThreadPriorityResult{ false, kPlatform, g_detailBuf };
    }

    auto ns_to_ticks = [&](uint64_t ns) -> uint32_t {
        // ticks = ns * denom / numer
        const uint64_t ticks = (ns * tb.denom) / tb.numer;
        return static_cast<uint32_t>(ticks);
    };

    // Conservative defaults: 5ms period, 2ms compute, 5ms constraint.
    // Suitable for typical 48 kHz / 512-frame configurations
    // (10.6ms audio buffers) — gives the thread ~40% of its period
    // as guaranteed compute, the rest as slack. Hosts running shorter
    // or longer buffer sizes can call thread_policy_set directly with
    // tuned values rather than using this helper.
    thread_time_constraint_policy_data_t policy{};
    policy.period      = ns_to_ticks(5'000'000ull);
    policy.computation = ns_to_ticks(2'000'000ull);
    policy.constraint  = ns_to_ticks(5'000'000ull);
    policy.preemptible = 0;

    const thread_port_t mach_thread =
        ::pthread_mach_thread_np(::pthread_self());
    const kern_return_t rc = ::thread_policy_set(
        mach_thread,
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    if (rc == KERN_SUCCESS) {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "THREAD_TIME_CONSTRAINT_POLICY applied "
            "(period=5ms, computation=2ms, constraint=5ms)");
        return ThreadPriorityResult{ true, kPlatform, g_detailBuf };
    }
    (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
        "thread_policy_set(TIME_CONSTRAINT_POLICY) failed "
        "(kern_return_t=%d)", static_cast<int>(rc));
    return ThreadPriorityResult{ false, kPlatform, g_detailBuf };
}

#elif defined(_WIN32)

constexpr const char* kPlatform = "windows";

ThreadPriorityResult ApplyWindows(AudioThreadKind /*kind*/) noexcept {
    // We use THREAD_PRIORITY_TIME_CRITICAL (+15 above normal). This is
    // the highest standard priority without entering REALTIME_PRIORITY_CLASS
    // territory (which requires the process to be in REALTIME class and
    // can starve system threads).
    //
    // We deliberately don't link avrt.lib and call
    // AvSetMmThreadCharacteristics here — keeping the engine's standard
    // Windows link dependency-free. Hosts that want MMCSS on top of
    // this priority can call it themselves after this returns:
    //
    //   DWORD taskIndex = 0;
    //   HANDLE h = AvSetMmThreadCharacteristicsA("Audio", &taskIndex);
    //   // h reverts automatically when the thread exits.
    HANDLE thread = ::GetCurrentThread();
    if (::SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL)) {
        (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
            "THREAD_PRIORITY_TIME_CRITICAL applied "
            "(consider also AvSetMmThreadCharacteristics(\"Audio\"))");
        return ThreadPriorityResult{ true, kPlatform, g_detailBuf };
    }
    const DWORD err = ::GetLastError();
    (void)std::snprintf(g_detailBuf, sizeof(g_detailBuf),
        "SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL) failed "
        "(GetLastError=%lu)",
        static_cast<unsigned long>(err));
    return ThreadPriorityResult{ false, kPlatform, g_detailBuf };
}

#else

constexpr const char* kPlatform = "unsupported";

ThreadPriorityResult ApplyUnsupported(AudioThreadKind /*kind*/) noexcept {
    return ThreadPriorityResult{
        false,
        kPlatform,
        "Real-time thread scheduling is not implemented on this "
        "platform. The thread keeps its existing priority."
    };
}

#endif

} // namespace

ThreadPriorityResult SetCurrentThreadAudioPriority(
        AudioThreadKind kind) noexcept {
#if defined(__linux__)
    return ApplyLinux(kind);
#elif defined(__APPLE__)
    return ApplyMacOS(kind);
#elif defined(_WIN32)
    return ApplyWindows(kind);
#else
    return ApplyUnsupported(kind);
#endif
}

} // namespace audio
