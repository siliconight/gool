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

// audio_engine/memory_locking.h
//
// Opt-in memory locking for real-time audio reliability under memory
// pressure.
//
// WHEN TO USE:
//
// Audio glitches occur when the OS pages out memory the audio thread
// needs, then has to page it back in during the next render callback.
// On modern desktops with plenty of RAM this almost never happens; on
// constrained systems (low-end laptops, machines running heavy
// background workloads, mobile, embedded), it can.
//
// LockEngineMemory() asks the OS to keep the process's pages resident:
//
//   * Linux:   mlockall(MCL_CURRENT | MCL_FUTURE)
//   * macOS:   mlockall(MCL_CURRENT | MCL_FUTURE)
//   * Windows: SetProcessWorkingSetSizeEx with QUOTA_LIMITS_HARDWS_MIN_ENABLE
//
// This is one tool among several. For full real-time safety, ALSO
// elevate the audio-control thread's scheduler priority — see
// `audio/thread_priority.h` (`SetCurrentThreadAudioPriority`). RT
// scheduling without locked memory can still glitch when pages get
// swapped; locked memory without RT scheduling can still glitch when
// the OS deschedules the audio thread. They're complementary.
//
// PRIVILEGES REQUIRED:
//
//   * Linux/macOS: the process's RLIMIT_MEMLOCK must be high enough.
//     Default on most distros is 64KB — way too small. Raise via
//     `ulimit -l unlimited` in the shell, or grant CAP_IPC_LOCK to the
//     binary. Failure manifests as EPERM.
//   * Windows: the process needs SE_INC_WORKING_SET_NAME privilege
//     OR the working-set request must be within
//     SetProcessWorkingSetSizeEx's default bounds. Failure manifests
//     as ERROR_ACCESS_DENIED.
//
// COSTS:
//
//   * Memory: locked pages can't be reclaimed by the OS, reducing
//     headroom for other processes. Don't lock more than you need.
//   * Latency: ~0 at steady state; first call may take milliseconds
//     as the OS walks page tables.
//   * Compatibility: locked pages may interfere with debuggers,
//     profilers, and sanitizers (some allocators expect to page out).
//
// IDEMPOTENT: safe to call multiple times. After the first success,
// subsequent calls are recorded as no-ops.
//
// THREAD-SAFETY: callable from any thread. Internally serialized via
// an atomic flag; concurrent calls are safe but only the first
// performs the system call.
//
// NOT CALLED AUTOMATICALLY by AudioRuntime. The host (your game) must
// opt in explicitly, typically near process startup before the audio
// runtime initializes.

#ifndef AUDIO_ENGINE_MEMORY_LOCKING_H
#define AUDIO_ENGINE_MEMORY_LOCKING_H

namespace audio {

struct MemoryLockResult {
    // True if the OS accepted the lock request (or it was already in
    // effect from a previous successful call). False means the request
    // failed — see `details` for why.
    bool success;

    // Platform identifier: "linux", "macos", "windows", "unsupported".
    // Stable; safe for telemetry tag values.
    const char* platform;

    // Human-readable status. On success: brief description of what
    // was locked. On failure: errno (POSIX) / GetLastError (Windows)
    // name plus a remediation hint. String is statically allocated
    // or process-lifetime; do not free.
    const char* details;
};

// Attempt to lock the engine's resident memory. See header comment
// for platform mapping, privileges required, and limitations.
MemoryLockResult LockEngineMemory() noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_MEMORY_LOCKING_H
