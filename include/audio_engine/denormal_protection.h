// audio_engine/denormal_protection.h
//
// Per-thread denormal-arithmetic flush controls for the audio render
// path. Sets the CPU's floating-point control register (MXCSR on x86,
// FPCR on ARM64, FPSCR on ARM32) so that:
//
//   * Denormal RESULTS produced by arithmetic ops are flushed to zero
//     ("flush-to-zero", FTZ on x86, FZ on ARM)
//   * Denormal INPUTS to arithmetic ops are treated as zero
//     ("denormals-are-zero", DAZ on x86, FZ16 also covers half-float
//     denormals on ARMv8.2+)
//
// WHY THIS MATTERS
// ================
//
// IEEE 754 subnormal (a.k.a. denormal) floats sit in [~1e-45, ~1e-38).
// On most x86 CPUs (and some ARM cores), denormal arithmetic falls out
// of the FPU's fast path and into microcode. The slowdown is 10–100x.
//
// In audio DSP, denormals show up specifically in IIR feedback paths
// (biquad EQs, reverb tanks, compressor envelopes) when the input
// drops to silence. The state variables decay exponentially toward
// zero, hit the denormal range, and the audio thread stalls — usually
// not enough to underrun the buffer, but enough to spike CPU usage by
// 10x during quiet moments (menu screens, paused gameplay, the few
// seconds of reverb tail after a sound ends).
//
// Setting FTZ+DAZ once per audio thread costs nothing afterward and
// eliminates the entire class of bug. It's standard practice in every
// production audio engine (JUCE, Tracktion, Wwise, FMOD all do this
// at audio-thread entry).
//
// TRADE-OFFS
// ==========
//
// FTZ+DAZ deviate from strict IEEE 754. In exchange for the speed,
// you lose:
//
//   * Gradual underflow. Numbers in (0, smallest_normal) become 0
//     instead of denormal. For audio at any reasonable sample rate,
//     anything below ~1e-30 is inaudible by ~200 dB, so this is fine.
//   * Equality between (a - b) and 0 when a and b differ by a denormal
//     amount. Again: irrelevant at audio precision.
//
// What you DON'T lose:
//   * Normal-range arithmetic precision (unchanged).
//   * NaN, inf, and overflow handling (unchanged).
//   * Round-to-nearest-even (unchanged — we don't touch rounding mode).
//
// WHO SHOULD CALL THIS
// ====================
//
// The engine's miniaudio backend calls SetCurrentThreadDenormalProtection()
// at the top of every DataCallback invocation. Hosts with custom
// IAudioBackend implementations should call it from their own render
// callback's first invocation (or every invocation — the cost is
// ~5 ns on modern x86, negligible).
//
// Hosts that run DSP work on threads OTHER than the audio render
// thread (e.g. an offline mixdown worker, a real-time spectrum
// analyzer thread) should call this from those threads too. The MXCSR/
// FPCR state is per-thread, not per-process — setting it on the audio
// thread does NOT propagate to other threads.
//
// PER-PLATFORM MAPPING
// ====================
//
//   * x86/x86_64: _mm_setcsr with FTZ (bit 15) and DAZ (bit 6) set.
//                 FTZ has been available since SSE1 (1999); DAZ
//                 requires SSE3 (2004). All modern x86_64 CPUs have
//                 both. We set them both unconditionally; SSE3
//                 absence would have failed link long before this.
//
//   * ARM64:     FPCR FZ bit (24). Also FZ16 bit (19) on ARMv8.2+;
//                we set both — FZ16 on older cores is reserved and
//                ignored, no fault.
//
//   * ARM32:     FPSCR FZ bit (24). VFP / NEON only.
//
//   * Other:     no-op. Returns ok=true with platform="unsupported"
//                so callers don't need to special-case (the absence
//                of denormal protection on an unknown platform is
//                not an error — audio still works, just possibly
//                with the slowdown).

#ifndef AUDIO_ENGINE_DENORMAL_PROTECTION_H
#define AUDIO_ENGINE_DENORMAL_PROTECTION_H

namespace audio {

struct DenormalProtectionResult {
    // True on every platform where we either applied the flags or
    // are explicitly a no-op on an unsupported platform. False only
    // when a known platform's set-instruction is expected to work
    // but didn't — which in practice never happens on x86/ARM
    // because the setters are unconditional CSR writes that can't
    // fail at the ISA level. The bool exists for future platforms
    // that might fail (e.g. WASM SIMD with restricted FPU access).
    bool ok;

    // Identifies which code path ran. Useful for telemetry and bug
    // reports. Values:
    //   "x86_sse"    — SSE/SSE3 path (set FTZ + DAZ)
    //   "arm64_neon" — ARM64 path (set FZ + FZ16)
    //   "arm32_vfp"  — ARM32 path (set FZ)
    //   "unsupported"— compiled for a platform without a known FPU
    //                  control mechanism; no-op
    // Statically allocated or process-lifetime; do not free.
    const char* platform;

    // Human-readable description of what was applied (or what was
    // skipped, on unsupported). Statically allocated or
    // process-lifetime; do not free.
    const char* details;
};

// Apply denormal protection to the CALLING thread. Idempotent: calling
// it twice is exactly as cheap as calling it once; you can call from
// every callback if that's simpler than tracking a thread-local flag.
//
// noexcept and lock-free. Safe to call from real-time audio threads.
DenormalProtectionResult SetCurrentThreadDenormalProtection() noexcept;

} // namespace audio

#endif // AUDIO_ENGINE_DENORMAL_PROTECTION_H
