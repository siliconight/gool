# Determinism

What the engine guarantees about reproducibility, what it doesn't,
and what would need to change to close the gap. Written honestly:
this is a partial-determinism story today, with a clear path to full
bit-identical replay if you need it.

## Why determinism matters

For competitive multiplayer titles, three workflows want it:

- **Match replays** — Twitch streamers and tournament archives want
  to play back the exact mix the original player heard. If the
  audio diverges between recording and replay, voice-line cues and
  positional information for casters and viewers won't match the
  on-screen action.
- **Spectator audio** — when a third client spectates an ongoing
  match, they should hear what the player they're spectating
  hears. With deterministic audio, spectator and player can run
  the same engine state and produce the same mix.
- **Bug reports and regression** — "play this input sequence at
  this seed and tell me if you hear the pop" is a tractable
  request only if the engine produces the same output twice from
  the same input.

If your game doesn't ship replays, spectator views, or competitive
features, you can stop reading. The engine works fine for normal
gameplay without any of this.

## What is deterministic today

The math is deterministic. Given the same `SpatialParams` (gainL,
gainR, delaySamplesL/R, lpfAmountL/R, dopplerPitch) handed to the
mixer, the mixer produces the same int16 output samples. The
biquad LPF, the per-voice gain ramps, the bus-graph effect chain,
the reverb send, the binaural delay path — all are pure float
arithmetic with stable execution order. Two test runs of any unit
test in the suite produce identical output.

The DSP graph topology is deterministic. Voice eviction priority
is `EffectivePriority = (priority << 32) - distance_mm`, computed
from inputs the host controls. Eviction order under contention
matches across runs given the same emitter set.

Replicated event ingestion is deterministic. `SubmitReplicatedEvent`
inserts events into the per-tick processing queue keyed by
`SimulationTick`; events with the same tick are processed in
arrival order. As long as the network layer hands them in the
same order across runs (which is the network's job, not the
engine's), the engine's response is identical.

The PCM ring, slot map, and SPSC ring data structures are
deterministic. Their behavior depends only on push/pop call
sequences, not on timing.

## Where determinism breaks

Three sources of wall-clock dependency leak non-determinism into
the engine today.

### 1. Voice packet arrival timestamping

`AudioRuntime::OnVoicePacket` calls
`std::chrono::steady_clock::now()` to record arrival time, which
then feeds the jitter buffer's RFC 3550 inter-arrival jitter
estimate. Two runs of the same packet input arrive at
slightly-different wall-clock times; the jitter EMA evolves
slightly differently; the adaptive target depth oscillates by
±1 frame; PLC firing differs by one frame at the boundaries of
adaptation.

The audible effect is small (one frame ≈ 20 ms; only at
adaptation boundaries; usually masked by PLC) but it does mean
two replays of the same stream don't produce bit-identical output.

**Located at**: `src/audio_engine/runtime/audio_runtime.cpp` (the
`std::chrono::steady_clock::now()` call inside `OnVoicePacket`).

### 2. Null backend wall-clock pacing

The `NullAudioBackend` paces its render loop with
`std::chrono::steady_clock` and `std::this_thread::sleep_until`.
Render-thread tick frequency varies under OS scheduling jitter;
the mixer processes a varying number of frames per real second.
For a live game this is irrelevant — the audio device drives the
render thread either way — but for an offline replay you'd want
the render loop driven by an engine-supplied tick rather than by
the wall clock.

**Located at**: `src/audio_engine/backend/null_audio_backend.cpp`
(`steady_clock::now()` and `sleep_until`).

### 3. Control-thread wall-clock advance

`OnTickAdvanced(simTick, serverTimeMs)` is the deterministic
network-thread entry point: simulation tick + server time are
host-supplied. But the control-thread also advances a local
wall-clock counter for late-event discard when
`OnTickAdvanced` hasn't been called recently. If your network
layer does call `OnTickAdvanced` every tick, this fallback never
fires and the engine is purely server-time-driven. If it doesn't,
the wall-clock fallback adds non-determinism.

**Mitigation today**: call `OnTickAdvanced` every server tick
and the engine never falls back to wall clock.

## What's already non-issue

Things that look like non-determinism but aren't:

- **No `rand()`, no `std::random_device`, no `std::mt19937`** in
  production engine code. The test suite uses random number
  generators with fixed seeds for synthetic input; the engine
  itself never reads any random source at runtime.
- **No threading races for output state.** The mixer command ring
  is SPSC; the render thread is the only writer of the output
  buffer. Order is fully determined by the command-ring sequence.
- **No floating-point reductions across threads.** Each voice
  is processed by exactly one thread; sums are accumulated in a
  fixed order; no cross-thread float accumulators.
- **`atan2` in the spherical-head spatializer** is the same
  hardware instruction across x86, ARM, and Apple Silicon for
  IEEE-754-compliant inputs. Cross-platform float bit-identity
  is achievable with `-ffp-model=strict` (MSVC),
  `-ffp-contract=off` (Clang/GCC), and disabling fused-multiply-
  add. The engine doesn't currently set these compile flags;
  see "Path to bit-identical" below.

## Path to bit-identical replay

If you need true cross-run bit-identical output, three changes
get you there:

### Change 1 — Engine-supplied arrival clock

Replace the `steady_clock::now()` call in `OnVoicePacket` with an
engine-supplied tick time. Add a config flag
`AudioConfig::deterministicMode = true` and an API
`OnVoicePacketAtTick(playerId, bytes, size, seq, sendTs, arrivalTs)`
where the host passes the deterministic arrival time. Wire the
jitter buffer to use that timestamp instead of wall clock.

Estimated effort: half a day. Pure plumbing change; jitter buffer
already takes `arrivalMs` as a parameter.

### Change 2 — Engine-supplied render clock

Replace the `NullAudioBackend`'s `sleep_until` pacing with a host-
driven `Pump(framesToRender)` API. The host tells the backend
when to render, how many frames; backend renders that many frames
and returns. For offline replay, the host calls `Pump` in lockstep
with simulation ticks.

Estimated effort: 1-2 days. Touches the backend interface
(`IAudioBackend`) and one default impl. Existing live-game
integrations continue to work via the live-pacing implementation.

### Change 3 — Compile-time float determinism

Add `AUDIO_ENGINE_DETERMINISTIC_FLOAT=ON` CMake option that sets:
- Clang/GCC: `-ffp-contract=off -fno-fast-math -frounding-math`
- MSVC: `/fp:strict`
And opts out of FMA usage in critical DSP paths. Verify with a
cross-platform test that exercises the spatializer and produces
the same float bit pattern on x86_64 Linux, x86_64 macOS, and
ARM64 (Apple Silicon, Steam Deck running on Zen 2).

Estimated effort: 2-3 days, mostly testing across platforms.

### Total effort estimate

A focused engineer can land all three in about a week of work
plus a week of cross-platform validation. That's the honest cost
of "deterministic-mode is a real feature, not a marketing claim."

## Current honest status

- **Same input event sequence at same wall clock** → effectively
  deterministic in audible terms. Any divergence is below the
  perceptual floor.
- **Same input event sequence at different wall clocks** → not
  bit-identical. Jitter EMA evolves slightly differently; adaptive
  target depth oscillates by ±1 frame.
- **Cross-platform bit-identity** → not currently guaranteed. Float
  rounding modes and FMA usage may differ.

This puts the engine in good company — Wwise and FMOD make similar
choices, with their own deterministic-mode flags as opt-in
features rather than always-on guarantees. If determinism is
table stakes for your shipping product, plan for the 1-2 weeks of
work above. If it's a "nice to have someday," ship the engine as
is and revisit when the feature lands on your roadmap.

## Telemetry-driven validation

Even without full determinism, you can validate replay fidelity
empirically. Run the same input twice, compare:

- `AudioRuntime::Stats` snapshots at each tick — match exactly?
- `AudioRuntime::VoiceNetworkStats` per-player counters — match
  within ±1 (jitter EMA wiggle)?
- Sampled output buffer RMS values — match within 0.01 dB?

If those three hold, your replay is "perceptually identical" —
audible to a listener as the same playthrough — even without
bit-identical floats.
