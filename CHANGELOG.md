# Changelog

All notable changes to gool are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, minor bumps may include backward-incompatible
changes; consult the per-release `Changed` and `Removed` sections before
upgrading.

## [Unreleased]

Nothing yet — open the next release section here when a feature lands.

## [0.4.0] - 2026-05-09

Render-thread RTPC volume modulation. The disclaimer in v0.3.0
("`set_rtpc` stores values but does not yet drive sound modulation")
is closed. Calling `bind_volume_rtpc("heartbeat", "health", 0, 1, 1, 0)`
once, then `set_rtpc("health", v)` per frame, now actually changes the
heartbeat's rendered volume in real time.

### Added

- **`AudioRuntime::SetSoundVolumeRtpc`** / **`ClearSoundVolumeRtpc`** /
  **`GetSoundRtpcBindingCount`** — bind a sound's volume to a global
  parameter via a linear curve. Each `Update` tick the runtime walks
  active emitters, looks up each one's binding, reads the parameter,
  computes a target gain, and pushes it through the existing parameter
  smoother (`AudioParameterIds::Gain`). Same code path used by
  `SetEmitterParameter` so authored modulation and manual gain calls
  compose cleanly.
- **`Gool.bind_volume_rtpc(sound_name, param_name, ...)`** /
  **`Gool.clear_volume_rtpc(sound_name)`** — GDScript autoload facade
  + GDExtension bindings (`set_sound_volume_rtpc`,
  `clear_sound_volume_rtpc`, `sound_rtpc_binding_count`).
- **`AudioConfig::maxSoundRtpcBindings`** (default 256). Budget is
  enforced only on new sound IDs — re-binding an existing sound is
  always free. `BudgetExceeded` returned when the cap is hit.
- **Skip-when-unset semantics**: until `set_rtpc(name, ...)` is called
  at least once, the binding has no effect. Authored volume stays in
  place. Binding-installation order is then independent of gameplay
  state so prefab `_ready()` calls can wire bindings without worrying
  about sequencing.
- README Quick Start now shows the bind + set_rtpc pattern.

### Tests

- `tests/unit/sound_rtpc_test.cpp` — 8 sub-tests, audibility-verified
  end-to-end:
  - Unset parameter: rendered RMS = 0.25 (authored volume preserved)
  - Parameter at 0 with `0→0, 1→1` binding: rendered RMS = 0 (silent)
  - Parameter at 1: rendered RMS = 0.25 (full)
  - Parameter at 0.5: rendered RMS = 0.125 (exactly half, ratio = 0.5)
  - Inverted binding `1→0, 0→1` at full health: silent (heartbeat pattern)
  - Out-of-range parameter values clamp correctly to endpoints
  - Clear stops modulation
  - API validation rejects NaN, degenerate range, invalid IDs, negative smoothing

  Total now 30/30 passing.

### Limitations carried into the next iteration

- One binding per sound. Binding multiple parameters to one sound
  (volume + pitch + lowpass independently) is a future M-sized item.
- Volume only. Pitch / lowpass cutoff / send-level modulation are
  roadmap.
- Linear curve only. Exponential and custom-point curves are roadmap.
- The orchestrator's per-emitter `UpdateParams` pass that carries the
  modulated gain to the mixer only runs when a listener is registered.
  This was the original behavior; documented now in the test setup
  comments and `EvaluateRtpcBindings_` docs.

## [0.3.0] - 2026-05-09

The tiny API facade. Four canonical entry points users can copy-paste
verbatim into a fresh Godot project: `Gool.play_3d`, `Gool.play_music_state`,
`Gool.play_voice`, `Gool.set_rtpc`. Each is a thin GDScript wrapper over
the lower-level engine APIs; users drop down to the raw bindings when
they outgrow them.

### Added

- **`Gool.play_3d(name, position, priority=128)`** — one-shot 3D playback
  by authored sound name. Wraps `submit_event_local` with sane defaults.
- **`Gool.play_music_state(state_name, fade_ms=500)`** — equal-power
  crossfade to a new music state. Lazily creates a `GoolMusicChannel`
  on first call. Idempotent: re-passing the current state is a no-op.
- **`Gool.play_voice(player_id, audio_stream)`** — decode an
  `AudioStreamWAV` (FORMAT_16_BITS) to mono float32 PCM, register as
  an ephemeral one-shot, dispatch through the play path. Raises a
  push_error on unsupported formats. AudioStreamOggVorbis support is
  on the roadmap; for raw Opus voice traffic from a network layer,
  use `Gool.submit_voice_packet` directly.
- **`Gool.set_rtpc(name, value)`** / **`get_rtpc`** / **`has_rtpc`** /
  **`clear_rtpc`** — string-keyed real-time parameter store. Authored
  sound definitions reading these to drive volume / cutoff / pitch is
  a future feature; the storage and observability ship now so host
  code can build against the API.
- **`AudioRuntime::SetGlobalParameter`** / **`GetGlobalParameter`** /
  **`ClearGlobalParameter`** / **`GetGlobalParameterCount`** — C++ API
  for the global parameter store. Game-thread access at this stage;
  render-thread modulation is a follow-up.
- **`HashParameterName(name)`** constexpr in `types.h`. FNV-1a, same
  shape as `HashSoundName`. Hashes that would collide with the
  engine-reserved range `[1, HostBase)` are bumped above
  `AudioParameterIds::HostBase` so host names can't mask engine
  semantics.
- **`AudioConfig::maxGlobalParameters`** (default 256). Budget is
  enforced only on new IDs — updating an existing parameter is
  always free.
- **`Gool.stop_music(fade_ms=500)`** — companion to `play_music_state`.
- GDExtension bindings: `set_global_parameter`, `get_global_parameter`,
  `has_global_parameter`, `clear_global_parameter`,
  `global_parameter_count`, `hash_parameter_name`.

### Tests

- `tests/unit/global_parameter_test.cpp` — 7 sub-tests covering hash
  stability + reserved-range remapping, set/get round-trip, unset
  returns false, clear semantics, budget enforcement (only on new
  IDs), NotInitialized, InvalidArgument. Total 29/29 passing.

### Documentation

- README Quick Start now leads with the four facade lines, ahead of
  the prefab-node walkthrough.
- Phase 1.4 marked SHIPPED in 0.3.0 in the roadmap.

## [0.2.0] - 2026-05-09

The first public release with binary artifacts. Adds the multiplayer
hardening pass: rate limiting, replication-policy enforcement, threat
model documentation, and the `DefaultBoundsValidator` for malformed
input.

### Added

- **Replication rate limiter** (Phase 2.3): per-player, per-category
  token-bucket rate limiter on `SubmitReplicatedEvent`. Defaults
  sized for plausible gameplay (50 SFX/sec, 150 voice/sec, etc.).
  Surfaced via `Stats::replicationEventsRateLimited[6]`.
- **`IReplicationValidator`** interface for host-supplied policy
  hooks. Runtime calls before rate limiting; rejection silently
  drops + counts.
- **`AudioCategory category`** field on `AudioEvent`. Defaults to
  `SFX` so existing call sites work unchanged.
- **`AudioRuntime::SetReplicationValidator()`** / **`GetPerPlayerReplicationStats()`**.
- **Voice path rate limiting**: `OnVoicePacket` gates through the
  same per-player Voice category bucket. Per-player drops surface
  in `VoiceNetworkStats::packetsRateLimited` (new field).
- **PlayerId-cycling DoS defense**: per-tick admission cap on
  never-seen-before `playerId`s
  (`ReplicationRateLimitConfig::maxNewPlayersPerTick = 8` default).
  Surfaced via `Stats::replicationEventsRejectedNewIdBudget`.
- **`ReplicationSource` enum** + 2-arg `SubmitReplicatedEvent(event, source)`
  overload (Phase 2.5). Client-sourced `ServerAuthoritative` events
  rejected with `AudioResult::PolicyViolation` — verified via
  audibility check (rendered RMS = 0).
- **`replicationPolicyViolations` counter**, distinct from
  `replicationEventsRejectedByValidator`, so dashboards can tell
  protocol enforcement from host-policy denials.
- **`DefaultBoundsValidator`**: a shipped `IReplicationValidator`
  rejecting NaN/Inf vec3 fields, extreme magnitudes, malformed
  parameters, optional unknown soundIds via host callback.
- **`ChainReplicationValidator`**: composes up to 8 validators with
  short-circuit-on-reject.
- **`audio::GetVersion()`** + `version.h` constants. Compile-time
  major/minor/patch + the git SHA stamped at CMake configure time.
- **Threat model documentation** in `docs/replication_patterns.md` —
  what the runtime can and can't validate, four host-side rules,
  monitoring counters.
- **Release infrastructure**: `CHANGELOG.md`, `RELEASING.md`, this
  file. Release workflow (`release.yml`) builds versioned artifacts
  on `v*` tags.
- **Roadmap**: `docs/roadmap.md` with 28 phased work items,
  effort-sized.
- **`AudioResult::RateLimited`** and **`AudioResult::PolicyViolation`**
  return values.

### Changed

- README rewritten workflow-first (2075 → 559 lines). Leads with
  what online multiplayer audio demands and how gool fits, not the
  engine architecture.
- macOS lane temporarily disabled in CI matrix (Apple-Clang issue
  not yet investigated; Linux + Windows green).

### Fixed

- `examples/hello_audio` include path (was breaking miniaudio
  builds).
- `release.yml` multi-line cmake invocation flattened to single
  line (YAML scalar fragility).
- `tests/CMakeLists.txt` `biquad_eq_test` missing from the
  `src/`-on-include-path foreach.

### Security

- Validator-rejected events from never-seen players no longer
  consume LRU slots, closing a "validator hook is its own DoS
  surface" hole.
- `RecordPolicyViolation()` / `RecordValidatorRejection()` use
  `FindExisting()` instead of `FindOrAllocate()` so spoofs from
  unknown players can't inflate the slot table.

## [0.1.0] - 2026-04-XX

Initial private development snapshot. Not formally released; the
first tagged version is 0.2.0 above.

Headlines:

- C++20 audio engine with 25 unit tests passing
- Spatial audio: distance attenuation, Doppler, occlusion (material-
  aware), air absorption, reverb sends, optional binaural
  (`SphericalHeadSpatializer`)
- Voice chat: Opus codec wrapper, adaptive jitter buffer
  (97.84% continuity at 10% loss / 50 ms jitter), PLC, per-player
  telemetry
- Adaptive music: equal-power crossfade (±0.3% RMS through 300 ms
  transitions), `MusicChannel` helper, loop-boundary crossfade
  (158× click reduction)
- Bus graph + sidechain compressor + EQ palette (LP/HP/BP/Shelf/Peak)
- JSON sound banks, `.gpak` archives, hot reload
- Replication: `SubmitReplicatedEvent`, `UpdateReplicatedTransform`,
  `OnVoicePacket` with deterministic-replay arrival timestamp,
  `CancelPredictedEvent`, interest management
- Godot 4.2+ GDExtension binding with 7 prefab Nodes, editor plugin
  with autoload installation

[Unreleased]: https://github.com/siliconight/gool/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/siliconight/gool/releases/tag/v0.4.0
[0.3.0]: https://github.com/siliconight/gool/releases/tag/v0.3.0
[0.2.0]: https://github.com/siliconight/gool/releases/tag/v0.2.0
[0.1.0]: https://github.com/siliconight/gool/tree/main
