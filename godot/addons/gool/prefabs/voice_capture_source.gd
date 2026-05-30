# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

# addons/gool/prefabs/voice_capture_source.gd
#
## Voice capture front-end with push-to-talk and voice-activity-
## detection gating. Closes roadmap item 2.2.
#
## WHY THIS EXISTS
## ===============
#
## gool's C++ voice path is receive-side only: `submit_voice_packet`,
## `register_voice_source`, the jitter buffer, packet-loss interpolation,
## decode, route through the Voice bus, attenuate, output. The CAPTURE
## side — pulling PCM out of the OS microphone, gating it by user
## intent, encoding to Opus, dispatching over the network — is the
## host's responsibility. gool doesn't own the mic.
#
## This prefab is the recommended capture front-end. It wraps Godot's
## `AudioStreamMicrophone` + `AudioEffectCapture`, applies the gating
## policy (ALWAYS_ON, PUSH_TO_TALK, or VOICE_ACTIVITY), and emits PCM
## frames via a signal ONLY when transmit should happen. Consumers
## connect their encoder + dispatcher to that signal:
#
##     $VoiceCaptureSource.pcm_frame_ready.connect(_on_pcm_ready)
##
##     func _on_pcm_ready(frame: PackedFloat32Array) -> void:
##         var opus_bytes := _encode_opus(frame)
##         _dispatch_to_peers(opus_bytes)
#
## When transmit is gated (PTT released, VAD says no voice), the
## signal doesn't fire — the encoder never runs, no bytes hit the
## network. Bandwidth drops to zero for the gated window.
#
## THE THREE MODES
## ===============
#
##   ALWAYS_ON
##       Every PCM frame is emitted. Legacy "open mic" behavior.
##       Highest bandwidth, simplest, useful for loopback testing or
##       cooperative-team settings where the player count is small
##       and trust is high. Default for raw voice loopback demos.
##
##   PUSH_TO_TALK
##       Frame emitted only while the configured InputMap action is
##       held (default action name: "voice_ptt"). The action must be
##       defined in Project Settings → Input Map. If it isn't, the
##       prefab emits a warning at startup and gates everything
##       silently.
##
##       For scripted "force radio on" use cases that bypass the
##       InputMap (cutscenes, emergency channels), call
##       `begin_transmit()` / `end_transmit()` directly. These
##       take precedence over the InputMap state.
##
##   VOICE_ACTIVITY
##       RMS energy detection with a hangover window. When the
##       current PCM frame's RMS exceeds `vad_energy_threshold`,
##       transmit turns ON immediately. When RMS drops below
##       threshold, transmit stays ON for `vad_hangover_ms`
##       milliseconds (default 200ms) so word tails and inter-word
##       gaps don't clip. After the hangover expires with no voice,
##       transmit turns OFF.
##
##       No ML, no spectral analysis — energy-threshold VAD is
##       sufficient for the indie multiplayer use case where
##       background noise is reasonably consistent and the goal is
##       "stop transmitting when no one's talking," not
##       "distinguish speech from music."
##
## DIAGNOSTICS
## ===========
#
## Two counters are exposed for diagnostic / debug-overlay use:
#
##   - `frames_transmitted` — frames that made it through the gate
##   - `frames_gated`       — frames suppressed (PTT released or VAD off)
#
## Plus the `transmit_state_changed(transmitting: bool)` signal,
## useful for driving a mic-on/mic-off HUD indicator.
#
## USAGE
## =====
#
##   1. Add a VoiceCaptureSource to your local player's scene:
#
##         Player (Node3D)
##         ├── VoiceCaptureSource     # this prefab
##         └── ...
#
##   2. Set `capture_mode` in the inspector. For most games:
##      `VOICE_ACTIVITY` (no input required, auto-gates silence).
##      For competitive / streamer settings: `PUSH_TO_TALK`.
##
##   3. For PUSH_TO_TALK mode, add an InputMap action named
##      `voice_ptt` (or whatever you set `push_to_talk_action` to)
##      in Project Settings → Input Map. Bind it to a key the user
##      can hold (default convention: V or F).
##
##   4. Connect `pcm_frame_ready` to your encode-and-dispatch code:
#
##         func _ready() -> void:
##             $VoiceCaptureSource.pcm_frame_ready.connect(_on_voice_frame)
##
##         func _on_voice_frame(frame: PackedFloat32Array) -> void:
##             # frame is mono float32 PCM at sample_rate; length is
##             # pcm_frames_per_packet (default 960 = 20ms @ 48kHz,
##             # matching Opus's 20ms frame size)
##             var encoded := _opus_encoder.encode(frame)
##             _network.send_voice_packet.rpc(encoded)
#
##   5. (Optional) Drive a mic-on/mic-off indicator from
##      `transmit_state_changed`.

@tool
class_name VoiceCaptureSource
extends Node


# ---------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------

enum CaptureMode {
	ALWAYS_ON       = 0,
	PUSH_TO_TALK    = 1,
	VOICE_ACTIVITY  = 2,
}


# ---------------------------------------------------------------------
# Exports
# ---------------------------------------------------------------------

## Capture-gating policy. VOICE_ACTIVITY is the recommended default
## for most multiplayer games — no input binding required, silence
## auto-gates. Switch to PUSH_TO_TALK for competitive / streamer
## settings where players want explicit control.
@export var capture_mode: CaptureMode = CaptureMode.VOICE_ACTIVITY

## Master enable. When false, no frames are emitted regardless of
## mode. Use this to globally mute the player's mic without changing
## their preferred capture_mode.
@export var enabled: bool = true

## InputMap action name for PUSH_TO_TALK mode. Must be defined in
## Project Settings → Input Map. If undefined, the prefab warns at
## startup and gates everything silently.
@export var push_to_talk_action: StringName = &"voice_ptt"

## RMS energy threshold for VOICE_ACTIVITY mode. Frames with RMS
## above this transmit. Tune for your microphone and ambient noise:
##   - 0.005 = very sensitive (catches whispers, but also keyboard
##     clicks)
##   - 0.01  = default, balanced for typical headset + quiet room
##   - 0.03  = conservative, requires clear speech
##   - 0.10  = only loud / close-mic speech
##
## If you're not sure, leave the default and adjust based on the
## `frames_transmitted` / `frames_gated` ratio while running.
@export_range(0.0001, 1.0, 0.0001) var vad_energy_threshold: float = 0.01

## How long VOICE_ACTIVITY mode keeps transmitting after the last
## above-threshold frame. Prevents clipping word tails and short
## inter-word gaps. 200ms is the standard value — long enough for
## natural pauses, short enough that the gate closes promptly when
## the speaker actually finishes.
@export_range(50.0, 2000.0, 10.0, "suffix:ms") var vad_hangover_ms: float = 200.0

## PCM frames per emitted packet. 960 = 20ms at 48kHz, which matches
## Opus's standard frame size. Change only if your encoder expects a
## different frame length (e.g. 480 = 10ms, 1920 = 40ms).
@export var pcm_frames_per_packet: int = 960

## Sample rate for the capture bus. 48000 is the universal default
## and what Opus expects. Lower (24000, 16000) saves bandwidth but
## degrades quality.
@export var sample_rate: int = 48000

## Length of the AudioEffectCapture ring buffer, in seconds. The
## process loop polls this buffer; long enough to absorb frame-rate
## hiccups, short enough that latency stays low. 0.1s = 100ms is
## the typical value.
@export_range(0.05, 1.0, 0.01, "suffix:s") var capture_buffer_s: float = 0.1


# ---------------------------------------------------------------------
# Signals
# ---------------------------------------------------------------------

## Emitted for every PCM frame that passes the gate. `frame` is
## mono float32 at `sample_rate`, length `pcm_frames_per_packet`.
## Connect to your encode-and-dispatch code.
signal pcm_frame_ready(frame: PackedFloat32Array)

## Fires when the transmit state changes (off → on or on → off).
## Useful for driving a mic-on/mic-off HUD indicator. Doesn't fire
## on every frame, only on the transitions.
signal transmit_state_changed(transmitting: bool)


# ---------------------------------------------------------------------
# Diagnostic counters (read-only, public)
# ---------------------------------------------------------------------

var frames_transmitted: int = 0
var frames_gated: int = 0


# ---------------------------------------------------------------------
# Internal state
# ---------------------------------------------------------------------

# Godot capture pipeline.
var _stream_player: AudioStreamPlayer = null
var _capture: AudioEffectCapture = null
var _audio_bus_idx: int = -1
const _CAPTURE_BUS_NAME := "GoolVoiceCapture"

# VAD state.
var _vad_last_voice_time_ms: int = -1

# Transmit-state edge detection (for signal emission).
var _was_transmitting: bool = false

# Programmatic override: +1 = force-on, -1 = force-off, 0 = use mode.
# Set by begin_transmit() / end_transmit() / release_override().
# Takes precedence over capture_mode logic — useful for cinematic
# auto-radio moments where you want voice on regardless of player
# intent.
var _force_transmit_override: int = 0

# One-time warning gate so we don't spam the console every frame
# when PTT is set but the action isn't defined.
var _warned_missing_ptt_action: bool = false


# ---------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_setup_capture()


func _process(_delta: float) -> void:
	if Engine.is_editor_hint() or not enabled or _capture == null:
		return

	# Drain whatever's available in the capture ring. We may pull
	# multiple frames per process tick if the game's framerate is
	# lower than the audio rate.
	while _capture.get_frames_available() >= pcm_frames_per_packet:
		var stereo: PackedVector2Array = _capture.get_buffer(pcm_frames_per_packet)
		var mono: PackedFloat32Array = _stereo_to_mono(stereo)
		var should_transmit: bool = _should_transmit(mono)
		if should_transmit:
			frames_transmitted += 1
			pcm_frame_ready.emit(mono)
		else:
			frames_gated += 1
		_update_transmit_state(should_transmit)


func _exit_tree() -> void:
	if _stream_player != null and is_instance_valid(_stream_player):
		_stream_player.stop()
	if _audio_bus_idx >= 0 and _audio_bus_idx < AudioServer.bus_count:
		# Defensive: only remove if no other consumer is on this bus.
		# AudioServer.remove_bus shifts indices, so re-check.
		var current_name := AudioServer.get_bus_name(_audio_bus_idx)
		if current_name == _CAPTURE_BUS_NAME:
			AudioServer.remove_bus(_audio_bus_idx)
		_audio_bus_idx = -1


# ---------------------------------------------------------------------
# Public API — programmatic transmit control
# ---------------------------------------------------------------------

## Force transmit ON regardless of capture_mode or PTT state. Useful
## for scripted dialog, emergency-radio events, or testing. Stays
## on until `end_transmit()` or `release_override()` is called.
func begin_transmit() -> void:
	_force_transmit_override = 1


## Force transmit OFF regardless of capture_mode or PTT state.
## Useful for "shut up, the boss is talking" scripted moments. Stays
## off until `begin_transmit()` or `release_override()` is called.
func end_transmit() -> void:
	_force_transmit_override = -1


## Restore normal capture_mode-driven behavior after a
## begin_transmit() or end_transmit() call. After this, PTT input
## and VAD energy work normally again.
func release_override() -> void:
	_force_transmit_override = 0


## Reset the diagnostic counters to zero. Useful between test runs
## or when wrapping the prefab in an automated harness.
func reset_counters() -> void:
	frames_transmitted = 0
	frames_gated = 0


# ---------------------------------------------------------------------
# Internals — gating logic
# ---------------------------------------------------------------------

func _should_transmit(mono: PackedFloat32Array) -> bool:
	# Programmatic override takes precedence over any mode.
	if _force_transmit_override > 0:
		return true
	if _force_transmit_override < 0:
		return false

	match capture_mode:
		CaptureMode.ALWAYS_ON:
			return true
		CaptureMode.PUSH_TO_TALK:
			return _is_ptt_held()
		CaptureMode.VOICE_ACTIVITY:
			return _vad_decide(mono)
	# Unknown mode — fail safe (don't transmit garbage).
	return false


func _is_ptt_held() -> bool:
	if push_to_talk_action == &"":
		if not _warned_missing_ptt_action:
			push_warning("[VoiceCaptureSource] push_to_talk_action is " +
					"empty. Set it in the inspector to bind PTT to an " +
					"InputMap action. Gating all frames until then.")
			_warned_missing_ptt_action = true
		return false
	if not InputMap.has_action(push_to_talk_action):
		if not _warned_missing_ptt_action:
			push_warning("[VoiceCaptureSource] InputMap action '%s' " %
					push_to_talk_action + "is not defined. Add it under " +
					"Project Settings → Input Map. Gating all frames " +
					"until then.")
			_warned_missing_ptt_action = true
		return false
	return Input.is_action_pressed(push_to_talk_action)


func _vad_decide(mono: PackedFloat32Array) -> bool:
	# RMS over the frame.
	var sum_sq: float = 0.0
	for s in mono:
		sum_sq += s * s
	var rms: float = sqrt(sum_sq / mono.size())
	var now_ms: int = Time.get_ticks_msec()

	if rms >= vad_energy_threshold:
		# Above threshold: transmit and refresh the hangover timer.
		_vad_last_voice_time_ms = now_ms
		return true

	# Below threshold: check whether we're inside the hangover window.
	if _vad_last_voice_time_ms < 0:
		# Never had voice; nothing to extend.
		return false
	var since_voice: int = now_ms - _vad_last_voice_time_ms
	return since_voice < int(vad_hangover_ms)


func _update_transmit_state(transmitting: bool) -> void:
	if transmitting != _was_transmitting:
		_was_transmitting = transmitting
		transmit_state_changed.emit(transmitting)


# ---------------------------------------------------------------------
# Internals — capture pipeline setup
# ---------------------------------------------------------------------

func _setup_capture() -> void:
	# Confirm the user's project has microphone input enabled. If
	# not, AudioStreamMicrophone produces silence and the user is
	# confused why VAD never triggers.
	if not ProjectSettings.get_setting("audio/driver/enable_input", false):
		push_warning("[VoiceCaptureSource] Project setting " +
				"'audio/driver/enable_input' is false. Microphone " +
				"capture will produce silence. Enable it in " +
				"Project Settings → Audio.")
		# Continue anyway — the user might enable it at runtime, or
		# just want to test the gating logic with the AlwaysOn mode
		# (which doesn't depend on actual audio).

	# Dedicated audio bus so capture doesn't pollute the master mix.
	# Bus is muted at the output (we don't want to play the mic back
	# through speakers — that's a feedback loop) but the capture
	# effect still receives every frame.
	_audio_bus_idx = AudioServer.bus_count
	AudioServer.add_bus(_audio_bus_idx)
	AudioServer.set_bus_name(_audio_bus_idx, _CAPTURE_BUS_NAME)
	AudioServer.set_bus_mute(_audio_bus_idx, true)

	_capture = AudioEffectCapture.new()
	_capture.buffer_length = capture_buffer_s
	AudioServer.add_bus_effect(_audio_bus_idx, _capture)

	_stream_player = AudioStreamPlayer.new()
	_stream_player.bus = _CAPTURE_BUS_NAME
	_stream_player.stream = AudioStreamMicrophone.new()
	add_child(_stream_player)
	_stream_player.play()


# ---------------------------------------------------------------------
# Internals — utility
# ---------------------------------------------------------------------

func _stereo_to_mono(stereo: PackedVector2Array) -> PackedFloat32Array:
	# Godot's AudioEffectCapture returns stereo even when the source
	# is mono microphone. Sum-to-mono, average the channels.
	var mono := PackedFloat32Array()
	mono.resize(stereo.size())
	for i in range(stereo.size()):
		var v: Vector2 = stereo[i]
		mono[i] = (v.x + v.y) * 0.5
	return mono
