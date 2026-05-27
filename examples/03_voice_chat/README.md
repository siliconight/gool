# gool voice chat — loopback integration demo

A single-process Godot demo of the voice chat integration
pipeline. **The encoder is stubbed**, so no audible voice plays
back — the point is to show the wiring so you can swap in a real
Opus encoder for production.

## What this demonstrates

```
AudioStreamMicrophone + AudioEffectCapture
                   ↓ raw PCM frames (44.1 kHz)
        scripts/voice_capture.gd
            _encode_pcm_to_opus()  ←─ STUB — wire Opus here
                   ↓
            _dispatch_packet()  ←─── STUB — wire RPC here
                   ↓
        scripts/voice_loopback_receiver.gd
            (or in production: VoiceChatPlayer prefab)
                   ↓
        Gool.submit_voice_packet(player_id, bytes,
                                  sequence_number,
                                  send_timestamp_ms,
                                  arrival_timestamp_ms)
                   ↓
        gool engine decodes via internal Opus
            + jitter buffer + packet-loss interpolation
                   ↓
        Plays through the Voice bus
            (intelligibility priority — never ducked)
```

The plumbing is real: mic capture works, packets get sequence
numbers and timestamps, the runtime accepts and tracks them
(jitter and packet-loss metrics update). Only the audio encoding
+ decoding is stubbed.

## Run it

1. Open this project in Godot 4.2+
2. Make sure mic input is enabled in your OS audio settings
3. Press F5 — the demo grants itself mic permission via
   `OS.request_permissions()` on platforms that gate it
4. The on-screen label shows:
   - Packets sent per second (should be ~50 pps at 20 ms frames)
   - Current jitter (ms) and packet loss (%) for the loopback peer
5. **You will not hear your voice played back** — that requires
   a real encoder. The numbers update because gool is processing
   the packet stream regardless.

## Production checklist

To make this demo actually produce audible voice chat, swap the
three stubs for real implementations:

### 1. Real Opus encoder

`scripts/voice_capture.gd::_encode_pcm_to_opus()` returns raw
floats (~880 bytes per 20 ms frame). Real Opus output is 60-100
bytes per frame at typical bitrates. Options:

- **Steamworks voice** — if you're shipping on Steam, Steam Audio
  handles capture + encoding + transport in one go. You get
  Opus-encoded bytes back from Steam APIs; feed them straight
  into `Gool.submit_voice_packet`. **This is the recommended path
  for Steam shipments** — it solves networking + encoding +
  echo cancellation + push-to-talk in one shot.
- **godot-opus** addon — community Opus binding for GDScript.
  Wire `_encode_pcm_to_opus()` to its encoder API.
- **Custom GDExtension** — wrap libopus directly via the Godot
  GDExtension API. More work but full control.

### 2. Real network transport

`scripts/voice_capture.gd::_dispatch_packet()` just emits a signal
that the loopback receiver listens to. For multi-process voice
chat, replace it with one of:

- **Godot MultiplayerAPI** — use the shipping `VoiceChatPlayer`
  prefab (`res://addons/gool/prefabs/voice_chat_player.gd`).
  Spawn one per peer; the prefab handles the RPC plumbing. Your
  capture code calls an `@rpc("call_remote", "unreliable")` method
  on the receiver's VoiceChatPlayer, which forwards to gool.
- **Steam P2P** — use `SteamMultiplayerPeer.put_packet()` on
  Steam's voice channel. Same shape as MultiplayerAPI from gool's
  perspective.
- **Custom UDP** — emit packets via your custom protocol's voice
  channel. Set `MultiplayerBridge.transport_mode = CUSTOM`
  and connect to its signals.

### 3. Replace the loopback receiver with VoiceChatPlayer

In production, you don't use `voice_loopback_receiver.gd` —
that's an in-process simulator. Spawn the shipping `VoiceChatPlayer`
prefab per peer:

```gdscript
# When a peer joins
var vcp := preload("res://addons/gool/prefabs/voice_chat_player.tscn").instantiate()
vcp.player_id = peer_id
$VoiceChatPlayers.add_child(vcp)
```

The prefab handles registration, packet receive via RPC, and
quality monitoring (jitter + loss signals fire when degraded).

## Audio bus routing

The example uses `config_fps.json` from
`addons/gool/templates/`. Voice routes to the dedicated **Voice**
bus — note that:

- Voice is a **sibling of Sfx and Music**, parented to Master
- Voice is **never sidechained** by anything — intelligibility
  priority, voice doesn't duck under gunfire or music
- There's a **MicCapture** bus added programmatically by
  `voice_capture.gd` for capturing the local mic — this is
  separate from the Voice bus (which carries received remote
  voice) so you can mute the mic locally without affecting
  remote audio

## What the runtime gives you

`Gool.submit_voice_packet(player_id, bytes, sequence_number,
send_timestamp_ms, arrival_timestamp_ms)`

- **Jitter buffer** — out-of-order packets are reordered, late
  packets are buffered. Get current jitter (ms) via
  `Gool.get_voice_jitter_ms(player_id)`.
- **Packet-loss tracking** — missing sequence numbers are
  detected and counted. Loss ratio (0..1) via
  `Gool.get_voice_packet_loss_ratio(player_id)`.
- **3D positioning** — if your `VoiceChatPlayer` is a Node3D
  attached to the speaker's avatar, voice gets spatialized.

## Related docs

- `docs/quickstart_fps.md` — the FPS recipe; Step 11 covers voice
  chat scaffolding at a higher level.
- `docs/networking_bridge.md` — the bridge's transport-agnostic
  voice routing.
- `addons/gool/prefabs/voice_chat_player.gd` — the production
  receive-side prefab.

## Known limitations

- The stub encoder produces garbage bytes that gool's Opus
  decoder will reject silently. Packet counts and jitter
  measurements still work (those operate on packet metadata, not
  audio content).
- Mic permission on Linux requires PipeWire or PulseAudio access.
  On macOS, the system prompt for mic permission appears on first
  capture attempt. On Windows, no prompt is needed but mic must
  be enabled in Settings → Privacy → Microphone.
- If your `AudioStreamMicrophone` produces silence, check the
  Godot project settings: `audio/driver/enable_input` must be
  true (set in this example's `project.godot`).
