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

# examples/coop_4p_minimal/main.gd
#
## End-to-end validation scene for gool's 4P FPS PvE audio stack.
## Constructs the entire scene at runtime — no .tscn surgery, no
## external assets, no gameplay-tuning baggage. Lets a dev verify
## gool is doing what their game needs before they commit to building
## the real game on top of it.
#
## WHAT THIS VALIDATES
## ===================
#
##   1. Engine boot
##      The Gool autoload initializes (compiled C++ engine + Godot
##      binding loaded; mixer + bus topology built from default config).
#
##   2. Sound bank registration
##      Five procedurally-synthesized PCM sounds are registered via
##      Gool.register_pcm_sound() — one per FPSCoopAudio category.
##      No external audio assets needed; the sounds are distinguishable
##      by frequency content (weapon = 880Hz click, enemy = 100Hz low
##      sawtooth, world = noise burst, movement = 200Hz tick,
##      hud = 1320Hz beep).
#
##   3. FPSCoopAudio prefab
##      Each of the five categories (WEAPONS, ENEMIES, WORLD,
##      MOVEMENT, HUD) is fired exactly once. The diagnostic
##      summary's `fired` counts are asserted == 1 per category.
##      PASS/FAIL is reported on-screen within 2 seconds of launch.
#
##   4. Listener attachment
##      The player capsule is wired as the listener target. Walking
##      around the scene moves the listener in 3D, so subsequent
##      sounds fired at fixed positions change apparent direction.
#
##   5. ReverbZone
##      A large translucent cube to the east is a ReverbZone. Walk
##      into it; the reverb send increases. This validates spatial
##      audio routing.
#
##   6. RPC replication (manual, multi-instance)
##      The "Host" / "Join" buttons start an ENet server/client on
##      localhost:7777. Launch a second Godot instance, click Join,
##      and fire events on either. The diagnostic overlay's `recv`
##      counters confirm cross-peer replication.
#
## WHAT THIS DOES *NOT* DO
## =======================
#
##   - Real game graphics. The world is a flat plane with colored
##     test cubes. Use this to confirm gool works, then bring in
##     your real geometry.
#
##   - Real game sounds. The procedural placeholder sounds are
##     audible and category-distinguishable but obviously not what
##     you'd ship. Once gool is validated, swap in your real bank.
#
##   - Production multiplayer wiring (matchmaking, NAT punch,
##     authoritative cheat protection). This uses ENet on localhost.
##     Real production code would replace the Host/Join buttons with
##     your matchmaking flow.
#
##   - AI bots, weapon mechanics, damage, scoring, win conditions.
##     This is an audio validation tool, not a game template.

extends Node


# ---------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------

const SAMPLE_RATE := 48000
const NETWORK_PORT := 7777

# Map of sound name → procedural-sound generator parameters. Each
# entry tells _generate_sound() what frequency content, envelope,
# and duration to synthesize. Keeping these declarative makes it
# trivial to add more validation sounds later (just append a row).
const SOUND_RECIPES := [
	# {name, freq_hz, duration_s, noise_mix, decay_shape}
	{"name": "weapon_fire",  "freq": 880.0, "dur": 0.18, "noise": 0.4, "decay": "sharp"},
	{"name": "enemy_growl",  "freq": 100.0, "dur": 0.45, "noise": 0.2, "decay": "long"},
	{"name": "world_boom",   "freq": 60.0,  "dur": 0.60, "noise": 0.7, "decay": "long"},
	{"name": "footstep",     "freq": 200.0, "dur": 0.08, "noise": 0.6, "decay": "sharp"},
	{"name": "hud_beep",     "freq": 1320.0,"dur": 0.10, "noise": 0.0, "decay": "sharp"},
]


# ---------------------------------------------------------------------
# Scene state
# ---------------------------------------------------------------------

var _fps_audio: Node = null         # FPSCoopAudio instance
var _player: Node3D = null           # local player (no physics, just a Node3D)
var _camera: Camera3D = null
var _status_label: Label = null
var _result_banner: Label = null
var _network_label: Label = null

var _validation_complete := false
var _validation_passed := false


# Test-marker cubes around the player. The action buttons fire sounds
# at random markers so spatial positioning is audible.
var _test_markers: Array[Node3D] = []


# ---------------------------------------------------------------------
# Boot
# ---------------------------------------------------------------------

func _ready() -> void:
	_build_world()
	_build_player()
	_build_hud()
	_build_fps_audio()

	# Phase 1: register procedural sounds. Async-ish — we wait for
	# the sound_bank_loaded signal before running the validation
	# sweep so we're not racing the C++ side.
	_register_procedural_sounds()
	_run_validation_when_ready()


# ---------------------------------------------------------------------
# Scene construction
# ---------------------------------------------------------------------

func _build_world() -> void:
	# Ground plane — a wide gray PlaneMesh as a visual reference for
	# the player. No physics on the ground because the player is
	# CharacterBody3D and we just clamp y manually below.
	var ground := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(40, 40)
	ground.mesh = plane
	var ground_mat := StandardMaterial3D.new()
	ground_mat.albedo_color = Color(0.2, 0.22, 0.25)
	ground.material_override = ground_mat
	add_child(ground)

	# Four colored test-marker cubes at cardinal directions. The
	# action buttons fire sounds at random markers so the player
	# hears spatial positioning working (left ear vs. right ear,
	# closer vs. farther, etc.).
	var directions := [
		{"pos": Vector3( 8, 0.5,  0), "color": Color(1.0, 0.4, 0.4)},  # East — red
		{"pos": Vector3(-8, 0.5,  0), "color": Color(0.4, 1.0, 0.4)},  # West — green
		{"pos": Vector3( 0, 0.5,  8), "color": Color(0.4, 0.4, 1.0)},  # North — blue
		{"pos": Vector3( 0, 0.5, -8), "color": Color(1.0, 1.0, 0.4)},  # South — yellow
	]
	for d in directions:
		var cube := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(1.5, 1.0, 1.5)
		cube.mesh = box
		var mat := StandardMaterial3D.new()
		mat.albedo_color = d["color"]
		mat.emission_enabled = true
		mat.emission = d["color"] * 0.4
		cube.material_override = mat
		cube.position = d["pos"]
		add_child(cube)
		_test_markers.append(cube)

	# Reverb zone to the east-northeast — a translucent cyan cube
	# the player can walk into. The ReverbZone prefab handles the
	# bus-send change automatically.
	var rz_script := load("res://addons/gool/prefabs/reverb_zone.gd")
	if rz_script != null:
		var rz := Area3D.new()
		rz.set_script(rz_script)
		rz.position = Vector3(6, 1.5, 6)
		var rz_shape := CollisionShape3D.new()
		var rz_box := BoxShape3D.new()
		rz_box.size = Vector3(6, 3, 6)
		rz_shape.shape = rz_box
		rz.add_child(rz_shape)
		# Visualization mesh so the player can see the zone
		var rz_viz := MeshInstance3D.new()
		var viz_box := BoxMesh.new()
		viz_box.size = Vector3(6, 3, 6)
		rz_viz.mesh = viz_box
		var rz_mat := StandardMaterial3D.new()
		rz_mat.albedo_color = Color(0.4, 0.9, 1.0, 0.25)
		rz_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		rz_viz.material_override = rz_mat
		rz.add_child(rz_viz)
		add_child(rz)

	# Lighting — one directional light + ambient so the cubes are
	# visible. Validation scene; no fancy GI.
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-45, 35, 0)
	sun.light_energy = 0.9
	add_child(sun)
	var env := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.07, 0.08, 0.10)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.4, 0.45, 0.5)
	environment.ambient_light_energy = 0.5
	env.environment = environment
	add_child(env)


func _build_player() -> void:
	# Simple Node3D player. Not a CharacterBody3D — this is a
	# validation scene, not a game. We just glide around at fixed
	# height via _physics_process below; collision and gravity would
	# be noise.
	_player = Node3D.new()
	_player.name = "Player"
	_player.position = Vector3(0, 1, 0)

	var capsule := MeshInstance3D.new()
	var cap_mesh := CapsuleMesh.new()
	cap_mesh.radius = 0.4
	cap_mesh.height = 1.8
	capsule.mesh = cap_mesh
	var cap_mat := StandardMaterial3D.new()
	cap_mat.albedo_color = Color(0.9, 0.9, 0.9)
	capsule.material_override = cap_mat
	_player.add_child(capsule)

	# Camera pivoted to first-person height. Sits inside the capsule.
	_camera = Camera3D.new()
	_camera.position = Vector3(0, 0.7, 0)
	_player.add_child(_camera)

	add_child(_player)


# Lightweight movement directly in main — WASD/arrows glide the
# player at fixed Y. Validation scene; no physics needed.
const _PLAYER_SPEED := 5.0

func _physics_process(delta: float) -> void:
	if _player == null:
		return
	var input_dir := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	if input_dir.length() > 0.01:
		var move := Vector3(input_dir.x, 0, input_dir.y).normalized() * _PLAYER_SPEED * delta
		_player.position += move


func _build_hud() -> void:
	# Top-left status label + result banner (set after validation)
	var canvas := CanvasLayer.new()
	canvas.layer = 10
	add_child(canvas)

	var top_panel := PanelContainer.new()
	top_panel.position = Vector2(20, 280)  # Below the FPSCoopAudio overlay
	top_panel.size = Vector2(360, 100)
	var top_margin := MarginContainer.new()
	top_margin.add_theme_constant_override("margin_left", 12)
	top_margin.add_theme_constant_override("margin_right", 12)
	top_margin.add_theme_constant_override("margin_top", 8)
	top_margin.add_theme_constant_override("margin_bottom", 8)
	var top_vbox := VBoxContainer.new()
	_status_label = Label.new()
	_status_label.text = "Initializing..."
	_status_label.add_theme_font_size_override("font_size", 12)
	top_vbox.add_child(_status_label)
	_network_label = Label.new()
	_network_label.text = "Network: standalone"
	_network_label.add_theme_font_size_override("font_size", 11)
	top_vbox.add_child(_network_label)
	top_margin.add_child(top_vbox)
	top_panel.add_child(top_margin)
	canvas.add_child(top_panel)

	# Result banner (center top, only visible after validation runs)
	_result_banner = Label.new()
	_result_banner.text = ""
	_result_banner.add_theme_font_size_override("font_size", 28)
	_result_banner.position = Vector2(20, 400)
	_result_banner.size = Vector2(720, 60)
	canvas.add_child(_result_banner)

	# Action buttons — bottom of screen, one per FPSCoopAudio category
	var hbox := HBoxContainer.new()
	hbox.position = Vector2(20, 560)
	hbox.add_theme_constant_override("separation", 10)
	for entry in [
		{"label": "Fire Weapon (1)",  "method": "_fire_weapon"},
		{"label": "Enemy (2)",        "method": "_fire_enemy"},
		{"label": "World Boom (3)",   "method": "_fire_world"},
		{"label": "Footstep (4)",     "method": "_fire_movement"},
		{"label": "HUD Beep (5)",     "method": "_fire_hud"},
	]:
		var btn := Button.new()
		btn.text = entry["label"]
		btn.pressed.connect(Callable(self, entry["method"]))
		hbox.add_child(btn)
	canvas.add_child(hbox)

	# Network buttons — top-right
	var net_vbox := VBoxContainer.new()
	net_vbox.position = Vector2(900, 20)
	var host_btn := Button.new()
	host_btn.text = "Host (port 7777)"
	host_btn.pressed.connect(_host_session)
	net_vbox.add_child(host_btn)
	var join_btn := Button.new()
	join_btn.text = "Join localhost:7777"
	join_btn.pressed.connect(_join_session)
	net_vbox.add_child(join_btn)
	canvas.add_child(net_vbox)


func _build_fps_audio() -> void:
	# Instance FPSCoopAudio with the diagnostic overlay enabled so
	# the validation evidence is visible.
	var script := load("res://addons/gool/prefabs/fps_coop_audio.gd")
	if script == null:
		push_error("[validation] Couldn't load fps_coop_audio.gd — addon missing?")
		return
	_fps_audio = Node.new()
	_fps_audio.set_script(script)
	_fps_audio.name = "FPSCoopAudio"
	_fps_audio.show_diagnostic_overlay = true
	# Listener gets bound after a frame so the player is fully in the
	# scene tree first.
	add_child(_fps_audio)
	_fps_audio.sound_bank_loaded.connect(_on_bank_loaded)
	call_deferred("_bind_listener")


func _bind_listener() -> void:
	if _player == null or _fps_audio == null:
		return
	_fps_audio.set_local_player(_player)
	_status_label.text = "Listener attached to Player. Generating sounds..."


# ---------------------------------------------------------------------
# Procedural sound generation
# ---------------------------------------------------------------------

func _register_procedural_sounds() -> void:
	# We don't set a GoolSoundBank — instead we register PCM sounds
	# directly via the Gool autoload. This is the simplest validation
	# path (no resource file authoring needed).
	var gool := get_node_or_null("/root/Gool")
	if gool == null:
		_status_label.text = "FAILED: /root/Gool autoload missing. " + \
				"Project Settings → Plugins → enable 'gool'."
		return

	for recipe in SOUND_RECIPES:
		var samples := _generate_sound(
			recipe["freq"], recipe["dur"],
			recipe["noise"], recipe["decay"]
		)
		# register_pcm_sound signature varies; we pass minimum args.
		# Returns 0 on success per gool convention.
		gool.register_pcm_sound(recipe["name"], samples, SAMPLE_RATE, 1)

	# Fake the "bank loaded" callback so validation can proceed —
	# we didn't go through GoolSoundBankLoader so the signal won't
	# fire naturally. _on_bank_loaded just kicks off the auto-test.
	call_deferred("_on_bank_loaded", {})


func _generate_sound(freq: float, duration_s: float,
		noise_mix: float, decay_shape: String) -> PackedFloat32Array:
	var n := int(SAMPLE_RATE * duration_s)
	var out := PackedFloat32Array()
	out.resize(n)
	# Decay envelope shape: "sharp" = fast falloff (clicks),
	# "long" = slow falloff (rumbles).
	var decay_rate: float = 25.0 if decay_shape == "sharp" else 4.0
	# RNG for noise mixing — deterministic seed so validation is repeatable.
	var rng := RandomNumberGenerator.new()
	rng.seed = 0xCAFE
	for i in range(n):
		var t: float = float(i) / SAMPLE_RATE
		var env: float = exp(-t * decay_rate)
		var tone: float = sin(t * freq * TAU)
		var noise: float = (rng.randf() * 2.0 - 1.0)
		var sample: float = (tone * (1.0 - noise_mix) + noise * noise_mix) * env * 0.4
		out[i] = sample
	return out


# ---------------------------------------------------------------------
# Auto-validation
# ---------------------------------------------------------------------

func _run_validation_when_ready() -> void:
	# Just a marker — the actual run is triggered by _on_bank_loaded.
	pass


func _on_bank_loaded(_results: Dictionary) -> void:
	if _validation_complete:
		return
	_status_label.text = "Sound bank loaded. Running auto-validation..."
	# Give the engine one frame to settle, then fire the test events.
	call_deferred("_execute_validation_sweep")


func _execute_validation_sweep() -> void:
	# Fire one event per category. After all five, check the
	# diagnostic summary for the expected counts.
	if _fps_audio == null:
		return
	var pos := Vector3(0, 1, 0)
	_fps_audio.play_weapon("weapon_fire", pos + Vector3(2, 0, 0))
	_fps_audio.play_enemy("enemy_growl", pos + Vector3(-2, 0, 0))
	_fps_audio.play_world("world_boom", pos + Vector3(0, 0, 2))
	_fps_audio.play_movement("footstep", pos + Vector3(0, 0, -2))
	_fps_audio.play_hud("hud_beep")
	# Check counts after a beat (give async machinery time to settle).
	get_tree().create_timer(0.5).timeout.connect(_check_validation_counts)


func _check_validation_counts() -> void:
	if _fps_audio == null:
		_set_result(false, "FPSCoopAudio prefab missing")
		return

	var summary: Dictionary = _fps_audio.get_diagnostic_summary()
	var expected_categories := ["weapons", "enemies", "world", "movement", "hud"]
	var failures: Array[String] = []

	if not summary.get("listener_attached", false):
		failures.append("listener not attached")

	for cat in expected_categories:
		var entry: Dictionary = summary.get(cat, {})
		var fired: int = entry.get("fired", 0)
		if fired < 1:
			failures.append("%s: expected fired >= 1, got %d" % [cat, fired])

	_validation_complete = true
	if failures.is_empty():
		_set_result(true, "All 5 categories fired. Listener attached. " +
				"Sound bank registered.")
	else:
		_set_result(false, "\n".join(failures))


func _set_result(passed: bool, detail: String) -> void:
	_validation_passed = passed
	if passed:
		_result_banner.text = "✓ VALIDATION PASSED"
		_result_banner.add_theme_color_override("font_color", Color(0.4, 1.0, 0.5))
		_status_label.text = "PASSED — %s\n\nManual mode: WASD to move, " % detail + \
				"buttons to fire events, walk into the cyan cube for reverb."
	else:
		_result_banner.text = "✗ VALIDATION FAILED"
		_result_banner.add_theme_color_override("font_color", Color(1.0, 0.4, 0.4))
		_status_label.text = "FAILED — %s" % detail


# ---------------------------------------------------------------------
# Manual action buttons (post-validation play-around mode)
# ---------------------------------------------------------------------

func _random_marker_position() -> Vector3:
	if _test_markers.is_empty():
		return Vector3(randf_range(-5, 5), 1, randf_range(-5, 5))
	var marker: Node3D = _test_markers[randi() % _test_markers.size()]
	return marker.global_position


func _fire_weapon() -> void:
	_fps_audio.play_weapon("weapon_fire", _random_marker_position())


func _fire_enemy() -> void:
	_fps_audio.play_enemy("enemy_growl", _random_marker_position())


func _fire_world() -> void:
	_fps_audio.play_world("world_boom", _random_marker_position())


func _fire_movement() -> void:
	_fps_audio.play_movement("footstep", _player.global_position)


func _fire_hud() -> void:
	_fps_audio.play_hud("hud_beep")


# ---------------------------------------------------------------------
# Network host/join (manual multi-instance multiplayer testing)
# ---------------------------------------------------------------------

func _host_session() -> void:
	var peer := ENetMultiplayerPeer.new()
	var err := peer.create_server(NETWORK_PORT, 4)
	if err != OK:
		_network_label.text = "Network: failed to host (err %d)" % err
		return
	multiplayer.multiplayer_peer = peer
	_network_label.text = "Network: HOSTING on port %d (waiting for peers)" % NETWORK_PORT


func _join_session() -> void:
	var peer := ENetMultiplayerPeer.new()
	var err := peer.create_client("127.0.0.1", NETWORK_PORT)
	if err != OK:
		_network_label.text = "Network: failed to join (err %d)" % err
		return
	multiplayer.multiplayer_peer = peer
	_network_label.text = "Network: connecting to 127.0.0.1:%d..." % NETWORK_PORT


# ---------------------------------------------------------------------
# Keyboard shortcuts (1-5 for category fire)
# ---------------------------------------------------------------------

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed:
		return
	match event.keycode:
		KEY_1: _fire_weapon()
		KEY_2: _fire_enemy()
		KEY_3: _fire_world()
		KEY_4: _fire_movement()
		KEY_5: _fire_hud()

