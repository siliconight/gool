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

# addons/gool/prefabs/fps_coop_audio.gd
#
## Genre-specific drop-in audio prefab for 4-player FPS PvE games.
## One level above CoopAudioRoot in the prefab hierarchy: where
## CoopAudioRoot is generic ("pick a mode per call"), this is shaped
## for the specific use case of "4 humans vs. AI in first-person."
#
## WHAT MAKES THIS DIFFERENT FROM CoopAudioRoot
## ============================================
#
## FPSCoopAudio creates five purpose-built audio categories instead
## of one generic channel:
#
##   WEAPONS     — CLIENT_PREDICTED, 100m radius, priority 200
##                 Gunshots, reloads, weapon swaps. Shooter hears
##                 instantly (prediction), other players hear after
##                 one network round-trip. Server validates so a
##                 desync from cheating or packet loss is recoverable.
#
##   ENEMIES     — SERVER_AUTHORITATIVE, 80m radius, priority 150
##                 AI bot footsteps, attack telegraphs, voicelines,
##                 death sounds. Server owns the AI, so server owns
##                 the audio. Authoritative replication prevents
##                 ghost-sounds when a bot dies on one client but
##                 not yet on others.
#
##   WORLD       — SERVER_AUTHORITATIVE, 200m radius, priority 180
##                 Explosions, scripted events, destructibles, door
##                 mechanisms. Wide radius because explosions carry.
##                 Server-authoritative because game state changes
##                 (e.g. a door opening triggers AI spawns) need
##                 audio to match the state truth.
#
##   MOVEMENT    — CLIENT_AUTHORITATIVE, 30m radius, priority 80
##                 Footsteps, jumps, landings, slides. Low priority
##                 (gets stolen first when the mixer is voice-capped),
##                 short radius (only nearby players care), cheap
##                 client-authoritative replication (cheating cost
##                 is low — at worst someone fakes footsteps).
#
##   HUD         — Local only. No replication.
##                 UI clicks, damage-taken feedback, low-health
##                 heartbeat, objective stings. Priority 240 (very
##                 high — UI should always play). Never sent over
##                 the network — each player's HUD is their own.
#
## With these defaults baked in, your game code becomes:
#
##     $FPSCoopAudio.play_weapon("rifle_fire", muzzle_pos)
##     $FPSCoopAudio.play_enemy("zombie_groan", bot.global_position)
##     $FPSCoopAudio.play_world("c4_explosion", bomb_pos)
##     $FPSCoopAudio.play_movement("footstep_concrete", foot_pos)
##     $FPSCoopAudio.play_hud("damage_taken")
#
## No mode arguments, no radius arguments, no priority arguments.
## The category determines all three.
#
## DEV VALIDATION
## ==============
#
## You can drop this prefab into a fresh project and verify gool is
## doing what your game needs without writing test code:
#
##   1. Enable `show_diagnostic_overlay` in the inspector.
##   2. Run your project.
##   3. The overlay shows per-category event counts (fired locally
##      and received from peers), listener-attached state, last-
##      event timestamps, and total replication latency p95.
#
## If you see weapons-fired counts going up on the shooter's client
## but staying at zero on other clients, you have a multiplayer
## wiring problem (NetworkedAudioEvent isn't reaching peers). If
## counts are matched but you don't hear anything, you have a
## sound-bank problem (the sound name isn't registered). The
## overlay makes these failure modes visible without debugging.
#
## You can also query the diagnostic state programmatically:
#
##     var summary: Dictionary = $FPSCoopAudio.get_diagnostic_summary()
##     # {
##     #   "listener_attached": true,
##     #   "listener_target": "Player1",
##     #   "sound_bank_loaded": true,
##     #   "weapons":  {"fired": 14, "received": 9, "last_ms": 1234},
##     #   "enemies":  {"fired": 0, "received": 6, "last_ms": 5678},
##     #   ... etc
##     # }
#
## This is useful in your own test scenes ("after firing 10 weapon
## events, expect weapons.fired == 10").
#
## WHEN NOT TO USE THIS
## ====================
#
##   - Your game isn't FPS PvE. If you're making a coop puzzle game,
##     a top-down twin-stick shooter, or a non-combat coop game,
##     CoopAudioRoot's generic interface is a better fit.
#
##   - You need more than 5 audio categories or per-sound replication
##     mode overrides. Drop down to NetworkedAudioEvent + the other
##     primitives.
#
##   - Your game is PvP. The opinionated defaults here assume
##     cooperating players — CLIENT_AUTHORITATIVE for movement is
##     fine when "cheating with footsteps" doesn't ruin anyone's
##     game. In PvP, audio cheating is a real concern and you'd
##     want SERVER_AUTHORITATIVE everywhere.
#
##   - You need voice chat. Voice chat is a separate prefab
##     (VoiceChatPlayer), per-player. The two don't conflict —
##     add VoiceChatPlayer to each player scene.
#
## USAGE
## =====
#
##   1. Add FPSCoopAudio to your scene tree:
#
##         Main (Node3D)
##         ├── FPSCoopAudio                      # this prefab
##         ├── World (Node3D)
##         └── Players (Node3D)
#
##   2. Set `sound_bank` in the inspector. Expected sound names
##      (these are conventions you populate in your GoolSoundBank;
##      none are hard requirements):
#
##         Weapons:  weapon_fire_*, weapon_reload_*, weapon_swap
##         Enemies:  enemy_step_*, enemy_attack_*, enemy_death_*
##         World:    explosion_*, glass_break, door_open, ...
##         Movement: footstep_*, jump, land
##         HUD:      ui_click, damage_taken, low_health, objective_*
#
##   3. When your local player spawns, attach the listener:
#
##         func _on_local_player_spawned(player: Node3D) -> void:
##             $FPSCoopAudio.set_local_player(player)
#
##   4. Call the semantic methods from your game code. Done.

@tool
class_name FPSCoopAudio
extends Node


# ---------------------------------------------------------------------
# Category constants. Stable indices so values can be checked in
# scripts that reference this prefab by class_name.
# ---------------------------------------------------------------------

enum Category {
	WEAPONS = 0,
	ENEMIES = 1,
	WORLD = 2,
	MOVEMENT = 3,
	HUD = 4,
}

const _CATEGORY_KEYS := [
	"weapons", "enemies", "world", "movement", "hud",
]


# ---------------------------------------------------------------------
# Exports
# ---------------------------------------------------------------------

## Sound bank to register on _ready. Required for any sound to play.
@export var sound_bank: GoolSoundBank = null

## Whether to show a runtime diagnostic overlay (events per category,
## listener state, etc.). Useful during development; turn off for
## shipping builds.
@export var show_diagnostic_overlay: bool = false

## How often the diagnostic overlay refreshes its display, in seconds.
## 0.25 (4 Hz) is a reasonable default — fast enough to feel live,
## slow enough not to dominate frame time.
@export_range(0.05, 2.0, 0.05, "suffix:s") var diagnostic_refresh_s: float = 0.25

## Whether the SoundBankLoader should warn (via push_warning) if
## sound_bank is null. Off by default for tool-mode editing where
## the bank may not yet be assigned.
@export var warn_if_bank_unassigned: bool = false


# ---------------------------------------------------------------------
# Signals
# ---------------------------------------------------------------------

signal sound_bank_loaded(results: Dictionary)
signal event_played(category: int, sound_name: String,
		position: Vector3, source_peer_id: int)
signal listener_attached(target: Node3D)
signal diagnostic_updated(summary: Dictionary)


# ---------------------------------------------------------------------
# Internal state
# ---------------------------------------------------------------------

# Per-category replication channel nodes. Index by Category enum.
# Created during _ready() in run mode. In @tool mode they aren't
# created (to avoid leaking state into editor sessions).
var _channels: Array = []

# Sound bank loader child.
var _bank_loader: Node = null

# Listener child + the target it's attached to.
var _listener: Node = null
var _local_player: Node3D = null

# Diagnostic counters, one entry per category.
# Each entry: {"fired": int, "received": int, "last_ms": int}.
var _diag: Array = []

# Optional overlay node (created if show_diagnostic_overlay is true).
var _overlay: CanvasLayer = null
var _overlay_label: Label = null
var _overlay_timer: float = 0.0

# Cached reference to the /root/Gool autoload. Resolved once in
# _ready; matches the pattern NetworkedAudioEvent uses. Null if the
# gool plugin isn't enabled, in which case play_hud (and the
# overlay) degrade gracefully.
var _runtime: Node = null


# Per-category configuration tables. Indexed by Category enum.
# Comments next to each value explain why it was chosen.
const _MODES := [
	1,    # WEAPONS  → CLIENT_PREDICTED (shooter hears instantly)
	0,    # ENEMIES  → SERVER_AUTHORITATIVE (server owns AI truth)
	0,    # WORLD    → SERVER_AUTHORITATIVE (state-coupled audio)
	2,    # MOVEMENT → CLIENT_AUTHORITATIVE (cheap, high-frequency)
	-1,   # HUD      → no channel (local-only, special-cased)
]

const _RADII := [
	100.0,  # WEAPONS  — gunshots carry far
	80.0,   # ENEMIES  — enemy proximity matters at medium range
	200.0,  # WORLD    — explosions audible far
	30.0,   # MOVEMENT — only nearby players care about footsteps
	0.0,    # HUD      — non-positional
]

const _PRIORITIES := [
	200,    # WEAPONS  — critical gameplay feedback
	150,    # ENEMIES  — important for situational awareness
	180,    # WORLD    — explosions are loud and important
	80,     # MOVEMENT — gets stolen first under voice cap
	240,    # HUD      — UI should always play
]


# Prefab script paths. Resolved at load time so this works whether
# the addon is at the canonical `res://addons/gool/` or copied into
# an example project at a different path.
const _BANK_LOADER_SCRIPT := "res://addons/gool/prefabs/gool_sound_bank_loader.gd"
const _EVENT_SCRIPT       := "res://addons/gool/prefabs/networked_audio_event.gd"
const _LISTENER_SCRIPT    := "res://addons/gool/prefabs/gool_listener_3d.gd"


# ---------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------

func _ready() -> void:
	if Engine.is_editor_hint():
		return

	# Resolve the Gool autoload once. NetworkedAudioEvent uses the
	# same path-based lookup; doing it here too means play_hud
	# (which goes around NetworkedAudioEvent) can find the runtime
	# directly.
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("[FPSCoopAudio] /root/Gool autoload not found. " +
				"The gool plugin is installed but not enabled. Fix: " +
				"Project Settings → Plugins → tick 'gool'. " +
				"HUD audio and diagnostics will be degraded until then.")

	_init_diagnostic_counters()
	_create_bank_loader()
	_create_category_channels()

	if show_diagnostic_overlay:
		_create_overlay()


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if not show_diagnostic_overlay or _overlay_label == null:
		return

	_overlay_timer += delta
	if _overlay_timer < diagnostic_refresh_s:
		return
	_overlay_timer = 0.0
	_refresh_overlay()


func _exit_tree() -> void:
	# Defensive cleanup — Godot will free children automatically,
	# but explicit listener detach ensures the engine-side handle
	# is released before C++ teardown.
	if _listener != null and is_instance_valid(_listener):
		_detach_listener()


# ---------------------------------------------------------------------
# Public API — per-category play methods
# ---------------------------------------------------------------------

## Play a weapon sound (gunshot, reload, swap). Client-predicted:
## shooter hears instantly, other players hear after one RTT, server
## validates. Returns the prediction ID for use with cancel_event().
func play_weapon(sound_name: String, position: Vector3) -> int:
	return _play(Category.WEAPONS, sound_name, position)


## Play an enemy sound (bot footstep, attack, voiceline, death).
## Server-authoritative: only the server can fire, all peers receive.
## Use this for any AI-owned audio.
func play_enemy(sound_name: String, position: Vector3) -> int:
	return _play(Category.ENEMIES, sound_name, position)


## Play a world sound (explosion, glass break, door, scripted event).
## Server-authoritative with a 200m radius — explosions carry.
func play_world(sound_name: String, position: Vector3) -> int:
	return _play(Category.WORLD, sound_name, position)


## Play a movement sound (footstep, jump, landing, slide).
## Client-authoritative — cheap, low-priority, short radius. Suitable
## for high-frequency events where exact server arbitration would
## be wasteful.
func play_movement(sound_name: String, position: Vector3) -> int:
	return _play(Category.MOVEMENT, sound_name, position)


## Play a HUD sound (UI click, damage feedback, low-health). LOCAL
## ONLY — never replicates. Each player's HUD is their own.
func play_hud(sound_name: String) -> void:
	if _runtime == null:
		# Autoload not yet resolved (called before _ready). Defer.
		call_deferred("play_hud", sound_name)
		return
	# Gool.play_one_shot is the canonical local one-shot API.
	# Returns an emitter ID we ignore — fire-and-forget for HUD.
	_runtime.play_one_shot(sound_name, Vector3.ZERO)
	_record_event(Category.HUD, sound_name, Vector3.ZERO, 0)


## Cancel an in-flight prediction (only meaningful for WEAPONS, which
## is the only client-predicted category). `fade_out_ms` softens the
## cancellation.
func cancel_weapon(prediction_id: int, fade_out_ms: float = 50.0) -> void:
	var channel := _channels[Category.WEAPONS] as Node
	if channel == null:
		return
	channel.cancel(prediction_id, fade_out_ms)


# ---------------------------------------------------------------------
# Public API — listener management
# ---------------------------------------------------------------------

## Bind the audio listener to your local player's Node3D. Call this
## once the player spawns. Safe to call multiple times for spectator
## handoff or possession changes.
func set_local_player(target: Node3D) -> void:
	if target == null:
		push_warning("[FPSCoopAudio] set_local_player(null) — detaching listener")
		_detach_listener()
		return
	_local_player = target
	_attach_listener_to(target)
	listener_attached.emit(target)


## Release the listener without binding a new one. Useful for
## spectator transitions or pause-menu states.
func detach_listener() -> void:
	_detach_listener()


# ---------------------------------------------------------------------
# Public API — diagnostics
# ---------------------------------------------------------------------

## Returns a snapshot of the current diagnostic state. Safe to call
## every frame — it's a Dictionary built from already-tracked
## counters, not a fresh poll of subsystems.
func get_diagnostic_summary() -> Dictionary:
	var summary: Dictionary = {
		"listener_attached": _listener != null,
		"listener_target": (
			_local_player.name if _local_player != null else ""
		),
		"sound_bank_loaded": (
			_bank_loader != null and
			_bank_loader.has_meta("registration_complete") and
			_bank_loader.get_meta("registration_complete", false)
		),
	}
	for i in range(_CATEGORY_KEYS.size()):
		summary[_CATEGORY_KEYS[i]] = _diag[i].duplicate()
	return summary


# ---------------------------------------------------------------------
# Internals — channel setup
# ---------------------------------------------------------------------

func _init_diagnostic_counters() -> void:
	_diag = []
	for _i in range(_CATEGORY_KEYS.size()):
		_diag.append({"fired": 0, "received": 0, "last_ms": 0})


func _create_bank_loader() -> void:
	var script := load(_BANK_LOADER_SCRIPT)
	if script == null:
		push_error("[FPSCoopAudio] Couldn't load GoolSoundBankLoader script. " +
				"Is the gool addon installed correctly?")
		return
	_bank_loader = Node.new()
	_bank_loader.set_script(script)
	_bank_loader.name = "BankLoader"
	_bank_loader.bank = sound_bank
	_bank_loader.warn_if_unassigned = warn_if_bank_unassigned
	_bank_loader.registration_complete.connect(_on_bank_registered)
	add_child(_bank_loader)


func _create_category_channels() -> void:
	var script := load(_EVENT_SCRIPT)
	if script == null:
		push_error("[FPSCoopAudio] Couldn't load NetworkedAudioEvent script.")
		return

	_channels = []
	for i in range(_CATEGORY_KEYS.size()):
		var mode: int = _MODES[i]
		if mode < 0:
			# HUD category has no channel — local-only.
			_channels.append(null)
			continue

		var ch := Node.new()
		ch.set_script(script)
		ch.name = "Channel_%s" % _CATEGORY_KEYS[i].capitalize()
		ch.mode = mode
		ch.default_audible_radius = _RADII[i]
		ch.default_priority = _PRIORITIES[i]
		ch.received_remote.connect(_on_remote_event.bind(i))
		add_child(ch)
		_channels.append(ch)


func _attach_listener_to(target: Node3D) -> void:
	if _listener == null:
		var script := load(_LISTENER_SCRIPT)
		if script == null:
			push_error("[FPSCoopAudio] Couldn't load GoolListener3D script.")
			return
		_listener = Node3D.new()
		_listener.set_script(script)
		_listener.name = "Listener"
	else:
		var parent := _listener.get_parent()
		if parent != null:
			parent.remove_child(_listener)
	target.add_child(_listener)


func _detach_listener() -> void:
	if _listener == null:
		return
	var parent := _listener.get_parent()
	if parent != null:
		parent.remove_child(_listener)
	_listener.queue_free()
	_listener = null
	_local_player = null


# ---------------------------------------------------------------------
# Internals — play dispatch + diagnostics
# ---------------------------------------------------------------------

func _play(category: int, sound_name: String, position: Vector3) -> int:
	if category < 0 or category >= _channels.size():
		push_error("[FPSCoopAudio] _play with invalid category %d" % category)
		return -1
	var channel := _channels[category]
	if channel == null:
		push_error("[FPSCoopAudio] _play called on null channel for category %d; " %
				category + "if you're trying to play HUD audio, use play_hud() instead.")
		return -1
	var pid: int = channel.play(sound_name, position,
			_MODES[category], _RADII[category], _PRIORITIES[category])
	_record_event(category, sound_name, position, 0)
	return pid


func _record_event(category: int, sound_name: String,
		position: Vector3, source_peer_id: int) -> void:
	if source_peer_id == 0:
		_diag[category]["fired"] += 1
	else:
		_diag[category]["received"] += 1
	_diag[category]["last_ms"] = Time.get_ticks_msec()
	event_played.emit(category, sound_name, position, source_peer_id)


# Bound to NetworkedAudioEvent.received_remote with the category
# index already partially applied (see _create_category_channels).
func _on_remote_event(sound_name: String, position: Vector3,
		peer_id: int, category: int) -> void:
	_record_event(category, sound_name, position, peer_id)


func _on_bank_registered(results: Dictionary) -> void:
	if _bank_loader != null:
		_bank_loader.set_meta("registration_complete", true)
	sound_bank_loaded.emit(results)


# ---------------------------------------------------------------------
# Internals — diagnostic overlay
# ---------------------------------------------------------------------

func _create_overlay() -> void:
	_overlay = CanvasLayer.new()
	_overlay.name = "FPSCoopAudio_DiagnosticOverlay"
	_overlay.layer = 100  # On top of game UI

	var panel := PanelContainer.new()
	panel.position = Vector2(20, 20)
	panel.size = Vector2(360, 240)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 8)
	margin.add_theme_constant_override("margin_bottom", 8)

	_overlay_label = Label.new()
	_overlay_label.text = "FPSCoopAudio diagnostic — initializing..."
	_overlay_label.add_theme_font_size_override("font_size", 12)

	margin.add_child(_overlay_label)
	panel.add_child(margin)
	_overlay.add_child(panel)
	add_child(_overlay)


func _refresh_overlay() -> void:
	var summary := get_diagnostic_summary()
	var lines: Array[String] = []
	lines.append("FPSCoopAudio diagnostic")
	lines.append("─────────────────────")
	lines.append("listener: %s" % (
		"✓ %s" % summary["listener_target"]
		if summary["listener_attached"] else "✗ unattached"
	))
	lines.append("sound bank: %s" % (
		"✓ loaded" if summary["sound_bank_loaded"] else "✗ pending"
	))
	lines.append("")
	for key in _CATEGORY_KEYS:
		var entry: Dictionary = summary[key]
		var age_s: float = 0.0
		if entry["last_ms"] > 0:
			age_s = (Time.get_ticks_msec() - entry["last_ms"]) / 1000.0
		var age_str: String = "—" if entry["last_ms"] == 0 else (
			"%.1fs ago" % age_s
		)
		lines.append("%-9s fired %4d  recv %4d  (%s)" % [
			key.capitalize() + ":", entry["fired"], entry["received"], age_str,
		])
	_overlay_label.text = "\n".join(lines)
	diagnostic_updated.emit(summary)
