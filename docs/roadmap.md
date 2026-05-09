# gool roadmap

This document is the execution plan for turning gool from
"engineering-grade audio engine" into "the multiplayer audio
middleware indie Godot teams reach for first."

The work is organized into four phases by dependency and leverage:

1. **Adoptable** — anyone can install and run in 5 minutes
2. **Production-safe** — teams trust it for real online games
3. **Designer-first** — non-engineers can author content
4. **Platform** — interoperates with the broader ecosystem

Phases unlock the next: nobody adopts an engine they can't
install, nobody trusts middleware they can't reason about
operationally, nobody scales without designer self-service, and
nobody picks a tool that locks them out of Steam Audio / OpenXR /
WebRTC.

Each item lists the user-facing outcome, the engineering work,
a rough effort size (**S** = 1-2 days, **M** = 3-7 days,
**L** = 2-3 weeks, **XL** = month+), and a concrete
definition-of-done.

---

## Phase 0: What's already shipped

Baseline against which everything below is measured.

| Capability                                  | Status                                                  |
|---------------------------------------------|---------------------------------------------------------|
| C++20 audio engine, lock-free render thread | shipped, 25/25 tests green, CI on Linux + Windows       |
| Spatial audio (distance, Doppler, occlusion, air absorption, reverb, optional binaural) | shipped, measured ±42 dB occlusion |
| Adaptive music with equal-power crossfade   | shipped, ±0.3% RMS through 300 ms transitions           |
| Voice chat (Opus + adaptive jitter buffer + PLC) | shipped, 97.8% continuity at 10% loss / 50 ms jitter |
| JSON sound banks + `.gpak` archives + hot reload | shipped, 1000-entry bank loads in 0.6 ms             |
| Bus graph + sidechain compressor + EQ palette | shipped, L4D2-style multi-tier ducking demo runnable  |
| Replication patterns (server-auth, predicted, client-auth) | shipped, deterministic replay verified     |
| GDExtension binding + 7 prefab Nodes        | shipped, editor plugin auto-installs `/root/Gool` autoload |
| Quickstart example Godot project            | shipped at `examples/quickstart/`                       |
| Migration docs (FMOD, Wwise, terminology)   | shipped                                                 |
| Workflow-focused README                     | shipped, 535 lines                                      |

What's **not** shipped — and what the rest of this document covers:
prebuilt binaries, game templates, encrypted voice, anti-spam,
designer tools, ecosystem adapters, and the visible proof that
makes adoption feel safe.

---

## Phase 1: Adoptable

Goal: a Godot multiplayer dev hears about gool, downloads it, and
has weapon fire + voice chat working in their game in 30 minutes
flat — without compiling C++.

This phase is the single biggest blocker to adoption today.
Everything below it is irrelevant if step one is "compile the
GDExtension yourself."

### 1.1 Cut a v0.1 release with prebuilt GDExtension binaries — **S** [PARTIALLY SHIPPED in 0.2.0]

**Outcome:** users download a single zip, drop it in `addons/`, enable
the plugin. No CMake, no Godot-cpp, no compilation.

**Status:** versioning infrastructure shipped in 0.2.0 — `release.yml`
now derives version from the git tag and produces
`gool-X.Y.Z-{linux,windows}-x86_64.{tar.gz,zip}`. `CHANGELOG.md` and
`RELEASING.md` document the cut-a-release procedure. Once `v0.2.0` is
tagged the C++ library archives ship.

**Remaining work:**
- Bundle the GDExtension `.so`/`.dll` alongside the C++ library so
  Godot users get a drop-in addon zip, not just a C++ library.
- Stage `addons/gool/bin/{linux,windows}/libgool_godot.{so,dll}`
  inside the zip so plugin enable on either OS Just Works.
- Attach the Godot-specific zip to the GitHub Release, alongside
  the C++-library archives the workflow already builds.

**DoD:** a fresh user downloads `gool-godot-X.Y.Z.zip`, opens
`examples/quickstart` in Godot 4.2+, and presses Play without
modifying anything. Audio plays.

### 1.2 Submit to the Godot Asset Library — **S**

**Outcome:** users find gool by typing "audio" or "voice chat" in
the Godot editor's AssetLib tab. One click installs.

**Work:**
- Trim `examples/quickstart/` to a clean Asset Library submission:
  no build artifacts, only `addons/gool/` + `main.tscn` + `main.gd`
  + `project.godot` + a thumbnail screenshot.
- Write the AssetLib description (~120 words, outcome-framed).
- Submit via godotengine.org/asset-library form. Iterate on review
  feedback.

**DoD:** the asset is live and downloadable from the in-editor
AssetLib panel.

### 1.3 Hero recording at the top of the README — **S**

**Outcome:** the first thing a reader sees is gool working, not
prose about what it would do.

**Work:**
- Record a 15-second screen capture: open `examples/quickstart`,
  walk a player around, fire a weapon, hear voice chat between
  two clients on the same machine.
- Convert to a sub-5 MB GIF (or a YouTube embed; either works).
- Insert at the very top of the README, above the tagline.

**DoD:** anyone visiting the GitHub page sees what the engine does
within 3 seconds without reading.

### 1.4 Tiny API facade — **S** [SHIPPED in 0.3.0]

**Outcome:** the four lines the consultant called out actually work
verbatim:
```gdscript
Gool.play_3d("rifle_fire", global_position)
Gool.play_music_state("combat")
Gool.play_voice(player_id, audio_stream)
Gool.set_rtpc("health", hp)
```

**Status:** Shipped. All four facade methods live in
`godot/addons/gool/runtime_singleton.gd` as thin wrappers over the
lower-level engine APIs. README Quick Start now leads with these
four lines, ahead of the prefab-node walkthrough.

* `play_3d` wraps `submit_event_local` with sane defaults (no
  prediction, normal priority).
* `play_music_state` lazily creates a `GoolMusicChannel` on first
  call and is idempotent on the current state — re-passing the
  same name is a no-op so callers can poll-style invoke every
  frame without churn.
* `play_voice` decodes `AudioStreamWAV` (FORMAT_16_BITS) to mono
  float32 PCM, registers as an ephemeral one-shot, and dispatches
  through the play path. AudioStreamOggVorbis support is
  documented as future work; for raw Opus voice traffic from a
  network layer, hosts use `Gool.submit_voice_packet` directly.
* `set_rtpc` / `get_rtpc` / `has_rtpc` / `clear_rtpc` map to a new
  C++ global parameter store on `AudioRuntime`. `HashParameterName`
  remaps engine-reserved IDs above `HostBase` so host names can't
  mask engine semantics. Budget is enforced only on new IDs —
  updates are always free.

**Limitations carried into the next iteration:**
- `set_rtpc` stores values but does not yet drive sound-definition
  modulation on the render thread (sound definitions reading these
  to adjust volume / cutoff / pitch). The storage and observability
  ship now so host code can build against the API; render-thread
  modulation is a future M-sized item.
- `play_voice` accepts only `AudioStreamWAV` with FORMAT_16_BITS.
  AudioStreamOggVorbis support requires hooking the existing Ogg
  decoder to AudioStream input and is on the roadmap.

### 1.4b Render-thread RTPC volume modulation — **M** [SHIPPED in 0.4.0]

**Outcome:** the v0.3.0 disclaimer is closed. `set_rtpc` actually
drives audio. Bind once per sound, push values per frame, hear the
modulation at the rendered output.

**Status:** Shipped. New `SetSoundVolumeRtpc(soundId, paramId,
minValue, maxValue, minVolume, maxVolume, smoothingMs)` API. Per-tick
evaluator inserted into `Update` between transform interpolation and
the orchestrator tick: walks active emitters, evaluates each binding
against the global parameter store, pushes target volume through the
existing `ParameterSmoother`. Same code path as `SetEmitterParameter`
so authored RTPC and manual gain calls compose cleanly. GDScript
facade: `Gool.bind_volume_rtpc` / `Gool.clear_volume_rtpc`.

**Audibility-verified DoD:** `tests/unit/sound_rtpc_test.cpp` registers
a 440 Hz sine, binds volume to `health` with `{0→0, 1→1}`, sets health
to 0, ticks 30 frames, renders 0.5s of audio, asserts measured RMS = 0.
Same setup with health=0.5 produces RMS exactly halfway between zero
and full. Inverted bindings (`{1→0, 0→1}`) silence the sound at full
health — the heartbeat pattern. 8 sub-tests covering all branches.

**Limitations carried into the next iteration:**
- One binding per sound. Multi-binding (volume + pitch + lowpass
  independently driven by different parameters) is the next M-sized
  item.
- Volume only. Pitch / lowpass / send modulation needs additional
  per-tick parameter routing.
- Linear curve only. Exponential and custom-point curves require a
  curve-format design pass.
- Bindings are programmatic; JSON sound-bank declaration of RTPC
  curves is a separate roadmap item.

### 1.5 Co-op shooter starter template — **M**

**Outcome:** a downloadable Godot project that's a complete co-op
shooter audio stack: 4 players, footsteps, gun tails, combat
music, ducking, proximity voice. Press Play, hear it work.

**Work:**
- Build `examples/coop_shooter_template/` as a new Godot project.
- Include: 4-player local lobby (split-screen or single-host),
  three weapon types with distinct fire/tail/reload sounds,
  surface-aware footsteps, looping ambient world, combat
  music that triggers on weapon fire, multi-tier ducking
  (local-gun > remote-gun > music), proximity voice between
  players using the existing voice prefab.
- Source all sound assets from CC0 freesound.org packs; ship
  them as a `.gpak`.
- Document the audio architecture in `examples/coop_shooter_template/README.md`:
  how each subsystem is wired, where to swap in your own assets.

**DoD:** a Godot dev clones the repo, opens
`examples/coop_shooter_template/`, presses Play, and is
playing 4-player co-op shooter audio without writing any code.

### 1.6 Stress test + benchmark demo — **S**

**Outcome:** verifiable proof that the engine handles real
workloads. Drops the "experimental framework" doubt.

**Work:**
- Build `examples/stress_test/` as a Godot project that spawns
  100 simultaneous gunshot emitters at random positions, with
  a CPU/memory readout in-scene.
- Record a video showing it running at 60 fps with the engine
  consuming <X% CPU. Ship the video on the README.
- Add a `benchmarks/` directory with a `BENCHMARKS.md` that
  reports headline numbers alongside the conditions they were
  measured under (machine, build flags, scenario).

**DoD:** stress demo runs and benchmarks reproduce on at least
one Linux and one Windows machine.

### 1.7 Comparison page: gool vs Godot built-in vs FMOD vs Wwise — **S**

**Outcome:** the reader who's middleware-shopping has one page
they can scan to make the decision.

**Work:**
- `docs/comparison.md`: feature matrix expanded from the README's
  "Why not Godot" table. Compare against FMOD Studio and Wwise
  on: licensing, voice chat, replication, music transitions,
  bus topology, asset pipeline, designer authoring tools, replay
  determinism, switching cost, supported engines, integration time.
- Be factual; cite FMOD/Wwise public docs for their behavior.
- Conclude with a "when to pick which" decision tree.

**DoD:** page is publishable as the canonical answer to "should I
use gool, FMOD, Wwise, or stock Godot."

---

## Phase 2: Production-safe

Goal: an online multiplayer team shipping a real game can deploy
gool without their security/networking/design lead vetoing it.

Phase 1 makes adoption frictionless. Phase 2 makes adoption
defensible inside a team that has to ship.

### 2.1 Encrypted voice transport — **M**

**Outcome:** voice chat over the public internet doesn't leak
plaintext audio to anyone with a packet capture.

**Work:**
- Add a per-session symmetric key negotiated at connect time
  (host-supplied; we don't run a key exchange ourselves —
  that's the network layer's job).
- AES-128-GCM wrapper around encoded Opus payloads, with a
  per-packet sequence-derived nonce.
- New `IVoiceTransportCipher` seam so hosts integrating with
  Steam GameNetworkingSockets (which already encrypts) can use
  a passthrough; hosts on raw UDP get the wrapper.
- Test: known-key round-trip, sequence reuse rejection,
  tamper detection.

**DoD:** voice payload is unintelligible to a packet sniffer
without the session key. Performance impact under 5% throughput.

### 2.2 Push-to-talk + voice activity detection — **S**

**Outcome:** voice doesn't transmit when nobody's speaking.
Reduces bandwidth, eliminates open-mic fatigue, prevents
accidental hot mic.

**Work:**
- Add `VoiceCaptureMode { AlwaysOn, PushToTalk, VoiceActivity }`
  to the voice capture path.
- VAD: simple energy-threshold detector with hangover (200 ms
  trailing window so words don't get clipped). No ML needed
  for the indie use case.
- PTT: `BeginTransmit()` / `EndTransmit()` API tied to a
  configurable input action in the Godot prefab.
- Telemetry: `framesTransmitted`, `framesGated` counters.

**DoD:** prefab inspector exposes capture mode dropdown.
Switching to PTT during gameplay gates the upstream packet rate
to zero outside of held input.

### 2.3 Server-side anti-spam rate limiting — **M** [SHIPPED in 0.2.0]

**Outcome:** a malicious or buggy client can't flood the server
with 10,000 gunshot events per tick.

**Status:** Shipped, then hardened in a follow-up pass to close
attack surfaces identified in security review:

* `ReplicationRateLimiter` runs on the network thread before the
  SPSC ring push. Per-player token buckets per category,
  deterministic against `latestServerTimeMs_`.
* `IReplicationValidator` host hook for custom policy.
* **Voice path rate limited too** — `OnVoicePacket` gates through
  the same per-player Voice category bucket. Per-player drops
  surface in `VoiceNetworkStats::packetsRateLimited`.
* **PlayerId-cycling DoS defense** — per-tick admission cap on
  never-seen-before playerIds (`maxNewPlayersPerTick = 8`
  default). Without this, an attacker can flood with fake
  playerIds to evict legitimate state from the LRU table.
  Surfaced as `Stats::replicationEventsRejectedNewIdBudget`.
* **Validator hook hardened** — validator-rejected events from
  unknown players no longer consume LRU slots, closing
  "validator hook is its own DoS surface" hole.

**Defaults:** SFX 50/sec, Voice 150/sec, Music 5/sec, Dialogue
20/sec, Ambience 10/sec, UI unlimited; tracks 64 distinct
players with LRU recycling; admits 8 new playerIds per tick.
Test coverage in `tests/unit/replication_rate_limit_test.cpp`
covers all paths plus determinism.

### 2.4 Mute/block API — **S**

**Outcome:** a player can mute another player's voice, and the
mute persists across sessions if the host wants.

**Work:**
- `runtime.SetVoiceSourceMuted(playerId, bool)` — runtime mute,
  no decode work for muted sources.
- `runtime.SetVoiceSourceVolume(playerId, float)` — partial
  attenuation for "they're loud, not muted."
- Persistent mute is the host's job (we don't own the player
  database); we just expose the state setters and a reload-time
  callback.
- Godot prefab API: `VoiceChatPlayer.muted`, `VoiceChatPlayer.volume`.

**DoD:** muted player's packets still arrive but are dropped at
the decode boundary; `voiceFramesDropped[mute]` counter
increments; CPU savings measurable.

### 2.5 Authoritative event policy enforcement — **S** [SHIPPED in 0.2.0]

**Outcome:** when a sound is marked `ServerAuthoritative`, the
server is the only entity that can spawn it on remote clients.
A malicious client can't fake an explosion on every remote
listener.

**Status:** Shipped, with full DoD met:

* New 2-arg `SubmitReplicatedEvent(event, ReplicationSource)`
  overload. When source is `Client` and `replicationPolicy` is
  `ServerAuthoritative`, the runtime rejects with
  `AudioResult::PolicyViolation`. The 1-arg overload remains
  backward-compatible (treats source as `Unknown`, permissive).
* **Distinct `replicationPolicyViolations` counter** in
  `AudioRuntime::Stats`, separate from
  `replicationEventsRejectedByValidator` so observability
  dashboards distinguish "runtime caught a wire-layer spoof"
  from "host's custom validator said no."
* **Audibility verified end-to-end** in
  `tests/unit/replication_rate_limit_test.cpp::TestSourceEnforcement`:
  registers a real PCM sine wave, submits a Client-source
  ServerAuthoritative event referencing it, ticks the runtime,
  and renders 0.5 seconds of audio. Measured render RMS = 0.0
  (perfect silence) for the rejected case; RMS = 0.25 (clearly
  audible) for the same event submitted with Server source. No
  remote listener can hear the spoofed sound — that's the DoD.
* Threat model documented in `docs/replication_patterns.md`
  with the four host-side rules: authenticate playerId, derive
  category from sound definition, enforce policy by sender role,
  validate numeric ranges via `DefaultBoundsValidator`.

### 2.6 Bandwidth budget hooks — **S**

**Outcome:** hosts can cap voice chat bandwidth to avoid blowing
out a low-tier server tier or a player's mobile connection.

**Work:**
- Per-player upstream-bytes/sec accounting in the voice path.
- `runtime.SetVoiceBandwidthBudget(playerId, bytesPerSec)`
  applies a token bucket; over-budget frames downgrade
  (Opus 32 kbps → 24 kbps → 16 kbps), then drop.
- Telemetry: `voiceBytesSent`, `voiceFramesBudgetDowngraded`,
  `voiceFramesBudgetDropped`.

**DoD:** setting a budget causes Opus to downgrade encoding
quality; observed continuity stays >95% on a budget that's 70%
of nominal.

### 2.7 Steam GameNetworkingSockets sample integration — **M**

**Outcome:** a working example showing gool wired into Steam's
networking. Indie devs targeting Steam stop having to figure
this out themselves.

**Work:**
- `examples/integration_steam_networking/`: a Godot project
  that uses GodotSteam (community Steam wrapper) for
  GameNetworkingSockets, routes our voice + replicated events
  through it, runs across two real Steam clients.
- Document host-to-host setup: lobby creation, voice channel
  ID, replicated event channel ID, NAT punch-through behavior.
- Cross-link from `docs/multiplayer.md`.

**DoD:** two players in different cities on Steam can join a
lobby and hear each other through gool's voice path.

---

## Phase 3: Designer-first

Goal: a sound designer with no C++ experience can author content
for a gool-powered game day-one. Engineers stop being the
bottleneck on every audio change.

This is the phase where gool actually starts to behave like
middleware, not a library. Phases 1-2 make the engine adoptable
and trustworthy; Phase 3 makes it *productive* for the team.

### 3.1 Custom inspector for AudioEmitter3D with preview — **M**

**Outcome:** a designer drops an AudioEmitter3D, picks a sound
from a dropdown of every name in the loaded bank, hears it play
right there in the editor without entering Play mode.

**Work:**
- Custom `EditorInspectorPlugin` that renders sound name as a
  searchable dropdown reading from the loaded bank.
- "Play" button in the inspector that triggers the sound at the
  emitter's editor-time position via the audio runtime running
  in editor mode.
- Distance / falloff curve preview chart inline in the inspector.
- Surface attenuation visualizer in 3D viewport (gizmo showing
  min/max distance spheres).

**DoD:** designer can author an emitter end-to-end without
leaving the inspector or running the game.

### 3.2 Audio event browser dock — **M**

**Outcome:** a dedicated Godot editor dock listing every event,
group, bus, and parameter in the loaded bank. Filter by category,
search by name, drag-and-drop into a scene.

**Work:**
- Godot `EditorPlugin` that creates a dock at the bottom of the
  main editor area.
- Tree view of the bank: events / groups / buses / parameters,
  each with their authored properties.
- Drag-and-drop from the dock onto a 3D scene creates a
  pre-configured AudioEmitter3D wired to that event.
- Reload button that re-parses the bank file.

**DoD:** designer can navigate a 200-entry sound bank in <30s and
drop a configured emitter into a scene with one drag.

### 3.3 Mixer / bus graph UI — **L**

**Outcome:** designers see the bus graph as a graph (not JSON),
adjust gain, mute/solo, see live meters, and snapshot/restore
named mixer states.

**Work:**
- Custom dock with a graph view: nodes are buses, edges are
  parent/child + sidechain links.
- Per-bus channel strip: gain slider, mute, solo, peak meter
  (live, polled from runtime stats).
- Snapshot system: name the current bus state, recall it later
  with a transition time. Snapshots persist to bank JSON.
- Effect chain editor inline on each bus (add/remove/reorder
  Gain, Biquad, Compressor, Reverb, etc.).

**DoD:** designer can build the L4D2 multi-tier ducking topology
graphically, name it as a snapshot, switch to "stealth" snapshot
on demand, all without writing code or editing JSON.

### 3.4 RTPC / parameter UI — **M**

**Outcome:** designers create runtime parameters (health, danger,
proximity), wire them to bus or emitter properties via curves,
and tweak the curves graphically.

**Work:**
- Parameter editor dock: list of named parameters with current
  values (live in editor when game is running), associated
  curve editor (multi-segment Bezier).
- Each curve binds an input parameter to an output property
  (e.g., "danger" → "music_intensity_bus.gain").
- Parameters serialize into bank JSON; gameplay code calls
  `Gool.set_rtpc("danger", 0.7)` to drive the binding.
- Curve preview shows current parameter value as a vertical line.

**DoD:** designer creates a "danger" parameter, binds it to a
combat music low-pass cutoff via a curve, sees the cutoff
respond live as the parameter is dragged.

### 3.5 Music transition editor — **M**

**Outcome:** designers author the music state machine
graphically: states as nodes, transitions as edges with
configurable fade times, sync points, and conditions.

**Work:**
- Custom dock with state graph view.
- Nodes are states (each a sound + loop config); edges are
  transitions (fade duration, sync-on-bar/beat option,
  one-shot stinger overlay).
- Authored data serializes to bank JSON; the
  `MusicStateController` prefab loads and executes it.
- Live preview: trigger transitions from the editor while game
  is running.

**DoD:** designer authors a 4-state music graph (explore /
combat / victory / defeat) with sync-on-bar transitions, plays
the game, hears states transition correctly without writing
GDScript.

### 3.6 Live profiler dock — **M**

**Outcome:** designers and engineers can see exactly what's
happening in the audio runtime while playing: active voices,
bus levels, voice chat stats, eviction rate, CPU time per
subsystem.

**Work:**
- Editor dock that polls `runtime.GetStats()` at 30 Hz.
- Tabs: Voices (live list with name, position, gain, age),
  Buses (per-bus peak/RMS meters), Voice chat (per-player
  stats), Performance (frame budget per subsystem),
  Eviction (rolling counter of evictions and dropped events).
- Per-row "show in scene" button that selects the source
  emitter Node.

**DoD:** designer playing the game can identify which emitter
is the loudest at any moment, why a sound got evicted, and
whether voice chat continuity is degrading — all from the
editor without restarting.

### 3.7 Waveform preview in inspector — **S**

**Outcome:** designers see the actual waveform of a sound when
inspecting an emitter or sound bank entry. Confirms "this is
the right take" without playing it.

**Work:**
- On bank load, decode each sound to a downsampled mip (256-
  point peak/min envelope per file). Cache to disk under
  `.gool_cache/`.
- Render the envelope as a small inline SVG in the inspector
  preview panel.
- Click-to-play scrubs from the clicked position.

**DoD:** dropping an AudioEmitter3D and selecting it shows the
waveform; clicking different points plays from those points.

---

## Phase 4: Platform

Goal: gool plays well with the broader ecosystem indie teams
already use. Adoption isn't gated by "do I have to drop my
existing networking / VR / raytracer to use this?"

Phases 1-3 make gool the right pick for new projects. Phase 4
makes it the right pick for projects that already have
infrastructure.

### 4.1 Steam Audio adapter — **L**

**Outcome:** Steam Audio's pathing-based occlusion, reverb, and
HRTF can be plugged into gool through the existing seam pattern.

**Work:**
- New `SteamAudioGeometryQuery` implementing `IAudioGeometryQuery`
  that wraps Valve's Phonon SDK.
- New `SteamAudioSpatializer` implementing `ISpatializer` for
  HRTF + reflection effects.
- Document Phonon scene setup, geometry baking, runtime cost.
- Optional CMake flag `AUDIO_ENGINE_STEAM_AUDIO=ON`; default off.

**DoD:** swap our `DefaultSpatializer` for `SteamAudioSpatializer`
in the dependencies struct, hear baked reverb tails and
pathing-based occlusion in the quickstart project.

### 4.2 OpenXR spatial audio integration — **M**

**Outcome:** VR projects in Godot get gool's spatial audio with
proper head tracking from the OpenXR runtime.

**Work:**
- Godot prefab `XRAudioListener` that subscribes to OpenXR head
  pose updates and feeds them to gool's `SetListener` each frame.
- Test: walk around in VR, hear emitters reposition correctly
  relative to head + room scale.

**DoD:** VR project shipping with gool produces correct binaural
audio in a head-mounted display.

### 4.3 EOS Voice integration sample — **M**

**Outcome:** indie devs using Epic Online Services for voice
have a working integration sample.

**Work:**
- `examples/integration_eos_voice/`: a Godot project that
  routes EOS voice channels through gool's spatial path.
- Document EOS voice room setup, channel ID mapping, NAT
  traversal differences vs raw UDP.

**DoD:** two players on EOS Voice hear each other through gool's
positional audio.

### 4.4 WebRTC voice transport — **L**

**Outcome:** browser-based games (Godot HTML5 export) can do
voice chat through gool.

**Work:**
- New `IVoiceTransport` seam with a WebRTC implementation
  using Godot's WebRTCPeerConnection API.
- Handle the differences from UDP (already-encrypted, ICE/STUN
  required, lower-level control).
- Test: two browser tabs, voice chat working through gool.

**DoD:** the quickstart example exports to HTML5 and runs voice
chat between two browser tabs in different cities.

### 4.5 FMOD/Wwise import compatibility — **L**

**Outcome:** teams switching from FMOD/Wwise can bring their
existing event metadata over without re-authoring everything.

**Work:**
- FMOD: parse FMOD Studio's bank XML metadata + WAV export,
  emit a gool sound bank JSON.
- Wwise: parse `.wwu` (work unit) XML, emit a gool sound bank
  JSON.
- Lossy by design — we don't claim feature parity, we claim
  "your event names, files, and bus routing carry over."
- CLI tools: `gool_import_fmod`, `gool_import_wwise`.

**DoD:** an FMOD project's events appear in gool's event browser
after a one-shot import, with names and bus routing preserved.

### 4.6 Dedicated server build mode — **S**

**Outcome:** teams running headless dedicated servers don't pay
for audio rendering they'll never play; the runtime stays
authoritative for replication but skips the mixer entirely.

**Work:**
- `AudioConfig::headlessAuthoritativeMode` flag: when set, the
  control thread runs (replicated event validation, voice
  packet receipt for relay), but the mixer doesn't allocate
  voice slots and doesn't run.
- Backend selection auto-routes to `NullAudioBackend` regardless
  of CMake flags.
- Document the wire-protocol contract: what the server is
  responsible for vs the clients.

**DoD:** headless dedicated server build authoritatively
validates events from 32 clients without consuming any voice
slot or mixer cycle; clients hear the validated audio.

### 4.7 Telemetry hooks — **S**

**Outcome:** teams running real games can wire gool's stats into
their existing observability stack (Prometheus, Datadog, custom
analytics).

**Work:**
- `IRuntimeTelemetrySink` seam: sink receives stats counters at
  configurable intervals, emits whatever format the host wants.
- Default implementation: structured stdout / JSON lines.
- Sample integration: Prometheus exposition format adapter.

**DoD:** running game emits jitter buffer continuity, eviction
rate, bandwidth utilization to a Prometheus endpoint that a
real-time dashboard scrapes.

---

## What this roadmap deliberately doesn't include

These came up in feedback but are out-of-scope, at least for now.
Calling them out explicitly so the priorities stay defensible:

- **Cloud-hosted bank authoring SaaS.** The middleware tier doesn't
  need a hosted product; sound banks are JSON files in the repo.
  Ship offline-first; revisit if multi-team collaboration becomes
  a real ask.
- **Visual scripting for audio events.** Godot has GDScript;
  designers who can author JSON banks don't need a node graph
  for event triggering. Lower priority than the core authoring
  tools.
- **AAA-grade convolution reverb / impulse-response library.**
  The Freeverb-derived reverb send is good enough for 95% of
  multiplayer indie titles. AAA reverb belongs in Steam Audio
  (covered by 4.1).
- **Proprietary asset format with DRM.** Anti-pattern for indie
  middleware. `.gpak` is uncompressed + unencrypted by design;
  hosts that need DRM wrap the file at the OS layer.

---

## How to read this roadmap

This is an ordering of work, not a Gantt chart. Each phase
delivers something on its own that's adoptable, even if later
phases are pending. Phase 1 alone gets gool to "indie teams can
adopt this for new projects." Phase 1 + 2 gets it to "indie teams
can ship with this." Phase 1 + 2 + 3 gets it to "the audio
designer on the team prefers it to FMOD." Phase 4 gets it to
"the FMOD team is migrating off."

Items within a phase are roughly ordered by leverage. Phase 1.1
(release binaries) is a half-day of work that unblocks ten times
as much downstream value as 1.6 (stress demo). Phase 3.3 (mixer
UI) is a 2-week build that's worth more to retention than two
days on 3.7 (waveform preview).

When picking the next thing to build:
- If it's an XL item, decompose into Ms first.
- If a Phase-N item depends on a Phase-(N-1) item that hasn't
  shipped, ship the prereq first.
- Re-order items within a phase based on what users are actually
  asking for, not what's prettiest to build.
