# Predictive playback guide

How to use predicted local audio events with server reconciliation,
without producing weird audio when the server rejects a prediction.

## Why predict at all

Network round-trip time is the enemy of action games. A 50 ms ping
means every input takes 100 ms to be confirmed. If the gunshot sound
waits for that confirmation, every shot feels heavy and laggy.

The standard fix is **client-side prediction**: when the local player
fires, play the gunshot immediately at the predicted position, and
let the server confirmation arrive later. If the server agrees, the
prediction was correct — the local audio was on time. If the server
rejects (out of ammo, weapon swap mid-press, anti-cheat
disagreement), the prediction must be unwound.

The audio engine supports this via two pieces:

1. **`AudioEvent::predictionId`** — a host-stamped id on the
   predicted event. Non-zero means "this event is provisional;
   tag the resulting voice with this id so I can find it later."
2. **`AudioRuntime::CancelPredictedEvent(predictionId, fadeOutMs)`** —
   fades the predicted voice down over `fadeOutMs` if it's still
   alive. Call from the network thread when reconciliation rejects.

The engine handles the rest: storing the id on the emitter,
matching it on cancel, posting a faded `Stop` through the same
plumbing eviction uses, gracefully no-op'ing if the voice already
finished naturally.

## When to predict

Predict events that meet **all three** criteria:

- **The local player is the cause.** Their own weapon, footstep,
  ability use. Other players' actions cannot be predicted; play
  those when they arrive.
- **The action has perceptual immediacy.** Things the player is
  doing right now and expects to hear. A weapon swap that takes
  500 ms of wind-up doesn't need prediction — the wind-up animation
  is already buffering enough latency that confirmation arrives
  before the firing instant.
- **Misprediction is rare and recoverable.** If the server rejects
  this action 5% of the time, predicting it is fine; you'll occasionally
  fade out a phantom shot. If the server rejects 30% of the time
  (say, because anti-cheat is flagging your prediction model), don't
  predict — the constant fade-outs are worse than the latency.

Don't predict events caused by other players. Don't predict events
where the local player has no perceptual expectation (a music
transition triggered by a server-side event). Don't predict
state-based emitters (vehicle engine sounds) — those are replicated
state, not events.

## When to wait for the server

Default to waiting if any of these apply:

- The event's source isn't the local player.
- The latency cost of waiting is acceptable (e.g. < 30 ms ping; or
  the event is non-critical).
- The action's outcome is server-authoritative and historically
  rejected often (rare items, contested resources, anti-cheat
  surface).
- The event has a long tail (music, dialogue) where a fade-out on
  reject would be more disruptive than just waiting.

## The predict-then-reconcile pattern

```cpp
// Game thread, on player input:
uint64_t predictionId = nextPredictionId++;

audio::AudioEvent ev = audio::AudioEvent::MakePlaySoundAtLocation(
    kSndPlayerShot,
    localPlayer.position);
ev.priority       = audio::AudioPriority::High;
ev.predictionId   = predictionId;
ev.maxStalenessMs = 0;              // local; staleness doesn't apply
runtime.SubmitEvent(ev);

// Send the input to the server.
network.SendInput({
    .type         = InputType::Fire,
    .timestampMs  = currentLocalTimeMs,
    .predictionId = predictionId,
});
```

```cpp
// Network thread, when server confirmation arrives:
void OnServerInputResponse(const InputResponse& resp) {
    if (!resp.accepted) {
        // Prediction was wrong. Fade out the local audio.
        runtime.CancelPredictedEvent(resp.predictionId,
                                       /*fadeOutMs=*/ 50.0f);
    }
    // If accepted, do nothing — the predicted audio is already
    // playing correctly and will retire naturally.
}
```

## Tuning the fade

`CancelPredictedEvent` takes a `fadeOutMs` in milliseconds. The right
value depends on what you're cancelling:

| Sound type           | Suggested fade | Why                                         |
|----------------------|----------------|---------------------------------------------|
| Hit-confirm beep     | 5-10 ms        | Short anyway; rip the bandaid               |
| Weapon firing        | 30-50 ms       | Default; cleanly suppresses the tail        |
| Explosion / impact   | 80-120 ms      | Long natural decay; abrupt cut would clack  |
| Continuous (laser)   | 100-150 ms     | Smooth retraction more important than speed |

The shorter the fade, the more "instant correction" it sounds. The
longer, the smoother but the longer the audio plays after it
shouldn't have. 50 ms is the sweet spot for action SFX.

## Recovery patterns

### Hit-confirm replacement

Player fires; predicted gunshot plays; server confirms but reports a
different hit result (e.g. headshot vs body shot). Don't cancel the
gunshot — that part was right. Submit the corrected hit-confirm
sound separately, the gunshot continues.

### Action substitution

Player fires; server says "you actually melee'd." Cancel the
predicted gunshot with a fast fade (10-20 ms), then submit the
server-authoritative melee sound. The result is a brief muddy moment
where both sounds overlap during the fade — usually less noticeable
than a sudden cut.

### Reload mid-fire

Player fires; server says "you were already reloading." Cancel the
gunshot fast (5-10 ms); the reload sound was likely already playing.
This is common for reload-cancel inputs and the audio designer
should test it specifically.

### Multi-shot bursts

Burst-fire weapons (3-round burst) can submit three predicted events
with sequential ids. If the server rejects mid-burst (e.g. last
round caused weapon overheat), cancel the rejected events
individually. The accepted ones play through unchanged.

## Telemetry

Stats counters surface what's happening:

```cpp
const auto stats = runtime.GetStats();
stats.predictionsCancelled;          // cancellations that found a voice
stats.predictionsCancelledNotFound;  // already retired / never started
```

If `predictionsCancelledNotFound` dominates, your fade is firing too
late — the voice is finishing naturally before the server response
arrives. Either increase the fade window for those events, or
shorten the events themselves so they don't outlast typical server
response time.

If `predictionsCancelled` dominates and you hear a lot of fade-outs
in normal play, your prediction model is too optimistic — the server
is rejecting more than it should. Inspect prediction inputs for
mismatches.

## What this isn't

The engine's prediction support is for **sound suppression on
mispredict**, not for **time-rewinding the mix**. If your game does
full rollback netcode (the player visually rewinds 100 ms when
mispredicted), the audio engine doesn't rewind — it keeps playing
forward, cancelling individual sounds. This is the right behavior
for real-time audio (rewinding the audio mix would itself sound
worse than what you're trying to fix), but it's worth knowing
explicitly.

For replays of completed matches, use the determinism mode
described in `determinism.md` instead. Replay is a different
problem from live-play prediction.
