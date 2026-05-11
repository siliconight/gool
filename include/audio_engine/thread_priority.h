// audio_engine/thread_priority.h
//
// Opt-in real-time thread scheduling for hosts whose audio workload runs
// on a dedicated thread and needs latency guarantees stronger than what
// the OS scheduler gives a normal thread.
//
// WHO NEEDS THIS:
//
// Most hosts DON'T. The default engine architecture has no
// engine-owned worker threads — everything runs on either (a) the
// host's game thread (via `AudioRuntime::Update`/`Tick`) or (b) the
// audio backend's render thread (managed by miniaudio or your custom
// IAudioBackend, which sets its own priority correctly). For (a), the
// host's game thread already gets adequate priority from the OS for
// typical gameplay scenarios.
//
// You'd want this helper if:
//   * You've moved audio control to a dedicated thread (not the game
//     thread) and that thread is missing its deadlines.
//   * You're shipping on constrained hardware (mobile, embedded,
//     low-end laptops) where the default scheduler is too generous to
//     other apps.
//   * You're seeing audio glitches under heavy CPU contention and
//     profiling shows scheduler latency, not engine work.
//
// WHO SHOULDN'T USE THIS:
//
//   * Don't call this on a thread that does general-purpose work
//     (rendering, physics, networking). Elevating that thread can
//     starve other subsystems. RT scheduling is for dedicated
//     low-latency workers, not catch-all threads.
//   * Don't call this from a thread that holds locks shared with
//     non-RT threads. Classic priority-inversion territory.
//
// PER-PLATFORM MAPPING:
//
//   * Linux:   pthread_setschedparam(SCHED_FIFO, priority=5).
//              Conservative — well below kernel threads (50+) but
//              above all normal threads (default 0). Requires either
//              CAP_SYS_NICE on the binary or RLIMIT_RTPRIO via
//              `ulimit -r 99` (or pam_limits.so). Failure: EPERM.
//   * macOS:   thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY) with
//              period=5ms, computation=2ms, constraint=5ms. The values
//              are conservative defaults that work for typical 48kHz
//              audio with 512-frame buffers (~10.6ms). Hosts running
//              other sample rates can call thread_policy_set directly
//              with tuned values.
//   * Windows: SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL).
//              We don't use MMCSS (AvSetMmThreadCharacteristics) here
//              to avoid linking avrt.lib by default — hosts that want
//              MMCSS on top can call AvSetMmThreadCharacteristics
//              themselves after this returns.
//
// IDEMPOTENT: subsequent calls on the same thread re-apply the same
// policy (cheap, no-op effect).
//
// NOT CALLED AUTOMATICALLY by AudioRuntime. Opt-in only.
//
// COMPLEMENTS LockEngineMemory(): RT scheduling without locked memory
// can still glitch when pages get swapped; locked memory without RT
// scheduling can still glitch when the OS deschedules the audio thread.
// For full real-time reliability under contention, call both.

#ifndef AUDIO_ENGINE_THREAD_PRIORITY_H
#define AUDIO_ENGINE_THREAD_PRIORITY_H

namespace audio {

enum class AudioThreadKind {
    // The thread that calls AudioRuntime::Update()/Tick(). Typically
    // the host's main game thread.
    AudioControlThread,

    // A dedicated audio-worker thread (decode, streaming pump, etc.)
    // that does latency-sensitive work but is NOT the device callback
    // thread. The device callback thread is owned by the backend; do
    // not call this helper on it.
    AudioWorkerThread,
};

struct ThreadPriorityResult {
    // True if the OS accepted the priority/policy request. False on
    // failure (typical: insufficient privileges); the calling thread
    // keeps whatever priority it had.
    bool success;

    // Platform identifier: "linux", "macos", "windows", "unsupported".
    const char* platform;

    // Human-readable status. On success: brief description of what
    // policy was applied. On failure: errno (POSIX) / kern_return_t
    // (Mach) / GetLastError (Windows) name plus a remediation hint.
    // Statically allocated or process-lifetime; do not free.
    const char* details;
};

// Apply low-latency scheduling to the CALLING thread. See header
// comment for who should and shouldn't use this.
ThreadPriorityResult SetCurrentThreadAudioPriority(
    AudioThreadKind kind) noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_THREAD_PRIORITY_H
