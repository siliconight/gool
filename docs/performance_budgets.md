# Performance budgets — voice, emitter, and rate limits

Reference for every limit gool enforces at runtime, what happens
when you hit one, how to spot the hit in the editor mixer dock,
and when to raise the limit vs accept it.

If you're new to gool, read `docs/quickstart_fps.md` first. This
doc is the next-level deep-dive once you've got a working audio
loop.

## Two independent budget systems

gool has **two** independent budget systems. Hitting either one
causes new sounds to be rejected or evicted; the symptom looks
the same (sounds going missing) but the fix is different.

| System | What it limits | Configured in |
|---|---|---|
| **Runtime budgets** (`AudioRuntimeBudget`) | How many sounds can be active at the same time | `include/audio_engine/config.h` → `AudioConfig::budget` |
| **Replication rate limits** (`ReplicationRateLimitConfig`) | How many `SubmitReplicatedEvent` calls per second per player can be accepted | `include/audio_engine/config.h` → `AudioConfig::replicationLimits` |

Runtime budgets are about **CPU and memory**: there's only so
much you can mix per audio callback. Replication rate limits are
about **anti-flood / anti-DoS**: a misbehaving (or malicious)
peer can't drive everyone else's audio engine into the ground.

## Runtime budgets — what gool ships with

These are the defaults from `AudioRuntimeBudget`. Override in
your engine init code if your game needs more headroom.

| Budget | Default | What it limits |
|---|---:|---|
| `maxActiveEmitters` | **128** | Total concurrent emitter instances (one-shots + looping combined) |
| `maxSpatialEmitters` | **64** | Of those, how many can be 3D-positioned (gunshots, footsteps, world sounds) |
| `maxVoiceSources` | **16** | Concurrent voice chat sources (peers actively speaking) |
| `maxOcclusionChecksPerFrame` | **12** | Per-emitter occlusion raycasts each frame |
| `maxStreamingAssets` | **32** | Streaming sound assets that can be open simultaneously |
| `maxStreamingVoices` | **8** | Concurrent streaming voices (music + ambient stems) |
| `maxRegisteredSounds` | **256** | Total sounds in the bank registry |
| `maxGameEventsPerFrame` | **256** | Local game events the runtime can process per tick |
| `maxNetworkEventsPerFrame` | **256** | Replicated events the runtime can ingest per tick |
| `maxActiveEmittersProcessedPerTick` | **0** (unlimited) | Interest-management cap: only process the N closest emitters; 0 = process all |

### When you hit a runtime budget

The runtime uses **priority-based eviction** for emitters: when
`maxSpatialEmitters` is reached and a new sound arrives with
higher priority than the lowest-priority active emitter, the
quiet one gets stopped and the new one starts. If the new
sound's priority is *lower* than every active emitter, it gets
**dropped at submission time** (never starts playing).

This is the right behavior for an FPS: a gunshot at priority 200
will always step on background ambience at priority 64. But it
means low-priority sounds are silently lost when the budget is
saturated.

Drops are counted in `RenderStats::drops` and surface in the
**Live Stats** panel of the editor mixer dock (see below).

### When to scale a runtime budget

The defaults are sized for typical mid-size games. Push them up
when:

- **`maxSpatialEmitters`** — 32-player FPS with everyone firing:
  64 fills up immediately. Push to 96-128 if you have CPU
  headroom. Mobile + battle royale: leave at 64 and use
  `maxActiveEmittersProcessedPerTick` instead.
- **`maxVoiceSources`** — multiplayer games beyond 16 simultaneous
  speakers. Most FPS designs cap voice at ~8 per team / ~16 total
  so 16 is usually enough.
- **`maxRegisteredSounds`** — large games with many distinct
  weapon firing samples, footstep variants, dialogue lines.
  Diminishing returns past 1024 — at that scale, switch to
  streaming.

Conversely, **drop budgets** for tighter platforms:

- **Mobile / handheld** — `maxSpatialEmitters = 32`,
  `maxOcclusionChecksPerFrame = 6`. Each spatial emitter costs
  CPU + per-frame raycasts. Halving both is a meaningful saving.
- **Web (HTML5)** — `maxStreamingVoices = 4` and consider
  preloading more, streaming less. Disk I/O patterns are
  different.

### Interest management — when emitter count is uncapped by gameplay

For very-large-scale games (60-player BR, MMO districts), the
*existence* of an emitter shouldn't tie up your spatial budget
just because it's in the world. Use
`maxActiveEmittersProcessedPerTick = N` (e.g. 30) to make the
runtime sort active emitters by distance to listener each tick
and process only the closest N. Out-of-top-N emitters get a
single zero-gain `UpdateParams` (muted but not stopped) until
they come back into the closest-N (at which point a fresh
`UpdateParams` unmutes them).

This is a softer cap than `maxSpatialEmitters` — emitters keep
existing, they just stop costing CPU. Use it for the "all 60
players' footsteps exist; only 30 should be audible" case.

## Replication rate limits — what gool ships with

Per-player token-bucket rate limits applied to
`SubmitReplicatedEvent`. From `ReplicationRateLimitConfig`:

| Category | Tokens/sec | Burst | Notes |
|---|---:|---:|---|
| `SFX` | **50** | **50** | Comfortably above any realistic single-player gunshot/explosion/footstep rate |
| `Voice` | **150** | **150** | 50 Hz Opus = 50 packets/sec; 3× headroom for retransmits and re-broadcast |
| `Music` | **5** | **5** | Music transitions are rare; absorbs legitimate state-flapping, rejects flooding |
| `Ambience` | **10** | **10** | Zone changes, weather; mostly host-driven |
| `UI` | **0** (unlimited) | **0** | UI is rarely replicated |
| `Dialogue` | **20** | **20** | Bursty in combat (NPC barks during firefights) |

To **disable rate limiting entirely** (e.g. for trusted-host
single-machine testing), zero out every category's
`tokensPerSecond`. The runtime treats 0 as "no limit" rather
than "0 events/sec."

### When a replication rate limit fires

Rejected events are counted in
`Stats::replicationEventsRejectedRateLimit` and **don't play**
on the receiving peer. The receiving peer's game state may now
disagree with the sending peer's about what audio happened —
usually fine for SFX (one missing footstep) but bad for Music
(state transitions don't apply). If you're seeing audio desync
on receiving peers, check the rejection stat first.

### When to scale a replication rate limit

Push **up** when:
- **SFX > 50/sec/player** — burst-fire weapons, full-auto with
  per-bullet audio events. Consider grouping into a single
  "burst" event server-side instead.
- **Voice** — usually never; 150 is already 3× the legitimate
  rate.

Push **down** when:
- Internet-facing host hardening — for shipping titles you might
  drop SFX to 30/sec to leave less room for flooding. The
  legitimate game won't notice.

### Anti-DoS budgets (separate from rate limits)

`maxTrackedPlayers = 64` and `maxNewPlayersPerTick = 8` defend
against **player-id cycling** attacks where an attacker fakes a
new `playerId` per packet to dodge the per-player rate limit.

- `maxTrackedPlayers` is the LRU table size. When exceeded, the
  least-recently-seen slot is evicted. **Bump for battle royale
  lobbies** (60-100 players).
- `maxNewPlayersPerTick = 8` caps how many never-seen-before
  `playerId`s can enter the rate limiter per tick. Sized for 4-8
  players showing up at session start, occasional reconnects
  mid-session. **Bump for tournament lobbies** that admit dozens
  in one tick. Set to 0 to disable (not recommended on
  internet-facing hosts).

## Reading the Live Stats panel during F5

The mixer dock's Live Stats panel (v0.44.0+) surfaces the
budget metrics in real time. Open the dock, hit F5 — these
fields update at 30 Hz:

| Label | What it shows | Healthy range | Watch for |
|---|---|---|---|
| **Voices** | Active emitter count / `maxActiveEmitters` | < 80% | Sustained > 90% = approaching saturation |
| **Emitters** | Spatial emitter count / `maxSpatialEmitters` | < 80% | Same — if this saturates and drops climb, bump the budget |
| **Master peak** | dBFS at the master bus output | < −3 dBFS | > 0 dBFS = clipping. Pull faders. |
| **Pre-mix peak** | dBFS BEFORE the master fader | < +3 dBFS | > +6 dBFS suggests too-hot input even though Master shows OK |
| **Drops** | Sounds rejected/evicted since last reset | Counter trending up = problem | Any sustained nonzero = budget hit, look at category breakdown |
| **Jitter** (per voice player) | Voice chat jitter ms | < 80 ms | > 80 ms = degraded voice quality, network issue |
| **Loss** (per voice player) | Voice chat packet loss ratio | < 10% | > 10% = noticeable cut-out |

The drops field is the **first thing to check** when sounds are
going missing in gameplay. A sustained nonzero drops count means
budget is being hit; cross-reference with Voices/Emitters to see
which category is saturating.

## AudioRelevancyFilter — preventing budget exhaustion

`addons/gool/audio_relevancy_filter.gd` is a pre-emitter filter
that drops sounds **before they consume a voice slot** if they're
beyond a configurable relevance threshold (typically distance
from listener).

For a 60-player BR where every player's footstep + every
weapon's firing emitter exists, the relevancy filter lets you
ignore the 40 players who are >100m away without them costing
anything.

```gdscript
# In your weapon firing code
if AudioRelevancyFilter.is_relevant(muzzle_pos, listener_pos, 100.0):
    Gool.play_3d("gunshot", muzzle_pos, 200)
# else: silently skip — no voice consumed, no occlusion check,
# no spatial processing
```

The filter's distance check is `O(1)` so calling it before every
`play_3d` is cheap. Use it as a first-line defense before voice
budget tuning.

## Priority guidance — convention for FPS

`Gool.play_3d(name, pos, priority)` and similar take a 0-255
priority. Higher = wins eviction battles. Conventions for FPS:

| Priority | Use case |
|---:|---|
| **250** | Critical narrative — boss intros, mission-ending dialogue |
| **220** | Local player gunshot, reload, melee swing — your own actions |
| **200** | Important enemy callouts (NPC barks via DialogueDirector), local impact sounds |
| **180** | Remote player gunshots — teammates' fire, important but second to yours |
| **160** | Enemy fire (incoming bullets, distant gunfire) |
| **128** | Default — most one-shots, ambient triggers, generic effects |
| **96** | Background variation — looping ambience, idle creature sounds |
| **64** | Nice-to-have polish — distant world chatter, low-importance feedback |
| **32** | Filler — wind gusts, distant rain, very low priority |

Sound rule: **your local actions outrank the same action by
remote players**. If your gun fires at priority 220 and a
teammate's at 180, your audio wins eviction when the budget is
hit. This matches FPS player expectations.

Voice chat is special — its budget is separate (`maxVoiceSources`)
so it doesn't fight SFX for slots. Treat voice priority as
"intelligibility tier" — barks at 220, comms at 200, chatter at
160.

## Memory budgets — a quick aside

Voice budgets are about CPU. Memory budgets are about asset
loading patterns. Quick rules:

- **`maxRegisteredSounds = 256`** — each registered sound holds
  metadata (~200 bytes) plus the actual sample data. 256 sounds
  of 100KB each = 25 MB of audio resident. Reasonable for a
  shooter.
- **Streaming** — sounds > 1 MB should generally stream (set
  `streaming = true` in the bank entry). Streaming uses
  `maxStreamingVoices` slots, of which the default is 8.
  Streaming uses a small ring buffer (a few KB) per voice
  instead of holding the whole sample in memory.
- **Reverb** — gool's reverb doesn't load convolution impulse
  responses; it's algorithmic. Memory cost is fixed and small.

## Putting it together — a 32-player FPS profile

If you're building a 32-player FPS, here's a starting budget
configuration:

```cpp
AudioConfig cfg;
cfg.budget.maxActiveEmitters         = 192;   // (default 128)
cfg.budget.maxSpatialEmitters        = 96;    // (default 64)
cfg.budget.maxVoiceSources           = 32;    // (default 16) — 16/team
cfg.budget.maxOcclusionChecksPerFrame = 16;   // (default 12)
cfg.budget.maxRegisteredSounds       = 512;   // weapon variants, footstep variants

cfg.replicationLimits.perCategory[SFX]      = {  60.0f,  60 };  // (default 50)
cfg.replicationLimits.perCategory[Voice]    = { 200.0f, 200 };  // (default 150)
cfg.replicationLimits.perCategory[Dialogue] = {  30.0f,  30 };  // (default 20)

cfg.replicationLimits.maxTrackedPlayers     = 64;   // matches lobby size
cfg.replicationLimits.maxNewPlayersPerTick  = 16;   // tournament-friendly
```

For a **60-player battle royale**, also set
`maxActiveEmittersProcessedPerTick = 40` so distant players'
emitters exist but don't burn CPU.

For **mobile FPS**, halve `maxSpatialEmitters` and
`maxOcclusionChecksPerFrame`, and use the relevancy filter
aggressively.

## Where the constants live

If you're modifying these, the canonical source is
`include/audio_engine/config.h`. The `AudioConfig` struct passed
to `AudioRuntime::Initialize()` carries:

- `budget` (`AudioRuntimeBudget`) — runtime emitter/voice pools
- `replicationLimits` (`ReplicationRateLimitConfig`) — per-player
  per-category token buckets + anti-DoS caps

These are read **once at runtime initialization** — they're not
mutable post-init. Plan your budget at integration time.

## Where to go next

- `docs/quickstart_fps.md` — the integration recipe
- `docs/networking_bridge.md` — how the bridge interacts with
  rate limits (predicted events vs replicated events)
- `addons/gool/tools/fps_scene_smoke_test.gd` — v0.50.0 static
  validator that checks your scenes against the budget shape
- `addons/gool/audio_relevancy_filter.gd` — the relevancy
  filter's source + parameters
