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
| Quickstart example Godot project            | shipped at `examples/01_quickstart/`                       |
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
- Trim `examples/01_quickstart/` to a clean Asset Library submission:
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

### 1.4c Multi-target RTPC + curves + JSON authoring — **M** [SHIPPED in 0.5.0]

**Outcome:** the v0.4 limitations are closed in one iteration. Sounds
can have up to four bindings simultaneously (volume + pitch + lowpass
+ reverb), each driven by its own parameter and shaped by its own
curve. Bindings can be authored programmatically or declaratively in
JSON sound banks.

**Status:** Shipped. New `RtpcTarget` enum (Volume / Pitch /
LowPassCutoff / ReverbSend) and `RtpcCurve` enum (Linear / Exponential
/ InverseExponential / SCurve). `SetSoundRtpc(soundId, binding)`
replaces v0.4's `SetSoundVolumeRtpc` (mechanical migration). Storage
moved to vector-of-bindings-per-sound, capped at one per (sound,
target). JSON `rtpc` array on each sound entry parses with line-numbered
errors on bad target/curve names. GDScript autoload exposes
`bind_volume_rtpc` / `bind_pitch_rtpc` / `bind_lowpass_rtpc` /
`bind_reverb_rtpc` simple facades plus `bind_rtpc(name, dict)` for the
advanced full API.

**Audibility-verified DoD:** `tests/unit/sound_rtpc_test.cpp` registers
real PCM, binds volume + pitch + lowpass on three different test sounds,
asserts measured RMS for each. Multi-binding coexistence test confirms
volume and pitch bindings on the same sound apply independently. Curve
test confirms linear / exp / inv-exp / scurve at parameter midpoint
produce 0.125 / 0.0625 / 0.1875 / 0.125 RMS respectively (0.25
reference). 7 sub-tests total. Plus 2 new sub-tests in
`sound_bank_test.cpp` for JSON authoring + error handling.

**Limitations carried into the next iteration:**
- Custom point-list curves (arbitrary curve shapes via JSON control
  points) — still future. Linear / Exponential / InverseExponential /
  SCurve cover typical FMOD/Wwise authoring needs but not exotic
  artist-authored shapes.
- LowPassCutoff combines via `max()` with the spatial baseline. Cases
  that want RTPC to *override* spatial filtering (underwater zone
  replaces occlusion) need a different combiner.
- Bindings are per-sound only. Per-bus RTPC ("all music quiets when
  combat starts" without binding every track individually) is a
  separate feature.

### 1.5 Co-op shooter starter template — **M**

**Outcome:** a downloadable Godot project that's a complete co-op
shooter audio stack: 4 players, footsteps, gun tails, combat
music, ducking, proximity voice. Press Play, hear it work.

**Work:**
- Build `examples/04_coop_shooter_template/` as a new Godot project.
- Include: 4-player local lobby (split-screen or single-host),
  three weapon types with distinct fire/tail/reload sounds,
  surface-aware footsteps, looping ambient world, combat
  music that triggers on weapon fire, multi-tier ducking
  (local-gun > remote-gun > music), proximity voice between
  players using the existing voice prefab.
- Source all sound assets from CC0 freesound.org packs; ship
  them as a `.gpak`.
- Document the audio architecture in `examples/04_coop_shooter_template/README.md`:
  how each subsystem is wired, where to swap in your own assets.

**DoD:** a Godot dev clones the repo, opens
`examples/04_coop_shooter_template/`, presses Play, and is
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

**Decomposed into four shippable sub-items** so each can land
independently without committing to the full L-effort up front.
3.3a alone is the highest-leverage single chunk: it turns gool
from "black box you trust your ears about" into "live-visualized
audio engine you can debug."

The C++ engine APIs needed for all four sub-items already exist
(`get_render_stats`, `SetBusGain`, solo/mute/bypass on `BusGraph`,
`SetBusParameter`, live effect add/remove, bus config JSON
round-trip). This is editor-plugin work, not engine work.

#### 3.3a Read-only meters dock — **S** (1-2 days)

Vertical strip per bus showing live peak meters polled from
`Gool.get_render_stats()` each editor tick. No interactivity.
Validates the visual + polling architecture and immediately
makes gool's internals visible during play. Build order:

- Editor plugin scaffolding (`addons/gool/editor/mixer_dock.gd`)
- Dock registration via existing `plugin.gd` add_control flow
- Per-bus strip widget with peak meter (Control + Custom Draw)
- Polling loop reading `get_render_stats()` every ~30 Hz
- Bus topology read from active config (Master + children)

Done when: with the sandbox running, you can see Music dipping
during gunshots and Sfx spiking on each fire, in real time.

#### 3.3b Volume + S/M/B controls — **M** (3-7 days)

Fader per strip + Solo/Mute/Bypass buttons. Edits go through
`SetBusGain` / mute / solo APIs. Live during Play; on stop,
prompt: discard changes vs. write back to `config.json`. Build:

- Drag-to-set fader Control with dB-mapped value
- S/M/B buttons with mutually-exclusive solo semantics
- Bridge to gool's existing bus operations
- Dirty-state tracking + save dialog on play-mode exit
- Undo/redo integration with Godot's EditorUndoRedoManager

Done when: you can mix the sandbox by ear with two clients
running, save the mix back to config.json, and the saved values
restore on next run.

#### 3.3c Effect chain edit — **M** (3-7 days)

"Add Effect ▾" dropdown listing gool's effect kinds (Compressor,
EQ, Reverb, Limiter, Saturation, others). Per-effect parameter
panel using Godot's inspector pattern for typed exports.

- Effect-kind dropdown populated from `bus.h::EffectKind` enum
- Parameter widgets per effect type (sliders, dropdowns,
  threshold/ratio/etc. for compressor; bands for EQ; room/damp
  for reverb)
- Live preview during Play via `SetBusParameter`
- Add/remove/reorder effects in chain
- Persist to `config.json` `effects` array per bus

Done when: you can tune the Music bus's sidechain compressor in
real-time during play, hear the result, save it, and restart
with the saved parameters applied.

#### 3.3d Topology + persistence — **M** (3-7 days)

Bus hierarchy visualization (lines or indentation showing parent
→ child). Add/remove buses. Round-trip `config.json` cleanly,
preserving `_comment` fields and the human-friendly structure of
hand-edited configs.

- Topology renderer (parent → child arrows or indented strips)
- "Add Bus" / "Remove Bus" affordances with parent picker
- JSON serializer that preserves _comment, member ordering, and
  significant whitespace (use a comment-preserving parser, not
  raw `JSON.stringify`)
- Validation: bus names unique, no cycles, references resolvable
- Migration path: if existing config has unknown fields, preserve
  them on save

Done when: a config.json edited via the dock is byte-equivalent
(modulo intended changes) to the same file edited by hand,
including comments.

### Rationale for 3.3a-first ordering

The four sub-items are roughly independent — you can do any
single one and ship it. Recommended order is 3.3a → 3.3b → 3.3c
→ 3.3d because:

- 3.3a's polling loop is a prerequisite for 3.3b/c (same data
  pipeline)
- 3.3b's fader UI patterns inform 3.3c's parameter editors
- 3.3d (topology + persistence) is the heaviest lift and benefits
  from all three predecessors being in place to test against

But there's no hard dependency between them — if your priorities
change mid-stream, ship what's done and defer the rest.

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

### 4.7 Telemetry hooks — **S** [SHIPPED in 0.6.0]

**Outcome:** teams running real games can wire gool's stats into
their existing observability stack (Prometheus, Datadog, custom
analytics) at a configurable cadence, plus query in-process time
series for debug overlays and post-mortems without leaving the
process.

**Status:** Shipped. New `include/audio_engine/telemetry.h` exposes
`IRuntimeTelemetrySink` plus three built-in implementations:
`JsonLinesTelemetrySink` (one JSON object per emit to any `FILE*`),
`PrometheusTelemetrySink` (thread-safe exposition-format snapshot
for `/metrics` HTTP handlers, with HELP / TYPE blocks and per-category
labels), and `RingTelemetrySink` (in-memory circular buffer of last
N samples for time-series queries — at the default 250 ms cadence
the default 512-sample capacity gives ~2 minutes of rolling history).
Wired through `AudioRuntimeDependencies::telemetrySink` (host-owned
raw pointer) and `AudioConfig::telemetryIntervalMs` (default 0 =
disabled). Update step 12 emits via accumulator-based scheduling
that catches up rather than dropping samples on long host frames,
and wraps the sink call in `try`/`catch` so a misbehaving host sink
can't break Update mid-flight. Working sample at
`examples/cpp/telemetry/main.cpp`.

**DoD:** `tests/unit/telemetry_test.cpp` (9 sub-tests) covers each
sink's output format, runtime emit cadence (9 samples over 1 s at
100 ms — within ±1 expected slack), interval=0 disables emission,
nullptr sink with non-zero interval is safe, end-to-end ring sink
fed by runtime captures monotonic time series. Sample program runs
end-to-end and produces a valid Prometheus exposition body that a
real scrape endpoint can serve verbatim.

**Limitations carried into the next iteration:**
- Per-player voice metrics (jitter, packet loss per player) are not
  pushed through the sink automatically. Cardinality is host-dependent
  (player IDs come and go), so the sink interface only carries
  global stats. Hosts that need per-player breakdowns iterate active
  players themselves on the same cadence and call
  `GetVoiceNetworkStats(playerId)` — pattern is self-evident from
  the test setup but not codified in the sink interface yet. Could
  be revisited with a `OnVoiceNetworkStats(playerId, stats)` hook
  if real users need it.
- No event-level structured logging. Counters tell you *that*
  something happened; logging would tell you *why*. See 4.8 below.

### 4.8 Event-level structured logging — **S** [SHIPPED in 0.7.0]

**Outcome:** dev-loop visibility into individual events that drove
counters: which voice packets were rejected and why, which one-shot
got evicted, which RTPC binding hit its budget, which replication
event the validator rejected. Distinct from telemetry (which
aggregates state); same observability shape (host-supplied sink,
configurable level, structured fields) but called per-event rather
than per-interval.

**Status:** Shipped. New `include/audio_engine/logging.h` exposes
`IRuntimeLogSink` plus `JsonLinesLogSink` (one compact JSON object
per event, atomic at FD level for typical line sizes) and
`RingLogSink` (in-memory circular buffer of last N events for
in-process queries — debug overlays, post-mortems, replay
correlation; deep-copies StrView fields so stored events outlive
the originating call). Wired through
`AudioRuntimeDependencies::logSink` (host-owned raw pointer) and
`AudioConfig::logMinLevel` (default Info — Trace and Debug events
stay disabled in shipped builds unless explicitly enabled).

**Hook points** wired in v0.7.0: late event discard (game and
replicated paths), RTPC budget exceeded, one-shot eviction,
one-shot drops (full-pool and post-eviction-failure variants),
replication policy violation, replication validator rejection,
replication rate-limit rejection, and render-thread underrun delta
detection (game thread observes the counter delta and emits the
log line — render thread never logs directly).

**Threading:** the runtime serializes sink calls via one global
mutex so sink implementations don't need to be thread-safe
themselves. `ShouldLog_(level)` fast-paths via a nullptr check on
the sink pointer + uint8 level compare on members that don't
change after Initialize, so disabled categories cost a branch, not
a sink call. Field-array construction at the call site is also
skipped via `ShouldLog_` so the disabled path doesn't pay for
field formatting.

**DoD:** `tests/unit/logging_test.cpp` (9 sub-tests) — sink
formats, level filtering both ways, null-safety, end-to-end RTPC
budget overflow + replication policy violation + late-event
discard each producing exactly one log line of the expected
category and level with the expected structured fields.

**Limitations carried forward:**
- Single global mutex; no per-category locks. Highly contended
  rejection paths would serialize through this mutex, but rejections
  are rare by definition.
- Per-category level filtering not exposed in v0.7.0 — only global
  `logMinLevel`. Hosts can filter inside their sink if they need
  finer control.
- No log rotation / retention / compression in built-in sinks.
  Those concerns belong to the host's log shipper (vector,
  fluentd, journald).

---

## Phase 5: Material & acoustic environment authoring

**Queued behind:** multiplayer audio sandbox proving the basic
networked audio chain works end-to-end (in progress as of v0.23.17).
Once 2-client co-op with music + gunshots + ducking is empirically
verified, this becomes the next priority.

**Motivation.** The C++ engine already has substantial material-
aware acoustic infrastructure that isn't reachable from GDScript:
the `AudioMaterial` enum (Glass / Wood / Drywall / Concrete /
Metal / Curtain / Foliage) with tuned absorption + damping
coefficients, the `IAudioGeometryQuery` host hook, the
`OcclusionSystem` with per-frame budgeted raycasts and smoothed
LPF + gain targets, and the reverb DSP that `ReverbZone` exercises
at a coarse zone level. What's missing is the bridge: a default
Godot adapter for the geometry query, a GDScript-visible material
taxonomy, and an impact-sound API that uses it.

Industry alignment: this is the "Switch" (Wwise) / "Parameter"
(FMOD) pattern for content lookup, plus standard occlusion +
transmission DSP. Audio designers from those backgrounds expect
this surface — gool has the engine for it but hides it.

### 5.1 Expose `AudioMaterial` taxonomy + impact sound API — **M** [v0.24.0]

User-facing outcome: a game dev can write

```gdscript
var hit := raycast(...)
if hit:
    var material := Gool.material_from_collider(hit.collider)
    Gool.play_impact_sound("bullet_impact", hit.position, material)
```

…and get the metal trash can clang vs. grass thump distinction
working in their game, without hand-rolling a Dictionary lookup
per call site.

Work breakdown:
- Expose `AudioMaterial` enum in GDScript as `Gool.MATERIAL_*`
  constants (Default = 0 .. Foliage = 8), plus `Gool.material_name(int)`
  reverse lookup
- New `GoolAudioMaterial` resource (`Resource` subclass with one
  `@export var material: int` field) so material tags are
  inspector-assignable on `PhysicsBody3D`/`Area3D` via metadata
- `Gool.material_from_collider(Node) -> int` helper checking
  `gool_audio_material` metadata first, falling back to legacy
  group membership for backward compat with `FootstepSurfacePlayer`
- `Gool.play_impact_sound(event_name, position, material)` API
- Sound bank `.tres` format extended with `by_material` variant
  set (parsing change in `src/audio_engine/assets/sound_bank.cpp`)
- Sandbox demo addition: a wall with 3 sub-segments of different
  material (concrete, wood, foliage); demonstrate raycast-and-
  impact pattern from `fps_player.gd`'s `_try_fire`

Refactor concern: `FootstepSurfacePlayer` currently uses a
freeform `Dictionary[group_name → Array[String]]` pattern. Keep
that working but document the new typed material API as
preferred. Deprecation deferred to v0.25 at earliest.

Done when: sandbox demo plays distinct impact sounds for hits
against three different surfaces, with the variant selection
visible in the gool debug overlay.

### 5.2 Default Godot geometry query — **L** [v0.25.0]

User-facing outcome: occlusion + material-aware HF rolloff
"just works" once bodies are tagged. A gunshot through a concrete
wall sounds different from a gunshot through a curtain, with
zero engine code changes by the game dev.

Work breakdown:
- New C++ class `audio::godot_integration::GodotGeometryQuery`
  implementing `IAudioGeometryQuery` via Godot's `PhysicsServer3D`
- Maps `CollisionObject3D` material metadata → `AudioMaterial`
  using the helper from 5.1
- Set as the default during gool runtime init unless the host
  provides a custom geometry query
- Project settings: `addons/gool/occlusion/enabled` (default true),
  `addons/gool/occlusion/max_raycasts_per_tick` (default 16)
- New prefab `GoolMaterialVolume` (Area3D variant) for tagging
  geometry that doesn't have a `CollisionObject3D` (e.g.,
  visual-only walls that should still occlude audio)
- Integration test: sandbox box_level gets a "behind wall"
  configuration where firing through a wall produces audibly
  muffled remote-peer audio

Decision the user needs to make before this ships:
- Default occlusion to ON (auto-magical, ~16 raycasts/tick cost)
  or OFF (explicit opt-in, zero default cost)?
- Default vote: ON, but configurable via project setting.

Done when: in the sandbox, two players on opposite sides of a
concrete wall hear each other's gunshots low-pass filtered ~10dB
HF rolloff + 3-6dB level reduction, with smooth transitions as
players move out from cover.

### 5.3 Source-aware acoustic environment — **L** [v0.26.0, optional polish]

User-facing outcome: a gunshot fired *inside* a building
reverberates differently than one fired *outside*, regardless of
where the listener is positioned. Helldivers 2 / Hunt: Showdown /
Halo Infinite level fidelity.

Work breakdown:
- New prefab `GoolAcousticSpace` (extends or supersedes
  `ReverbZone`) with named environment tag
- Emitters auto-inherit their containing space's tag at create
  time via Area3D-style detection
- Each listener maintains a set of "audible spaces" — their own
  plus any whose emitters they can currently hear
- Per-audible-space rendering: route source through that space's
  reverb send when its tag differs from the listener's space
- Networking: emitter creation RPCs carry environment tag
- Cap concurrent audible spaces per listener (default 4) to
  bound DSP cost

Defer-or-skip rationale: for a 4-player co-op shooter on a 20Hz
tick budget and RTX 2060-class hardware, listener-based reverb
(what `ReverbZone` does today) is probably good enough. Build
5.3 only if playtest feedback says "the audio outside a building
should reveal what's happening inside it."

Done when: in the sandbox, a player firing in a small reverb-
tagged room is audible to a listener outside the room with the
room's reverb tail mixed in, not the listener's open-air
environment.

### Phase 5 risks and open questions

- **`FootstepSurfacePlayer` divergence.** Should the existing
  prefab be refactored in 5.1 to use the new typed API, or kept
  as a freeform Dictionary alternative? Refactoring is cleaner
  but risks breaking existing user content. Default: keep both,
  document the typed API as preferred for new content.

- **AudioMaterial coefficient tunability.** The hardcoded
  defaults in `geometry_query.h::AudioMaterialDefaults` are
  reasonable but game-specific. Should hosts override globally
  (preset table) or per-call (pass absorption/damping directly)?
  Both paths exist in `AudioOcclusionHit` already; just need
  GDScript wrappers.

- **Audio designer onboarding.** If gool adopts the typed
  taxonomy in 5.1, designers coming from Wwise will recognize
  "Switch on Material" semantics immediately. Document this in
  `docs/terminology.md` and `migration_from_wwise.md`.

- **Phase 5.3 ordering.** If the multiplayer game playtests show
  listener-based reverb is sufficient, 5.3 can be skipped or
  deferred indefinitely. The phase's "done when" criterion
  should be tested with a prototype before committing to the
  full L-effort.

---



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
