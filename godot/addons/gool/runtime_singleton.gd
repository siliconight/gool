# addons/gool/runtime_singleton.gd
#
# Autoload wrapper around the C++ GoolAudioRuntime. Loaded at
# /root/Gool by the editor plugin. Every prefab calls into this
# singleton.
#
# The wrapper exists so:
#   - Init/shutdown is centralized (one runtime per project)
#   - Default config from res://gool/config.json is auto-applied
#   - Update is driven from _process automatically
#
# If you need to call binding methods directly, _runtime exposes
# the underlying GoolAudioRuntime instance.

extends Node

const CONFIG_PATH := "res://gool/config.json"

var _runtime: Node = null

# v0.54.2: rate-limit flag for play_networked's "called before
# init" warning. Auto-firing weapons that trigger play_networked
# every frame can produce 40+ identical errors per second when
# init has failed. The first warning is actionable; the rest are
# noise that buries the actual init-failure error higher in the
# log. Set true on first warn, never reset (init only happens
# once per process lifetime, so re-init isn't a real scenario).
var _play_networked_warning_emitted: bool = false

# v0.55.0: bus-name cache built from cfg_dict at init time. Used
# by has_bus() and register_sound_definition's pre-check, so we
# can detect missing target_bus_name values without triggering
# the C++ side's "unknown bus" log on every miss. Keys are bus
# names (String), values are true. The empty dict represents
# "init hasn't run yet or no buses configured"; has_bus returns
# false in both cases.
var _known_bus_names: Dictionary = {}

# v0.55.0: dedupe set for register_sound_definition's "missing
# target_bus_name" warning. Each missing bus name warns once
# per session — without this, a project that registers 8 impact
# sounds all targeting the same missing bus produces 8 identical
# warnings.
var _warned_missing_target_buses: Dictionary = {}
var _ready_emitted: bool = false

# Lazy-instantiated GoolMusicChannel for the play_music_state facade.
# Created on first call so projects that don't use music never pay
# any setup cost.
var _music_channel: Node = null
var _music_state: String = ""

# Counter feeding unique sound-bank names for play_voice clips
# extracted from AudioStreamWAV resources. Each call registers a
# fresh ephemeral sound; reuse is intentionally avoided so concurrent
# voice playback for the same player works without state coordination.
var _voice_counter: int = 0

# v0.44.0: track registered voice player IDs so the editor's Live
# Stats panel can enumerate them for per-player jitter / packet-loss
# display. Appended on successful register_voice_source; never
# auto-removed in this release (host code that wants to remove a
# player after disconnect can do so manually if needed).
var _known_voice_player_ids: Array = []

# v0.34.0 (Phase 6.B): per-material impact EQ state.
#
# The autoload checks once at _ready whether the configured bus
# (default "ImpactEq") exists with the 3-biquad shape from cookbook
# section 14. If yes, `_impact_eq_bus_name` holds the bus name and
# `play_impact_sound` automatically pushes the material's EQ curve
# before playing. If no, it stays empty and the auto-EQ behavior is
# silently disabled — impacts still play through their normal bus
# routing, just without the per-material coloration.
#
# The check is one-shot to avoid re-scanning the bus graph on every
# impact (impacts can fire at 60+ Hz in a shooter, and get_bus_effects
# is O(buses × effects)).
const _IMPACT_EQ_BUS_SETTING := "gool/material_eq/impact_bus"
var _impact_eq_bus_name: String = ""

# v0.35.0 (Phase 6.C): per-material listener-space EQ state.
#
# Same one-shot detection pattern as the impact EQ above. A
# ReverbZone with `apply_listener_eq = true` reads
# `_listener_eq_bus_name` to know whether listener-space EQ is
# available. If empty (the bus doesn't exist or doesn't match
# the 3-biquad shape), zones silently skip the listener EQ ramp
# and only the reverb portion of the zone applies.
#
# This is opt-in per ReverbZone, not per-runtime. Even with the
# bus configured, no listener EQ happens unless a zone explicitly
# requests it — listener-space coloring is editorially stronger
# than reverb alone and shouldn't be automatic.
const _LISTENER_EQ_BUS_SETTING := "gool/material_eq/listener_bus"
var _listener_eq_bus_name: String = ""

# v0.36.0 (Phase 6.D): realism multiplier for ALL per-material EQ.
#
# A single global dial in [0..2] that scales the EQ curve gains
# uniformly. Affects both Phase 6.B impact EQ and Phase 6.C
# listener-space EQ so the two stages stay in proportion.
#
#   0.0  bypass — all gains forced to 0 dB, EQ effectively off
#   0.5  gentle — half-strength coloring, useful for clarity-
#        first gameplay where the material reads as flavor
#        rather than dominant texture
#   1.0  realistic — curves applied as defined in
#        MaterialEqByMaterial (the v0.33.0 table). Default.
#   1.5  amplified — pushed past realism for atmospheric or
#        horror moments
#   2.0  surreal — caricatured material presence; cutoff
#        cap to avoid run-away gains
#
# Frequency/Q values are NOT scaled — they're frequency-domain
# anchors, not amplitudes. Only the three gain_db values
# (low/mid/high) get the multiplier applied.
#
# The dial is read from project settings at init and cached.
# Runtime adjustment via set_eq_intensity() (planned hook-in
# point for Phase 4's player audio settings menu).
const _EQ_INTENSITY_SETTING := "gool/material_eq/intensity"
var _eq_intensity: float = 1.0

signal ready_to_play

func _ready() -> void:
	_runtime = ClassDB.instantiate("GoolAudioRuntime")
	if _runtime == null:
		push_error(
			"[gool] GoolAudioRuntime class not registered. This almost "
			+ "always means the GDExtension binary is missing from "
			+ "addons/gool/bin/. Fixes:\n"
			+ "  (1) Download the addon zip from "
			+ "https://github.com/siliconight/gool/releases that matches "
			+ "your OS, and unzip its addons/gool/ over yours.\n"
			+ "  (2) Or build from source: see SETUP.md.\n"
			+ "Check addons/gool/bin/ for one of: libgool_godot.so "
			+ "(Linux), gool_godot.dll (Windows), libgool_godot.dylib "
			+ "(macOS). If the file is there but Godot still can't "
			+ "load it, the binary likely targets a different Godot "
			+ "minor version than yours — match versions or rebuild."
		)
		return
	add_child(_runtime)

	# Load the project's audio config. If config.json contains a
	# "buses" array, we route through init_with_config() so the
	# engine builds the multi-tier bus graph at startup. If the file
	# is missing, empty, or has no buses key, we fall back to plain
	# init() — which builds a single-master topology, the legacy
	# behavior.
	var cfg_text := _load_config_text()
	var cfg_dict := _parse_config_dict(cfg_text)
	var sr : int = cfg_dict.get("sample_rate", 48000)
	var bs : int = cfg_dict.get("buffer_size", 512)
	var has_bus_graph: bool = cfg_dict.has("buses") \
		and cfg_dict["buses"] is Array \
		and (cfg_dict["buses"] as Array).size() > 0

	var ok: bool
	if has_bus_graph:
		# Pass the raw JSON text through — the C++ side parses it
		# using the same loader that's unit-tested at the engine
		# level. This keeps GDScript out of the schema-translation
		# business: the binding is the only place the format is
		# interpreted.
		ok = _runtime.init_with_config(cfg_text, sr, bs)
	else:
		ok = _runtime.init(sr, bs)

	if not ok:
		if has_bus_graph:
			# v0.54.2: expanded message — added the JSON-escape
			# failure mode (bad \u or stray backslash, surfaced in
			# the screenshot from v0.54.1 era as "bad escape \u")
			# and pointed users at the dock's empty-state recovery
			# buttons (shipped in v0.51.0) which are usually the
			# fastest fix path.
			push_error(
				"[gool] runtime init failed: bus config rejected. "
				+ "Check the prior error from the JSON parser above for "
				+ "the specific line. Common causes:\n"
				+ "  (1) Bad JSON escape sequence — `\\u` not followed "
				+ "by 4 hex digits, stray backslash from a Windows path, "
				+ "or unescaped quote inside a string. The parser error "
				+ "above names the line.\n"
				+ "  (2) Duplicate bus ids.\n"
				+ "  (3) A bus references a parent which doesn't exist.\n"
				+ "  (4) An effect kind that isn't recognized.\n"
				+ "Recovery: open res://gool/config.json in a text "
				+ "editor and fix the line named in the parser error. "
				+ "Or delete the file entirely — the mixer dock's empty "
				+ "state has Create default config / Use FPS template "
				+ "buttons that rebuild a known-good config."
			)
		else:
			push_error(
				"[gool] runtime init failed: no audio device available. "
				+ "Possible causes:\n"
				+ "  (1) Sample rate or buffer size in res://gool/config.json "
				+ "isn't supported by your audio device. Try sample_rate=44100 "
				+ "or buffer_size=1024.\n"
				+ "  (2) Another app has exclusive access to the device "
				+ "(some DAWs do this on Windows).\n"
				+ "  (3) Running headless without an audio device — set the "
				+ "AUDIO_ENGINE_BACKEND env var to 'null' to use the silent "
				+ "backend for CI / server use."
			)
		return
	# v0.22.4: audible "I'm alive" line. Previously a successful init
	# was completely silent — the autoload's _ready returned cleanly
	# with no output, making "no sound" indistinguishable from
	# "runtime never started." This single print is the
	# most-requested diagnostic from the v0.22.3 session.
	var v: Dictionary = _runtime.get_version()
	var version_str: String = v.get("full", "unknown") if v else "unknown"
	var bus_count: int = 0
	if has_bus_graph and cfg_dict.has("buses"):
		bus_count = cfg_dict["buses"].size()
	else:
		bus_count = 1   # single Master bus when no config
	var config_source: String = CONFIG_PATH if has_bus_graph else "defaults"
	# v0.23.2: routed through GoolLog so projects can silence/redirect
	# via Project Settings → addons/gool/logging. Same visible-by-
	# default behavior as before; users can opt-in to a quieter
	# output with `runtime:warn` in the categories override.
	GoolLog.info("runtime", "ready", {
		"version": version_str,
		"rate": "%dHz" % sr,
		"buffer": bs,
		"buses": bus_count,
		"config": config_source,
	})
	# v0.22.7: log the audio device miniaudio actually opened. If the
	# name doesn't match where the user expects sound, that's the bug
	# (wrong default output device) — no further C++ debugging needed.
	var device_desc: String = _runtime.get_backend_description()
	if device_desc != "":
		GoolLog.info("runtime", "audio device", {"name": device_desc})
	else:
		GoolLog.info("runtime", "audio device unknown",
			{"reason": "backend doesn't expose name"})
	_ready_emitted = true

	# v0.55.0: cache the bus names declared in the user's config so
	# downstream code (register_sound_definition, ReverbZone, the
	# Phase 6 EQ setup) can do cheap existence checks WITHOUT
	# triggering the C++ side's "unknown bus" log on each miss. The
	# cache is read-only after init; if the user reloads the config
	# at runtime (rare), they'll need to call _rebuild_known_buses.
	_rebuild_known_buses(cfg_dict)

	# v0.34.0 (Phase 6.B): set up automatic per-material EQ for
	# impact sounds. Reads the configured impact-EQ bus name from
	# project settings, verifies its effect chain matches the 3-
	# biquad convention from cookbook section 14, and caches the
	# result. If anything's off, the auto-EQ silently disables
	# (with a single warning) and impacts still play through
	# their normal bus routing.
	_setup_impact_eq()
	# v0.35.0 (Phase 6.C): same one-shot check for the listener-
	# space EQ bus. ReverbZones with apply_listener_eq=true will
	# consult _listener_eq_bus_name to know whether to ramp.
	_setup_listener_eq()
	# v0.36.0 (Phase 6.D): cache the realism multiplier from
	# project settings. Affects both 6.B impact EQ and 6.C
	# listener EQ when applied via the GDScript helpers below.
	_setup_eq_intensity()
	# v0.68.0: register a default global hotkey (Ctrl+Shift+G) for
	# dumping the session log to JSONL. Zero-code "press a key,
	# attach the file to a bug report" workflow available in any
	# project that has gool installed — no per-project keybinding
	# code needed. Configurable via Project Settings → Input Map
	# (action "gool_dump_session_log") and the addons/gool/logging/
	# dump_log_* settings registered below.
	_register_session_log_hotkey_settings()
	_setup_session_log_hotkey()
	ready_to_play.emit()

# v0.22.7: render-thread health polling. Reads the diagnostic atomics
# the C++ data callback writes (callback invocations, frames rendered,
# peak sample amplitude, render-callback exceptions) and prints them
# every _RENDER_STATS_INTERVAL seconds. The output decisively answers
# "why is gool silent" without needing C++ debugging:
#
#   callback_invocations == 0  →  miniaudio's audio thread never ran.
#                                  Backend Start() returned Success
#                                  but the device wasn't actually
#                                  opened. Real bug in device init.
#
#   callback_invocations > 0
#   AND frames_rendered > 0
#   AND peak_amplitude == 0.0  →  audio thread IS running and IS
#                                  writing frames, but every sample
#                                  is zero. Silence comes from
#                                  upstream (mixer producing
#                                  silence: no active emitters, bus
#                                  at -inf dB, decoder fed empty
#                                  data). C++ mixer instrumentation
#                                  needed.
#
#   peak_amplitude > 0          →  non-silent samples ARE reaching
#                                  the device. Any inaudibility is
#                                  on the Windows side: wrong output
#                                  device, app volume muted, etc.
#                                  Not a gool bug.
#
# After each log, peak is reset so the next reading reflects samples
# written SINCE that log, not since process start. That way a "peak=0"
# reading means "silent in the last 2 seconds" not "silent at any
# point."
const _RENDER_STATS_INTERVAL: float = 2.0   # seconds between logs
var _render_stats_accum: float = 0.0
var _render_stats_last_invocations: int = 0
var _render_stats_last_frames: int = 0

# v0.25.0: cross-process metering for the editor mixer dock. When
# the game is launched from F5 (debugger attached), the runtime
# pushes per-bus stats over Godot's EngineDebugger message channel
# at 30 Hz. The editor-side EditorDebuggerPlugin
# (addons/gool/editor/debugger_plugin.gd) receives them and stores
# the latest snapshot for the mixer dock to render. Cost: one
# get_bus_stats() call + one EngineDebugger.send_message every
# ~33ms when running, zero when there's no debugger attached (e.g.
# exported builds).
#
# v0.26.0: this channel is now bidirectional. The dock's faders
# send `gool:set_bus_gain` commands back to the running game,
# which we receive via the EngineDebugger capture registered in
# _ready. The "gool" prefix routes any message starting with
# "gool:" to our _on_debugger_capture callback below.
const _DEBUGGER_EMIT_INTERVAL: float = 1.0 / 30.0   # 30 Hz
var _debugger_emit_accum: float = 0.0
var _debugger_capture_registered: bool = false


# v0.26.0: register the EngineDebugger capture handler so the
# editor's mixer dock can send commands (currently set_bus_gain,
# more in 3.3b-2). Idempotent — safe to call multiple times.
# Only registers when a debugger is attached (i.e. running from
# F5); in exported builds this is a no-op.
func _register_debugger_capture_if_needed() -> void:
	if _debugger_capture_registered:
		return
	if not EngineDebugger.is_active():
		return
	# register_message_capture takes the *prefix* (no colon). Messages
	# starting with "gool:" route to _on_debugger_capture, which
	# receives the part AFTER the colon as the `message` arg.
	EngineDebugger.register_message_capture(
			"gool", _on_debugger_capture)
	_debugger_capture_registered = true
	print("[gool] debugger message capture registered (gool:* routes to runtime)")


# Receive a command from the editor's mixer dock via the
# EngineDebugger channel. Return true if handled, false to let
# the editor see "Invalid message received" (we don't want that
# for unknown messages — return true on anything starting with
# "gool:" even if we don't recognize it, with a warning print).
#
# Message format (Godot strips the "gool:" prefix before delivery,
# so we match against the bare suffix):
#   "set_bus_gain"  data=[bus_name: String, db: float]
#                   → forwards to set_bus_gain_db
#
# Future (3.3b-2):
#   "set_bus_mute"  data=[bus_name: String, muted: bool]
#   "set_bus_solo"  data=[bus_name: String, solo: bool]
#   "set_bus_bypass" data=[bus_name: String, bypass: bool]
func _on_debugger_capture(message: String, data: Array) -> bool:
	# Defensive: Godot's docs are inconsistent about whether the
	# prefix is stripped. Strip it ourselves if present so we
	# work regardless of Godot version's exact behavior.
	var cmd := message
	if cmd.begins_with("gool:"):
		cmd = cmd.substr(5)
	match cmd:
		"set_bus_gain":
			if data.size() >= 2:
				var bus_name: String = String(data[0])
				var db: float = float(data[1])
				_handle_set_bus_gain(bus_name, db)
			return true
		"set_bus_mute":
			# v0.27.0
			if data.size() >= 2:
				var bus_name: String = String(data[0])
				var muted: bool = bool(data[1])
				_handle_set_bus_mute(bus_name, muted)
			return true
		"set_bus_solo":
			# v0.27.0
			if data.size() >= 2:
				var bus_name: String = String(data[0])
				var soloed: bool = bool(data[1])
				_handle_set_bus_solo(bus_name, soloed)
			return true
		"set_bus_bypass":
			# v0.27.0
			if data.size() >= 2:
				var bus_name: String = String(data[0])
				var bypassed: bool = bool(data[1])
				_handle_set_bus_bypass(bus_name, bypassed)
			return true
		"set_effect_parameter":
			# v0.28.0 (Phase 3.3c-1): live effect chain edit. Data is
			# [bus_name: String, effect_index: int, param_id: int,
			#  value: float]. Out-of-range / unknown bus / unknown
			# param all silent-no-op at the engine layer (matches
			# OnParameter's "ignored if unrecognized").
			if data.size() >= 4:
				var bus_name: String = String(data[0])
				var effect_index: int = int(data[1])
				var param_id: int = int(data[2])
				var value: float = float(data[3])
				_handle_set_effect_parameter(
						bus_name, effect_index, param_id, value)
			return true
		_:
			push_warning("[gool] unrecognized debugger command: %s" % message)
			return true   # still "handled" — don't let editor complain


func _handle_set_bus_gain(bus_name: String, db: float) -> void:
	if not _check_init("_handle_set_bus_gain"):
		return
	# Clamp to a sensible range matching the mixer dock's fader.
	# Out-of-range values shouldn't crash the runtime, just clamp.
	db = clampf(db, -72.0, 6.0)
	# v0.26.2: call _runtime.set_bus_gain_db directly. The autoload
	# doesn't expose a public set_bus_gain_db wrapper (only the
	# Godot-bus-mirror variants sync_volume_from_godot_bus and the
	# bind_*_to_godot_bus pollers), so the prior `set_bus_gain_db(...)`
	# call on self was a runtime-error-only-when-reached bug — never
	# triggered in headless smoke (which only parses, doesn't run
	# the editor↔game channel), but would surface the first time a
	# user dragged a fader during F5.
	var ok: bool = _runtime.set_bus_gain_db(bus_name, db)
	if not ok:
		push_warning("[gool] set_bus_gain('%s', %.2fdB) failed" % [bus_name, db])


# v0.27.0: per-bus mute / solo / effect-bypass debugger-command handlers.
# Same pattern as _handle_set_bus_gain — call _runtime.X directly (no
# autoload-level wrapper, per the discipline in
# docs/engineering/lessons_learned.md §"Wrappers vs direct member calls").
func _handle_set_bus_mute(bus_name: String, muted: bool) -> void:
	if not _check_init("_handle_set_bus_mute"):
		return
	var ok: bool = _runtime.set_bus_muted(bus_name, muted)
	if not ok:
		push_warning("[gool] set_bus_mute('%s', %s) failed" % [bus_name, muted])


func _handle_set_bus_solo(bus_name: String, soloed: bool) -> void:
	if not _check_init("_handle_set_bus_solo"):
		return
	var ok: bool = _runtime.set_bus_soloed(bus_name, soloed)
	if not ok:
		push_warning("[gool] set_bus_solo('%s', %s) failed" % [bus_name, soloed])


func _handle_set_bus_bypass(bus_name: String, bypassed: bool) -> void:
	if not _check_init("_handle_set_bus_bypass"):
		return
	var ok: bool = _runtime.set_bus_effects_bypassed(bus_name, bypassed)
	if not ok:
		push_warning("[gool] set_bus_bypass('%s', %s) failed" % [bus_name, bypassed])


# v0.28.0 (Phase 3.3c-1): live effect parameter set. Same direct-call
# discipline as the v0.27.0 S/M/B handlers — go through _runtime.X
# rather than any autoload-level wrapper. The engine validates
# effect_index and param_id internally (out-of-range → silent no-op
# at the OnParameter layer).
func _handle_set_effect_parameter(bus_name: String, effect_index: int,
		param_id: int, value: float) -> void:
	if not _check_init("_handle_set_effect_parameter"):
		return
	var ok: bool = _runtime.set_effect_parameter(
			bus_name, effect_index, param_id, value)
	if not ok:
		push_warning(
				"[gool] set_effect_parameter('%s', %d, %d, %.4f) failed"
				% [bus_name, effect_index, param_id, value])

func _process(delta: float) -> void:
	if _runtime != null and _runtime.is_initialized():
		_runtime.update(delta)
		if not _mirrored_buses.is_empty():
			_poll_mirrored_buses()
		_render_stats_accum += delta
		if _render_stats_accum >= _RENDER_STATS_INTERVAL:
			_render_stats_accum = 0.0
			_log_render_stats()
		# v0.25.0: push per-bus stats over the EngineDebugger channel
		# so the editor's mixer dock can render meters during F5
		# playback. Only emits when a debugger is attached (so this
		# is free in exported builds and headless runs).
		_debugger_emit_accum += delta
		if _debugger_emit_accum >= _DEBUGGER_EMIT_INTERVAL:
			_debugger_emit_accum = 0.0
			_emit_bus_stats_to_debugger()
			# v0.44.0: also emit engine-wide render stats so the editor
			# mixer dock's new Live Stats panel can show voice count,
			# master peak, callback rate, dropouts, and per-player VOIP
			# jitter. Separate channel from bus_stats because the shape
			# is engine-wide rather than per-bus; consumers cache them
			# independently.
			_emit_render_stats_to_debugger()

# Push the current bus-stats array as a debugger message. The
# editor side (GoolDebuggerPlugin) listens for "gool:bus_stats".
# Payload format matches Gool.get_bus_stats() directly:
#   [{name: String, parent: int, peak_linear: float}, ...]
#
# Safe to call when no debugger is attached — EngineDebugger.is_active()
# returns false in exported builds, and we early-out without sending.
#
# v0.26.0: this is also where we lazily register the inbound
# capture handler so the editor can send us commands back.
# Lazy-registration (vs. registering in _ready) avoids the case
# where _ready runs before the debugger is fully attached.
func _emit_bus_stats_to_debugger() -> void:
	if not EngineDebugger.is_active():
		return
	# v0.26.0: lazily register the inbound capture on first emit
	# tick (by which time the debugger is definitely up). Idempotent.
	_register_debugger_capture_if_needed()
	var stats: Array = get_bus_stats()
	if stats.is_empty():
		return
	# v0.28.3 (Phase 3.3c-2): augment each per-bus dict with its
	# effect chain so the mixer dock can show an "Fx (N)" badge
	# and populate the effect-edit panel without a separate
	# editor→game request/response round-trip. Effects rarely
	# change at runtime; the added bandwidth (~5 KB/s for a 4-bus
	# config with ~2 effects per bus) is negligible at the 30 Hz
	# emit rate. has_method() guard so an older binding without
	# get_bus_effects (pre-v0.28.0) just sends a stats payload
	# without an `effects` field, which the dock treats as 0
	# effects per bus and hides the Fx button.
	if _runtime != null and _runtime.has_method("get_bus_effects"):
		for s in stats:
			var bn := String(s.get("name", ""))
			if bn.is_empty():
				s["effects"] = []
			else:
				s["effects"] = _runtime.get_bus_effects(bn)
	EngineDebugger.send_message("gool:bus_stats", [stats])

# v0.44.0: Push engine-wide render stats over the same debugger
# channel pattern as bus_stats. Editor side (GoolDebuggerPlugin)
# listens for "gool:render_stats". Payload is a single Dictionary
# with the keys returned by Gool.get_render_stats() (active_voices,
# active_emitters, mixer_peak, master_gain, callback_invocations,
# frames_rendered, peak_amplitude, exception_count, ...) plus an
# additional `voice_chat` sub-dict mapping player_id → {jitter_ms,
# packet_loss}. Editor's Live Stats panel reads from the cache.
#
# Same safety pattern as _emit_bus_stats_to_debugger: early-out if
# no debugger is attached, so this is free in exported builds and
# headless runs.
func _emit_render_stats_to_debugger() -> void:
	if not EngineDebugger.is_active():
		return
	if _runtime == null:
		return
	var stats: Dictionary = _runtime.get_render_stats()
	if stats.is_empty():
		return
	# Augment with VOIP jitter for any players the engine knows
	# about. The has_method guard handles binaries that predate the
	# voice-source enumeration API — those just get an empty
	# voice_chat dict and the editor panel hides the section.
	var voice_chat: Dictionary = {}
	if _runtime.has_method("get_known_voice_player_ids"):
		var ids: Array = _runtime.get_known_voice_player_ids()
		for pid in ids:
			voice_chat[pid] = {
				"jitter_ms": get_voice_jitter_ms(int(pid)),
				"packet_loss": get_voice_packet_loss_ratio(int(pid)),
			}
	stats["voice_chat"] = voice_chat
	EngineDebugger.send_message("gool:render_stats", [stats])

func _log_render_stats() -> void:
	var stats: Dictionary = _runtime.get_render_stats()
	if stats.is_empty():
		return
	var invocations: int = stats.get("callback_invocations", 0)
	var frames: int = stats.get("frames_rendered", 0)
	var peak: float = stats.get("peak_amplitude", 0.0)
	var exceptions: int = stats.get("exception_count", 0)
	# v0.22.8: mixer-level stats (may be absent on older binaries).
	var active_voices: int = stats.get("active_voices", -1)
	var mixer_peak: float = stats.get("mixer_peak", -1.0)
	var master_gain: float = stats.get("master_gain", -1.0)
	# v0.39.0: emitter pool count. -1 means the binary predates v0.39.0
	# and we should keep the legacy single-branch behavior (i.e.
	# can't discriminate idle from real bug; fall back to the old
	# combined warning).
	var active_emitters: int = stats.get("active_emitters", -1)
	var delta_invocations: int = invocations - _render_stats_last_invocations
	var delta_frames: int = frames - _render_stats_last_frames
	_render_stats_last_invocations = invocations
	_render_stats_last_frames = frames
	# Diagnosis line — one log every 2 seconds when running.
	# v0.23.2: routed through GoolLog at DEBUG level on the "mixer"
	# category so projects can quiet this verbose per-interval ping
	# via Project Settings (categories="mixer:warn"). The DEAD AIR
	# warnings below remain WARN-level so they're always visible
	# even when the routine ping is silenced.
	if active_voices >= 0:
		GoolLog.debug("mixer", "render", {
			"cb": invocations, "dcb": delta_invocations,
			"frames": frames, "dframes": delta_frames,
			"peak": "%.4f" % peak, "mixer_peak": "%.4f" % mixer_peak,
			"voices": active_voices, "gain": "%.2f" % master_gain,
			"emitters": active_emitters,
			"exc": exceptions,
		})
	else:
		# Older binary — only backend stats available
		GoolLog.debug("mixer", "render", {
			"cb": invocations, "dcb": delta_invocations,
			"frames": frames, "dframes": delta_frames,
			"peak": "%.4f" % peak, "exc": exceptions,
		})
	# Layered diagnosis when something looks broken. v0.22.8 ordering
	# is bottom-up: most-upstream cause first (audio thread dead) →
	# most-downstream cause last (master gain silencing).
	if delta_invocations == 0 and invocations == 0:
		GoolLog.warn("mixer", "DEAD AIR: render callback never invoked",
			{"cause": "miniaudio audio thread didn't start; backend "
					+ "init reported success but playback device was "
					+ "never opened",
			 "action": "file a report at github.com/siliconight/gool/issues"})
	elif delta_invocations == 0:
		GoolLog.warn("mixer", "DEAD AIR: render callback stopped",
			{"cause": "miniaudio audio thread died or paused",
			 "effect": "audio output frozen as of this interval"})
	elif peak == 0.0 and delta_frames > 0:
		# v0.22.8: now use mixer stats to discriminate the cause.
		if active_voices == 0:
			# v0.39.0: split into idle (no emitters) vs real bug
			# (emitters exist but their voice slots never promoted
			# out of Inactive). Pre-v0.39.0 this was one combined
			# warning that fired every 2 seconds in any scene with
			# no SFX currently playing — a false positive.
			if active_emitters == 0:
				# Truly idle. Audio engine is healthy; nothing is
				# playing. Demote to DEBUG so projects that
				# silence the mixer category by default don't
				# see it, but the routine "render" debug log
				# above already captures the same state so this
				# extra line would be redundant — skip entirely.
				pass
			elif active_emitters > 0:
				# THE real bug case. Emitters exist (someone
				# called create_emitter and got handles back) but
				# none of them have an active voice slot in the
				# mixer. Most likely cause v0.39.0+: the
				# CreateEmitter asset-lookup fall-through (look
				# for "CreateEmitter: asset lookup failed"
				# warnings in the log just before this fired).
				# Less likely: command queue overflow (would
				# also fire underrun warnings) or actual mixer
				# dispatch bug.
				GoolLog.warn("mixer", "DEAD AIR: emitters exist but no voice slots active",
					{"frames_this_interval": delta_frames,
					 "active_emitters": active_emitters,
					 "cause": "voice slots didn't promote from Inactive — "
							+ "likely asset lookup failure in CreateEmitter "
							+ "(check for 'asset lookup failed' warnings above), "
							+ "or PostCommand queue full",
					 "investigate": "audio_runtime.cpp::CreateEmitter, "
							+ "or mixer command ring depth"})
			else:
				# active_emitters == -1: legacy binary without
				# the v0.39.0 accessor. Fall back to the old
				# combined warning so users on older binaries
				# still get diagnostics (just less precise).
				GoolLog.warn("mixer", "DEAD AIR: no active voices",
					{"frames_this_interval": delta_frames,
					 "cause": "no voice slots active; legacy binary "
							+ "(pre-v0.39.0) can't tell idle from bug",
					 "action": "upgrade gool to v0.39.0+ for precise diagnosis"})
		elif mixer_peak == 0.0 and active_voices > 0:
			GoolLog.warn("mixer", "DEAD AIR: mixer silent with voices active",
				{"active_voices": active_voices,
				 "cause": "voices producing silence (empty PCM, mode mismatch, "
						+ "wrong bus routing) OR bus chain summing to zero "
						+ "upstream of Master",
				 "investigate": "decoder PCM state, bus-graph routing"})
		elif mixer_peak > 0.0 and master_gain == 0.0:
			GoolLog.warn("mixer", "DEAD AIR: master gain = 0",
				{"mixer_peak": "%.4f" % mixer_peak, "master_gain": 0.0,
				 "cause": "Master bus output gain is -inf dB",
				 "investigate": "config.json Master gain_db, runtime set_bus_gain_db calls"})
		elif mixer_peak > 0.0 and master_gain > 0.0 and peak == 0.0:
			GoolLog.warn("mixer", "DEAD AIR: post-gain mystery",
				{"mixer_peak": "%.4f" % mixer_peak,
				 "master_gain": "%.2f" % master_gain,
				 "device_peak": 0.0,
				 "cause": "unexpected — buffer copy to wrong destination, "
						+ "format-conversion truncating, or unknown bug",
				 "action": "file a report with this output"})
		else:
			GoolLog.warn("mixer", "DEAD AIR: unknown cause",
				{"frames_this_interval": delta_frames,
				 "active_voices": active_voices,
				 "mixer_peak": "%.4f" % mixer_peak,
				 "master_gain": "%.2f" % master_gain,
				 "note": "symptom doesn't match any predicted cause; manual investigation needed"})
	elif exceptions > 0:
		GoolLog.warn("mixer", "render-callback exceptions caught",
			{"count": exceptions,
			 "since": "Initialize",
			 "effect": "audio frames dropped to silence by catch-all barrier"})
	# Reset peak for the next interval window (resets BOTH backend
	# peak AND mixer peak — v0.22.8).
	_runtime.reset_render_peak()


# Called from _process when at least one bus pair is registered for
# automatic mirroring. Cheap when nothing changed — the cached-db
# check short-circuits the C++ call. Logs a one-time warning if a
# registered Godot bus name disappears between frames.
func _poll_mirrored_buses() -> void:
	for godot_bus_name in _mirrored_buses:
		var idx := AudioServer.get_bus_index(godot_bus_name)
		if idx < 0:
			continue   # bus was renamed/removed; silently skip
		var db := AudioServer.get_bus_volume_db(idx)
		if _mirrored_last_db.get(godot_bus_name, 1e9) == db:
			continue
		_mirrored_last_db[godot_bus_name] = db
		var gool_bus_name: String = _mirrored_buses[godot_bus_name]
		_runtime.set_bus_gain_db(gool_bus_name, db)

func _exit_tree() -> void:
	if _runtime != null and _runtime.is_initialized():
		_runtime.shutdown()

# Forward common methods so prefabs can call get_node("/root/Gool")
# directly without reaching into _runtime.

func is_initialized() -> bool:
	return _runtime != null and _runtime.is_initialized()


# v0.71.0: Loud first-call errors. Previously, calling any Gool API
# method before the autoload finished initializing silently returned
# 0/null/false/[] — and new users hit this and didn't understand why
# their sounds weren't playing. The classic "register_sound called
# from _ready" timing trap.
#
# _check_init() replaces the bare `if not is_initialized()` guard at
# every API site. First time it sees a pre-init call for a given
# method name, it pushes a warning explaining the timing and pointing
# the user at the ready_to_play signal. Subsequent pre-init calls
# from the same method stay quiet (so we don't spam Output when an
# autoload-local loop hammers an API in _ready).
#
# Hot-path cost: one is_initialized() check, identical to the
# previous guard. The dict-lookup slow path only fires when the
# runtime isn't ready, which is the rare case we want to surface.
# v0.73.1 hotfix: a previous v0.71.0 form of this constant used
# multi-line string concatenation with `+`. That syntax fails to
# parse on real Godot because GDScript's constant-folding pass
# does not evaluate `+` between string literals at parse time —
# the operator stays runtime-only. Result: runtime_singleton.gd
# wouldn't compile on fresh installs, the Gool autoload never
# registered, every script referencing `Gool.X` errored. Static
# analysis on this end missed it (it only checked shape, not
# parse-time constant rules). Now uses a single literal —
# unambiguously parser-friendly across all GDScript versions.
const _NOT_INITIALIZED_WARNING: String = "[gool] %s() called before the runtime is ready. Call this after the Gool autoload's `ready_to_play` signal fires, or guard with `if Gool.is_initialized():`. (This warning fires once per method per session.)"
var _not_init_warned: Dictionary = {}

# Returns true if the runtime is ready (caller may proceed).
# Returns false otherwise, having emitted a one-time warning for
# the given method name.
func _check_init(method_name: String) -> bool:
	if is_initialized():
		return true
	if not _not_init_warned.get(method_name, false):
		push_warning(_NOT_INITIALIZED_WARNING % method_name)
		_not_init_warned[method_name] = true
	return false


# ---- v0.67.0: session log dump (game-thread API) ---------------------
#
# Convenience wrappers on the Gool singleton for the always-on session
# log buffer maintained by GoolLog. Every emit() call (info/warn/error/
# etc.) pushes a structured entry into a bounded ring; these methods
# read and dump it.
#
# Why expose these here as well as on GoolLog directly?
# Two reasons:
#   1. Discoverability — Gool.dump_session_log() is the path a user
#      finds via autocomplete on the Gool autoload. GoolLog.dump_*
#      requires knowing the helper class exists.
#   2. Symmetry with the existing Gool API surface — register_sound,
#      get_bus_stats, etc. all live on Gool. The session dump fits the
#      same shape: "diagnostic capability you reach through the
#      autoload."
#
# Typical usage:
#
#   var path := Gool.dump_session_log()        # auto-named .jsonl in user://
#   prints("[gool] %d entries dumped to %s" % [
#       Gool.get_session_log_size(), path])
#
#   Gool.dump_session_log("res://debug/last.jsonl")   # explicit path
#   Gool.clear_session_log()                          # reset between tests
#
# The JSONL format is one JSON object per line (timestamp_ms, level,
# category, msg, fields, source, label) — grep/jq-friendly for
# post-session analysis.

func dump_session_log(path: String = "") -> String:
	# When path is empty, GoolLog auto-generates
	# user://gool_session_<datetime>_<ms>.jsonl so a user who just
	# wants "a file I can attach to a bug report" doesn't have to
	# invent a path. Returns the resolved path on success, "" on
	# failure (an error was already logged via GoolLog.error in the
	# helper, so we don't double-log here).
	return GoolLog.dump_session_to_file(path)


func clear_session_log() -> void:
	# Resets the ring buffer to empty and rebases the t_ms timeline
	# to "now." Useful for bracketing the part of a session a user
	# wants to inspect — call before reproducing a bug, then dump
	# after, and the resulting file contains only the relevant
	# window rather than all setup noise.
	GoolLog.clear_session()


func get_session_log_size() -> int:
	# Current entry count in the ring (saturates at the configured
	# capacity; default 4096). Useful for sanity checks like "did
	# the suspect path actually log anything between this clear()
	# and this dump()?"
	return GoolLog.session_entry_count()


# ---- v0.66.0 introspection (v0.67.1: autoload forwarders) -------------
#
# These three methods exist on the C++ binding (GoolAudioRuntime;
# bound in gool_godot.cpp around line 513 via ClassDB::bind_method),
# but in v0.66.0 they were NOT exposed as forwarders on the Gool
# autoload — even though the documentation everywhere (including
# the gool_godot.cpp doc comment around line 1656) shows the
# intended usage as Gool.has_sound("music"), Gool.get_sound_info(...),
# etc. v0.67.1 closes the gap.
#
# Defensive coding pattern (the original v0.66.0 motivation):
#
#   if not Gool.has_sound("music"):
#       push_warning("[Audition] 'music' not registered; "
#                  + "is audio_setup.gd running as an autoload AND "
#                  + "completing before audition's _ready?")
#       return
#   var handle := Gool.create_emitter("music", Vector3.ZERO, true, 250.0)
#
# Returns safe values when the runtime isn't initialized yet:
# has_sound = false, get_sound_info = {}, get_registered_sound_count = 0.
# This mirrors the C++ side's contract and means callers can ask
# these questions before init without special-casing.

func has_sound(name: String) -> bool:
	if _runtime == null:
		return false
	return _runtime.has_sound(name)


func get_sound_info(name: String) -> Dictionary:
	if _runtime == null:
		return {}
	return _runtime.get_sound_info(name)


func get_registered_sound_count() -> int:
	if _runtime == null:
		return 0
	return _runtime.get_registered_sound_count()


# ---- v0.68.0: built-in session log dump hotkey ------------------------
#
# Default Ctrl+Shift+G in any gool-using project. The whole point is
# "drop in gool, get logs" — no per-project keybinding code.
#
# Mechanism:
#
#   1. Project settings registered with sensible defaults (in
#      _register_session_log_hotkey_settings, called from _ready).
#   2. InputMap action "gool_dump_session_log" registered at runtime
#      with default Ctrl+Shift+G binding IF the action doesn't
#      already exist. If the user has added the action to their
#      project's Input Map (in Project Settings) with custom keys,
#      we respect that — InputMap.has_action returns true, we skip
#      the default binding, and the user's binding wins.
#   3. _unhandled_input listens for the action. On press, dumps the
#      session log via GoolLog.dump_session_to_file(""), prints the
#      resolved path + entry count to Output, and (optionally)
#      reveals the dumped file in the host OS file manager via
#      OS.shell_show_in_file_manager. On Windows that opens Explorer
#      with the .jsonl highlighted; on macOS, Finder; on Linux,
#      the containing folder in the default file manager.
#
# Project settings (all under addons/gool/logging/):
#
#   dump_log_enabled   (bool, default true)
#       Master switch. Set false in release/export builds if you
#       don't want the hotkey active in shipped games.
#
#   dump_log_open_dir  (bool, default true)
#       Whether to reveal the dumped .jsonl in the host OS file
#       manager via OS.shell_show_in_file_manager. Disable if the
#       file-manager pop is jarring (e.g. fullscreen game). When
#       disabled, the file path is still printed to Output for
#       manual copy/attach.
#
# To rebind the key without code: Project Settings → Input Map,
# find "gool_dump_session_log", change/add events.

const _PS_DUMP_LOG_ENABLED: String  = "addons/gool/logging/dump_log_enabled"
const _PS_DUMP_LOG_OPEN_DIR: String = "addons/gool/logging/dump_log_open_dir"
const _DUMP_LOG_ACTION: String      = "gool_dump_session_log"

var _dump_log_enabled: bool  = true
var _dump_log_open_dir: bool = true


func _register_session_log_hotkey_settings() -> void:
	if not ProjectSettings.has_setting(_PS_DUMP_LOG_ENABLED):
		ProjectSettings.set_setting(_PS_DUMP_LOG_ENABLED, true)
		ProjectSettings.add_property_info({
			"name": _PS_DUMP_LOG_ENABLED,
			"type": TYPE_BOOL,
		})
		ProjectSettings.set_initial_value(_PS_DUMP_LOG_ENABLED, true)
	if not ProjectSettings.has_setting(_PS_DUMP_LOG_OPEN_DIR):
		ProjectSettings.set_setting(_PS_DUMP_LOG_OPEN_DIR, true)
		ProjectSettings.add_property_info({
			"name": _PS_DUMP_LOG_OPEN_DIR,
			"type": TYPE_BOOL,
		})
		ProjectSettings.set_initial_value(_PS_DUMP_LOG_OPEN_DIR, true)


func _setup_session_log_hotkey() -> void:
	# Cache settings once so _unhandled_input is a cheap predicate.
	_dump_log_enabled  = bool(ProjectSettings.get_setting(
			_PS_DUMP_LOG_ENABLED, true))
	_dump_log_open_dir = bool(ProjectSettings.get_setting(
			_PS_DUMP_LOG_OPEN_DIR, true))
	# Register the default InputMap action only if absent — respects
	# any rebind the user did in Project Settings → Input Map.
	if not InputMap.has_action(_DUMP_LOG_ACTION):
		InputMap.add_action(_DUMP_LOG_ACTION)
		var ev := InputEventKey.new()
		ev.keycode = KEY_G
		ev.ctrl_pressed = true
		ev.shift_pressed = true
		InputMap.action_add_event(_DUMP_LOG_ACTION, ev)


func _unhandled_input(event: InputEvent) -> void:
	# Hot-path predicate: bail before any allocation if disabled.
	if not _dump_log_enabled:
		return
	# Defensive: action could have been removed at runtime by the
	# host project. Treat that as "feature disabled" rather than
	# crashing.
	if not InputMap.has_action(_DUMP_LOG_ACTION):
		return
	if event.is_action_pressed(_DUMP_LOG_ACTION):
		# Consume the event so it doesn't also fire any host-
		# project action bound to the same keys.
		get_viewport().set_input_as_handled()
		_do_dump_session_log()


func _do_dump_session_log() -> void:
	var path: String = GoolLog.dump_session_to_file("")
	if path == "":
		push_warning("[gool] session log dump failed (empty path returned). "
				+ "Check the Output panel for the underlying file-write error.")
		return
	var n: int = GoolLog.session_entry_count()
	# Globalize the path so we can show it in OS-native form and
	# pass it to shell APIs. ProjectSettings.globalize_path on a
	# "user://" path returns the absolute filesystem path (e.g.
	# C:\Users\...\AppData\Roaming\Godot\app_userdata\<proj>\
	# gool_session_...jsonl on Windows). On an already-absolute
	# path it's a no-op.
	var os_path: String = ProjectSettings.globalize_path(path)
	# Print BOTH paths. The user:// form is what Godot scripts use
	# internally; the OS path is what the user pastes into a bug
	# report or copies into their file manager's address bar.
	# Having both visible means even if shell_show_in_file_manager
	# fails, the user has a copyable path printed in plain text.
	print("[gool] session log dumped (%d entries):" % n)
	print("    user://  %s" % path)
	print("    OS path  %s" % os_path)
	if _dump_log_open_dir:
		# v0.69.1: switched from OS.shell_open(dir) to
		# OS.shell_show_in_file_manager(file).
		#
		# OS.shell_open on a directory uses Windows ShellExecute's
		# default verb on a folder path; on some configurations
		# that falls through to the Microsoft Store's "which app
		# should we use?" picker — clearly the wrong outcome for
		# "show me where this file is."
		#
		# OS.shell_show_in_file_manager (Godot 4.3+, gool compat
		# min 4.4 so available) is purpose-built for this exact
		# use case: on Windows it opens Explorer with the file
		# selected; on macOS it reveals in Finder; on Linux it
		# opens the containing folder via xdg-open. Different,
		# more reliable code path per OS.
		#
		# As a bonus, "reveal the file with it highlighted" is
		# better UX than "open the folder and let the user hunt
		# for the most recent .jsonl" anyway.
		var err: int = OS.shell_show_in_file_manager(os_path)
		if err != OK:
			push_warning("[gool] shell_show_in_file_manager(\"%s\") "
					% os_path
					+ "failed (err=%d). The dumped file is still at " % err
					+ "the OS path printed above; paste it into your "
					+ "file manager's address bar.")


# v0.55.0: cheap bus-existence check. Reads from a cache built at
# init time, NOT from the C++ runtime — the C++ side logs "unknown
# bus" when queried for a missing bus, which is exactly the noise
# we want to avoid in graceful-degradation paths. Returns false if
# init hasn't run yet or if the bus isn't in the cache.
func has_bus(bus_name: String) -> bool:
	return _known_bus_names.has(bus_name)


# v0.55.0: rebuild _known_bus_names from the parsed config dict.
# Called from _ready after init succeeds. Tolerates malformed
# buses entries (missing name, non-string name) by skipping them
# silently — those would have been caught at init time by the C++
# side's stricter parse.
func _rebuild_known_buses(cfg_dict: Dictionary) -> void:
	_known_bus_names.clear()
	var buses_v: Variant = cfg_dict.get("buses", [])
	if not (buses_v is Array):
		return
	var buses: Array = buses_v
	for b_v in buses:
		if not (b_v is Dictionary):
			continue
		var n: String = String(b_v.get("name", ""))
		if n != "":
			_known_bus_names[n] = true

# Returns the engine version as a Dictionary:
#   { "major": int, "minor": int, "patch": int,
#     "full":  String, "commit": String }
# Useful in debug overlays, crash reports, and bug-report forms.
# Available before init() since the version is compile-time.
func get_version() -> Dictionary:
	if _runtime == null:
		return {}
	return _runtime.get_version()

# Render-thread health & activity stats. Polled by GoolDebugOverlay
# (and any user-facing debug HUDs) to surface mix-thread metrics
# like voice count, peak amplitude, invocation count, and frame
# count. The dictionary shape is documented in gool_godot.cpp
# around line 477; see `get_render_stats[\"peak_amplitude\"]` etc.
#
# Returns an empty dict before init() so this is safe to call from
# overlay UI that initializes before the audio runtime is ready.
#
# Added in v0.23.13 to surface the C++-binding method through Gool
# as a first-class public API (previously callers reached into
# `_runtime.get_render_stats()` directly, e.g. plugin.gd:49's
# documentation comment referenced `Gool.get_render_stats()` even
# though that wrapper didn't exist).
func get_render_stats() -> Dictionary:
	if _runtime == null:
		return {}
	return _runtime.get_render_stats()

# v0.24.0: per-bus metering for the editor mixer dock. Returns
# Array of Dictionaries: [{name: String, parent: int, peak_linear: float}, ...]
# with one entry per bus (master always present, others in graph order).
# parent is -1 for the master bus. Peak is read-and-reset on every call
# so consecutive calls cover the audio samples between them — poll at
# a steady cadence (~30 Hz) for meter behavior.
#
# Returns [] if the runtime isn't initialized or no bus graph is built.
func get_bus_stats() -> Array:
	if _runtime == null:
		return []
	return _runtime.get_bus_stats()

## Reset the C++ render-thread peak-sample-amplitude counter. Pairs
## with get_render_stats(): callers poll get_render_stats() to read
## the peak accumulated since the last reset, then call this to start
## a fresh window. Used by GoolDebugOverlay._refresh to produce
## per-refresh-window peak readings instead of a monotonic since-
## startup peak (which would only ever grow).
##
## No-op if the runtime isn't initialized or the backend isn't a
## MiniaudioBackend (the C++ side handles those cases).
func reset_render_peak() -> void:
	if _runtime == null:
		return
	_runtime.reset_render_peak()

## Human-readable description of the audio backend miniaudio
## negotiated at init() time (e.g. "WASAPI / Speakers", "coreaudio
## / MacBook Pro Speakers", "alsa / default"). Returns "" before
## init or if the backend doesn't expose a name.
##
## Used by GoolDebugOverlay to show which device audio is actually
## coming out of.
func get_backend_description() -> String:
	if _runtime == null:
		return ""
	return _runtime.get_backend_description()

func register_pcm_sound(name: String, samples: PackedFloat32Array,
						 sr: int = 48000, ch: int = 1) -> int:
	if not _check_init("register_pcm_sound"):
		return 0
	return _runtime.register_pcm_sound(name, samples, sr, ch)

## AudioFileFormat constants — pass to register_sound_from_bytes()
## as `format_hint`. FORMAT_AUTO sniffs by magic bytes (recommended);
## the others are explicit overrides for hosts that already know
## what format they're passing.
const FORMAT_AUTO:        int = 0
const FORMAT_WAV:         int = 1
const FORMAT_OGG_VORBIS:  int = 2
const FORMAT_FLAC:        int = 3
const FORMAT_OPUS:        int = 4

## Load a sound file from any Godot-readable path (including res://
## in PCK-packaged builds) and register it as a one-shot PCM asset.
## Supported formats depend on what was compiled in:
##   - WAV    (AUDIO_ENGINE_DECODERS_WAV)
##   - Vorbis (AUDIO_ENGINE_DECODERS_OGG, .ogg/.oga extension)
##   - FLAC   (AUDIO_ENGINE_DECODERS_FLAC)
##   - Opus   (AUDIO_ENGINE_DECODERS_OPUS, .opus extension)
##
## The default CMake build has all decoders OFF — projects that
## want file playback must enable the relevant flag(s). When a
## decoder is compiled out, the binding pushes a clear error.
##
## Returns the AudioSoundId (positive 64-bit int) on success, 0 on
## failure. The returned id can be paired with
## register_sound_definition() to wire spatialization, looping, bus
## routing, etc.
##
## For long music tracks where the decoded PCM would be too large
## to keep resident, see the upcoming streaming-from-file binding
## (deferred to a follow-up release; see CHANGELOG).
func register_sound_from_file(name: String, path: String) -> int:
	if not _check_init("register_sound_from_file"):
		return 0
	return _runtime.register_sound_from_file(name, path)

## Same as register_sound_from_file but takes already-loaded bytes.
## Useful when the host wants to manage file I/O (e.g. custom asset
## packs, network downloads, encrypted blobs).
##
## `format_hint` is one of FORMAT_*; FORMAT_AUTO (the default) sniffs
## by magic bytes — RIFF/WAVE for WAV, OggS+OpusHead for Opus,
## OggS+Vorbis for Vorbis, fLaC for FLAC.
func register_sound_from_bytes(name: String, bytes: PackedByteArray,
								  format_hint: int = FORMAT_AUTO) -> int:
	if not _check_init("register_sound_from_bytes"):
		return 0
	return _runtime.register_sound_from_bytes(name, bytes, format_hint)

## Register a Godot AudioStream resource as a gool sound. Convenience
## wrapper over the C++ binding's `register_sound_from_stream` (added
## in v0.14.0). Two paths internally:
##
##   * If `stream.resource_path` is set (the 95% case — the stream
##     was loaded from a .wav/.ogg/.flac/.opus file), the binding
##     reads those original bytes and registers them. No re-decoding
##     through Godot's runtime; gool's own decoder owns the asset.
##   * If `stream` is an AudioStreamWAV with no resource path
##     (procedurally constructed), the raw PCM in its `data`
##     property is registered directly.
##
## Procedural stream subtypes that can't be reduced to a single PCM
## asset (AudioStreamRandomizer, AudioStreamPolyphonic,
## AudioStreamGenerator) are rejected with a diagnostic; for those
## paths, use `register_pcm_sound()` from script with the samples
## you already have.
##
## Returns the AudioSoundId on success, 0 on failure.
func register_sound_from_stream(name: String, stream: AudioStream) -> int:
	if not _check_init("register_sound_from_stream"):
		return 0
	return _runtime.register_sound_from_stream(name, stream)

## AudioCategory enum mirrored for GDScript callers. Matches the
## C++ enum order (audio::AudioCategory in types.h). Hosts pass one
## of these to register_sound_definition() to control routing
## through `category_routing` in config.json.
const CATEGORY_SFX:      int = 0
const CATEGORY_VOICE:    int = 1
const CATEGORY_MUSIC:    int = 2
const CATEGORY_AMBIENCE: int = 3
const CATEGORY_UI:       int = 4
const CATEGORY_DIALOGUE: int = 5

## Register a sound definition.
##
## `category` controls which bus the runtime picks when no explicit
## bus override is set. Default is SFX (0). See CATEGORY_* constants.
##
## `target_bus_name` overrides category routing. Pass the bus's
## `name` from config.json (e.g. "LocalSfx", "RemoteSfx", "Music").
## Empty string (the default) → use category routing. Unknown bus
## names produce a warning and fall back to category routing.
##
## To route the same audio asset to different buses, register it
## under different gameplay names: e.g., "rifle_fire_local" →
## LocalSfx, "rifle_fire_remote" → RemoteSfx.
func register_sound_definition(name: String, spatialized: bool = true,
								 looping: bool = false,
								 min_distance: float = 1.0,
								 max_distance: float = 50.0,
								 loop_crossfade_ms: float = 0.0,
								 category: int = CATEGORY_SFX,
								 target_bus_name: String = "",
								 occlusion_enabled: bool = true,
								 priority: int = 128) -> void:
	if not _check_init("register_sound_definition"):
		return
	# v0.55.0: pre-check target_bus_name against the cache to avoid
	# the C++ side's "unknown bus" log when it's missing. Without
	# this, registering 8 impact sounds all targeting a missing
	# ImpactEq bus produces 8 identical warnings; one per missing
	# bus name is enough.
	var resolved_target: String = target_bus_name
	if target_bus_name != "" and not has_bus(target_bus_name):
		if not _warned_missing_target_buses.has(target_bus_name):
			_warned_missing_target_buses[target_bus_name] = true
			push_warning("[gool] register_sound_definition: target_bus_name '%s' doesn't exist in res://gool/config.json. Sounds targeting this bus will fall back to category routing. (Further warnings for this bus suppressed.)" % target_bus_name)
		resolved_target = ""
	_runtime.register_sound_definition(name, spatialized, looping,
										 min_distance, max_distance,
										 loop_crossfade_ms,
										 category, resolved_target,
										 occlusion_enabled, priority)

## v0.49.0: Dictionary-based form of register_sound_definition.
## Eliminates the 9-positional-argument footgun in the original
## signature — argument order is easy to get wrong silently because
## skipping arguments uses defaults you didn't intend.
##
## Recognized keys (all optional; defaults match register_sound_definition):
##   - "spatialized": bool (default true)
##   - "looping": bool (default false)
##   - "min_distance": float meters (default 1.0)
##   - "max_distance": float meters (default 50.0)
##   - "loop_crossfade_ms": float ms (default 0.0)
##   - "category": int from CATEGORY_* enum (default CATEGORY_SFX)
##   - "target_bus_name": String (default "" — use category routing)
##   - "occlusion_enabled": bool (default true)
##
## Unrecognized keys are silently ignored — feel free to stash
## game-specific notes in the same dict (e.g. for documentation),
## they'll be dropped at registration time.
##
## Example:
##     Gool.register_sound({
##         "name": "gunshot",
##         "spatialized": true,
##         "max_distance": 80.0,
##         "category": Gool.CATEGORY_SFX,
##         "target_bus_name": "LocalSfx",
##         "occlusion_enabled": true,
##     })
##
## Equivalent to the v0.48.0-era positional form but with all
## arguments named at the call site — your IDE can autocomplete
## the keys, and a wrong key prints a warning instead of silently
## using the default.
func register_sound(name: String, opts: Dictionary = {}) -> void:
	if not _check_init("register_sound"):
		return

	# Validate keys so typos surface as warnings rather than
	# silent default-fallback (the whole point of this wrapper).
	const RECOGNIZED_KEYS: Array = [
		"spatialized", "looping", "min_distance", "max_distance",
		"loop_crossfade_ms", "category", "target_bus_name",
		"occlusion_enabled",
	]
	for k in opts.keys():
		var k_str: String = String(k)
		if not RECOGNIZED_KEYS.has(k_str):
			push_warning(
					"[gool] register_sound: unknown key '%s' for sound '%s' (silently ignored). "
					% [k_str, name]
					+ "Recognized keys: %s" % str(RECOGNIZED_KEYS))

	var spatialized: bool = bool(opts.get("spatialized", true))
	var looping: bool = bool(opts.get("looping", false))
	var min_distance: float = float(opts.get("min_distance", 1.0))
	var max_distance: float = float(opts.get("max_distance", 50.0))
	var loop_crossfade_ms: float = float(opts.get("loop_crossfade_ms", 0.0))
	var category: int = int(opts.get("category", CATEGORY_SFX))
	var target_bus_name: String = String(opts.get("target_bus_name", ""))
	var occlusion_enabled: bool = bool(opts.get("occlusion_enabled", true))

	_runtime.register_sound_definition(name, spatialized, looping,
										 min_distance, max_distance,
										 loop_crossfade_ms,
										 category, target_bus_name,
										 occlusion_enabled)

## Resolve a bus name to its BusId. Returns -1 if no bus matches.
## Use to bridge between code that knows bus names (config files,
## hosts) and code that needs BusId tokens (set_bus_gain_db,
## set_effect_parameter). O(N) over kMaxBuses; fine for init/
## registration time, not per-frame.
func find_bus_id_by_name(name: String) -> int:
	if not _check_init("find_bus_id_by_name"):
		return -1
	return _runtime.find_bus_id_by_name(name)

## v0.35.0: forwarder for the engine's set_effect_parameter.
## ReverbZone and the impact/listener EQ paths call this; ad-hoc
## designer code can also use it directly. Signature mirrors the
## C++ binding — takes bus_name (String), effect_index (chain
## position 0-based), param_id (one of EffectParameter::*, see
## include/audio_engine/bus.h), and value (float). Returns true
## on success.
##
## (v0.32.0 / v0.34.0 note: this wrapper closes a hole where
## ReverbZone called _runtime.set_effect_parameter through the
## autoload's `_runtime` field — which was the autoload Node, not
## the C++ runtime. The call would have errored at runtime when
## a listener entered a zone. ReverbZone's existing call site is
## unchanged; this wrapper makes it actually work.)
func set_effect_parameter(bus_name: String, effect_index: int,
							 param_id: int, value: float) -> bool:
	if not _check_init("set_effect_parameter"):
		return false
	return _runtime.set_effect_parameter(bus_name, effect_index,
										   param_id, value)

## Apply a named preset Dictionary from GoolPresets (or any
## Dictionary in the same shape) to a reverb effect on the named
## bus. The preset's keys are the JSON parameter names ("decay",
## "predelay_ms", "lf_damping", "hf_damping", "diffusion",
## "wet_gain_db", "dry_gain_db"); each gets translated to the
## C++ EffectParameter ID and applied via set_effect_parameter.
##
## Typical example:
##   Gool.apply_reverb_preset("Sfx", 0, GoolPresets.REVERB_CATHEDRAL)
##
## Where 0 is the index of the reverb effect in the Sfx bus's
## effects chain (use get_bus_effects() to discover it, or check
## your gool/config.json). To switch presets at runtime, just call
## again with a different preset — the C++ side smooths transitions
## so the change won't click.
##
## Returns true if every parameter in the preset applied
## successfully. Returns false if the runtime isn't initialized,
## any set_effect_parameter call failed, or any preset key wasn't
## a known reverb parameter. Unknown keys produce a warning so a
## designer typo (e.g. "predelay" missing "_ms") doesn't fail
## silently.
##
## You can pass a partial preset — only the keys present get
## applied, others retain their current values. Useful for tweak
## layering: apply a base preset, then call again with just
## { "wet_gain_db": -6.0 } to dip it.
func apply_reverb_preset(bus_name: String, effect_index: int,
						   preset: Dictionary) -> bool:
	if not _check_init("apply_reverb_preset"):
		return false
	# JSON key → EffectParameter ID. Mirrors GoolPresets._REVERB_PARAM_ID;
	# duplicated as a local const so this method has no hard dependency
	# on the GoolPresets class load order (the autoload runs early).
	const PARAM_ID: Dictionary = {
		"predelay_ms":  23,
		"decay":         9,
		"lf_damping":   24,
		"hf_damping":   10,
		"diffusion":    25,
		"wet_gain_db":  11,
		"dry_gain_db":  26,
	}
	var all_ok: bool = true
	for key in preset:
		var param_id: int = PARAM_ID.get(key, -1)
		if param_id < 0:
			push_warning(("[Gool] apply_reverb_preset: unknown key "
					+ "'%s' on bus='%s' — expected one of: "
					+ "predelay_ms, decay, lf_damping, hf_damping, "
					+ "diffusion, wet_gain_db, dry_gain_db")
					% [key, bus_name])
			all_ok = false
			continue
		var value: float = float(preset[key])
		var ok: bool = _runtime.set_effect_parameter(bus_name,
				effect_index, param_id, value)
		if not ok:
			push_warning(("[Gool] apply_reverb_preset: "
					+ "set_effect_parameter failed for bus='%s' "
					+ "effect=%d param='%s' value=%f — check that "
					+ "the bus exists and effect_index points at a "
					+ "reverb effect (try get_bus_effects to inspect)")
					% [bus_name, effect_index, key, value])
			all_ok = false
	return all_ok

## v0.35.0: forwarder for the engine's get_bus_effects. Returns
## an Array of Dictionaries describing each effect on the named
## bus (kind, kind_name, params keyed by EffectParameter::* IDs).
## Used by the auto-EQ setup paths to verify bus shape, and by
## ReverbZone to discover the reverb effect's chain index.
##
## (Closes the same auto-load hole as set_effect_parameter — the
## v0.32.0 ReverbZone called this through `_runtime` expecting
## it to forward, but the autoload didn't expose it. Now it
## does.)
func get_bus_effects(bus_name: String) -> Array:
	if not _check_init("get_bus_effects"):
		return []
	return _runtime.get_bus_effects(bus_name)

## Apply a preset Dictionary to a sidechain-compressor effect on
## a bus. `preset` keys are the same names used in `gool/config.json`
## ("threshold_db", "ratio", "attack_ms", "release_ms",
## "knee_width_db", "max_reduction_db", plus optional "makeup_db",
## "mix_ratio", "sidechain_hpf_hz", "hold_ms", "detection_mode").
##
## Use the `GoolPresets.COMPRESSOR_*` constants for ready-made starting
## points; the values come straight from
## docs/audio_design/sidechain_tuning.md.
##
## Typical example:
##   Gool.apply_compressor_preset("Music", 0,
##           GoolPresets.COMPRESSOR_ACTION_SHOOTER)
##
## NOTE: this helper does NOT change the sidechain wiring — which
## bus drives which compressor is set in the bus graph (in
## `gool/config.json`) at build time, not at apply time. The preset
## only tunes how aggressively the compressor responds.
##
## Same return semantics as apply_reverb_preset: true if every
## value applied, false if any individual call failed or if the
## runtime isn't initialized. Unknown keys produce push_warning
## and are skipped; partial presets are valid.
func apply_compressor_preset(bus_name: String, effect_index: int,
								preset: Dictionary) -> bool:
	if not _check_init("apply_compressor_preset"):
		return false
	# JSON key → EffectParameter ID. Mirrors GoolPresets._COMPRESSOR_PARAM_ID;
	# duplicated as a local const so this method has no hard dependency on
	# the GoolPresets class load order (same pattern apply_reverb_preset uses).
	const PARAM_ID: Dictionary = {
		"threshold_db":      4,
		"ratio":             5,
		"attack_ms":         6,
		"release_ms":        7,
		"makeup_db":         8,
		"knee_width_db":    13,
		"mix_ratio":        14,
		"max_reduction_db": 15,
		"sidechain_hpf_hz":16,
		"hold_ms":         17,
		"detection_mode":  18,
	}
	var all_ok: bool = true
	for key in preset:
		var param_id: int = PARAM_ID.get(key, -1)
		if param_id < 0:
			push_warning(("[Gool] apply_compressor_preset: unknown key "
					+ "'%s' on bus='%s' — expected one of: %s")
					% [key, bus_name, ", ".join(PARAM_ID.keys())])
			all_ok = false
			continue
		var value: float = float(preset[key])
		var ok: bool = _runtime.set_effect_parameter(bus_name,
				effect_index, param_id, value)
		if not ok:
			push_warning(("[Gool] apply_compressor_preset: "
					+ "set_effect_parameter failed for bus='%s' "
					+ "effect=%d param='%s' value=%f — check that "
					+ "the bus exists and effect_index points at a "
					+ "compressor effect (try get_bus_effects to "
					+ "inspect)") % [bus_name, effect_index, key, value])
			all_ok = false
	return all_ok

## Apply a preset Dictionary to a 3-band EQ chain on a bus. Unlike
## the other apply_*_preset helpers, EQ presets span MULTIPLE biquad
## effects (one per band), so this helper takes per-band effect
## indices instead of a single effect_index.
##
## The preset keys describe a logical EQ shape:
##   low_gain_db, low_freq_hz       → applied to the lowshelf biquad
##   mid_gain_db, mid_freq_hz, mid_q→ applied to the peak biquad
##   high_gain_db, high_freq_hz     → applied to the highshelf biquad
##
## The default indices (low=0, mid=1, high=2) match gool's built-in
## EQ buses (ImpactEq, ListenerEq) which use LowShelf/Peak/HighShelf
## in that order. If your project has a custom EQ chain with a
## different order or band count, pass the matching indices:
##
##   # Bus where lowshelf is at index 2, peak is at 0, highshelf at 1:
##   Gool.apply_eq_preset("MyEq", GoolPresets.EQ_WARM, 2, 0, 1)
##
## Use the `GoolPresets.EQ_*` constants for ready-made starting points.
##
## Returns true if every band+parameter applied successfully. Missing
## bands in the preset (e.g. a preset with only `mid_*` keys) are
## fine — only present bands get applied. Per-band failures push_warning
## and are counted toward the boolean return, same as the other
## apply_*_preset helpers.
func apply_eq_preset(bus_name: String, preset: Dictionary,
					   low_effect_index: int = 0,
					   mid_effect_index: int = 1,
					   high_effect_index: int = 2) -> bool:
	if not _check_init("apply_eq_preset"):
		return false
	# JSON key → EffectParameter ID for biquad params, used per band.
	# Mirrors GoolPresets._BIQUAD_PARAM_ID; local const for the same
	# load-order reason as apply_reverb_preset.
	const BIQUAD_PARAM_ID: Dictionary = {
		"cutoff_hz":       2,    # Biquad_CutoffHz
		"q":               3,    # Biquad_Q
		"biquad_gain_db": 12,    # Biquad_GainDb
	}
	# Per-band sub-presets, in the {biquad_param: value} shape that
	# BIQUAD_PARAM_ID can translate. We build them from the
	# logical-shape keys (low_gain_db, low_freq_hz, ...) so the helper
	# is forgiving about partial presets.
	var low_band: Dictionary = {}
	var mid_band: Dictionary = {}
	var high_band: Dictionary = {}
	if preset.has("low_freq_hz"):
		low_band["cutoff_hz"] = float(preset["low_freq_hz"])
	if preset.has("low_gain_db"):
		low_band["biquad_gain_db"] = float(preset["low_gain_db"])
	if preset.has("mid_freq_hz"):
		mid_band["cutoff_hz"] = float(preset["mid_freq_hz"])
	if preset.has("mid_q"):
		mid_band["q"] = float(preset["mid_q"])
	if preset.has("mid_gain_db"):
		mid_band["biquad_gain_db"] = float(preset["mid_gain_db"])
	if preset.has("high_freq_hz"):
		high_band["cutoff_hz"] = float(preset["high_freq_hz"])
	if preset.has("high_gain_db"):
		high_band["biquad_gain_db"] = float(preset["high_gain_db"])

	var all_ok: bool = true
	all_ok = _apply_biquad_band(bus_name, low_effect_index,  low_band,
			BIQUAD_PARAM_ID) and all_ok
	all_ok = _apply_biquad_band(bus_name, mid_effect_index,  mid_band,
			BIQUAD_PARAM_ID) and all_ok
	all_ok = _apply_biquad_band(bus_name, high_effect_index, high_band,
			BIQUAD_PARAM_ID) and all_ok

	# Warn on unknown logical-shape keys (a typo like "low_gan_db" gets
	# caught here, after the rest of the work is done).
	const VALID_KEYS: Array = ["low_gain_db", "low_freq_hz",
			"mid_gain_db", "mid_freq_hz", "mid_q",
			"high_gain_db", "high_freq_hz"]
	for key in preset:
		if not VALID_KEYS.has(key):
			push_warning(("[Gool] apply_eq_preset: unknown key '%s' "
					+ "on bus='%s' — expected one of: %s")
					% [key, bus_name, ", ".join(VALID_KEYS)])
			all_ok = false
	return all_ok

# Internal: apply a {biquad_param_name: value} dict to a single
# biquad effect. Called three times by apply_eq_preset, once per band.
# Returns true if every value applied (or the dict was empty).
func _apply_biquad_band(bus_name: String, effect_index: int,
						   band: Dictionary,
						   param_id_map: Dictionary) -> bool:
	var all_ok: bool = true
	for key in band:
		var param_id: int = param_id_map.get(key, -1)
		if param_id < 0:
			push_warning(("[Gool] apply_eq_preset (internal): unknown "
					+ "biquad key '%s' — bug in apply_eq_preset; "
					+ "report this") % key)
			all_ok = false
			continue
		var value: float = float(band[key])
		var ok: bool = _runtime.set_effect_parameter(bus_name,
				effect_index, param_id, value)
		if not ok:
			push_warning(("[Gool] apply_eq_preset: "
					+ "set_effect_parameter failed for bus='%s' "
					+ "effect=%d param='%s' value=%f — check that "
					+ "the bus exists and effect_index points at a "
					+ "biquad effect of the right type "
					+ "(LowShelf/Peak/HighShelf)")
					% [bus_name, effect_index, key, value])
			all_ok = false
	return all_ok

## Apply a preset Dictionary to a saturation effect on a bus.
## `preset` keys are the same names used in `gool/config.json`
## ("drive", "mix", "output_gain", "bias", "mode"). Mode is an int
## (0=Tanh, 1=Tube, 2=Tape, 3=Diode); pass it as a float in the
## dictionary and the helper will coerce.
##
## Use the `GoolPresets.SATURATION_*` constants for ready-made
## starting points.
##
## Typical example:
##   Gool.apply_saturation_preset("Sfx", 1,
##           GoolPresets.SATURATION_RADIO_CRUSH)
##
## Same return / warning semantics as apply_reverb_preset.
func apply_saturation_preset(bus_name: String, effect_index: int,
								preset: Dictionary) -> bool:
	if not _check_init("apply_saturation_preset"):
		return false
	# Mirrors GoolPresets._SATURATION_PARAM_ID; same load-order pattern.
	const PARAM_ID: Dictionary = {
		"drive":        19,    # Saturation_Drive
		"mix":          20,    # Saturation_Mix
		"output_gain":  21,    # Saturation_OutputGain
		"bias":         22,    # Saturation_Bias
		"mode":         27,    # Saturation_Mode (int)
	}
	var all_ok: bool = true
	for key in preset:
		var param_id: int = PARAM_ID.get(key, -1)
		if param_id < 0:
			push_warning(("[Gool] apply_saturation_preset: unknown key "
					+ "'%s' on bus='%s' — expected one of: %s")
					% [key, bus_name, ", ".join(PARAM_ID.keys())])
			all_ok = false
			continue
		var value: float = float(preset[key])
		var ok: bool = _runtime.set_effect_parameter(bus_name,
				effect_index, param_id, value)
		if not ok:
			push_warning(("[Gool] apply_saturation_preset: "
					+ "set_effect_parameter failed for bus='%s' "
					+ "effect=%d param='%s' value=%f — check that "
					+ "the bus exists and effect_index points at a "
					+ "saturation effect (try get_bus_effects to "
					+ "inspect)") % [bus_name, effect_index, key, value])
			all_ok = false
	return all_ok

## v0.43.0: capture the current bus parameter state into a
## GoolMixSnapshot Resource. Pass the bus names you want captured
## (typically the ones whose state you'll later restore — e.g.
## ["Music", "LocalSfx", "RemoteSfx"]). Returns a fresh Resource
## that can be saved via ResourceSaver or kept in memory for later
## apply_mix_snapshot().
##
## Buses that don't exist in the graph are skipped with a
## push_warning; the snapshot still captures the ones that do.
## Returns null if the runtime isn't initialized.
##
## Usage:
##   var combat := Gool.capture_mix_snapshot(
##           PackedStringArray(["Music", "LocalSfx"]))
##   # ... game runs, bus parameters change ...
##   Gool.apply_mix_snapshot(combat)   # restore exact captured state
##
## Or author once and save as a .tres:
##   var stealth := Gool.capture_mix_snapshot(
##           PackedStringArray(["Music", "LocalSfx",
##                              "RemoteSfx", "Dialogue"]))
##   stealth.label = "stealth"
##   ResourceSaver.save(stealth, "res://mixes/stealth.tres")
func capture_mix_snapshot(bus_names: PackedStringArray) -> GoolMixSnapshot:
	if not _check_init("capture_mix_snapshot"):
		return null
	return GoolMixSnapshot.capture_from(bus_names)

## v0.43.0: apply a previously-captured GoolMixSnapshot, restoring
## every captured (bus, effect, param) triple in one call. Returns
## true if every parameter applied successfully; false if any
## individual set_effect_parameter failed (snapshot may be stale
## relative to the current bus graph) or if the runtime isn't
## initialized.
##
## Per-parameter changes go through gool's normal set_effect_parameter
## path, which means most gain-type parameters ramp ~5 ms internally
## at the C++ level. The perceived transition is smooth in practice
## for ducking-sensitive parameters. If you need a guaranteed long
## crossfade (e.g. 2-second mood shift), that's a Tier-2 capability
## gool doesn't yet have — author intermediate snapshots and apply
## them in sequence as a workaround.
func apply_mix_snapshot(snap: GoolMixSnapshot) -> bool:
	if not is_initialized() or snap == null:
		return false
	return snap.apply()

## v0.37.0: forwarder for the engine's set_master_volume_db.
## Sets the post-mixdown master volume in decibels. Used by the
## GoolAudioSettings persistence helper (Phase 4 settings menu)
## and by anyone wiring a volume slider to overall game audio.
##
## (Closes a hole where the C++ binding exposed this but the
## autoload didn't wrap it. Anyone trying to call
## Gool.set_master_volume_db before v0.37.0 would have hit
## "Nonexistent function".)
func set_master_volume_db(db: float) -> void:
	if not _check_init("set_master_volume_db"):
		return
	_runtime.set_master_volume_db(db)

## v0.37.0: forwarder for the engine's set_bus_gain_db. Sets the
## gain of a named bus in decibels.
##
## Takes the bus *name* (String), not the BusId. The engine
## resolves the name internally each call — fine for menu-driven
## use (a few writes per second at most). For high-frequency
## writes, hold a BusId from find_bus_id_by_name() and call the
## engine's bus_id-based path directly via _runtime.
##
## (Same auto-load wrapper hole as set_master_volume_db.)
func set_bus_gain_db(bus_name: String, gain_db: float) -> void:
	if not _check_init("set_bus_gain_db"):
		return
	_runtime.set_bus_gain_db(bus_name, gain_db)

## Toggle occlusion globally at runtime.
##
## Useful for accessibility settings ("disable audio occlusion"
## for players who find muffling disorienting) or for situations
## where the entire game briefly needs flat acoustics (think
## menu screens, cutscenes with critical dialogue).
##
## When disabled, the engine stops raycasting; per-emitter
## occlusion smooths back to 0 over ~150 ms so there's no
## discontinuity. Re-enabling resumes the same gentle ramp.
##
## The default (enabled=true) and the initial intensity come from
## the project settings under Gool → Occlusion. Calling this
## doesn't write back to project settings — it's a runtime
## override for this session.
func set_occlusion_enabled(enabled: bool) -> void:
	if not _check_init("set_occlusion_enabled"):
		return
	_runtime.set_occlusion_enabled(enabled)

## Dial the global occlusion intensity multiplier.
##
##   0.0        bypass — audible occlusion off
##   0.4-0.6    conservative, prioritises clarity
##   0.7        default — present, not aggressive
##   1.0        physically realistic per-material defaults
##   1.5-2.0    exaggerated — surreal/horror, "the room is wrong"
##
## Applied as a multiplier on per-material absorption + damping
## after the geometry query resolves a hit. Materials with strong
## defaults (concrete) saturate first as intensity rises above 1.
##
## Safe to call mid-game — the ~150 ms smoother handles transitions
## cleanly. Useful for dramatic moments: bump intensity during a
## cutscene corridor, drop it during a critical conversation.
func set_occlusion_intensity(intensity: float) -> void:
	if not _check_init("set_occlusion_intensity"):
		return
	_runtime.set_occlusion_intensity(intensity)

## v0.44.2: pass the physics world's space RID to the audio runtime
## so geometry-query / raycast-based occlusion knows which physics
## world to query.
##
## GoolListener3D._ready() calls this — but before v0.44.2 the
## autoload had no wrapper, so the prefab's
## `if _runtime.has_method("set_audio_world_space_rid"): ...` guard
## returned false and the call was silently skipped. Net effect:
## the RID was never set, occlusion queries had no physics world
## to consult, and the feature silently no-op'd in every scene
## using gool_listener_3d. This wrapper closes that loop.
func set_audio_world_space_rid(rid: RID) -> void:
	if not _check_init("set_audio_world_space_rid"):
		return
	_runtime.set_audio_world_space_rid(rid)

func play_sound_at_location(name: String, position: Vector3) -> void:
	if not _check_init("play_sound_at_location"):
		return
	_runtime.play_sound_at_location(name, position)

## Load a JSON sound bank into the runtime.
##
## `json_string` is the JSON text — usually from a .json file in
## res://, loaded via FileAccess.get_file_as_string() or a packed
## resource. See docs/asset_pipeline.md for the full schema.
##
## `gpak_path` is an optional .gpak archive path for bundled
## binary audio assets. Empty string (default) means the bank
## reads files via the standard filesystem.
##
## `skip_validation` (default false): if true, the bank doesn't
## require its groups' member sound names to be declared in the
## same JSON. Use this when authoring a "group-only" bank — a
## small JSON file with just groups, referencing sounds the
## runtime knows about from somewhere else (programmatic
## registration via register_pcm_sound / register_sound_from_stream,
## or a separate bank already loaded). Unknown member names
## hash to ids that the runtime resolves at play time — if
## the runtime has the sound, it plays; if not, silently nothing
## (the lenient rule from docs/cookbook.md section 11).
##
## Returns true on success, false on parse or validation error.
## On failure, an error with the line number is pushed via
## push_error so it's visible in the editor's output panel.
func load_sound_bank_from_json(json_string: String,
								 gpak_path: String = "",
								 skip_validation: bool = false) -> bool:
	if not _check_init("load_sound_bank_from_json"):
		return false
	return _runtime.load_sound_bank_from_json(
			json_string, gpak_path, skip_validation)

## AudioMaterial taxonomy (Phase 5.1).
##
## Mirrors the C++ `audio::AudioMaterial` enum in
## `include/audio_engine/geometry_query.h`. Use these values in
## `play_impact_sound` calls and as the `material` field on
## `GoolAudioMaterial` resources tagging your level geometry.
##
## Designers: tag a CollisionObject3D / Area3D / StaticBody3D with
## its surface material by either setting the `gool_audio_material`
## metadata to one of these constants in the inspector, or assigning
## a GoolAudioMaterial resource to the node. `material_from_collider`
## reads either path.
const MATERIAL_DEFAULT:   int = 0   ## unknown / fallback
const MATERIAL_AIR:       int = 1   ## pass-through (no surface)
const MATERIAL_GLASS:     int = 2
const MATERIAL_WOOD:      int = 3
const MATERIAL_DRYWALL:   int = 4
const MATERIAL_CONCRETE:  int = 5
const MATERIAL_METAL:     int = 6
const MATERIAL_CURTAIN:   int = 7
const MATERIAL_FOLIAGE:   int = 8
const MATERIAL_MEAT:      int = 9   ## soft, dense, wet — creature bodies
const MATERIAL_CARDBOARD: int = 10  ## light, porous, papery — boxes
const MATERIAL_RUBBER:    int = 11  ## dense, soft, dead — tires, mats
const MATERIAL_LIQUID:    int = 12  ## wet surface — water, blood, slime

const _MATERIAL_NAMES := [
	"Default", "Air", "Glass", "Wood", "Drywall",
	"Concrete", "Metal", "Curtain", "Foliage", "Meat",
	"Cardboard", "Rubber", "Liquid",
]

# v0.49.0: custom material registry.
#
# Built-in materials (IDs 0-12) live in the C++ engine. To add a
# new material without forking the engine, register one here —
# custom material IDs start at MATERIAL_CUSTOM_BASE (100) so they
# never collide with built-ins. The registry is checked BEFORE
# delegating to the C++ runtime in get_reverb_preset_for_material,
# get_material_eq_for_material, and play_impact_sound.
#
# Registry entry shape:
#   {
#     "name": "Wet Stone",
#     "eq": { "low_gain_db": ..., "low_freq_hz": ..., ... },
#     "reverb_preset": { "decay": ..., "lf_damping": ..., ... },
#     "impact_sound_suffix": "wet_stone",  # for play_impact_sound name lookup
#   }
#
# Built-in materials still use the C++ path — this is purely an
# extension mechanism, not a replacement for the engine-side table.
const MATERIAL_CUSTOM_BASE: int = 100
var _custom_materials: Dictionary = {}
var _next_custom_material_id: int = MATERIAL_CUSTOM_BASE

## Return the string name of an AudioMaterial, e.g.
## `material_name(Gool.MATERIAL_CONCRETE)` returns `"Concrete"`.
## Returns `"Unknown"` for out-of-range values. Useful for debug
## overlays and logging.
##
## v0.49.0: also resolves custom-registered materials (IDs >= 100).
func material_name(material: int) -> String:
	if not _check_init("material_name"):
		return ""
	if material >= 0 and material < _MATERIAL_NAMES.size():
		return _MATERIAL_NAMES[material]
	if _custom_materials.has(material):
		return String(_custom_materials[material].get("name", "CustomMaterial%d" % material))
	return "Unknown"

## v0.49.0: register a custom material at runtime. Returns the
## allocated material ID (>= MATERIAL_CUSTOM_BASE = 100). Use the
## returned ID anywhere a built-in MATERIAL_* constant works —
## ReverbZone, AudioMaterialTag, play_impact_sound, etc.
##
## opts keys:
##   - "name" (String): human-readable label for debug overlays.
##     Default: "CustomMaterial<id>".
##   - "eq" (Dictionary): the per-material EQ curve. Same shape as
##     get_material_eq_for_material returns:
##       low_gain_db, low_freq_hz, mid_gain_db, mid_freq_hz, mid_q,
##       high_gain_db, high_freq_hz, is_neutral (optional bool)
##     Default: empty (treated as neutral / no coloring).
##   - "reverb_preset" (Dictionary): tail-shape values, same shape
##     as get_reverb_preset_for_material returns:
##       decay, predelay_ms, lf_damping, hf_damping, diffusion,
##       wet_gain_db, send_hpf_hz, return_lpf_hz
##     Defaults to {} (ReverbZone falls back to its @export
##     defaults if a key is missing).
##   - "impact_sound_suffix" (String): used by play_impact_sound to
##     construct the actual sound bank name. If set, an impact
##     call like `play_impact_sound("footstep", pos, my_id)` looks
##     up the sound `"footstep_<suffix>"` in the bank instead of
##     calling the C++ material-aware path. Default: "" (uses the
##     bare `name` argument as the sound name).
##
## Example — register "Wet Stone" using the existing Cave reverb
## preset but a bespoke EQ curve:
##
##     var wet_stone_id = Gool.register_material({
##         "name": "Wet Stone",
##         "eq": {
##             "low_gain_db": 0.5, "low_freq_hz": 200.0,
##             "mid_gain_db": -2.0, "mid_freq_hz": 1500.0, "mid_q": 0.7,
##             "high_gain_db": -3.5, "high_freq_hz": 6000.0,
##         },
##         "reverb_preset": GoolPresets.REVERB_CAVE,
##         "impact_sound_suffix": "wet_stone",
##     })
##     # Then in level scripts:
##     reverb_zone.material = wet_stone_id
##     # Or for AudioMaterialTag:
##     audio_tag.material = wet_stone_id
##     # Or for one-shot impacts:
##     Gool.play_impact_sound("footstep", hit_pos, wet_stone_id)
##     # Requires you've registered a sound named "footstep_wet_stone"
##     # in your bank (or matching whatever suffix you chose).
func register_material(opts: Dictionary) -> int:
	if not _check_init("register_material"):
		return MATERIAL_DEFAULT
	var id: int = _next_custom_material_id
	_next_custom_material_id += 1
	var name_default: String = "CustomMaterial%d" % id
	_custom_materials[id] = {
		"name": String(opts.get("name", name_default)),
		"eq": opts.get("eq", {}) if opts.get("eq", {}) is Dictionary else {},
		"reverb_preset": opts.get("reverb_preset", {}) if opts.get("reverb_preset", {}) is Dictionary else {},
		"impact_sound_suffix": String(opts.get("impact_sound_suffix", "")),
	}
	return id

## v0.49.0: remove a custom material by ID. No-op if the ID isn't
## registered or if it's a built-in (IDs 0-12). Materials still
## referenced by ReverbZone / AudioMaterialTag / etc. will fall
## through to Default after unregistration.
func unregister_material(id: int) -> bool:
	if not _check_init("unregister_material"):
		return false
	if id < MATERIAL_CUSTOM_BASE:
		return false
	if not _custom_materials.has(id):
		return false
	_custom_materials.erase(id)
	return true

## v0.49.0: return all custom material IDs currently registered.
## Useful for debug UIs that enumerate the material catalog.
func get_custom_material_ids() -> Array:
	if not _check_init("get_custom_material_ids"):
		return []
	return _custom_materials.keys()

## Return the engine's per-material reverb preset as a Dictionary
## with keys: `decay`, `lf_damping`, `hf_damping`, `diffusion` (all
## floats in [0, 1]).
##
## These are the values that paint each material's acoustic
## character on a reverb bus — concrete corridors have long bright
## decay, wooden cabins have shorter mid-rich tails, foliage has
## very damp short tails. The ReverbZone prefab consumes these
## automatically when its `material` is set; this function is for
## designers who want to apply presets programmatically (custom
## triggers, level-script reverb changes, etc).
##
## Example — push the concrete preset onto the Sfx bus's reverb:
##
##     var preset = Gool.get_reverb_preset_for_material(Gool.MATERIAL_CONCRETE)
##     # Effect index 0 = first effect in the chain (the reverb in
##     # the standard gool config). set_effect_parameter takes the
##     # bus NAME, not its BusId.
##     Gool._runtime.set_effect_parameter("Sfx", 0, 9,  preset.decay)
##     Gool._runtime.set_effect_parameter("Sfx", 0, 24, preset.lf_damping)
##     Gool._runtime.set_effect_parameter("Sfx", 0, 10, preset.hf_damping)
##     Gool._runtime.set_effect_parameter("Sfx", 0, 25, preset.diffusion)
##
## Out-of-range material values fall through to the Default
## preset ("average room").
##
## v0.44.1 rename: previously exposed as `reverb_preset_for_material`
## (no `get_` prefix), which didn't match the C++ binding name OR
## the ReverbZone prefab's callsite — so material-aware ReverbZones
## (material != 0) tripped a "Nonexistent function" error at runtime.
## The old name is preserved as a deprecated alias below for one
## release; migrate calls to this `get_` form.
func get_reverb_preset_for_material(material: int) -> Dictionary:
	if not _check_init("get_reverb_preset_for_material"):
		return {}
	# v0.49.0: custom materials take precedence. Built-in IDs (0-12)
	# fall through to the C++ engine table.
	if _custom_materials.has(material):
		var entry: Dictionary = _custom_materials[material]
		return entry.get("reverb_preset", {})
	return _runtime.get_reverb_preset_for_material(material)

## DEPRECATED (v0.44.1): use get_reverb_preset_for_material instead.
## Kept as a one-line alias for one release so any user code calling
## the pre-v0.44.1 name still works. Will be removed in v0.46.0.
func reverb_preset_for_material(material: int) -> Dictionary:
	return get_reverb_preset_for_material(material)

## Return the engine's per-material EQ curve as a Dictionary with
## keys: `low_gain_db`, `low_freq_hz`, `mid_gain_db`, `mid_freq_hz`,
## `mid_q`, `high_gain_db`, `high_freq_hz`, plus a convenience
## flag `is_neutral` (true for Air/Default materials where every
## band gain is ~0 dB — consumers should skip EQ entirely in this
## case rather than installing a no-op chain).
##
## These curves are the perceptual fingerprint of each material:
## concrete's upper-mid bite at 1.5 kHz, wood's warm 500 Hz body,
## curtain's broad HF cut, etc. Phase 6.A (this release) ships the
## table; Phase 6.B will wire it into the impact playback path so
## a sound played as an impact on Concrete automatically gets the
## Concrete EQ applied. Phase 6.C will wire it into the listener-
## space (reverb-zone) path so the room's material colors
## everything you hear inside it.
##
## For now, designers apply the curve manually by setting up a
## chain of 3 Biquad effects (LowShelf / Peaking / HighShelf) on
## a bus and pushing the returned values via set_effect_parameter.
## See docs/cookbook.md section 14 for the full walkthrough.
##
## Out-of-range material values fall through to the Default
## (neutral) curve.
##
## v0.44.2 rename: previously exposed as `material_eq_for_material`
## (no `get_` prefix), the same naming inconsistency that broke
## ReverbZone's material-aware path in v0.44.1. Renamed for the
## same reason — matches the C++ binding and every other gool
## getter. The old name is preserved as a deprecated alias below
## for one release; migrate calls to this `get_` form.
func get_material_eq_for_material(material: int) -> Dictionary:
	if not _check_init("get_material_eq_for_material"):
		return {}
	# v0.49.0: custom materials take precedence. Built-in IDs (0-12)
	# fall through to the C++ engine table.
	if _custom_materials.has(material):
		var entry: Dictionary = _custom_materials[material]
		return entry.get("eq", {})
	return _runtime.get_material_eq_for_material(material)

## DEPRECATED (v0.44.2): use get_material_eq_for_material instead.
## Kept as a one-line alias for one release so any user code calling
## the pre-v0.44.2 name still works. Will be removed in v0.46.0.
func material_eq_for_material(material: int) -> Dictionary:
	return get_material_eq_for_material(material)

## v0.60.0: same as material_from_collider but returns the
## GoolAudioMaterial Resource when one is set as collider metadata,
## preserving its override_enabled / per-band override fields.
## Falls back to int (MATERIAL_DEFAULT or group lookup) when no
## resource is set.
##
## Designers who want .tres-authored EQ overrides to flow through
## play_impact_sound should use this instead of
## material_from_collider:
##
##   var mat = Gool.material_resource_from_collider(hit.collider)
##   Gool.play_impact_sound("bullet_impact", hit.position, mat)
##
## play_impact_sound accepts both forms (int or Resource).
## material_from_collider() continues to return just int and is
## the right choice for callers that don't need override values.
##
## Returns: GoolAudioMaterial Resource when collider metadata is
## one; otherwise int (MATERIAL_DEFAULT for unknown).
func material_resource_from_collider(node: Node) -> Variant:
	if node == null:
		return MATERIAL_DEFAULT
	# 1. Metadata path: prefer the resource form (preserves overrides).
	if node.has_meta("gool_audio_material"):
		var meta = node.get_meta("gool_audio_material")
		if meta is GoolAudioMaterial:
			return meta
		if meta is int:
			return meta
		# Duck-type fallback for older custom resource scripts
		# without class_name. Same conservative path
		# material_from_collider uses.
		if meta != null and "material" in meta:
			return int(meta.material)
	# 2. Group fallback — group is a string tag, no override fields
	# to preserve, so return as int.
	for i in range(_MATERIAL_NAMES.size()):
		if node.is_in_group("audio_material:" + _MATERIAL_NAMES[i]):
			return i
	return MATERIAL_DEFAULT


## Resolve a Node's AudioMaterial. Checks two sources in order:
##
##   1. `gool_audio_material` metadata. Can be either an int
##      (one of the MATERIAL_* constants) or a GoolAudioMaterial
##      resource — both are accepted.
##   2. Group membership of the form `audio_material:Concrete`,
##      `audio_material:Wood`, etc. Provided for backward compat
##      with the FootstepSurfacePlayer pattern; new content should
##      prefer the metadata or resource path.
##
## Returns MATERIAL_DEFAULT if neither source identifies a material
## (or if `node` is null). The lenient default means downstream
## `play_impact_sound` falls back to the Default bucket if the
## bank defines one, or silently does nothing if not.
func material_from_collider(node: Node) -> int:
	if node == null:
		return MATERIAL_DEFAULT
	# 1. Metadata path: explicit `gool_audio_material` meta. Accept
	# either an int (constant) or a GoolAudioMaterial resource.
	if node.has_meta("gool_audio_material"):
		var meta = node.get_meta("gool_audio_material")
		if meta is int:
			return meta
		# Duck-type: a GoolAudioMaterial resource has a `material`
		# property holding the int. Avoid hard typing here so the
		# resource script can be reloaded / replaced without
		# tripping a type mismatch.
		if meta != null and "material" in meta:
			return int(meta.material)
	# 2. Group fallback: audio_material:<MaterialName>.
	for i in range(_MATERIAL_NAMES.size()):
		if node.is_in_group("audio_material:" + _MATERIAL_NAMES[i]):
			return i
	return MATERIAL_DEFAULT

## Play a one-shot impact sound at `position`, picking the variant
## from the named bank group's bucket for `material`. If the group
## has no bucket for that material, falls back to the group's
## "Default" bucket; if that's also missing, plays nothing (the
## lenient rule — see docs/asset_pipeline.md).
##
## v0.34.0 (Phase 6.B): when an impact-EQ bus is configured (the
## project setting `gool/material_eq/impact_bus`, default "ImpactEq")
## and the bus has the conventional 3-biquad shape (LowShelf →
## Peak → HighShelf), the material's EQ curve is automatically
## pushed to that bus before playback. Concrete impacts get the
## upper-mid bite, wood impacts get the warm low-mid body, foliage
## impacts get the broadband softness — without designer setup
## beyond the bus authoring.
##
## If the bus doesn't exist or doesn't match the convention, the
## auto-EQ behavior is silently disabled and the impact plays
## through its normal bus routing without per-material coloration.
## A single warning at startup explains why.
##
## Typical use in a weapon's _try_fire:
##   var hit = space_state.intersect_ray(query)
##   if hit:
##       var mat := Gool.material_from_collider(hit.collider)
##       Gool.play_impact_sound("bullet_impact", hit.position, mat)
##
## For non-by_material groups (or plain sounds), `material` is
## ignored and behavior matches `play_sound_at_location`.
## v0.60.0: `material` accepts either an int (legacy, fast C++ path)
## OR a GoolAudioMaterial Resource (new, preserves override fields).
## Passing the resource is how designers get per-instance EQ tweaks
## (override_enabled=true on the .tres) routed through this call.
## Passing an int continues to work unchanged — backward compat for
## every existing caller.
##
## When given a Resource with override_enabled=false, behavior is
## identical to passing `.material` (the int): zero override cost,
## C++ fast path. When given a Resource with override_enabled=true,
## the per-band override values flow through the GDScript-side
## apply helper (same path v0.36.0 intensity scaling uses).
func play_impact_sound(name: String, position: Vector3,
		material) -> void:
	if not _check_init("play_impact_sound"):
		return

	# v0.60.0: unwrap Resource → (material_int, override_curve_or_null).
	# Override-disabled resources fall through to the int path so
	# they take the C++ fast path; only override-enabled resources
	# get the slower GDScript apply.
	var material_int: int = MATERIAL_DEFAULT
	var override_curve: Dictionary = {}
	if material is int:
		material_int = material
	elif material is GoolAudioMaterial:
		material_int = material.material
		if material.override_enabled:
			override_curve = material.get_curve()
	else:
		push_warning("[gool] play_impact_sound: invalid material "
				+ "argument (expected int or GoolAudioMaterial, "
				+ "got %s). Defaulting to MATERIAL_DEFAULT."
				% typeof(material))
		return

	# v0.49.0: custom material path. Custom materials live entirely
	# in the GDScript registry — the C++ engine doesn't know about
	# them, so we can't call play_sound_at_location_for_material.
	# Instead, construct the effective sound name from the suffix
	# and route through play_3d. EQ is applied via the scaled helper
	# using the registered custom EQ dict (the C++
	# apply_material_eq_to_bus would fall through to Default for an
	# unrecognized material ID).
	if _custom_materials.has(material_int):
		var entry: Dictionary = _custom_materials[material_int]
		var suffix: String = String(entry.get("impact_sound_suffix", ""))
		var effective_name: String = name
		if not suffix.is_empty():
			effective_name = "%s_%s" % [name, suffix]
		# EQ apply: only if we have a configured impact bus AND the
		# custom EQ dict isn't empty/neutral. Uses the scaled helper
		# so the eq_intensity slider still applies.
		var custom_eq: Dictionary = entry.get("eq", {})
		if _impact_eq_bus_name != "" and not custom_eq.is_empty() \
				and not bool(custom_eq.get("is_neutral", false)):
			_apply_custom_material_eq_to_bus(_impact_eq_bus_name, custom_eq)
		play_3d(effective_name, position, 128)
		return

	# v0.60.0: per-instance override path. The resource provided a
	# custom curve via override_enabled=true. Apply it through the
	# same GDScript helper the custom material path uses (intensity
	# scaling included). We use the curve directly rather than
	# routing through _apply_scaled_material_eq_to_bus(int) so the
	# engine table doesn't get queried again.
	if not override_curve.is_empty():
		if _impact_eq_bus_name != "" \
				and not bool(override_curve.get("is_neutral", false)):
			_apply_custom_material_eq_to_bus(_impact_eq_bus_name, override_curve)
		# Sound variant lookup still uses the material int (so a
		# Concrete override still picks Concrete impact variants).
		_runtime.play_sound_at_location_for_material(name, position, material_int)
		return

	# v0.34.0: push the material's EQ curve to the impact bus
	# before play. The C++ method is a no-op for unrecognized
	# bus configurations, but our _setup_impact_eq has already
	# verified the bus is shaped right at startup, so this is
	# the fast path (write 7 atomic params, then play).
	#
	# Skip the apply call entirely for materials whose curves are
	# neutral (Air, Default) — pushing zeros for those is correct
	# but wasteful, and skipping also avoids relaxing the EQ for
	# an unrelated impact that was just colored for, say, Concrete.
	# (If a Concrete impact at t=0 sets the EQ, and a Default
	# impact at t=10ms reset it to flat, the Concrete impact's
	# tail would lose its color. The "skip Default" rule preserves
	# the most recent non-neutral material's coloring until another
	# non-neutral impact overwrites it.)
	#
	# v0.36.0 (Phase 6.D): if the realism multiplier isn't 1.0,
	# route through the scaled GDScript helper (applies intensity
	# to the three gain bands). At intensity exactly 1.0, the
	# C++ binding is cheaper and the result identical.
	if _impact_eq_bus_name != "" \
			and material_int != MATERIAL_DEFAULT \
			and material_int != MATERIAL_AIR:
		if abs(_eq_intensity - 1.0) < 0.001:
			_runtime.apply_material_eq_to_bus(_impact_eq_bus_name, material_int)
		else:
			_apply_scaled_material_eq_to_bus(_impact_eq_bus_name, material_int)
	_runtime.play_sound_at_location_for_material(name, position, material_int)

# v0.49.0: apply a custom-material EQ dict to a bus. Used by
# play_impact_sound for custom materials. Mirrors the structure of
# _apply_scaled_material_eq_to_bus but reads from the supplied
# dict instead of get_material_eq_for_material(id). The bus is
# expected to have the 3-biquad shape from cookbook section 14
# (LowShelf at index 0, Peaking at 1, HighShelf at 2).
func _apply_custom_material_eq_to_bus(bus_name: String, eq: Dictionary) -> void:
	var low_gain_db: float = float(eq.get("low_gain_db", 0.0)) * _eq_intensity
	var low_freq_hz: float = float(eq.get("low_freq_hz", 200.0))
	var mid_gain_db: float = float(eq.get("mid_gain_db", 0.0)) * _eq_intensity
	var mid_freq_hz: float = float(eq.get("mid_freq_hz", 1000.0))
	var mid_q: float = float(eq.get("mid_q", 0.707))
	var high_gain_db: float = float(eq.get("high_gain_db", 0.0)) * _eq_intensity
	var high_freq_hz: float = float(eq.get("high_freq_hz", 6000.0))
	# Effect param IDs (BiquadFilter): 2=cutoff_hz, 3=q, 12=gain_db.
	_runtime.set_effect_parameter(bus_name, 0, 12, low_gain_db)
	_runtime.set_effect_parameter(bus_name, 0, 2, low_freq_hz)
	_runtime.set_effect_parameter(bus_name, 1, 12, mid_gain_db)
	_runtime.set_effect_parameter(bus_name, 1, 2, mid_freq_hz)
	_runtime.set_effect_parameter(bus_name, 1, 3, mid_q)
	_runtime.set_effect_parameter(bus_name, 2, 12, high_gain_db)
	_runtime.set_effect_parameter(bus_name, 2, 2, high_freq_hz)

# v0.34.0 (Phase 6.B): one-shot check at _ready to determine
# whether the auto-EQ for impacts is viable. Reads the configured
# bus name from project settings, registers the setting if absent,
# verifies the bus has at least 3 effects of kind BiquadFilter at
# indices 0, 1, 2. If anything's off, warns once and leaves
# `_impact_eq_bus_name` empty (auto-EQ disabled).
func _setup_impact_eq() -> void:
	# Register the project setting on first run so it appears
	# editable under Project Settings → General → Gool → Material Eq.
	# Default "ImpactEq" is the conventional name for the EQ bus,
	# but the FEATURE is opt-in: you opt in by adding a bus with
	# that name + the right 3-biquad shape to your gool config.
	# Just leaving the default doesn't enable it.
	if not ProjectSettings.has_setting(_IMPACT_EQ_BUS_SETTING):
		ProjectSettings.set_setting(_IMPACT_EQ_BUS_SETTING, "ImpactEq")
		ProjectSettings.set_initial_value(_IMPACT_EQ_BUS_SETTING, "ImpactEq")
	var configured: String = ProjectSettings.get_setting(_IMPACT_EQ_BUS_SETTING, "ImpactEq")
	if configured == "":
		# Designer explicitly disabled auto-EQ. No warning — this is
		# a valid choice (e.g. for projects that handle material
		# coloring through some other mechanism).
		return

	# v0.55.0: cheap existence check via the cached bus list. The
	# pre-v0.55.0 path called _runtime.get_bus_effects(configured)
	# immediately, which triggers the C++ side's "unknown bus" log
	# on every fresh install (no project ships with an ImpactEq bus
	# by default — not even the FPS template). Now we use the cache
	# and stay silent when the bus is missing, matching the v0.35.0
	# listener_eq graceful-degradation pattern. The feature being
	# opt-in means missing-bus is the EXPECTED default state, not
	# a configuration error worth warning about.
	if not has_bus(configured):
		return

	# Bus exists — now verify its effect chain matches the contract.
	# If the chain is misshapen (wrong count, wrong kinds), THAT is
	# a real misconfiguration worth a warning: the designer added
	# the bus on purpose but got the contract wrong.
	var effects: Array = _runtime.get_bus_effects(configured)
	if effects.is_empty():
		# Bus exists in config but has no effects yet. Same opt-in
		# logic: stay quiet. Designer either hasn't finished adding
		# the chain or wants the bus for some other purpose.
		return
	if effects.size() < 3:
		push_warning(
			"[gool] Phase 6.B impact EQ disabled: bus '%s' " % configured
			+ "has only %d effect(s); need at least 3 biquads. " % effects.size()
			+ "See docs/cookbook.md section 14."
		)
		return
	for i in range(3):
		var e: Dictionary = effects[i]
		var kind_name: String = String(e.get("kind_name", ""))
		# The engine's effect-kind enum is `BiquadFilter`; that's
		# the literal string get_bus_effects returns in kind_name.
		# A v0.34.0 bug checked for "Biquad" instead, which caused
		# the auto-EQ to disable itself with a misleading warning
		# on a correctly-authored bus. Fixed here.
		if kind_name != "BiquadFilter":
			push_warning(
				"[gool] Phase 6.B impact EQ disabled: bus '%s' " % configured
				+ "effect #%d is '%s', expected 'BiquadFilter'. " % [i, kind_name]
				+ "The first three effects on the impact EQ bus must "
				+ "be biquads in order LowShelf → Peak → HighShelf. "
				+ "See docs/cookbook.md section 14."
			)
			return

	# All checks passed. Cache the bus name; play_impact_sound
	# will use it on every impact from here on.
	_impact_eq_bus_name = configured
	GoolLog.info("runtime", "phase 6.B impact eq enabled",
		{"bus": configured})

# v0.35.0 (Phase 6.C): one-shot check at _ready for the listener-
# space EQ bus. The bus is expected to sit between Sfx (and other
# diegetic buses) and Master, with three biquads (LowShelf, Peak,
# HighShelf in that order) as its effects, identical authoring
# contract to the impact EQ bus.
#
# Result is cached in _listener_eq_bus_name. ReverbZones with
# apply_listener_eq=true read this; if empty, they silently skip
# the listener-EQ ramp and only the reverb portion of the zone
# applies. Same graceful-degradation pattern as 6.B.
func _setup_listener_eq() -> void:
	if not ProjectSettings.has_setting(_LISTENER_EQ_BUS_SETTING):
		ProjectSettings.set_setting(_LISTENER_EQ_BUS_SETTING, "ListenerEq")
		ProjectSettings.set_initial_value(_LISTENER_EQ_BUS_SETTING, "ListenerEq")
	var configured: String = ProjectSettings.get_setting(_LISTENER_EQ_BUS_SETTING, "ListenerEq")
	if configured == "":
		# Designer explicitly disabled listener-space EQ. No
		# warning — opting out is a valid choice for projects
		# that don't want this strong an editorial effect.
		return

	# v0.55.0: same cache-based pre-check as _setup_impact_eq. The
	# GDScript already stayed silent when the bus was missing, but
	# the C++ side's get_bus_effects() call would still log
	# "unknown bus" each time. Now we skip the C++ call entirely
	# when the bus isn't in the cache.
	if not has_bus(configured):
		return

	var effects: Array = _runtime.get_bus_effects(configured)
	if effects.is_empty():
		# Bus exists in config but has no effects yet. Same opt-in
		# logic: stay quiet — designer hasn't finished setup or
		# wants the bus for some other purpose.
		return
	if effects.size() < 3:
		push_warning(
			"[gool] Phase 6.C listener EQ disabled: bus '%s' " % configured
			+ "has only %d effect(s); need at least 3 biquads. " % effects.size()
			+ "See docs/cookbook.md section 14."
		)
		return
	for i in range(3):
		var e: Dictionary = effects[i]
		var kind_name: String = String(e.get("kind_name", ""))
		if kind_name != "BiquadFilter":
			push_warning(
				"[gool] Phase 6.C listener EQ disabled: bus '%s' " % configured
				+ "effect #%d is '%s', expected 'BiquadFilter'. " % [i, kind_name]
				+ "The first three effects on the listener EQ bus must "
				+ "be biquads in order LowShelf → Peak → HighShelf. "
				+ "See docs/cookbook.md section 14."
			)
			return

	_listener_eq_bus_name = configured
	GoolLog.info("runtime", "phase 6.C listener eq available",
		{"bus": configured})

# v0.36.0 (Phase 6.D): cache the realism multiplier from project
# settings. The dial is read once at init and stored in
# _eq_intensity; runtime changes go through set_eq_intensity().
# This keeps the hot path (per-impact, per-frame ramp) free of
# ProjectSettings.get_setting() calls which involve a dictionary
# lookup each time.
func _setup_eq_intensity() -> void:
	if not ProjectSettings.has_setting(_EQ_INTENSITY_SETTING):
		ProjectSettings.set_setting(_EQ_INTENSITY_SETTING, 1.0)
		ProjectSettings.set_initial_value(_EQ_INTENSITY_SETTING, 1.0)
		# Give the editor UI a hint so the setting appears as a
		# slider with the expected range, not just a free-form
		# float field.
		ProjectSettings.add_property_info({
			"name": _EQ_INTENSITY_SETTING,
			"type": TYPE_FLOAT,
			"hint": PROPERTY_HINT_RANGE,
			"hint_string": "0.0,2.0,0.05",
		})
	var v: float = float(ProjectSettings.get_setting(_EQ_INTENSITY_SETTING, 1.0))
	_eq_intensity = clamp(v, 0.0, 2.0)
	if abs(_eq_intensity - 1.0) > 0.001:
		GoolLog.info("runtime", "phase 6.D eq intensity set",
			{"value": _eq_intensity})

## Set the global EQ realism multiplier (Phase 6.D).
##
## Scales every per-material EQ gain uniformly:
## - 0.0 disables material coloring entirely (gains -> 0 dB)
## - 1.0 (default) applies curves as defined in
##   MaterialEqByMaterial — physically realistic
## - >1.0 amplifies for cinematic / surreal effect, capped at 2.0
##
## Takes effect on the next impact play (6.B) and the next zone
## enter/exit ramp (6.C). Currently-active ramps are not
## retroactively rescaled — change persists from here forward.
##
## Intended as the engine hook for Phase 4's player audio settings
## menu: a "Material EQ intensity" slider can call this directly
## to give players agency over how aggressive the material
## coloring feels.
##
## Values outside [0, 2] are clamped. NaN and inf are rejected
## (function returns without changing state).
func set_eq_intensity(value: float) -> void:
	if not is_finite(value):
		push_warning("[gool] set_eq_intensity: rejected non-finite value")
		return
	_eq_intensity = clamp(value, 0.0, 2.0)

## Get the currently active EQ realism multiplier.
## Defaults to 1.0; changes when set_eq_intensity() is called or
## when project setting gool/material_eq/intensity is loaded at
## startup.
func get_eq_intensity() -> float:
	return _eq_intensity

# v0.36.0 (Phase 6.D): push a material's EQ curve to a bus with
# the current realism multiplier applied to all three gain bands.
# Cutoffs and Q stay unscaled — they're frequency-domain anchors,
# not amplitudes.
#
# Used internally by play_impact_sound() when intensity != 1.0.
# At intensity exactly 1.0, the original C++ binding
# apply_material_eq_to_bus is used instead (cheaper, no
# duplication of param-push logic in GDScript).
#
# Returns false on the same conditions as the C++ binding:
# bus doesn't exist, or first 3 effects aren't biquads.
func _apply_scaled_material_eq_to_bus(bus_name: String,
									   material: int) -> bool:
	if not _check_init("_apply_scaled_material_eq_to_bus"):
		return false
	var curve: Dictionary = material_eq_for_material(material)
	if curve.is_empty():
		return false
	# Same authoring contract as the C++ binding: bus has 3
	# biquads at indices 0/1/2 in order LowShelf/Peak/HighShelf.
	# We trust _setup_impact_eq / _setup_listener_eq already
	# validated this at startup.
	var intensity := _eq_intensity
	# LowShelf at index 0
	_runtime.set_effect_parameter(bus_name, 0,  2, # Biquad_CutoffHz
			float(curve.get("low_freq_hz", 200.0)))
	_runtime.set_effect_parameter(bus_name, 0, 12, # Biquad_GainDb
			float(curve.get("low_gain_db", 0.0)) * intensity)
	# Peak at index 1
	_runtime.set_effect_parameter(bus_name, 1,  2,
			float(curve.get("mid_freq_hz", 1000.0)))
	_runtime.set_effect_parameter(bus_name, 1,  3, # Biquad_Q
			float(curve.get("mid_q", 1.0)))
	_runtime.set_effect_parameter(bus_name, 1, 12,
			float(curve.get("mid_gain_db", 0.0)) * intensity)
	# HighShelf at index 2
	_runtime.set_effect_parameter(bus_name, 2,  2,
			float(curve.get("high_freq_hz", 8000.0)))
	_runtime.set_effect_parameter(bus_name, 2, 12,
			float(curve.get("high_gain_db", 0.0)) * intensity)
	return true

## Apply a material's 3-band EQ curve to a named bus. The bus must
## have at least 3 effects, and the first three must be biquads in
## order LowShelf / Peak / HighShelf (cookbook section 14
## convention). Returns true on success, false if the bus or chain
## isn't shaped right.
##
## Most designers don't need to call this directly — `play_impact_sound`
## handles it automatically for the configured impact-EQ bus. Use
## this when applying material coloring to a non-impact context
## (e.g. a custom UI sound, a cinematic moment, a one-off whose
## bus isn't the default impact bus).
func apply_material_eq_to_bus(bus_name: String, material: int) -> bool:
	if not _check_init("apply_material_eq_to_bus"):
		return false
	return _runtime.apply_material_eq_to_bus(bus_name, material)

## v0.59.3 (Phase 6.E.1 audition): pure-DSP offline processing of a
## sample buffer through a material's EQ curve. Used by the editor
## inspector's audition button so designers can hear what a material
## sounds like without running an F5 session.
##
## Bit-identical to what the runtime impact / listener EQ paths
## produce for the same input (uses the same BiquadFilterEffect
## class with the same RBJ cookbook coefficients).
##
## NOTE: This calls a STATIC method on GoolAudioRuntime; it does not
## require the autoload to be initialized. Editor inspector code
## can equivalently call `GoolAudioRuntime.process_buffer_through_material_eq`
## directly — both routes hit the same C++ entry point. This wrapper
## exists so game-context callers have a stable Gool.* API surface
## consistent with every other gool getter.
##
## buffer    : a PackedFloat32Array of mono input samples. Typical
##             length is 1 second at the target sample rate.
## material  : AudioMaterial int. Same range/semantics as the rest
##             of the material API.
## intensity : the realism-intensity multiplier scaling all three
##             band gains. Defaults to 1.0 (curves as-tabled).
## sample_rate : 48000 default. Should match whatever the caller
##             plays the returned buffer back at.
##
## Returns: a PackedFloat32Array of the same length as input.
## Empty array on invalid input.
func process_buffer_through_material_eq(buffer: PackedFloat32Array,
		material: int, intensity: float = 1.0,
		sample_rate: int = 48000) -> PackedFloat32Array:
	# No is_initialized() check: the underlying C++ method is static
	# and works regardless of runtime init state. Editor inspector
	# code calls GoolAudioRuntime.process_buffer_through_material_eq()
	# directly, bypassing this wrapper.
	return GoolAudioRuntime.process_buffer_through_material_eq(
			buffer, material, intensity, sample_rate)

## v0.71.0: Convenience wrapper around `create_emitter` for the
## most common case: "just make a sound play at a position, I don't
## need to control it."
##
## Returns the emitter handle for parity with `create_emitter`, but
## you can ignore the return value for true fire-and-forget. The
## sound auto-frees when it finishes.
##
## Defaults: not looping, no fade-in. If you need either, use
## `create_emitter` directly.
##
## Equivalent to: `Gool.create_emitter(name, position, false, 0.0)`
##
## Example:
##   [codeblock]
##   func _on_button_pressed() -> void:
##       Gool.play_one_shot("ui_click")            # at origin
##   
##   func _on_enemy_hit(pos: Vector3) -> void:
##       Gool.play_one_shot("hit_sfx", pos)        # at a 3D pos
##   [/codeblock]
func play_one_shot(name: String, position: Vector3 = Vector3.ZERO) -> int:
	return create_emitter(name, position, false, 0.0)

func create_emitter(name: String, position: Vector3,
					 looping: bool = false,
					 fade_in_ms: float = 0.0,
					 priority: int = -1) -> int:
	if not _check_init("create_emitter"):
		return 0
	return _runtime.create_emitter(name, position, looping, fade_in_ms, priority)

func destroy_emitter(handle: int, fade_out_ms: float = 0.0) -> void:
	if not _check_init("destroy_emitter"):
		return
	_runtime.destroy_emitter(handle, fade_out_ms)

## Returns the priority assigned to a live emitter (0..255), or -1 if
## the handle is invalid / the emitter has been destroyed / the
## runtime isn't initialized. New in v0.74.0. See create_emitter's
## priority parameter for the band convention.
func get_emitter_priority(handle: int) -> int:
	if not _check_init("get_emitter_priority"):
		return -1
	return _runtime.get_emitter_priority(handle)

func set_emitter_transform(handle: int, position: Vector3,
							  forward: Vector3, velocity: Vector3) -> void:
	if not _check_init("set_emitter_transform"):
		return
	_runtime.set_emitter_transform(handle, position, forward, velocity)

func set_emitter_playback_speed(handle: int, speed: float,
								   smoothing_ms: float = 50.0) -> void:
	if not _check_init("set_emitter_playback_speed"):
		return
	_runtime.set_emitter_playback_speed(handle, speed, smoothing_ms)

func set_listener_transform(position: Vector3, forward: Vector3,
							  velocity: Vector3 = Vector3.ZERO) -> void:
	if not _check_init("set_listener_transform"):
		return
	_runtime.set_listener_transform(position, forward, velocity)

func register_voice_source(player_id: int) -> bool:
	if not _check_init("register_voice_source"):
		return false
	var ok: bool = _runtime.register_voice_source(player_id)
	# v0.44.0: track registered player IDs so the editor's Live
	# Stats panel can enumerate them for per-player jitter/loss
	# display. Tracked only on successful registration to avoid
	# stale entries if the C++ side rejected the call.
	if ok and not _known_voice_player_ids.has(player_id):
		_known_voice_player_ids.append(player_id)
	return ok

## v0.44.0: returns the player IDs the autoload knows about
## (i.e. that have been passed to register_voice_source since
## the singleton started). Used by the editor's Live Stats panel
## to enumerate per-player VOIP health.
##
## Note: this is a GDScript-side tracking list, not a query of
## the engine's actual voice-source state. If a host calls
## _runtime.register_voice_source directly (bypassing this
## autoload), those IDs won't appear here. All gool docs route
## through Gool.register_voice_source, so this is the canonical
## path for production code.
func get_known_voice_player_ids() -> Array:
	return _known_voice_player_ids.duplicate()

func submit_voice_packet(player_id: int, bytes: PackedByteArray,
							sequence_number: int,
							send_timestamp_ms: int,
							arrival_timestamp_ms: int = -1) -> bool:
	if not _check_init("submit_voice_packet"):
		return false
	return _runtime.submit_voice_packet(
		player_id, bytes, sequence_number,
		send_timestamp_ms, arrival_timestamp_ms)

func get_voice_jitter_ms(player_id: int) -> float:
	if not _check_init("get_voice_jitter_ms"):
		return 0.0
	return _runtime.get_voice_jitter_ms(player_id)

func get_voice_packet_loss_ratio(player_id: int) -> float:
	if not _check_init("get_voice_packet_loss_ratio"):
		return 0.0
	return _runtime.get_voice_packet_loss_ratio(player_id)

## v0.44.2: mute/unmute a per-player VOIP voice source. Returns
## true on success. Wrappers added to fix the VoiceChatPlayer
## crash where the prefab called `_runtime.set_voice_source_muted`
## (autoload-side) but the autoload had no such wrapper — the
## C++ binding existed all along, just not surfaced through the
## autoload that VoiceChatPlayer reaches through.
func set_voice_source_muted(player_id: int, muted: bool) -> bool:
	if not _check_init("set_voice_source_muted"):
		return false
	return _runtime.set_voice_source_muted(player_id, muted)

## v0.44.2: set per-player VOIP voice-source volume (linear, 0.0–1.0+).
## Returns true on success. Same backfill reason as
## set_voice_source_muted above.
func set_voice_source_volume(player_id: int, volume: float) -> bool:
	if not _check_init("set_voice_source_volume"):
		return false
	return _runtime.set_voice_source_volume(player_id, volume)

# ---- Replication / multiplayer ----

func on_tick_advanced(simulation_tick: int, server_time_ms: int) -> void:
	if not _check_init("on_tick_advanced"):
		return
	_runtime.on_tick_advanced(simulation_tick, server_time_ms)

func submit_event_local(sound_name: String, position: Vector3,
						  prediction_id: int = 0,
						  priority: int = 128,
						  timestamp_ms: int = 0) -> void:
	if not _check_init("submit_event_local"):
		return
	_runtime.submit_event_local(sound_name, position,
								  prediction_id, priority, timestamp_ms)

func submit_replicated_event(sound_name: String, position: Vector3,
							   simulation_tick: int = 0,
							   server_time_ms: int = 0,
							   priority: int = 128) -> void:
	if not _check_init("submit_replicated_event"):
		return
	_runtime.submit_replicated_event(sound_name, position,
									   simulation_tick, server_time_ms,
									   priority)

func cancel_predicted_event(prediction_id: int,
							   fade_out_ms: float = 50.0) -> void:
	if not _check_init("cancel_predicted_event"):
		return
	_runtime.cancel_predicted_event(prediction_id, fade_out_ms)

func update_replicated_transform(handle: int, position: Vector3,
									forward: Vector3, velocity: Vector3,
									simulation_tick: int) -> void:
	if not _check_init("update_replicated_transform"):
		return
	_runtime.update_replicated_transform(handle, position, forward,
											velocity, simulation_tick)

func make_prediction_id() -> int:
	if not _check_init("make_prediction_id"):
		return 0
	return _runtime.make_prediction_id()

# ---- v0.22.0: simplified one-shot networked SFX ------------------------

## Fire-and-forget one-shot SFX across the network. The sound plays
## locally immediately on the caller, and is replicated to every
## connected peer via an unreliable RPC.
##
## Use this when you have a discrete sound event (gunshot, impact,
## button click, ability cast) that should be heard everywhere with
## drop-if-late semantics — if a packet arrives stale, the audio is
## already irrelevant, so just drop it gracefully rather than
## playing late.
##
## What this DOESN'T do:
##   * Client prediction → use NetworkedAudioEvent.predict() instead.
##   * Server authority / validation → use NetworkedAudioEvent with
##     Mode.SERVER_AUTHORITATIVE.
##   * Team filtering / audible-radius gating → use NetworkedAudioEvent
##     with default_audible_radius and team parameters.
##   * Persistent positioned sources (loops, vehicles, ambient
##     emitters) → use NetworkedAudioEmitter3D for those.
##
## This is the simplest possible networked-SFX path: one call, all
## peers hear it, no scene-tree nodes required. About 30 lines of
## code under the hood, including the RPC marshalling.
##
## When called without an active multiplayer peer, plays locally and
## skips the RPC silently — useful for testing networked events in
## single-player without a separate code path.
func play_networked(sound_name: String,
					   position: Vector3 = Vector3.ZERO,
					   volume_db: float = 0.0,
					   pitch: float = 1.0) -> void:
	if not _check_init("play_networked"):
		# v0.54.2: rate-limit this warning. Auto-firing weapons or
		# any sound triggered every frame can spam 40+ identical
		# errors per second when init has failed; the underlying
		# init failure is already in the log at this point and is
		# the actionable diagnostic. Emit once and let the cascade
		# stay quiet so the real root cause stays visible.
		if not _play_networked_warning_emitted:
			_play_networked_warning_emitted = true
			push_error(
				"[gool] play_networked('%s') called before runtime init. "
				% sound_name
				+ "Either wait for the ready_to_play signal or call from "
				+ "_ready() after the autoload has finished initializing. "
				+ "(Further warnings of this kind suppressed for this "
				+ "session — check the FIRST error in the log for the "
				+ "real cause.)"
			)
		return
	var t_ms: int = Time.get_ticks_msec()
	# Local immediate play.
	_runtime.submit_event_local(sound_name, position, 0, 128, t_ms)
	# If no multiplayer peer is connected, the local play is all that's
	# needed. Otherwise broadcast to other peers. We use the default
	# priority of 128 (medium) which the engine routes to the
	# drop-if-late event ring.
	if multiplayer != null and multiplayer.has_multiplayer_peer() \
			and multiplayer.get_multiplayer_peer().get_connection_status() \
				== MultiplayerPeer.CONNECTION_CONNECTED:
		_rpc_play_networked.rpc(sound_name, position, volume_db, pitch, t_ms)

# RPC handler invoked on every receiving peer. Plays the sound locally
# on the receiver with the same parameters the sender used. The Vector3
# position is passed as-is (Godot's RPC machinery serializes Vector3
# natively, no per-component packing needed).
#
# `volume_db` and `pitch` are accepted for forward compatibility — the
# current submit_event_local path doesn't use them, but the C++ engine
# already supports both via the event payload and a future autoload
# expansion can wire them through without a binding compatibility break.
@rpc("any_peer", "call_remote", "unreliable")
func _rpc_play_networked(sound_name: String, position: Vector3,
							volume_db: float, pitch: float,
							sender_t_ms: int) -> void:
	if not _check_init("_rpc_play_networked"):
		return
	# Optional staleness check: if the event is more than 250ms old
	# by our clock, drop it. Matches the default category-staleness
	# for SFX (see DefaultStalenessMsForCategory in events.h). This
	# is the "drop-if-late" behaviour we promised in the doc above.
	var local_t_ms: int = Time.get_ticks_msec()
	if local_t_ms - sender_t_ms > 250:
		# Telemetry could go here if needed.
		return
	_runtime.submit_event_local(sound_name, position, 0, 128, sender_t_ms)


# =============================================================================
# Tiny API facade
# =============================================================================
#
# These four methods are the canonical entry points for fast prototyping.
# Each is a thin wrapper around lower-level APIs. Drop down to the raw
# bindings (submit_event_local, register_pcm_sound, submit_voice_packet,
# set_global_parameter) when you outgrow them.
#
#   Gool.play_3d("rifle_fire", global_position)
#   Gool.play_music_state("combat")
#   Gool.play_voice(player_id, audio_stream)
#   Gool.set_rtpc("health", hp)

# Play a one-shot 3D sound by authored name at `position`. Sound must be
# registered (via sound bank or register_pcm_sound) ahead of this call.
# `priority` is 0..255; higher survives culling under voice budgets.
# Returns true if the event was queued; false if the runtime is not
# initialized or the queue is full.
func play_3d(name: String, position: Vector3, priority: int = 128) -> bool:
	if not _check_init("play_3d"):
		return false
	# v0.46.1 fix: submit_event_local is void on the C++ side. Pre-
	# v0.46.1 this function tried to capture an int return value
	# ("var rc: int = _runtime.submit_event_local(...)") which crashed
	# with "Trying to get a return value of a method that returns void"
	# the moment anyone called Gool.play_3d (notably DialogueDirector
	# routes barks through here). Engine drop policies still apply
	# internally; callers just can't observe drops through this path.
	# If you need drop telemetry, watch the gool:render_stats debugger
	# channel (or the Live Stats panel's Drops counter, v0.44.0+).
	_runtime.submit_event_local(name, position, 0, priority, 0)
	return true

# Switch the music channel to `state_name` with an equal-power crossfade.
# Idempotent: passing the currently-playing state is a no-op so callers
# can poll-style invoke this every frame without churn. The first call
# lazily creates a GoolMusicChannel under this autoload.
func play_music_state(state_name: String, fade_ms: float = 500.0) -> bool:
	if _runtime == null:
		return false
	if _music_channel == null:
		_music_channel = ClassDB.instantiate("GoolMusicChannel")
		if _music_channel == null:
			push_error("[gool] GoolMusicChannel class not registered. "
					   + "Build and install the GDExtension first.")
			return false
		add_child(_music_channel)
		_music_channel.attach(_runtime)
	if state_name == _music_state and _music_channel.is_playing():
		return true  # already in this state
	_music_state = state_name
	_music_channel.play(state_name, fade_ms)
	return true

# Stop the music channel with a fade-out. Subsequent play_music_state
# calls work normally afterward.
func stop_music(fade_ms: float = 500.0) -> void:
	if _music_channel == null:
		return
	_music_state = ""
	_music_channel.stop(fade_ms)

# Play `audio_stream` as voice for `player_id`. The clip is decoded to
# PCM, registered as an ephemeral sound, and dispatched through the
# normal play path.
#
# Currently supports AudioStreamWAV with FORMAT_16_BITS only. For
# AudioStreamOggVorbis, decode upstream to AudioStreamWAV (or use the
# raw voice path: Gool.submit_voice_packet for Opus traffic from a
# real network layer). AudioStreamOggVorbis support is on the roadmap.
#
# Returns true if the clip was queued; false on input errors or
# missing runtime.
func play_voice(player_id: int, audio_stream: AudioStream) -> bool:
	if _runtime == null:
		return false
	if not (audio_stream is AudioStreamWAV):
		push_error("[gool] play_voice currently supports AudioStreamWAV only. "
				   + "AudioStreamOggVorbis support is on the roadmap. For "
				   + "Opus voice packets from a network layer, use "
				   + "Gool.submit_voice_packet directly.")
		return false
	var wav: AudioStreamWAV = audio_stream
	var samples := _wav_to_pcm_mono(wav)
	if samples.is_empty():
		return false
	_voice_counter += 1
	var clip_name := "_voice_%d_%d" % [player_id, _voice_counter]
	var sample_rate: int = int(wav.mix_rate)
	if sample_rate <= 0:
		sample_rate = 48000
	_runtime.register_pcm_sound(clip_name, samples, sample_rate, 1)
	_runtime.register_sound_definition(clip_name, false)
	_runtime.play_sound_at_location(clip_name, Vector3.ZERO)
	return true

# Set a real-time parameter ("RTPC" in middleware lingo) by name.
# Stored as a key-value pair in the runtime; authored sound definitions
# can reference these in future updates. For now: useful for game-state
# polling, debug overlays, and getting comfortable with the API shape.
# Returns true if the value was stored; false on budget exhaustion or
# missing runtime. See AudioConfig::maxGlobalParameters (default 256).
func set_rtpc(name: String, value: float) -> bool:
	if _runtime == null:
		return false
	return _runtime.set_global_parameter(name, value)

# Read the current value of an RTPC. Returns 0.0 if the parameter has
# never been set; use has_rtpc() to disambiguate "set to zero" from
# "never set."
func get_rtpc(name: String) -> float:
	if _runtime == null:
		return 0.0
	return _runtime.get_global_parameter(name)

func has_rtpc(name: String) -> bool:
	if _runtime == null:
		return false
	return _runtime.has_global_parameter(name)

func clear_rtpc(name: String) -> bool:
	if _runtime == null:
		return false
	return _runtime.clear_global_parameter(name)

# =============================================================================
# RTPC binding facades (v0.5+)
# =============================================================================
#
# Bind a sound's per-voice parameter (Volume / Pitch / LowPass / ReverbSend)
# to a global RTPC. Each `Update` tick the runtime reads the current value of
# `param_name`, applies the configured curve and remap, and pushes the
# result through the parameter smoother.
#
# Skip-when-unset semantics: until `set_rtpc(param_name, ...)` is called at
# least once, the binding has no effect. Authored values stay in place.
#
# At most one binding per (sound, target) pair. Re-binding the same target
# replaces the old binding. A sound can have up to four bindings simultaneously
# (volume + pitch + lowpass + reverb_send, each driven by its own parameter).
#
# Examples:
#   # Heartbeat gets louder and pitches up as health drops:
#   Gool.bind_volume_rtpc("heartbeat", "health", 0, 1, 1.0, 0.0)
#   Gool.bind_pitch_rtpc("heartbeat",  "health", 0, 1, 1.4, 1.0)
#
#   # Music ducks under combat with smoothstep curve for an organic feel:
#   Gool.bind_rtpc("ambient", {
#       "parameter": "combat_intensity",
#       "target":    "volume",
#       "curve":     "scurve",
#       "min_value": 0.0, "max_value": 1.0,
#       "min_output": 1.0, "max_output": 0.3,
#       "smoothing_ms": 300.0,
#   })

# Bind a Volume target with a linear curve. The most common case.
func bind_volume_rtpc(sound_name: String, param_name: String,
					   min_value: float, max_value: float,
					   min_output: float, max_output: float,
					   smoothing_ms: float = 50.0) -> bool:
	if _runtime == null:
		return false
	return _runtime.set_sound_rtpc(sound_name, param_name,
									 "volume", "linear",
									 min_value, max_value,
									 min_output, max_output,
									 2.0, smoothing_ms)

# Bind a Pitch target with a linear curve. Output is a pitch multiplier
# (1.0 = unchanged, 2.0 = octave up, 0.5 = octave down).
func bind_pitch_rtpc(sound_name: String, param_name: String,
					  min_value: float, max_value: float,
					  min_output: float, max_output: float,
					  smoothing_ms: float = 50.0) -> bool:
	if _runtime == null:
		return false
	return _runtime.set_sound_rtpc(sound_name, param_name,
									 "pitch", "linear",
									 min_value, max_value,
									 min_output, max_output,
									 2.0, smoothing_ms)

# Bind a LowPassCutoff target. Output in [0, 1]; 0 = no filter, 1 = fully
# muffled. Combined with the spatializer's baseline via max(), so RTPC
# can add filtering on top of occlusion / air absorption but never reduce
# what the world applied.
func bind_lowpass_rtpc(sound_name: String, param_name: String,
						min_value: float, max_value: float,
						min_output: float, max_output: float,
						smoothing_ms: float = 50.0) -> bool:
	if _runtime == null:
		return false
	return _runtime.set_sound_rtpc(sound_name, param_name,
									 "lowpass", "linear",
									 min_value, max_value,
									 min_output, max_output,
									 2.0, smoothing_ms)

# Bind a ReverbSend target. Output in [0, 1] is added to the global
# reverb send amount with a clamp at 1.0.
func bind_reverb_rtpc(sound_name: String, param_name: String,
					   min_value: float, max_value: float,
					   min_output: float, max_output: float,
					   smoothing_ms: float = 50.0) -> bool:
	if _runtime == null:
		return false
	return _runtime.set_sound_rtpc(sound_name, param_name,
									 "reverb", "linear",
									 min_value, max_value,
									 min_output, max_output,
									 2.0, smoothing_ms)

# Advanced: bind any target with any curve, configured via Dictionary.
# Keys (all required unless marked optional):
#   parameter:     String — global parameter name
#   target:        String — "volume" | "pitch" | "lowpass" | "reverb"
#   curve:         String (optional, default "linear") —
#                  "linear" | "exponential" | "inverse_exp" | "scurve"
#   exponent:      float (optional, default 2.0) — used by exp / inv_exp
#   min_value:     float — input range start
#   max_value:     float — input range end
#   min_output:    float — output at min_value (after curve)
#   max_output:    float — output at max_value (after curve)
#   smoothing_ms:  float (optional, default 50.0)
func bind_rtpc(sound_name: String, binding: Dictionary) -> bool:
	if _runtime == null:
		return false
	var param      = binding.get("parameter", "")
	var target     = binding.get("target",    "")
	var curve      = binding.get("curve",     "linear")
	var exponent   = binding.get("exponent",      2.0)
	var min_value  = binding.get("min_value",     0.0)
	var max_value  = binding.get("max_value",     1.0)
	var min_output = binding.get("min_output",    0.0)
	var max_output = binding.get("max_output",    1.0)
	var smoothing  = binding.get("smoothing_ms",  50.0)
	if param == "" or target == "":
		push_error("[gool] bind_rtpc requires 'parameter' and 'target' keys")
		return false
	return _runtime.set_sound_rtpc(sound_name, param, target, curve,
									 min_value, max_value,
									 min_output, max_output,
									 exponent, smoothing)

# Remove one binding for (sound, target). Returns true if it existed.
func clear_rtpc_binding(sound_name: String, target: String) -> bool:
	if _runtime == null:
		return false
	return _runtime.clear_sound_rtpc(sound_name, target)

# Remove every binding for a sound. Returns the number of bindings removed.
func clear_all_rtpc_bindings(sound_name: String) -> int:
	if _runtime == null:
		return 0
	return _runtime.clear_all_sound_rtpc(sound_name)

# Backward-compat convenience: same as clear_rtpc_binding(name, "volume").
# Kept so v0.4 call sites don't break on upgrade.
func clear_volume_rtpc(sound_name: String) -> bool:
	return clear_rtpc_binding(sound_name, "volume")

# =============================================================================
# Internal helpers
# =============================================================================

# Convert a 16-bit signed little-endian PCM AudioStreamWAV to mono
# float32 samples in [-1, 1]. Stereo sources are downmixed by
# averaging L+R per frame (no fancy panning preservation; voice
# clips are typically mono anyway).
#
# Returns an empty array if the format is unsupported or the buffer
# is malformed; the caller has already pushed an error message in
# that case so we just bail.
func _wav_to_pcm_mono(wav: AudioStreamWAV) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	if wav.format != AudioStreamWAV.FORMAT_16_BITS:
		push_error("[gool] play_voice supports FORMAT_16_BITS WAVs only. "
				   + "Re-import your asset as 16-bit PCM in Godot's "
				   + "import dock.")
		return out
	var data: PackedByteArray = wav.data
	var byte_count: int = data.size()
	if byte_count == 0 or byte_count % 2 != 0:
		return out
	var sample_count: int = byte_count / 2
	var stereo: bool = wav.stereo
	if stereo and sample_count % 2 != 0:
		return out
	var frames: int = (sample_count / 2) if stereo else sample_count
	out.resize(frames)
	var inv: float = 1.0 / 32768.0
	if stereo:
		for i in range(frames):
			var lo_l: int = data[i * 4 + 0]
			var hi_l: int = data[i * 4 + 1]
			var lo_r: int = data[i * 4 + 2]
			var hi_r: int = data[i * 4 + 3]
			var l: int = (hi_l << 8) | lo_l
			var r: int = (hi_r << 8) | lo_r
			if l >= 32768:
				l -= 65536
			if r >= 32768:
				r -= 65536
			out[i] = ((l + r) * 0.5) * inv
	else:
		for i in range(frames):
			var lo: int = data[i * 2 + 0]
			var hi: int = data[i * 2 + 1]
			var s: int = (hi << 8) | lo
			if s >= 32768:
				s -= 65536
			out[i] = float(s) * inv
	return out

func _load_config_text() -> String:
	# Returns the raw config.json text, or "" if missing / unreadable.
	# The text (not the parsed dict) is what gets passed to the C++
	# parser when bus graph is configured.
	if not FileAccess.file_exists(CONFIG_PATH):
		return ""
	var f := FileAccess.open(CONFIG_PATH, FileAccess.READ)
	if f == null:
		return ""
	var text := f.get_as_text()
	f.close()
	return text

func _parse_config_dict(text: String) -> Dictionary:
	# Parses the config text into a Godot Dictionary. Used to peek
	# at top-level fields (sample_rate, buffer_size, buses) before
	# deciding which runtime init() variant to call. If parsing
	# fails OR the top-level isn't an object, returns {}.
	if text.is_empty():
		return {}
	var parsed = JSON.parse_string(text)
	if parsed is Dictionary:
		return parsed
	return {}

# ---- v0.14.0: native-Godot integration helpers ---------------------

## Mirror a Godot AudioServer bus's current volume into a gool bus.
##
## One-way binding (Godot → gool); call this whenever the user
## adjusts a Godot volume slider. Example — hooking gool's master
## output into an existing settings menu:
##
##   func _on_master_slider_changed(value: float) -> void:
##       var idx := AudioServer.get_bus_index("Master")
##       AudioServer.set_bus_volume_db(idx, linear_to_db(value))
##       Gool.sync_volume_from_godot_bus("Master")
##
## If your gool bus name differs from the Godot bus name (e.g. you
## have a "SFX" bus in Godot and a "Sfx" bus in gool's config), pass
## the gool name as the second argument.
##
## Returns true on success, false if either bus name is unknown.
func sync_volume_from_godot_bus(godot_bus_name: String = "Master",
								  gool_bus_name: String = "") -> bool:
	if _runtime == null:
		return false
	var idx := AudioServer.get_bus_index(godot_bus_name)
	if idx < 0:
		push_warning("[gool] sync_volume_from_godot_bus: no Godot bus named '%s'"
					   % godot_bus_name)
		return false
	var db := AudioServer.get_bus_volume_db(idx)
	var target := gool_bus_name if gool_bus_name != "" else godot_bus_name
	return _runtime.set_bus_gain_db(target, db)


## Continuous variant: install an every-frame poll that mirrors the
## given Godot bus volume to the matching gool bus. Cheap (one float
## read + one bus-id lookup per frame) but ensures changes from any
## external source — third-party settings plugins, MIDI controllers,
## animation tracks driving AudioServer — propagate to gool without
## manual sync calls.
##
## Pass `false` to disable polling for a specific bus pair, e.g.
## when switching back to explicit per-callback syncing.
func auto_mirror_godot_bus(godot_bus_name: String,
							gool_bus_name: String = "",
							enabled: bool = true) -> void:
	if enabled:
		var target := gool_bus_name if gool_bus_name != "" else godot_bus_name
		_mirrored_buses[godot_bus_name] = target
	else:
		_mirrored_buses.erase(godot_bus_name)


# Storage for auto_mirror_godot_bus pairs. Keys are Godot bus names;
# values are gool bus names. Polled inside _process when non-empty.
var _mirrored_buses: Dictionary = {}

# Cached last-known dB per Godot bus, so the poll only forwards
# updates when the value actually changed — avoids hammering
# set_bus_gain_db every frame for static volumes.
var _mirrored_last_db: Dictionary = {}

