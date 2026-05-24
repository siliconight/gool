extends Node

# voice_capture.gd
#
# Mic capture + packet dispatch side of the voice chat pipeline.
# Demonstrates the structure an FPS dev needs to wire up to feed
# voice packets into gool. Producing packets that gool can actually
# DECODE requires a real Opus encoder — this example ships with a
# pass-through stub so you can see the wiring without hearing
# decoded voice playback.
#
# Pipeline:
#
#   AudioStreamMicrophone + AudioEffectCapture
#                      ↓ raw PCM frames (Vector2 samples at 44.1 kHz)
#                  encode_step()  ←─── YOU WIRE OPUS HERE
#                      ↓ encoded bytes
#                  packet_send()  ←─── YOU WIRE NETWORK HERE
#                      ↓ over the wire
#                  packet_recv()  (on the receiving peer)
#                      ↓
#                  Gool.submit_voice_packet(player_id, bytes,
#                                            sequence_number,
#                                            send_timestamp_ms)
#                      ↓
#               gool engine decodes + plays through the Voice bus
#
# This script handles the LEFT half (capture + encode + send).
# The VoiceChatPlayer prefab (gool ships it) handles the RIGHT
# half (RPC receive + submit_voice_packet).
#
# Production checklist (what you swap for real shipments):
#   1. Replace _encode_pcm_to_opus() with a real Opus encoder.
#      Options: godot-opus addon, Steamworks voice (auto-encoded),
#      or a GDExtension wrapping libopus directly.
#   2. Replace _dispatch_packet() with your network layer. For
#      Godot's MultiplayerAPI, call an @rpc("any_peer", "call_remote")
#      method on a VoiceChatPlayer node. For Steam P2P, call
#      SteamMultiplayerPeer.put_packet().
#   3. Tune CAPTURE_INTERVAL_MS to match your encoder's frame
#      duration (Opus typically uses 20 ms frames; this example
#      uses 20 ms by default).

const SAMPLE_RATE: int = 44100
const CAPTURE_INTERVAL_MS: int = 20      # Opus default frame duration
const LOOPBACK_PLAYER_ID: int = 9999     # Pseudo player_id for the loopback demo

@export var enabled: bool = true
@export var capture_bus_name: String = "MicCapture"

var _capture_effect: AudioEffectCapture = null
var _sequence_number: int = 0
var _accum_ms: float = 0.0
var _bus_idx: int = -1

# Signal: emitted whenever a packet has been "transmitted" (in this
# loopback demo, that means handed off to the receive side).
# A real network layer would emit nothing — its work is the RPC.
signal packet_dispatched(player_id: int, bytes: PackedByteArray, seq: int, ts_ms: int)

func _ready() -> void:
	if not enabled:
		return
	# Set up an editor-side audio bus to host the AudioEffectCapture.
	# In a real project you'd create this bus once in your audio bus
	# layout (.tres) and route AudioStreamMicrophone to it. Here we
	# build it programmatically so the example is self-contained.
	_ensure_mic_capture_bus()
	# Spawn a microphone stream playing through the capture bus.
	# AudioStreamMicrophone needs an AudioStreamPlayer node;
	# create one as a child.
	var mic_player := AudioStreamPlayer.new()
	mic_player.name = "MicPlayer"
	mic_player.stream = AudioStreamMicrophone.new()
	mic_player.bus = capture_bus_name
	mic_player.autoplay = true
	add_child(mic_player)

func _process(delta: float) -> void:
	if not enabled or _capture_effect == null:
		return
	_accum_ms += delta * 1000.0
	if _accum_ms < float(CAPTURE_INTERVAL_MS):
		return
	_accum_ms = 0.0

	# Capture exactly one frame's worth of PCM. AudioEffectCapture
	# returns interleaved L/R as Vector2 samples (44.1 kHz).
	var frames_per_packet: int = int(SAMPLE_RATE * CAPTURE_INTERVAL_MS / 1000)
	if _capture_effect.get_frames_available() < frames_per_packet:
		return  # Not enough mic data yet; try again next process.
	var pcm: PackedVector2Array = _capture_effect.get_buffer(frames_per_packet)
	if pcm.is_empty():
		return

	# Step 1: Encode the PCM. *** YOU WIRE OPUS HERE *** — this stub
	# just serializes the floats as bytes so the example runs end
	# to end. Real packets are 60-100 bytes (Opus); this stub
	# produces ~880 bytes per frame, which would saturate any real
	# network. Loopback demo only.
	var encoded_bytes: PackedByteArray = _encode_pcm_to_opus(pcm)

	# Step 2: Send the packet. *** YOU WIRE NETWORK HERE *** —
	# this stub just emits a signal that the loopback receiver
	# listens to. Real code would call an @rpc method.
	var send_ts_ms: int = Time.get_ticks_msec()
	_dispatch_packet(LOOPBACK_PLAYER_ID, encoded_bytes,
			_sequence_number, send_ts_ms)
	_sequence_number += 1

# *** STUB *** — replace with a real Opus encoder.
# Real encoders return ~60-100 bytes for 20ms of audio.
# This pass-through returns raw float bytes for demo purposes;
# gool's voice decoder will see garbage and produce no audible
# output, but the SEQUENCE_NUMBER / TIMESTAMP plumbing works.
func _encode_pcm_to_opus(pcm: PackedVector2Array) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(pcm.size() * 8)  # 2 floats × 4 bytes per sample
	# In a real encoder, you'd feed pcm through libopus and get
	# back a much smaller compressed buffer here.
	return bytes

# *** STUB *** — replace with your network transport.
# For Godot's MultiplayerAPI: emit an @rpc("call_remote", "unreliable")
# method on a VoiceChatPlayer node. For Steam P2P: put_packet to
# Steam's voice channel. The signal here just lets the loopback
# receiver in the same scene see the "transmitted" packets.
func _dispatch_packet(player_id: int, bytes: PackedByteArray,
		seq: int, send_ts_ms: int) -> void:
	packet_dispatched.emit(player_id, bytes, seq, send_ts_ms)

# Build (or reuse) the AudioEffectCapture-equipped bus that the
# AudioStreamMicrophone routes through. Idempotent — checks for
# the bus by name before creating it.
func _ensure_mic_capture_bus() -> void:
	_bus_idx = AudioServer.get_bus_index(capture_bus_name)
	if _bus_idx == -1:
		# Append a new bus at the end.
		var new_idx := AudioServer.bus_count
		AudioServer.add_bus(new_idx)
		AudioServer.set_bus_name(new_idx, capture_bus_name)
		# Route to Master so mic signal can also be monitored locally
		# if needed; mute by default so we don't echo the mic into
		# the speakers and create a feedback loop.
		AudioServer.set_bus_mute(new_idx, true)
		_bus_idx = new_idx
	# Ensure there's an AudioEffectCapture on slot 0 of the bus.
	_capture_effect = null
	for i in range(AudioServer.get_bus_effect_count(_bus_idx)):
		var fx := AudioServer.get_bus_effect(_bus_idx, i)
		if fx is AudioEffectCapture:
			_capture_effect = fx
			break
	if _capture_effect == null:
		_capture_effect = AudioEffectCapture.new()
		AudioServer.add_bus_effect(_bus_idx, _capture_effect, 0)
