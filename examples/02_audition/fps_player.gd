extends CharacterBody3D

# Minimal single-player FPS controller for the audition example.
# WASD + mouse look + LMB to fire an impact raycast. No networking,
# no health, no inventory — just enough to walk between rooms and
# shoot panels so the audition's audio features can be heard.
#
# The GoolListener3D under Camera3D is what gives gool the player's
# ears. Without it, all 3D audio plays at the world origin and
# spatial positioning is broken.

const MOUSE_SENSITIVITY: float = 0.0022
const MOVE_SPEED: float = 6.0
const JUMP_VELOCITY: float = 5.0
const GRAVITY: float = 18.0
const SHOOT_RANGE: float = 50.0

@onready var camera: Camera3D = $Camera3D

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	# Register the listener position with gool so spatial audio
	# attenuates correctly. Camera3D's global_transform is the
	# authoritative source for ears + forward direction.
	if Gool.has_method("set_audio_world_space_rid"):
		Gool.set_audio_world_space_rid(get_world_3d().space)

func _input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
		camera.rotate_x(-event.relative.y * MOUSE_SENSITIVITY)
		camera.rotation.x = clamp(camera.rotation.x, -1.4, 1.4)
	elif event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		# Release mouse so the user can interact with the editor /
		# close the window. F5 will recapture on next focus.
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			# First click recaptures mouse rather than firing.
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
			return
		_fire()

func _physics_process(delta: float) -> void:
	# Gravity
	if not is_on_floor():
		velocity.y -= GRAVITY * delta

	# Jump (Space)
	if Input.is_action_just_pressed("ui_accept") and is_on_floor():
		velocity.y = JUMP_VELOCITY

	# WASD planar movement, camera-relative
	var input_dir := Vector2(
			Input.get_action_strength("move_right") - Input.get_action_strength("move_left"),
			Input.get_action_strength("move_back") - Input.get_action_strength("move_forward"))
	var direction := (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()
	if direction.length_squared() > 0.001:
		velocity.x = direction.x * MOVE_SPEED
		velocity.z = direction.z * MOVE_SPEED
	else:
		velocity.x = move_toward(velocity.x, 0.0, MOVE_SPEED)
		velocity.z = move_toward(velocity.z, 0.0, MOVE_SPEED)

	move_and_slide()

	# Update the gool listener every frame so spatial audio tracks
	# the camera. GoolListener3D's _physics_process does this
	# automatically — this fallback covers users who didn't add it.

# Raycast from camera forward; on hit, read the gool_audio_material
# metadata (set by AudioMaterialTag prefab) and fire an impact sound
# at the hit point. The material-aware path picks per-material EQ.
func _fire() -> void:
	var space := get_world_3d().direct_space_state
	var from := camera.global_position
	var to := from - camera.global_transform.basis.z * SHOOT_RANGE
	var query := PhysicsRayQueryParameters3D.create(from, to)
	query.collide_with_areas = false
	var hit := space.intersect_ray(query)

	if hit.is_empty():
		# Missed — play in front of the camera as a "shot in space"
		# so the player still hears feedback.
		Gool.play_3d("impact_generic", to, 200)
		return

	var hit_point: Vector3 = hit.get("position", to)
	var collider = hit.get("collider")
	var material_id: int = 0
	if collider and collider.has_method("get_meta"):
		material_id = int(collider.get_meta("gool_audio_material", 0))

	# play_impact_sound applies per-material EQ + picks the right
	# bank entry. Falls back to play_3d if the impact bank entry
	# doesn't exist for that material.
	if Gool.has_method("play_impact_sound"):
		Gool.play_impact_sound("impact", hit_point, material_id)
	else:
		Gool.play_3d("impact_generic", hit_point, 200)
