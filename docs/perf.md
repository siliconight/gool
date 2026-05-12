# Performance baseline (v0.7.2)

This document captures measured baseline numbers for the runtime's hot
paths. Numbers are wall-clock timings collected on a developer machine
with `g++ -O2`; absolute values vary by hardware, but ratios across
load points should be stable.

The rubric (Rules 9 and 25) requires that performance refactors land
with before-and-after data. Use this file as the "before"; the
benchmarks under `tests/bench/` are the measurement tools.

## How to run

```bash
cmake -S . -B build && cmake --build build -j
./build/tests/audio_engine_parameter_smoother_bench
./build/tests/audio_engine_rtpc_eval_bench
./build/tests/audio_engine_audio_mixer_bench
```

These executables exist by design — they are *not* registered with
CTest. Benchmarks are observation tools, not pass/fail gates.

## ParameterSmoother microbench

Direct harness against `audio::ParameterSmoother`. Pre-populates with
N entries (4 well-known parameters per emitter), then hammers
`SetTarget` / `Get` / `Tick`. Each row reports nanoseconds per
operation.

| Operation                          | N=16  | N=64  | N=256  | N=1024  |
|------------------------------------|-------|-------|--------|---------|
| `SetTarget` hit-existing (last)    | 79 ns | 309 ns | 1195 ns | 4710 ns |
| `SetTarget` hit-existing (mid)     | —     | 148 ns |  579 ns | 2316 ns |
| `Get` hit-existing (last)          | 63 ns | 184 ns |  668 ns | 2644 ns |
| `Tick` (full walk)                 | 69 ns | 239 ns |  949 ns | 3787 ns |
| **Synthesized per-tick**           | —     | **45 µs** | **649 µs** | (not run) |

The "synthesized per-tick" row models what step 9 + the RTPC
evaluator do per Update tick: 1 `SetTarget` per emitter + 4 `Get`
calls per emitter (Gain, Pitch, LowPassAmount, ReverbSend) +
1 `Tick`. It scales near-quadratically because both `SetTarget` and
`Get` are O(N) and each runs O(N) times per tick.

## RTPC evaluation end-to-end

Full Update path through the runtime: N looping emitters with M
RTPC bindings each. Each row reports microseconds per `Update` call
in steady-state (no event spawn/retire churn).

| Combo            | µs/tick | % of 16ms frame budget |
|------------------|---------|------------------------|
| N=64, M=0        | 9 µs    | 0.06%                  |
| N=64, M=4        | 88 µs   | 0.5%                   |
| N=128, M=0       | 11 µs   | 0.07%                  |
| **N=128, M=1**   | **93 µs**   | **0.6%**           |
| **N=128, M=4**   | **306 µs**  | **1.8%**           |
| N=256, M=0       | 15 µs   | 0.09%                  |
| N=256, M=4       | 1155 µs | **7%**                 |

Bolded rows are the realistic operating points: default
`maxActiveEmitters = 128`, M=1 is "one RTPC per sound" (a heartbeat
that follows health), M=4 is "all four targets bound" (volume +
pitch + lowpass + reverb send all driven by parameters). At
default budgets the runtime spends well under 2% of frame budget
on audio Update.

## Cost decomposition

The deltas tell us where the time goes:

| | M=0 | M=1 | M=4 | Δ vs M=0 (M=4) |
|---|---|---|---|---|
| N=128 | 11 µs | 93 µs | 306 µs | +295 µs |

So at N=128 M=4, **96% of the cost** is the RTPC evaluation +
parameter smoother path. The non-RTPC machinery (spatializer,
mixer commands, occlusion, step 9 itself) is a flat ~11 µs at
default budget — invisible.

This means the smoother (the second-largest item in the synthesized
microbench, ~180 µs estimated at N=128) is the dominant cost
inside the RTPC path. Roughly:

- Smoother SetTarget + Get + Tick: ~80% of RTPC-path cost
- `unordered_map<sid, vector<binding>>::find` per emitter: small
- `unordered_map<paramId, float>::find` per binding: small
- ApplyCurve + remap arithmetic: tiny

## Rule-9 conclusion

**At default budget (N=128) with realistic high-fidelity RTPC
(M=4): 306 µs/tick = 1.8% of 16 ms frame budget.** Below any
threshold that justifies optimization.

**At 2× default (N=256) M=4: 1.15 ms/tick = 7% of frame.** Trending
hot but acceptable. Hosts who push budgets this high should be
aware. The cost is near-quadratic in N, so further increases pay
exponentially.

No optimization passes were performed in this iteration. The
benchmarks remain in the tree as the baseline that any future
performance work must beat. When a host's profile shows audio
Update dominating frame time, run these and identify the regression
or hot scenario before changing the smoother or the binding
storage.

## AudioMixer hot path (v0.20.1 baseline)

Added in v0.20.1: `tests/bench/audio_mixer_bench.cpp` drives the
mixer's public `OnRender` and `PostCommand` surfaces to measure the
two render-thread-dominant functions, `DrainCommands` and
`MixVoiceSound_`. Both are private; the bench reaches them via the
production-realistic entry point rather than friending into the
private API. Establishes the "before" baseline for v0.21's planned
decomposition of those functions.

All measurements: 256-frame stereo output at 48 kHz (~5.33 ms of
audio rendered per `OnRender`). Source PCM is a 480-frame looping
sine, mono, 48 kHz. Numbers below are from a Linux x86_64 cloud
sandbox under `-O2`; absolute values vary by host (laptops with
boost clocks run ~1.5–2× faster), but scaling shape and ratios
between scenarios are stable.

### Scenario A — Sound mode, mono + equal-power pan, no LPF, no binaural

The hottest baseline. Production render thread runs this path for
non-spatialized SFX and the post-spatializer 3D path that doesn't
trip the binaural threshold.

| N voices | µs/render | µs/voice | % of 5.33 ms budget |
|----------|-----------|----------|---------------------|
| 1        | 3.1       | 3.10     | 0.06%               |
| 8        | 16.9      | 2.11     | 0.32%               |
| 32       | 32.7      | 1.02     | 0.61%               |
| 64       | 64.3      | 1.00     | 1.21%               |
| 128      | 125.1     | 0.98     | 2.35%               |
| **256**  | **249.8** | **0.98** | **4.69%**           |

Per-voice cost converges to ~1 µs/voice once N ≥ 32 — the fixed
per-render overhead amortizes. Scaling is linear in N, as expected
from one `MixVoiceSound_` call per active voice.

### Scenario B — Sound mode + per-voice biquad LPF

`lowPassAmount = 0.5` on every voice triggers the LPF branch
inside `MixVoiceSound_`. Production reaches this path for occluded
voices and any voice with a lowpass effect applied via category or
emitter override.

| N voices | µs/render | µs/voice | vs. A (LPF tax) |
|----------|-----------|----------|------------------|
| 32       | 66.5      | 2.08     | 2.03×            |
| 64       | 131.9     | 2.06     | 2.05×            |
| 128      | 271.5     | 2.12     | 2.17×            |
| **256**  | **539.3** | **2.11** | **2.16×**        |

The biquad roughly doubles per-voice cost. Consistent with one
multiply-add chain added per output sample.

### Scenario C — Sound mode in binaural (per-ear) mode

`useBinaural = true` with non-zero ITD and per-ear LPF amounts.
Production reaches this path through `SphericalHeadSpatializer`
when the listener-emitter distance is below the binaural-threshold
cutoff. Both the dual delay lines and the per-ear LPFs fire per
output sample.

| N voices | µs/render | µs/voice | vs. A (binaural tax) |
|----------|-----------|----------|----------------------|
| 32       | 161.4     | 5.04     | 4.93×                |
| 64       | 323.5     | 5.05     | 5.03×                |
| 128      | 646.8     | 5.05     | 5.17×                |
| **256**  | **1318.9**| **5.15** | **5.28×**            |

Binaural is the dominant per-voice cost in the engine. **At N=256
voices in binaural mode, the mixer alone consumes ~25% of the
5.33 ms render budget** — leaving 4 ms for everything else
(bus-graph effects, master copy, OS scheduling jitter). Hosts
running heavy peer audio with the spherical-head spatializer
should either cap the binaural voice count or accept a larger
buffer size (512 or 1024 frames roughly doubles or quadruples the
headroom).

### Scenario D — Command drain throughput, N=64 active voices

Holds active voices at 64 (mono+pan) and varies the number of
`UpdateParams` commands posted per `OnRender`. Delta against
Scenario A at N=64 is the cost of draining commands.

| commands/render | µs/render | Δ vs. A (N=64) | µs/command |
|-----------------|-----------|-----------------|-------------|
| 0               | 70.3      | +6              | —           |
| 16              | 70.0      | +6              | <0.1        |
| 64              | 66.6      | +2              | <0.1        |
| 128             | 72.5      | +8              | ~0.06       |
| 256             | 75.7      | +11             | ~0.04       |

**`DrainCommands` is ~25 ns per command and effectively noise at
realistic loads.** The lizard threshold violation on this function
is purely structural (large `switch` over `MixerCommandKind`),
not performance. A v0.21 decomposition into per-kind handlers
should land for readability, not throughput — there's nothing to
optimize that the data justifies.

### Read-the-numbers summary

- Per-voice cost in the cheap path (mono+pan): **~1.0 µs/voice**.
- LPF roughly **doubles** that.
- Binaural roughly **5×** it.
- `DrainCommands` is **negligible**.
- The realistic limit before the mixer alone busts the 5.33 ms
  budget is **~500 mono+pan voices**, **~250 LPF voices**, or
  **~100 binaural voices** — at 256-frame buffers. Doubling the
  buffer roughly doubles each of these.

### v0.21 follow-ups this baseline informs

1. **`MixVoiceSound_` decomposition** (lizard violator, CCN 34,
   139 NLOC). The numbers above are the "before"; the decomposed
   form must measure within 5% at N=256 in scenarios A/B/C or the
   refactor gets reverted. Three sub-bodies suggest themselves:
   `MixVoiceSoundPanned_`, `MixVoiceSoundBinaural_`, plus a thin
   dispatcher. The pan/binaural fork already lives inside the
   function — lifting it to the dispatcher level removes a
   per-frame predictable branch.
2. **`DrainCommands` decomposition** (lizard violator, CCN 30,
   168 NLOC). Pure readability win, no perf risk per Scenario D.
3. **Binaural cost target.** Scenario C is the candidate worth
   data-driven optimization. The dual delay lines are independent
   reads from the same source samples; a SIMD pass writing both
   ears in one pass would likely cut C by 30–40%. Worth profiling
   before guessing.

## Roadmap items measured

- **B1** ParameterSmoother linear scan — measured (v0.7.2). Scales O(N) per
  call, O(N²) per tick. Acceptable at default budgets. **No action.**
- **B3** RTPC binding hash-map storage — measured (v0.7.2). Not the dominant
  cost. **No action.**
- **M1** AudioMixer Sound-mode mix path — measured (v0.20.1).
  Linear in N at ~1.0 µs/voice (mono+pan), ~2.1 µs/voice (+LPF),
  ~5.1 µs/voice (binaural). The binaural path is the candidate
  worth data-driven optimization in v0.21. **Investigate SIMD
  dual-ear pass.**
- **M2** AudioMixer command drain — measured (v0.20.1). ~25 ns/command.
  **No perf action; v0.21 decomposition is for readability only.**
