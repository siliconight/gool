# Networking integration guide

How to choose the right gool entry point for each kind of audio
data your host network code is moving. The choice is not arbitrary —
gool's network-thread API is shaped around four well-known classes
of game-network data, and picking the right class buys you the
right delivery semantics for free. Picking the wrong one is the
most common cause of audio that "feels off" in multiplayer.

This doc is the conceptual companion to
[`multiplayer.md`](multiplayer.md) (the hands-on integration guide)
and [`replication_patterns.md`](replication_patterns.md) (the
server-auth / client-predicted / client-authoritative split). The
three docs cover the same territory from different angles; if
you're just starting, read [`multiplayer.md`](multiplayer.md)
first.

## The four classes of game-network data

Every byte your network thread hands to gool falls into one of four
classes. The classes are distinguished by their answer to two
questions:

  - **What's the cost of dropping this byte?**
  - **What's the cost of this byte arriving late?**

That two-by-two gives the taxonomy:

|                       | Loss is cheap        | Loss is expensive       |
|-----------------------|----------------------|-------------------------|
| **Staleness is cheap**    | (no audio data here) | **Guaranteed in-order** |
| **Staleness is expensive**| **Drop if late** or **Most-recent-state** | **Quickest-possible**   |

The cell where both axes are cheap is empty for audio — there's no
data class that's both loss-tolerant *and* staleness-tolerant, by
construction (if you don't care about loss or staleness, you don't
need to send it at all). The other three cells each map to a
distinct delivery requirement, and the lower-right "expensive
staleness" cell splits into two depending on whether the data is
discrete (one-off events) or continuous (volatile state).

The four classes:

### 1. Drop if late — non-guaranteed ephemeral events

Time-sensitive triggers that lose their meaning if they arrive
after the visual frame they belong to has passed. A footstep at
t=1.0s playing at t=1.3s is audibly *wrong* — the player has
moved past that moment, and the audio cue contradicts what's
on-screen.

Examples: footsteps, ambient gunfire from distant peers, particle-
effect sounds, peripheral SFX that the player isn't focused on.

The right transport is unreliable. Retransmission would be worse
than loss: it adds latency that makes the stale-cue problem worse,
and the bandwidth would be better spent on data that hasn't already
expired.

### 2. Guaranteed in-order — state transitions

Discrete events that must arrive, must arrive in order, and whose
delivery delay is acceptable (a few ticks late is fine; missing
altogether is not). The event changes what happens to *all
subsequent audio* — a music transition, a bus-graph hot-swap, a
"player joined voice chat" handshake.

Examples: music state transitions ("combat → menu"), bus-graph
reconfiguration, voice-chat coordination ("player X joined the
party"), mute-state changes, sound-bank reload.

The right transport is reliable in-order (TCP, ENet's reliable
channel, Steam Sockets' reliable mode, your custom UDP layer with
retransmit + sequence-number sort). The cost is latency — you
wait for retransmits and reorder buffers — but the cost is
acceptable for events the player won't perceive as
sub-second-critical.

### 3. Most-recent-state — continuous volatile state

Continuous data that changes every tick, where only the latest
version matters. An emitter's position at t=10 is irrelevant once
you've sent the position at t=11 — there is no value in
retransmitting the t=10 sample, and active harm in delaying the
t=11 sample to do so.

Examples: emitter positions, emitter forwards, emitter velocities,
RTPC parameter values, listener orientation, master gain.

The right transport is unreliable with last-write-wins semantics.
You send the latest sample on every tick; receivers interpolate
between the two most recent samples to hide single-packet loss.
Retransmission is meaningless because by the time the retransmit
would arrive, a fresher sample has already been sent.

### 4. Quickest-possible — sub-tick latency signals

Discrete events where the player's perception of the gameplay
depends on the sound arriving within milliseconds of the event
that caused it. Voice chat is the canonical example — a 200ms
voice delay makes conversation feel broken — and a small set of
SFX share the same constraint, mostly hit-confirmation feedback
in twitchy shooters.

Examples: voice chat packets, hit confirmations in FPS / fighting
games, melee impact frames, weapon-readiness chirps, parry
windows.

The right transport is "as fast as possible, with redundancy
instead of retransmission." Send the data immediately, send it
again in the next two packets (the redundancy hides single
losses), accept that a triple loss drops the cue entirely
because retrying after a triple loss would arrive too late to
matter.

## Mapping the classes to gool's API

Each of gool's four network-thread entry points implements one
class:

| Class                  | gool entry point                                                                  | Defaults to       |
|------------------------|------------------------------------------------------------------------------------|-------------------|
| 1. Drop if late        | `SubmitReplicatedEvent(event, source, EventDelivery::Drop)`                        | Backward-compat   |
| 2. Guaranteed in-order | `SubmitReplicatedEvent(event, source, EventDelivery::Guaranteed)`                  | —                 |
| 3. Most-recent-state   | `UpdateReplicatedTransform(handle, mask, pos, fwd, vel, tick)`                     | mask = `All`      |
| 4. Quickest-possible   | `SubmitImmediateEvent(event, source)` for SFX, `OnVoicePacket(...)` for voice      | —                 |

The bottom three were added across v0.18.0 (`EventDelivery`,
`TransformStateMask`) and v0.19.0 (`SubmitImmediateEvent`,
`EmitterDescriptor::replicationPriority`). The top one — Drop if
late — is the default behavior of `SubmitReplicatedEvent` since
v0.1, just now with an explicit enum value.

### Why the split across two methods for class 4

Class 4 has two gool entry points because voice and SFX have
different shapes. Voice packets are continuous (one every 20ms),
arrive at predictable cadence, and are bounded in size; gool
handles them in a dedicated jitter buffer + decoder path that's
been there since v0.1. Time-critical SFX are sparse (a handful per
second at peak), don't have a steady cadence, and use the full
`AudioEvent` shape; the right path for them is
`SubmitImmediateEvent`, which lands in a small 8-entry ring drained
at the top of every `Update()` tick.

Mixing the two through one entry point would force either:
  - voice through the AudioEvent path (lossy: AudioEvent is bigger
    than a VoicePacket and would inflate the voice budget); or
  - SFX through the jitter-buffer path (lossy in a different way:
    jitter buffers add deliberate latency to smooth packet timing,
    which is the opposite of what hit-confirm SFX want).

Two entry points, two cost models, one class.

## Worked examples

### Class 1: Drop if late — ambient gunfire from a distant peer

```cpp
// Network thread, on receipt of a remote gunshot packet.
audio::AudioEvent ev;
ev.type            = audio::AudioEventType::PlaySoundAtLocation;
ev.soundId         = sounds_.gunfire_rifle_distant;
ev.position        = packet.position;
ev.timestampMs     = packet.sender_timestamp_ms;
ev.maxStalenessMs  = 200;   // gunshots at 200 ms or older are silently dropped
ev.category        = audio::AudioCategory::SFX;
ev.playerId        = packet.shooter_player_id;

// Drop class is the default; the 1-arg form already does the right
// thing. The 3-arg form makes the intent explicit at the call site —
// recommended when the call site is far from where the event was built.
runtime_.SubmitReplicatedEvent(ev,
    audio::ReplicationSource::Server,
    audio::EventDelivery::Drop);
```

The `maxStalenessMs = 200` says "if this arrives more than 200 ms
after the shooter pulled the trigger, don't play it." The runtime
enforces that against `latestServerTimeMs_` published from your
`OnTickAdvanced` calls. Below the threshold: played. Above: dropped
and bumped in `Stats::eventsLateDropped`.

Use `Drop` for: footsteps, ambient gunfire, peripheral SFX, particle
hits, distant explosions, peer-action acknowledgments, hit-impact
sounds from non-controlled actors.

### Class 2: Guaranteed in-order — switching the music bed at round end

```cpp
// Network thread, on receipt of a round-end packet.
audio::AudioEvent ev;
ev.type            = audio::AudioEventType::TriggerSequence;
ev.sequenceId      = sequences_.post_round_victory;
ev.timestampMs     = packet.server_timestamp_ms;
ev.maxStalenessMs  = 0;   // no per-event staleness limit — must stick
ev.category        = audio::AudioCategory::Music;

runtime_.SubmitReplicatedEvent(ev,
    audio::ReplicationSource::Server,
    audio::EventDelivery::Guaranteed);
```

The `Guaranteed` class tells the runtime "even if this arrives
late, play it — the host's transport already paid the
retransmission cost, and the player needs the music change to
land before whatever happens next." `Stats::eventsAcceptedGuaranteedLate`
counts how often the late path fired; non-zero in steady state
is your signal that the reliable transport is slow, or the
classification is wrong (something marked Guaranteed that should
be Drop).

Use `Guaranteed` for: music transitions, bus-graph reconfiguration,
voice-chat join/leave events, mute-state changes, sound-bank
reload, persistent ambience setpieces (the new wind direction
after a level transition).

### Class 3: Most-recent-state — replicating a peer's position

```cpp
// Network thread, on receipt of a per-tick state snapshot.
// Send the full transform.
runtime_.UpdateReplicatedTransform(
    peer.audio_emitter,
    audio::TransformStateMask::All,
    packet.position,
    packet.forward,
    packet.velocity,
    packet.simulation_tick);

// Or — bandwidth-aware variant — when you know only the orientation
// changed (a stationary turret tracking the player):
runtime_.UpdateReplicatedTransform(
    turret.audio_emitter,
    audio::TransformStateMask::Forward,   // mask covers only the dirty subfield
    {},                                    // position ignored
    packet.turret_forward,
    {},                                    // velocity ignored
    packet.simulation_tick);
```

The mask overload (v0.18.0) saves bandwidth on subfields that
haven't changed, and the runtime's two-tick history per subfield
ensures the interpolator keeps the prior value alive until the
next fresh sample arrives.

Use `UpdateReplicatedTransform` for: peer-player positions, AI
agent positions, vehicle positions, projectile positions (if you
spatialize them), turret rotations.

Set `EmitterDescriptor::replicationPriority` on the emitter to
let gool drop low-priority updates first when the ring saturates:

```cpp
audio::EmitterDescriptor desc;
desc.soundId               = sounds_.player_footsteps_loop;
desc.followsReplicatedTransform = true;
desc.replicationPriority   = 192;   // important: this is the local player's view
// ... rest of descriptor ...
runtime_.CreateEmitter(desc);

// vs. distant ambient peer:
audio::EmitterDescriptor distantDesc;
// ... soundId and position ...
distantDesc.replicationPriority   = 64;    // lower priority; dropped under pressure first
runtime_.CreateEmitter(distantDesc);
```

Defaults are 128 (middle of the band). Higher = more important;
lower = dropped first when the ring exceeds 75% capacity.

### Class 4a: Quickest-possible — hit confirmation

```cpp
// Network thread, on receipt of a hit-confirm packet.
audio::AudioEvent ev;
ev.type            = audio::AudioEventType::PlaySoundAtLocation;
ev.soundId         = sounds_.hit_confirm_headshot;
ev.position        = local_player_pos;     // local for immediate-feedback SFX
ev.timestampMs     = packet.server_timestamp_ms;
ev.category        = audio::AudioCategory::SFX;
ev.priority        = audio::AudioPriority::High;

auto r = runtime_.SubmitImmediateEvent(ev,
    audio::ReplicationSource::Server);

if (r == audio::AudioResult::QueueFull) {
    // The 8-entry ring is the natural rate limit. Above 8
    // immediate events per Update() tick the host should fall
    // back to the regular Drop path (still plays, just doesn't
    // get the sub-tick latency saving).
    runtime_.SubmitReplicatedEvent(ev,
        audio::ReplicationSource::Server,
        audio::EventDelivery::Drop);
}
```

`SubmitImmediateEvent` drops the event into a dedicated 8-entry
ring drained at the top of `Update()` — before any other phase
runs — so the network-thread-to-control-thread latency is one
phase shorter than the regular `SubmitReplicatedEvent` path. The
saving is small in absolute terms (~5-10 µs in typical hosts) but
the dominant component of perceived latency is the wait between
network arrival and the next `Update()` tick, and `Phase 0`
shaves a tick off that wait.

Use `SubmitImmediateEvent` for: hit confirmations, melee impact
frames, weapon-readiness chirps, parry-window indicators,
input-buffered timing cues.

### Class 4b: Quickest-possible — voice packets

```cpp
// Network thread, on receipt of a voice packet.
runtime_.OnVoicePacket(
    sender.player_id,
    packet.bytes,
    packet.size,
    packet.sequence_number,
    packet.send_timestamp_ms);
```

Voice rides its own path (jitter buffer + Opus decoder) and doesn't
share the immediate-event ring. The hot-path properties — sub-tick
latency, packet-loss concealment, redundancy via the jitter buffer's
forward-error-correction frames — are managed by gool internally;
the host's job is just to call `OnVoicePacket` as fast as packets
arrive.

## Telemetry — what each Stats counter tells you

A `Stats` snapshot pulled after every `Update()` tick gives you the
operational view. The counters relevant to network integration:

```cpp
const auto stats = runtime_.GetStats();
```

**Per-class submission volume (v0.18.0):**
  - `eventsSubmittedDrop` — total Drop-class submissions
  - `eventsSubmittedGuaranteed` — total Guaranteed-class submissions
  - `eventsImmediateProcessed` — total immediate-class events processed (v0.19.0)

The ratios between these tell you what shape your audio traffic
takes. A typical FPS sees ~95% Drop, ~3% Guaranteed, ~2% Immediate.
If the ratio is wildly different, you may be misclassifying — see
the anti-patterns below.

**Per-class failure modes:**
  - `eventsLateDropped` — Drop-class events that arrived past their
    `maxStalenessMs` and were silently discarded. Normal at low
    rates; high rates mean either the host's network is laggy or
    `maxStalenessMs` is too tight.
  - `eventsAcceptedGuaranteedLate` — Guaranteed-class events that
    arrived late but were processed anyway. **The actionable
    signal.** If this rises in steady state, either the host's
    reliable transport is slow, or events are being misclassified
    (something marked Guaranteed that should be Drop).
  - `eventsImmediateRejected` — immediate events that hit the
    8-entry ring's `QueueFull` ceiling. Non-zero means the host is
    exceeding the natural rate limit; either raise the host-side
    threshold for what qualifies as immediate, or accept the
    fallback to the regular `Drop` path.

**Bandwidth budget (v0.18.0):**
  - `eventRingCapacityRemaining` — free slots in the network event
    ring at the end of the last `Update()`
  - `transformRingCapacityRemaining` — same, for the transform ring
  - `nextTickProductionBudgetBytes` — soft target for total event +
    transform bytes the host should send before the next `Update()`

Below 25% capacity remaining, drop low-priority work at the host's
end before calling gool. Below 10%, expect `QueueFull` returns.
Forward visibility is always cheaper than reactive backpressure.

**Transform-priority drops (v0.19.0):**
  - `transformsDroppedByPriority` — count of transforms rejected at
    submission because the transform ring was over 75% full and the
    emitter's `replicationPriority` was below 128. Non-zero in
    steady state means you're producing transforms faster than gool
    can drain them, and lowering the priority of distant or
    off-screen emitters at the host's end would reduce unnecessary
    work.

## Common mistakes

### Marking ephemeral SFX as Guaranteed

The reflex is "I want this sound to play, so I should mark it
Guaranteed." This is wrong for time-sensitive cues. A gunshot
marked Guaranteed will play 300ms late if the network hiccups —
which is worse than not playing at all, because the player has
already moved past the moment it belonged to.

The Drop class isn't "we don't care about this event" — it's "if
we can't play it on time, *we'd rather not play it.*" That's the
correct cost model for almost all gameplay SFX.

### Calling SubmitImmediateEvent for music transitions

Music transitions don't need sub-tick latency — they need to stick
on the right tick. A music transition that's one tick late is
imperceptible; a music transition that's lost entirely is a bug.
Music wants `Guaranteed`. The immediate ring's 8-entry capacity is
small precisely because it's expensive to drain at the top of every
tick; reserve it for events that actually need that property.

### Sending position updates as events

The reflex from a pure-events networking model is "everything is an
event." Don't apply that to positions. `UpdateReplicatedTransform`
exists because positions are most-recent-state: only the latest
sample matters, and the runtime's two-tick history + interpolator
handles single-packet loss transparently. Sending position changes
as events forces the runtime to process every sample (even ones
that have already been superseded) and wastes the ring's capacity
on data that's about to be stale.

### Ignoring `replicationPriority` and seeing transform drops

If `Stats::transformsDroppedByPriority` is non-zero and you haven't
set `replicationPriority` on any emitters, it means the default
(128) is too high for all your emitters combined. Either raise the
transform ring depth via `AudioConfig.budget.maxActiveEmitters`, or
tier your emitters into priority bands so the runtime can drop the
ones that matter least.

A working pattern for an FPS with peer audio:

| Emitter type                          | priority |
|---------------------------------------|----------|
| Local-player UI / weapon-ready cues   | 255      |
| Boss / narrative-critical SFX         | 224      |
| Nearby peer footsteps / weapons       | 192      |
| Mid-range peers / vehicles            | 160      |
| Default (the implicit middle)         | 128      |
| Distant ambient peers / world clutter | 64       |
| Off-screen background ambience        | 32       |

The runtime drops priority < 128 when the ring is over 75% — so
the lower two bands are the elastic buffer that absorbs network-
side bursts.

### Treating `QueueFull` as a fatal error

`SubmitReplicatedEvent` returning `AudioResult::QueueFull` is a
backpressure signal, not a failure. The right host-side response
is the same as a TCP write returning EAGAIN: back off, drop
low-priority work, and try again next tick. Logging at error level
on QueueFull produces noise; logging at debug or info is right.
The corresponding telemetry — falling capacity-remaining counters
— is the leading indicator that lets you back off before
QueueFull fires.

## See also

- [`multiplayer.md`](multiplayer.md) — hands-on integration guide
  for voice / events / transforms with concrete Steam Sockets, ENet,
  and raw UDP patterns.
- [`replication_patterns.md`](replication_patterns.md) — the
  server-authoritative / client-predicted / client-authoritative
  patterns and the threat model for each.
- [`THREADING.md`](THREADING.md) — the three-thread split
  (game / control / network) and which entry points are valid from
  which thread.
- [`determinism.md`](determinism.md) — the deterministic-replay
  guarantees and which API forms preserve them.
