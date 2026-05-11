# Threading model

This document describes gool's thread architecture and the host-side
recommendations for real-time scheduling. The information is the
result of the v0.12.3 thread-scheduling audit (audit item 7 from the
v0.11.x memory-management audit).

## TL;DR

**The engine spawns no real-time-critical threads of its own.** All
real-time audio work happens on the audio-backend's device callback
thread, which is owned and prioritized by the backend implementation
(miniaudio for default builds, your `IAudioBackend` otherwise).
Everything else runs on the host's game/control thread.

**If you're seeing audio glitches under load**, the question is
whether your *host's* game thread is getting descheduled — not
whether the engine needs internal priority changes.

## Thread inventory

| Thread role | Created by | Priority concern? |
|-------------|------------|------------------|
| Audio device callback (render thread) | `IAudioBackend` impl (e.g. miniaudio) | Backend's responsibility — already handled |
| `AudioRuntime::Tick()` / `Update()` (control thread) | Host game loop | Host's responsibility |
| Network packet handler | Host network code | Host's responsibility |
| Voice decode | Runs on **control thread** inside `Tick()` | Same as control thread |
| Streaming asset pump | Runs on **control thread** inside `Tick()` | Same as control thread |
| Telemetry emission | Runs on **control thread** inside `Update()` | Same as control thread |
| `NullAudioBackend` render loop | Test-only backend | None (test infrastructure) |

The engine spawns exactly one `std::thread`, and it's in the test
backend (`NullAudioBackend::RenderLoop`). Real audio output uses
the backend's own thread. There is no internal worker pool, no
internal decoder thread, no internal streaming thread.

## What the backend does

The default miniaudio backend handles its render thread's priority
per platform:

- **Linux**: `pthread_setschedparam` with `SCHED_FIFO` if the process
  has `CAP_SYS_NICE` or sufficient `RLIMIT_RTPRIO`. Falls back to
  default priority otherwise.
- **macOS**: Core Audio's HAL applies
  `THREAD_TIME_CONSTRAINT_POLICY` to the I/O thread automatically.
- **Windows**: Sets `THREAD_PRIORITY_TIME_CRITICAL` and registers
  the thread with MMCSS under the `"Audio"` characteristic.

If you replace miniaudio with a custom `IAudioBackend`, you're
responsible for setting your render thread's priority appropriately.

## When the host should care

The host's *control thread* (the one that calls `AudioRuntime::Tick()`
each frame) is responsible for:

- Draining the game/network event ring buffers
- Decoding voice packets into the PCM ring
- Pumping streaming asset decoders
- Updating spatial state for active emitters

None of this is on the audio callback's hard deadline (that's the
backend's thread, reading from the rings that this work feeds). But
*if the control thread is delayed too long*, the rings can underrun
and the audio thread runs out of decoded samples → silence or
glitches.

Default OS scheduling is fine for this on most platforms. You'd
want to elevate the control thread (or move audio work to a
dedicated higher-priority thread) only if:

1. **You're on constrained hardware** — mobile, embedded, low-end
   laptops where the scheduler is generous to background apps.
2. **Profiling shows scheduler latency**, not engine compute, as the
   bottleneck (e.g. `tick took 30ms` but only 2ms of it was inside
   `Tick()`).
3. **You're running long-tail workloads on the same thread** —
   physics, AI, or networking spikes that occasionally starve audio
   from getting its slice.

## Helper API

For hosts who want elevated scheduling without writing per-platform
`#ifdef` ladders, gool provides:

```cpp
#include "audio_engine/thread_priority.h"

// Call this once on the thread that will be doing latency-sensitive
// audio work. Returns a result describing what happened.
audio::ThreadPriorityResult result = audio::SetCurrentThreadAudioPriority(
    audio::AudioThreadKind::AudioControlThread);

if (!result.success) {
    std::fprintf(stderr,
        "Could not elevate audio thread priority on %s: %s\n",
        result.platform, result.details);
    // Continue without RT scheduling; engine still works fine.
}
```

Per-platform mapping the helper applies:

- **Linux**: `pthread_setschedparam(SCHED_FIFO, priority=5)`.
  Conservative — well below kernel threads (50+) but above all
  normal threads (default 0).
- **macOS**: `thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)` with
  period=5ms, computation=2ms, constraint=5ms (safe defaults for
  48kHz / 512-frame typical).
- **Windows**: `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`.
  Hosts wanting MMCSS can call `AvSetMmThreadCharacteristics("Audio")`
  themselves after.

### When the helper might fail

- **Linux**: needs `CAP_SYS_NICE` capability or raised `RLIMIT_RTPRIO`.
  Without these, returns `success=false` with `EPERM` in `details`.
  Fix: `setcap cap_sys_nice=ep /path/to/binary`, or run as a user
  with `RLIMIT_RTPRIO` set via `ulimit -r 99` or `pam_limits.so`.
- **macOS**: usually succeeds even without privileges; rare failure
  modes return a `kern_return_t` code in `details`.
- **Windows**: usually succeeds; container/sandboxed environments
  may deny. `GetLastError` reported in `details`.

### When NOT to use the helper

- **Don't call it on your main game thread.** That thread does
  rendering, physics, AI, networking. Elevating it can starve
  those subsystems.
- **Don't call it on threads that hold locks shared with non-RT
  threads.** Classic priority-inversion territory — an RT thread
  blocking on a lock held by a normal-priority thread can deadlock
  on Linux's `SCHED_FIFO`.
- **Don't call it on the backend's device callback thread.** That
  thread is owned by the backend, which already set its priority
  appropriately. Touching it from outside is wrong.

## Complementary: `LockEngineMemory()`

Real-time reliability under contention needs both:

- **RT scheduling** (`SetCurrentThreadAudioPriority`) — ensures the
  audio thread gets CPU when it needs it
- **Memory locking** (`LockEngineMemory`) — ensures the audio
  thread's pages don't get swapped out

You can ship one without the other, but for full coverage, call both
at process startup. See [`memory_locking.h`](../include/audio_engine/memory_locking.h)
for details.

## What this audit deliberately doesn't do

- **Doesn't change anything inside the engine.** No internal worker
  threads were added; no priorities were forced. The architecture
  is already correct.
- **Doesn't ship MMCSS by default on Windows.** Linking `avrt.lib`
  adds a runtime dependency every distribution would carry. Hosts
  who want MMCSS can call `AvSetMmThreadCharacteristics` themselves
  — one line, no engine-level cost.
- **Doesn't audit the backend's render thread.** The backend is a
  pluggable seam; miniaudio is verified good, and custom backends
  are the implementer's responsibility. If you write your own
  `IAudioBackend`, set your render thread's priority before
  invoking the callback.

## See also

- [`thread_priority.h`](../include/audio_engine/thread_priority.h) — API reference
- [`memory_locking.h`](../include/audio_engine/memory_locking.h) — complementary memory-locking API
- [audit progress in CHANGELOG.md](../CHANGELOG.md) — full v0.12.x audit timeline
