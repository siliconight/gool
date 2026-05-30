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

// audio_engine/runtime/denormal_protection.cpp
//
// Per-platform implementation of SetCurrentThreadDenormalProtection().
// See include/audio_engine/denormal_protection.h for the contract and
// the rationale behind FTZ/DAZ on audio threads.

#include "audio_engine/denormal_protection.h"

#include <cstdint>

// Platform detection for the FPU control register access.
//
// We dispatch on architecture (x86/ARM), not OS, because MXCSR is an
// x86 ISA feature available on every x86 OS, and FPCR/FPSCR are ARM
// ISA features available on every ARM OS. The compiler intrinsics
// (_mm_getcsr/_mm_setcsr on x86, MRS/MSR on ARM) work identically
// across MSVC, GCC, and Clang on each architecture.

#if defined(__x86_64__) || defined(_M_X64) || \
    defined(__i386__)   || defined(_M_IX86)
  #define AUDIO_ENGINE_DENORMAL_PROTECTION_X86 1
  #include <xmmintrin.h>  // _mm_getcsr, _mm_setcsr
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define AUDIO_ENGINE_DENORMAL_PROTECTION_ARM64 1
#elif defined(__arm__)    || defined(_M_ARM)
  #define AUDIO_ENGINE_DENORMAL_PROTECTION_ARM32 1
#endif

namespace audio {

namespace {

#if defined(AUDIO_ENGINE_DENORMAL_PROTECTION_X86)

// MXCSR bit layout (Intel SDM Vol. 1 §10.2.3, AMD APM Vol. 1 §4.4.5):
//
//   bit  0 IE  — invalid op flag (sticky)
//   bit  1 DE  — denormal flag (sticky)
//   bit  2 ZE  — zero-divide flag (sticky)
//   bit  3 OE  — overflow flag (sticky)
//   bit  4 UE  — underflow flag (sticky)
//   bit  5 PE  — precision flag (sticky)
//   bit  6 DAZ — denormals-are-zero    ← we set this (requires SSE3+)
//   bit  7 IM  — invalid op mask
//   bit  8 DM  — denormal mask
//   bit  9 ZM  — zero-divide mask
//   bit 10 OM  — overflow mask
//   bit 11 UM  — underflow mask
//   bit 12 PM  — precision mask
//   bit 13 RC0 — rounding control low bit
//   bit 14 RC1 — rounding control high bit
//   bit 15 FTZ — flush-to-zero         ← we set this
//
// We touch ONLY bits 6 and 15. The rounding control, exception masks,
// and sticky flags are left at whatever the host runtime set them to —
// in practice round-to-nearest-even with all exceptions masked, which
// is what we want for audio anyway and what every audio library
// assumes. RMW (read-modify-write) avoids stomping on anything else
// that might be in MXCSR.
constexpr uint32_t kMxcsrDazBit = 1u << 6;
constexpr uint32_t kMxcsrFtzBit = 1u << 15;

DenormalProtectionResult Apply() noexcept {
    const uint32_t before = _mm_getcsr();
    const uint32_t after  = before | kMxcsrFtzBit | kMxcsrDazBit;
    _mm_setcsr(after);
    return DenormalProtectionResult{
        true,
        "x86_sse",
        "MXCSR FTZ (bit 15) + DAZ (bit 6) set"
    };
}

#elif defined(AUDIO_ENGINE_DENORMAL_PROTECTION_ARM64)

// FPCR bit layout (ARMv8 ARM, D5.2.5):
//
//   bit 24 FZ    — flush-to-zero (single+double precision)
//   bit 19 FZ16  — flush-to-zero for half-precision (ARMv8.2-FP16+)
//                  Reserved/ignored on cores without FP16; safe to set.
//   bit 23 DN    — default NaN mode (don't touch)
//   bits 22-25   — rounding mode / strict mode (don't touch)
//
// We use inline asm for MRS/MSR because there's no portable intrinsic.
// MSVC ARM64 supports __readstatusreg/__writestatusreg, but GCC/Clang
// don't, and the inline asm form works under all three compilers.
constexpr uint64_t kFpcrFz    = 1ull << 24;
constexpr uint64_t kFpcrFz16  = 1ull << 19;

DenormalProtectionResult Apply() noexcept {
    uint64_t fpcr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (kFpcrFz | kFpcrFz16);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    return DenormalProtectionResult{
        true,
        "arm64_neon",
        "FPCR FZ (bit 24) + FZ16 (bit 19) set"
    };
}

#elif defined(AUDIO_ENGINE_DENORMAL_PROTECTION_ARM32)

// FPSCR bit layout (ARMv7 ARM, B6.1.2):
//
//   bit 24 FZ — flush-to-zero
//
// VMRS/VMSR are the VFP equivalents of MRS/MSR. Same compiler-
// portability story as ARM64 — inline asm works everywhere.
constexpr uint32_t kFpscrFz = 1u << 24;

DenormalProtectionResult Apply() noexcept {
    uint32_t fpscr = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= kFpscrFz;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    return DenormalProtectionResult{
        true,
        "arm32_vfp",
        "FPSCR FZ (bit 24) set"
    };
}

#else

DenormalProtectionResult Apply() noexcept {
    // Unknown architecture. Audio still works — IEEE 754 default
    // behavior means denormals are produced and consumed correctly,
    // just slowly when they arise. Returning ok=true so callers
    // don't treat unsupported platforms as a failure (which would
    // pollute logs on e.g. WASM builds).
    return DenormalProtectionResult{
        true,
        "unsupported",
        "Denormal protection is not implemented for this "
        "architecture. Audio will work, but IIR filters and reverb "
        "tails may slow significantly when state variables decay "
        "into the denormal range."
    };
}

#endif

} // namespace

DenormalProtectionResult SetCurrentThreadDenormalProtection() noexcept {
    return Apply();
}

} // namespace audio
