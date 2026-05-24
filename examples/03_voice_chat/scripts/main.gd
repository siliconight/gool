extends Node3D

# main.gd — voice chat loopback demo entry point.
#
# Connects voice_capture → voice_loopback_receiver and surfaces
# packet flow + voice quality stats in an on-screen label so the
# user can see the pipeline working end-to-end (even though the
# stubbed encoder means there's no audible playback).

@onready var label: Label = $UI/StatusLabel
@onready var capture: Node = $VoiceCapture
@onready var receiver: Node = $VoiceLoopbackReceiver

var _packet_count: int = 0
var _last_packet_ms: int = 0
var _start_ms: int = 0

func _ready() -> void:
	_start_ms = Time.get_ticks_msec()
	if capture.has_signal("packet_dispatched"):
		capture.packet_dispatched.connect(_on_packet_sent)
	# Permission for mic access on platforms that gate it
	# (HTML5, mobile, some desktops). Godot exposes this via the
	# Os.request_permissions() API.
	if OS.has_method("request_permissions"):
		OS.request_permissions()

func _process(_delta: float) -> void:
	var uptime_s: float = float(Time.get_ticks_msec() - _start_ms) / 1000.0
	var pps: float = 0.0
	if uptime_s > 0.1:
		pps = float(_packet_count) / uptime_s
	var jitter: float = 0.0
	var loss: float = 0.0
	if Gool.has_method("get_voice_jitter_ms"):
		jitter = Gool.get_voice_jitter_ms(receiver.peer_id)
		loss = Gool.get_voice_packet_loss_ratio(receiver.peer_id)
	label.text = (
			"gool voice chat loopback demo\n"
			+ "----------------------------\n"
			+ "Uptime: %5.1f s\n" % uptime_s
			+ "Packets sent: %d (%.1f pps)\n" % [_packet_count, pps]
			+ "Voice source: %d (registered)\n" % receiver.peer_id
			+ "Jitter: %.1f ms\n" % jitter
			+ "Packet loss: %.1f%%\n" % (loss * 100.0)
			+ "\n"
			+ "NOTE: This demo uses a STUB encoder, so no audible\n"
			+ "voice plays back. The plumbing (capture → encode →\n"
			+ "send → receive → submit) all works — see scripts/\n"
			+ "voice_capture.gd for where to wire a real Opus\n"
			+ "encoder. See README.md for production checklist.\n"
	)

func _on_packet_sent(_player_id: int, _bytes: PackedByteArray,
		_seq: int, _send_ts_ms: int) -> void:
	_packet_count += 1
	_last_packet_ms = Time.get_ticks_msec()
