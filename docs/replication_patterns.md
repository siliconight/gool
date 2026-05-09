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
