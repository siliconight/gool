# Replication patterns for multiplayer audio

In a multiplayer game, "play this sound" can mean different things
depending on who has authority and how much latency you can
tolerate. gool supports three replication patterns; this doc
helps you pick which one to use for each kind of sound.

The patterns are exposed through the `NetworkedAudioEvent`
prefab's `mode` enum, the `NetworkedAudioEmitter3D` prefab's
authority awareness, and the underlying `SubmitReplicatedEvent` /
`CancelPredictedEvent` / `UpdateReplicatedTransform` C++ API.

## The three patterns

### 1. Server-authoritative

```
Game logic (server)  →  SubmitReplicatedEvent locally
                     →  RPC to relevant clients
Clients              →  receive RPC
                     →  SubmitReplicatedEvent on their runtime
```

**When:** explosions, environmental events, NPC vocals, anything
where the server owns truth.

**Latency:** 1 RTT. Clients hear the sound after one
round-trip's worth of network delay.

**Bandwidth:** O(N peers in audible range). Filter aggressively —
a 32-player server should not be sending grenade-explosion RPCs
to everyone in the level.

**Anti-cheat:** strong. Cheating clients can't fabricate sounds
that influence other players.

**Inspector setting:** `NetworkedAudioEvent.mode = SERVER_AUTHORITATIVE`.

### 2. Client-predicted

```
Local player (client) →  SubmitEvent locally with predictionId
                      →  RPC server to validate
Server                →  validate game rules
                      →  if OK: SubmitReplicatedEvent + RPC to OTHER clients
                          (predicting client already heard it)
                      →  if NOT OK: RPC predicting client to cancel
Predicting client     →  if rejected: CancelPredictedEvent(id, fadeMs)
Other clients         →  receive RPC, SubmitReplicatedEvent
```

**When:** local player firing their own weapon, casting an
ability, dashing — anything where the local player can't
tolerate the 1-RTT delay of server-authoritative but the server
must still own truth.

**Latency:** 0 for the predicting client (instant), 1 RTT for
others.

**Bandwidth:** roughly the same as server-authoritative, plus a
small validation message client→server. The
"already-heard-it-skip-this-peer" optimization saves one RPC
target per shot.

**Anti-cheat:** moderate. Cheating clients can hear illegal
predictions locally (they fade out within 50 ms when rejected),
but other clients only hear server-validated sounds.

**Failure mode:** **prediction storm.** If the server rejects
many predictions in quick succession (lag spikes, large
desync), the predicting client hears multiple aborted sounds.
Tune `NetworkedAudioEvent.late_threshold_ms` to drop predictions
that have aged too much by the time validation completes.

**Inspector setting:** `NetworkedAudioEvent.mode = CLIENT_PREDICTED`.

### 3. Client-authoritative

```
Client     →  SubmitEvent locally
           →  RPC to relevant other clients (skip server)
Other clients  →  receive RPC, SubmitReplicatedEvent
```

**When:** footsteps, locomotion sounds, low-stakes cosmetic
audio. Anything where the cheating cost is low enough that
server validation isn't worth the round-trip.

**Latency:** 0 for the source, 1 hop for others (no server
relay).

**Bandwidth:** lowest of the three. No server message at all.

**Anti-cheat:** weak. Cheating clients can fabricate footstep
sounds at arbitrary positions; server can't intercept. **Don't
use this for sounds that reveal tactical information to enemies.**

**Inspector setting:** `NetworkedAudioEvent.mode = CLIENT_AUTHORITATIVE`.

## Picking a pattern by sound type

| Sound                          | Pattern                  | Reason                                                     |
|--------------------------------|--------------------------|------------------------------------------------------------|
| Footsteps (local player)       | CLIENT_AUTHORITATIVE     | Low stakes; no need for server validation                  |
| Footsteps (other players)      | (received via above)     | —                                                          |
| Local weapon fire              | CLIENT_PREDICTED         | Instant feedback; server validates ammo/cooldown           |
| Other player's weapon fire     | (received via above)     | —                                                          |
| Grenade explosion              | SERVER_AUTHORITATIVE     | Server owns the physics simulation that triggered it       |
| Environment ambient (waterfall)| SERVER_AUTHORITATIVE     | Stationary; one-time replicate when player enters area     |
| Voice chat                     | (separate path: OnVoicePacket) | Has its own jitter buffer, PLC, and packet flow      |
| Music transitions              | (local only)             | Each client picks its own based on local game state        |
| UI sounds (button click)       | (local only)             | Never replicated                                           |
| Adaptive ambience              | (local only or server-auth) | Depends: if it cues other players, replicate            |

## Relevancy filtering

All three patterns benefit from peer-relevancy filtering — only
RPC to peers who could possibly hear the sound. Without
filtering, a 32-player server doing 100 sounds/second across a
wide map RPCs ~100,000 messages/second; with filtering by
audible radius, that drops to ~1000.

The `AudioRelevancyFilter` helper handles distance + team gating:

```gdscript
var filter := AudioRelevancyFilter.new()
filter.default_audible_radius = 50.0
# Update peer positions every network tick:
for peer in connected_peers:
    filter.update_peer(peer.id, peer.position, peer.team)

var event_node := $NetworkedExplosionEvent
event_node.relevancy_filter = filter

# Triggering:
event_node.play("explosion_grenade", explosion_pos,
                  /*audible_radius=*/100.0)   # explosions audible farther
```

## Bandwidth prioritization

Each event carries an `AudioPriority` (0=lowest, 64=low,
128=normal, 192=high, 255=critical). On the receiving side, the
engine's voice cap evicts lowest-priority sounds first when the
pool is saturated. On the sending side, the host can drop
low-priority events when bandwidth is constrained:

```gdscript
# In a custom NetworkedAudioEvent subclass:
func _on_validate_prediction(sound_name, position, peer):
    if _server_bandwidth_saturated() and priority < 128:
        return false   # reject low-priority predictions under load
    return true
```

## Determinism notes

All three patterns produce **bit-identical sample output** across
runs given identical input timelines, when:
- The host calls `runtime.OnTickAdvanced(simTick, serverTimeMs)`
  with deterministic timeline values during replay.
- Voice packets use the 6-arg `OnVoicePacket(...arrivalMs)` form
  with host-supplied tick time.
- The host keeps the order of `SubmitReplicatedEvent`,
  `UpdateReplicatedTransform`, and `CancelPredictedEvent` calls
  consistent between recording and replay.

This is what makes spectator mode and replay viewer features
practical: a recorded game produces the same audio mix on
playback that the original players heard.

The replicated_events_test in the engine test suite proves this
end-to-end.

## API summary

Engine API (C++):
- `runtime.SubmitReplicatedEvent(event)` — server-authoritative event
- `runtime.UpdateReplicatedTransform(handle, pos, fwd, vel, simTick)` — replicated emitter movement
- `runtime.CancelPredictedEvent(predictionId, fadeMs)` — abort a predicted local sound
- `runtime.OnTickAdvanced(simTick, serverTimeMs)` — sync engine tick clock

GDScript API (via `/root/Gool` autoload):
- `Gool.submit_event_local(name, pos, predictionId, priority, ts)`
- `Gool.submit_replicated_event(name, pos, simTick, serverTimeMs, priority)`
- `Gool.cancel_predicted_event(predictionId, fadeMs)`
- `Gool.update_replicated_transform(handle, pos, fwd, vel, simTick)`
- `Gool.make_prediction_id()` — unique id generator

Prefabs:
- `NetworkedAudioEvent` — wraps the three patterns with one-line `play(...)` API
- `NetworkedAudioEmitter3D` — persistent emitter with authority + transform replication
- `AudioRelevancyFilter` — distance + team based peer culling

For the actual scene wiring of these prefabs, see
`examples/quickstart` and the engine repo's `tests/unit/replicated_events_test.cpp`.

---

## Threat model

The runtime ships flow-control and field-validity defenses out of
the box; it cannot make decisions about peer identity or sender
authority. That's the host's job. This section spells out the
trust boundary so it's clear what shipped code defends against and
what host integration is responsible for.

### What the runtime can validate

These are enforced inside the engine, before any event reaches
the control thread:

- **Flow control.** Per-player, per-category token-bucket rate
  limiter on `SubmitReplicatedEvent` and `OnVoicePacket`. Defaults
  in `AudioConfig::replicationRateLimit`: 50 SFX/sec/player,
  150 voice packets/sec/player, 5 music/sec, 20 dialogue/sec,
  10 ambience/sec, UI unlimited. Rejections surface as
  `AudioResult::RateLimited` and aggregate in
  `Stats::replicationEventsRateLimited[6]`.

- **PlayerId-cycling DoS.** Per-tick admission cap on
  never-seen-before playerIds (`maxNewPlayersPerTick = 8` default).
  Above the cap, new ids are rejected for the rest of the tick;
  counter resets on `OnTickAdvanced`. Surfaced as
  `Stats::replicationEventsRejectedNewIdBudget`. Without this, an
  attacker could blow out the rate limiter's LRU table and reset
  every legitimate player's bucket counters.

- **Replication-policy spoofing.** When the host calls the 2-arg
  `SubmitReplicatedEvent(event, ReplicationSource::Client)`, the
  runtime rejects events declaring
  `replicationPolicy = ServerAuthoritative` — clients cannot
  author state changes that affect remote listeners. Returns
  `AudioResult::PolicyViolation`. Tracked in
  `Stats::replicationPolicyViolations` (separate from
  `replicationEventsRejectedByValidator` so dashboards can tell
  protocol enforcement from host-policy denials). The 1-arg
  overload is `ReplicationSource::Unknown` (legacy / permissive).

- **Field validity** (via opt-in `DefaultBoundsValidator`).
  Rejects events with NaN/Inf in `position`, `forward`, or
  `velocity`; extreme magnitudes (default caps: 1,000 km position,
  100 km/s velocity); NaN/Inf or out-of-range
  `parameterValue` / `parameterSmoothingMs`; optionally,
  unknown `soundId` via a host-supplied lookup callback.

### What the runtime CANNOT validate

The runtime has no view of the network layer, so the following
are the host's responsibility — full stop:

- **Who sent which packet.** The runtime sees `event.playerId` as
  a uint32; it cannot know whether that field matches the
  authenticated peer that delivered the packet. The host's
  network layer (Steam GameNetworkingSockets, ENet, EOS,
  WebRTC, raw UDP with auth tokens, etc.) knows the connection
  identity. The host is responsible for replacing
  `event.playerId` with the authenticated peer id BEFORE calling
  `SubmitReplicatedEvent`.

- **Whether a sender role matches a claimed policy.** The runtime
  can enforce "Client cannot submit ServerAuthoritative" only
  because the host tells it the source. The host knows whether a
  payload arrived from a server connection or a client connection;
  the runtime doesn't.

- **Content authority.** The runtime accepts whatever
  `event.category` field the host stamps. A malicious payload
  claiming `AudioCategory::UI` (default unlimited) can bypass
  the rate limiter for a category Godot would normally never
  replicate. The host must derive `event.category` from the
  authored sound definition (look it up by `soundId` in the
  loaded sound bank), not copy it from the wire payload.

- **Logical anti-cheat.** "This player just fired 30 gunshots in
  one second from a position 200 m from where they actually are"
  is gameplay state the runtime doesn't see. Install an
  `IReplicationValidator` to apply your game-mode-specific rules.

### The four host-side rules

If you're integrating gool into an online multiplayer game with
adversarial clients, follow all four. None are optional.

1. **Authenticate `playerId` server-side.** Before calling
   `SubmitReplicatedEvent`, replace whatever the wire payload
   claimed with the authenticated peer id from your network
   layer. Never trust a `playerId` field from an untrusted
   connection.

2. **Derive `category` from the sound definition, not the wire.**
   When you receive a replicated event from a client, look up
   the sound by `event.soundId` in your loaded `SoundBank`,
   take the authored `AudioCategory` from the `SoundDefinition`,
   and overwrite `event.category` with it. This closes the
   category-spoof gap.

3. **Enforce policy by sender role.** Use the 2-arg
   `SubmitReplicatedEvent(event, ReplicationSource::Client)` for
   every event you receive from a network peer. Use
   `ReplicationSource::Server` only for events authored by your
   own authoritative server logic. The runtime then enforces
   "client cannot author ServerAuthoritative" automatically.

4. **Validate numeric ranges before submit.** Install
   `DefaultBoundsValidator` (chained with your custom validator
   via `ChainReplicationValidator` if needed) so events with
   NaN/Inf or extreme magnitudes are rejected before they reach
   the spatializer. Configure tighter magnitude caps if your
   game world is smaller than the defaults.

### What this gets you, and what it doesn't

With the runtime defenses + the four host-side rules, the
remaining attack surface is:

- A legitimate authenticated peer abusing their own per-player
  budgets within configured limits (plain rate limiting handles
  this; tune the budgets for your game).
- Logical exploits requiring gameplay context the audio layer
  doesn't have (e.g. "fire underwater," "shoot through a wall").
  This is anti-cheat territory and lives in your gameplay logic
  or a separate validator.
- Side-channel attacks against the audio device or codec
  (libopus CVEs, etc.). The runtime wraps libopus but doesn't
  audit it; keep the underlying library patched.

What you should NOT expect:

- The runtime is not a substitute for transport-layer auth.
- The runtime does not encrypt voice payloads. Voice encryption
  is on the roadmap (Phase 2.1) but not yet shipped. Use a
  transport that encrypts (Steam GameNetworkingSockets, EOS,
  DTLS) until then.
- The runtime does not deduplicate retransmits at the event
  layer (it does for voice packets via sequence number). If
  your network can deliver the same event twice, dedupe before
  submit.

### Stats to monitor in production

Poll these counters from `runtime.GetStats()` and your dashboards
will tell you when something is wrong:

| Counter                                          | What a non-zero value means                              |
|--------------------------------------------------|----------------------------------------------------------|
| `replicationEventsRateLimited[i]`                | Per-category flow-control drops; tune budget if benign   |
| `replicationPolicyViolations`                    | Phase 2.5 caught a Client-source ServerAuthoritative spoof — active wire-layer attack |
| `replicationEventsRejectedByValidator`           | Host's `IReplicationValidator` denied — your custom policy fired |
| `replicationEventsRejectedNewIdBudget`           | New playerIds capped — likely id-cycling DoS attempt     |
| `voiceNetworkStats[playerId].packetsRateLimited` | Per-player voice flooding                                |

A spike in any of these on a running production server is a
signal worth alerting on, not just a counter to log.
