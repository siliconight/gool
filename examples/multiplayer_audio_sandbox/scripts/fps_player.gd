extends CharacterBody3D

# FpsPlayer
#
# Session 2+3: replaces session 1's static peer_cube with an
# actual playable character. Each peer's player runs locally on
# its owning client (authority = peer_id) and replicates its
# transform to other clients via MultiplayerSynchronizer.
#
# Controls (LOCAL player only — remote players' input is ignored):
#   WASD          — move
#   Mouse         — look
#   Left click    — fire (plays gunshot via Gool.play_networked)
#   Esc           — release mouse capture
#
# Audio:
#   - GoolListener3D is attached to the Camera3D so each client
#     hears positional audio from their own player's perspective.
#   - Firing calls Gool.play_networked("gunshot", muzzle_position)
#     which plays locally on the firing client AND broadcasts via
#     @rpc to other peers (textbook 0ms-local + async-fanout).
#   - Only the LOCAL player's listener is active; remote players'
#     listeners are present in the tree but Gool only respects
#     the one with .is_current = true.

const MOVE_SPEED: float = 5.0
const MOUSE_SENSITIVITY: float = 0.002  # radians per pixel
const PITCH_LIMIT: float = deg_to_rad(85.0)

# Cooldown to prevent firing faster than ~10 Hz (otherwise a held
# mouse button hammers Gool with overlapping gunshots and the
# voice budget exhausts quickly).
const FIRE_COOLDOWN_MS: int = 100

@onready var camera: Camera3D = $Camera3D
@onready var listener: Node = $Camera3D/GoolListener3D  # Node, not typed, since GoolListener3D may not be resolvable in some headless contexts

var _is_local: bool = false
var _last_fire_ms: int = 0
var _camera_pitch: float = 0.0


func _ready() -> void:
	var my_peer_id: int = multiplayer.get_unique_id()
	var owning_peer_id: int = get_multiplayer_authority()
	_is_local = (owning_peer_id == my_peer_id)

	# Color-code so users can visually distinguish local from remote.
	var mat := StandardMaterial3D.new()
	if _is_local:
		mat.albedo_color = Color(0.49, 0.78, 0.89)  # cyan
	else:
		mat.albedo_color = Color(0.95, 0.55, 0.25)  # orange
	$Body.material_override = mat

	if _is_local:
		# Local player: make our camera + listener current, capture mouse,
		# enable input handling. Remote players don't get this treatment.
		camera.current = true
		if listener.has_method("set_current"):
			listener.set_current(true)
		elif "is_current" in listener:
			listener.is_current = true
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		print("[fps_player] LOCAL player (peer_id=%d) — mouse captured, listener active" % my_peer_id)
	else:
		# Remote player: deactivate camera + listener so they don't
		# fight the local one for "current" status.
		camera.current = false
		if "is_current" in listener:
			listener.is_current = false
		print("[fps_player] REMOTE player (peer_id=%d) — passive" % owning_peer_id)


func _physics_process(delta: float) -> void:
	# Movement runs ONLY on the local (authoritative) client.
	# MultiplayerSynchronizer replicates the resulting position
	# and rotation to other peers; their _physics_process does
	# not move the body.
	if not _is_local:
		return

	var input_dir := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W):
		input_dir.z -= 1.0
	if Input.is_physical_key_pressed(KEY_S):
		input_dir.z += 1.0
	if Input.is_physical_key_pressed(KEY_A):
		input_dir.x -= 1.0
	if Input.is_physical_key_pressed(KEY_D):
		input_dir.x += 1.0
	input_dir = input_dir.normalized()

	# Transform local input by the player's yaw so WASD is relative
	# to where the player is looking.
	var direction: Vector3 = (transform.basis * input_dir)
	direction.y = 0.0
	direction = direction.normalized()

	velocity.x = direction.x * MOVE_SPEED
	velocity.z = direction.z * MOVE_SPEED

	# Gravity (just enough to keep player on the floor; no jumping).
	if not is_on_floor():
		velocity.y -= 9.8 * delta
	else:
		velocity.y = 0.0

	move_and_slide()


func _input(event: InputEvent) -> void:
	if not _is_local:
		return

	# Mouse-look. Yaw on the body (so WASD turns with the look),
	# pitch on the camera (so the body doesn't tilt absurdly).
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var motion := event as InputEventMouseMotion
		rotate_y(-motion.relative.x * MOUSE_SENSITIVITY)
		_camera_pitch = clamp(_camera_pitch - motion.relative.y * MOUSE_SENSITIVITY,
				-PITCH_LIMIT, PITCH_LIMIT)
		camera.rotation.x = _camera_pitch

	# Fire on left-click.
	elif event is InputEventMouseButton:
		var btn := event as InputEventMouseButton
		if btn.button_index == MOUSE_BUTTON_LEFT and btn.pressed:
			_try_fire()

	# Release mouse on Escape (so user can click outside the window).
	elif event is InputEventKey:
		var key := event as InputEventKey
		if key.keycode == KEY_ESCAPE and key.pressed:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _try_fire() -> void:
	var now := Time.get_ticks_msec()
	if now - _last_fire_ms < FIRE_COOLDOWN_MS:
		return
	_last_fire_ms = now

	# Compute muzzle position roughly forward of the camera.
	var muzzle_pos: Vector3 = camera.global_position - camera.global_transform.basis.z * 0.5

	# Gool.play_networked plays locally immediately (0ms latency
	# for the firing client) AND broadcasts via @rpc to other
	# peers for them to hear positionally. This is the actual
	# test of gool's multiplayer audio chain.
	if Gool.has_method("play_networked"):
		Gool.play_networked("gunshot", muzzle_pos)
	else:
		# Fallback for older gool versions without play_networked.
		Gool.play_sound_at_location("gunshot", muzzle_pos)

	# If we want a click feedback even without networking, the
	# local play above handles it. No need for additional UI sound.


# Recapture mouse when the user clicks back into the window. Useful
# after Esc to re-engage the FPS controls.
func _unhandled_input(event: InputEvent) -> void:
	if not _is_local:
		return
	if event is InputEventMouseButton:
		var btn := event as InputEventMouseButton
		if btn.pressed and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE:
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
