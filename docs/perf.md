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

## Roadmap items measured

- **B1** ParameterSmoother linear scan — measured. Scales O(N) per
  call, O(N²) per tick. Acceptable at default budgets. **No action.**
- **B3** RTPC binding hash-map storage — measured. Not the dominant
  cost. **No action.**
