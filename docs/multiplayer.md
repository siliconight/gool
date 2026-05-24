# Multiplayer integration guide

This is the practical guide for wiring the engine into a real online
co-op or small-team-PvP shooter. It covers four things:

1. **Voice chat that survives real internet connections** — what the
   adaptive jitter buffer gives you, how to drive it correctly from
   your network thread, and how to surface "your voice is dropping"
   to your UI.
2. **Replication of sound events** — how remote gunshots, footsteps,
   and ability triggers reach every client at the right time, with
   three concrete transport patterns (Steam Sockets, ENet, raw UDP).
3. **Replicated emitter transforms** — keeping vehicle engines,
   teammate footsteps, and persistent ambient sources positioned
   correctly under packet loss.
4. **Operational rules of thumb** — what budgets to set, what to log,
   what the failure modes look like.

Predictive playback (firing local audio before server confirmation)
has its own document: see `docs/predictive_playback.md`.
Replay determinism is in `docs/determinism.md`.

---

## 1. Voice chat

### The shape of the data flow

```
network thread                control thread             render thread
━━━━━━━━━━━━━                  ━━━━━━━━━━━━━━              ━━━━━━━━━━━━
OnVoicePacket(...)
    │
    └─ stamp arrival time (steady_clock::now)
    └─ enqueue VoicePacketCopy on SPSC ring
                                  │
                                  └─ drain ring (in Update)
                                  └─ JitterBuffer.Push per source
                                  └─ DecodeAndPush:
                                       PopNext from JitterBuffer:
                                         - real packet → codec.Decode
                                         - PLC signal  → codec.DecodeLost
                                         - empty       → wait
                                       push decoded PCM to per-source ring
                                                                │
                                                                └─ mixer pulls
                                                                   from ring,
                                                                   spatializes,
                                                                   sums
```

The host owns three things: receiving packets from the network,
calling `OnVoicePacket` on each, and reading telemetry to drive UI.
Everything else is internal to the engine.

### What the jitter buffer does for you

A naive voice path that decodes every packet as it arrives produces
unusable audio on real internet connections. Three things go wrong:

- **Jitter** (variable inter-arrival times) means decoded audio comes
  out in bursts and gaps even when no packets are lost.
- **Loss** (3-15% on residential connections under load) means
  silence drops mid-syllable.
- **Reorder** (~5-20% on multi-hop routes) means decoded audio plays
  in the wrong order if you don't sequence it.

The engine's `JitterBuffer` solves all three:

- Adaptive depth tracks observed jitter via the RFC 3550 EWMA. On a
  clean LAN it sits at minimum (3 frames ≈ 60 ms latency). Under
  jittery WiFi it grows up to a configured ceiling (default 10
  frames ≈ 200 ms).
- Lost packets are signaled to the codec as `DecodeLost`; Opus
  generates concealment audio that masks short gaps.
- Reordered packets land in their slot keyed by sequence number;
  consumption is always in order.

The jitter buffer is allocation-free on the hot path. Slots are
sized at construction; pushes and pops are O(1) memcpy plus a
fixed-size scan when the consumer hits a missing slot.

### What you call

```cpp
// Network thread, every received UDP packet:
runtime.OnVoicePacket(playerId, payload, payloadSize,
                       sequenceNumber, sendTimestampMs);

// Game thread, once per UI tick (e.g. 30 Hz):
AudioRuntime::VoiceNetworkStats stats;
if (runtime.GetVoiceNetworkStats(playerId, stats)) {
    if (stats.plcGenerated > prevPlc + 5) {
        ShowSignalWeakIcon(playerId);
    }
    prevPlc = stats.plcGenerated;
}
```

### Performance characteristics

Measured on the engine's `jitter_buffer_test`:

- **2 million Push+Pop pairs in 0.019 s** = ~105M ops/sec on a single
  thread. Real load (8 voice sources × 50 packets/sec aggregate) is
  ~5 orders of magnitude lighter.
- **Under 5% loss / 30 ms jitter** ("residential WiFi"): the buffer
  achieves ~98% audible continuity (real packets + PLC frames /
  total expected frames).
- **Under 10% loss / 50 ms jitter** ("rough internet"): ~97%
  continuity. Target depth grows from 3 to 4-5 frames adaptively.
- **Sequence wraparound** (`uint16` rolls past 65 535): handled by
  signed-delta comparisons. 70 000-packet test runs with zero
  spurious "late" or "lost" classifications.

You will not bottleneck your game on the jitter buffer. If voice chat
ever feels expensive, profile elsewhere first.

### Configuration knobs

`AudioConfig` carries voice-related budgets. Defaults are tuned for
4-8 player shooters; revisit if you have unusual needs.

```cpp
cfg.budget.maxVoiceSources    = 16;        // concurrent remote players
cfg.voicePacketRingDepth      = 32;        // SPSC ring per source
cfg.voicePcmRingFrames        = 9600;      // ~200 ms at 48 kHz
cfg.voiceMaxPacketBytes       = 1500;      // worst-case Opus frame
cfg.voiceJitterTargetMs       = 60;        // initial target depth
```

The jitter buffer's adaptive ceiling is hard-coded at construction
(`maxTargetDepth` defaults to 10 frames ≈ 200 ms). For competitive
PvP where latency matters more than concealment, lower this; for
chat-heavy co-op where dropouts are worse than latency, raise it.

---

## 2. Replicating sound events

When a remote player fires a gun, your network layer needs to tell
every other client. The engine has two entry points for this:

```cpp
// On the network thread, when a remote sound event arrives:
runtime.SubmitReplicatedEvent(audioEvent);

// audioEvent.timestampMs should be the SERVER timestamp the event
// was authoritatively scheduled at (so all clients agree on
// staleness). audioEvent.simulationTick can be set if you have a
// fixed-tick simulation.
```

The replicated event flows through the same drain + handler as local
events. The engine applies its staleness check (per-event override
via `event.maxStalenessMs`, or the global `lateEventDiscardMs`
fallback) and drops events older than the threshold.

### Pattern A: Steam Sockets (`ISteamNetworkingSockets`)

Steam's networking is the easiest path on Steam-distributed games.
You're using `SteamNetworkingMessages` already; audio events ride the
same channel as everything else. Recommended pattern:

```cpp
// Server side: when an authoritative event happens
SerializeAudioEvent(buf, kAudioMsgChannel,
                    {soundId, position, priority,
                     serverTimeMs, simulationTick});
SteamNetworkingMessages()->SendMessageToUser(
    targetIdentity, buf.data(), buf.size(),
    k_nSteamNetworkingSend_Reliable, kAudioMsgChannel);

// Client receive thread:
void OnNetMessage(SteamNetworkingMessage_t* msg) {
    if (msg->m_nChannel != kAudioMsgChannel) return;
    AudioEvent ev = DeserializeAudioEvent(msg->m_pData, msg->m_cbSize);
    runtime.SubmitReplicatedEvent(ev);
    msg->Release();
}
```

Use `Reliable` for events that must not be lost (boss kill stings,
quest progress chimes). Use `Unreliable` for events where loss is
preferable to latency (gunshot impacts, footsteps — if the packet
is gone by the time it would matter, the visual already moved on).

### Pattern B: ENet

ENet's reliable/unreliable distinction maps directly:

```cpp
// Server side
ENetPacket* p = enet_packet_create(buf.data(), buf.size(),
    ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
enet_peer_send(peer, kAudioChannel, p);

// Client receive
void OnEnetReceive(ENetEvent& evt) {
    if (evt.channelID != kAudioChannel) return;
    AudioEvent ev = DeserializeAudioEvent(
        evt.packet->data, evt.packet->dataLength);
    runtime.SubmitReplicatedEvent(ev);
    enet_packet_destroy(evt.packet);
}
```

ENet has a hard 16-channel limit; audio events typically share a
channel with other "fire-and-forget" gameplay events.

### Pattern C: Raw UDP (custom protocol)

If you've rolled your own networking, you handle delivery semantics
yourself. The engine doesn't care about transport. Minimum:

```cpp
// 16 bytes per audio event over the wire is plenty:
struct AudioEventPacket {
    uint8_t  msgType;          // 1 = audio event
    uint8_t  reserved;
    uint16_t soundId;
    int16_t  pos[3];           // millimeter resolution, ~32 m range
    uint8_t  priority;         // map to AudioPriority enum
    uint8_t  flags;            // bit 0 = predicted
    uint32_t serverTimeMs;     // for staleness comparison
};

// On client receive:
void OnUdpDatagram(span<uint8_t> bytes) {
    auto& pkt = *reinterpret_cast<const AudioEventPacket*>(bytes.data());
    if (pkt.msgType != 1) return;
    AudioEvent ev = AudioEvent::MakePlaySoundAtLocation(
        pkt.soundId,
        Vec3{pkt.pos[0]*0.001f, pkt.pos[1]*0.001f, pkt.pos[2]*0.001f});
    ev.priority    = static_cast<AudioPriority>(pkt.priority);
    ev.timestampMs = pkt.serverTimeMs;
    ev.maxStalenessMs = DefaultStalenessMsForCategory(AudioCategory::SFX);
    runtime.SubmitReplicatedEvent(ev);
}
```

### Per-event staleness rules of thumb

| Event type             | Suggested staleness | Notes                                         |
|------------------------|---------------------|-----------------------------------------------|
| Local hit confirm      | 100 ms              | If late, the player already saw the hitmark   |
| Remote gunshot         | 200 ms              | After this, the visual has moved on           |
| Footstep               | 150 ms              | Tactical info; stale data is misleading       |
| Death cry              | 500 ms              | Non-tactical; play it even if delayed         |
| Music transition       | 5 s                 | Nobody notices a 4-second-late zone change    |
| Voice chat             | use jitter buffer   | Don't apply event-level staleness             |
| UI sound (menu)        | 0 (never stale)     | Always play; user-driven                      |

Use `DefaultStalenessMsForCategory(AudioCategory::SFX)` etc. if you
don't want to think about it per event.

### Avoiding replication for emitters out of range

Don't send replicated events to clients who can't hear them. Even
with the engine's interest management cap, you save bandwidth and
encoding time by gating server-side:

```cpp
// Server-side filter before sending to a client:
const float audibleRangeSq = 100.0f * 100.0f;     // 100 m
const float dx = ev.position.x - clientListenerPos.x;
const float dy = ev.position.y - clientListenerPos.y;
const float dz = ev.position.z - clientListenerPos.z;
if (dx*dx + dy*dy + dz*dz > audibleRangeSq) return;     // skip
```

For battle royales with 60+ players, this is the difference between
shipping and not shipping.

---

## 3. Replicated emitter transforms

Persistent emitters that move (vehicle engines, teammate footstep
loops, helicopter rotor) need their transforms replicated so clients
hear them at the right position. The engine has a dedicated entry
point that interpolates correctly under packet loss:

```cpp
// Network thread, when a remote transform update arrives:
runtime.UpdateReplicatedTransform(emitterHandle,
                                    position,
                                    forward,
                                    velocity,
                                    sourceTick);
```

The engine maintains a 2-tick history per emitter and interpolates
linearly between the most-recent two samples with one tick of
intentional lag. If updates stop arriving (packet loss), velocity-based
extrapolation handles the trailing edge.

### How often to send

For a 30 Hz simulation tick, sending transform updates at 10 Hz is
enough for human-perceivable position smoothness. The engine's
linear interpolation hides higher-frequency jitter; you don't gain
audio fidelity by sending faster.

```cpp
if (currentTick % 3 == 0) {        // every 3rd tick = 10 Hz
    for (auto& e : persistentEmitters) {
        SendTransformUpdate(e);
    }
}
```

### Interest management cap

For more emitters than you can afford to spatialize each tick, set:

```cpp
cfg.budget.maxActiveEmittersProcessedPerTick = 32;     // top-N by distance
```

The engine sorts active emitters by distance to the listener every
tick (via `std::nth_element`, O(N) average) and runs the spatializer
only on the closest N. Emitters outside the budget get a zero-gain
mute the tick they fall out, and stay silent until they re-enter.
See `docs/multiplayer.md` and the `production_readiness_test` for
verification.

For a 60-player battle royale at 32 active-emitters cap, the
spatializer cost is bounded regardless of player count.

---

## 4. Operational rules of thumb

### Budgets that work for typical multiplayer

For a 4-8 player co-op shooter:

```cpp
cfg.budget.maxActiveEmitters                  = 64;
cfg.budget.maxActiveEmittersProcessedPerTick  = 0;        // unlimited
cfg.budget.maxVoiceSources                    = 8;
cfg.budget.maxOcclusionChecksPerFrame         = 16;
cfg.budget.maxStreamingVoices                 = 4;
cfg.budget.maxRegisteredSounds                = 256;
cfg.budget.maxGameEventsPerFrame              = 256;
cfg.budget.maxNetworkEventsPerFrame           = 256;
```

For a 60-player battle royale:

```cpp
cfg.budget.maxActiveEmitters                  = 256;      // many emitters
cfg.budget.maxActiveEmittersProcessedPerTick  = 32;       // bounded work
cfg.budget.maxVoiceSources                    = 8;        // squad-only voice
cfg.budget.maxOcclusionChecksPerFrame         = 24;
cfg.budget.maxStreamingVoices                 = 6;
cfg.budget.maxNetworkEventsPerFrame           = 512;      // high inflow
```

For a 5v5 competitive PvP:

```cpp
cfg.budget.maxActiveEmitters                  = 128;
cfg.budget.maxActiveEmittersProcessedPerTick  = 0;
cfg.budget.maxVoiceSources                    = 10;       // both teams
cfg.budget.maxOcclusionChecksPerFrame         = 16;
// Tighter staleness: competitive players notice late audio
cfg.lateEventDiscardMs                        = 150;
```

### Telemetry to log every match

Surface these to your match-end stats screen and to your analytics:

| Field                                | What it tells you                              |
|--------------------------------------|------------------------------------------------|
| `Stats.oneShotsDroppedFullPool`      | Pool too small for the gameplay                |
| `Stats.lateEventsDiscardedLastTick`  | Network-induced staleness drops                |
| `Stats.predictionsCancelled`         | Mispredict rate; tune your prediction model    |
| `Stats.emittersSkippedByInterestLastTick` | Interest-management active                |
| Per-player `VoiceNetworkStats.plcGenerated` | Voice quality per connection           |
| Per-player `VoiceNetworkStats.observedJitterMs` | Network condition per player        |

If `oneShotsDroppedFullPool` is non-zero in normal play, raise
`maxActiveEmitters`. If `lateEventsDiscardedLastTick` spikes, your
network layer has a bottleneck (or the host clock skewed). If a
specific player's `plcGenerated` is much higher than others', their
connection is rough — show them an indicator.

### Failure modes to test in QA

Before shipping, verify each of these:

1. **One player's connection drops mid-match.** Their voice goes
   silent (no PLC for them after disconnect) but the other players'
   audio is unaffected.
2. **Network thread temporarily blocks for 200 ms.** No render
   underrun; mixer keeps producing silence (or last samples) on the
   stalled voice; recovers when packets resume.
3. **A player's clock is 30 seconds ahead.** Their stamped events
   are not "future" enough to break staleness comparisons (the
   engine treats `nowMs <= eventMs` as "not late").
4. **High packet loss burst (200 ms outage on one client).** PLC
   covers it; no clicks; observed jitter stays bounded.
5. **Player disconnects and reconnects within 5 seconds.** Voice
   source is unregistered then re-registered cleanly; new
   sequence numbers don't trip "late" filters.

---

## See also

- `docs/predictive_playback.md` — client-side prediction with
  `CancelPredictedEvent`
- `docs/determinism.md` — replay and spectator mode
- `examples/cpp/multi_tier_ducking/` — L4D2-style "your shot ducks
  music + remote shots" pattern
- `tests/unit/jitter_buffer_test.cpp` — exact numbers under
  simulated loss/jitter/reorder
- `tests/unit/multiplayer_readiness_test.cpp` — staleness override,
  CancelPredictedEvent, interest management
