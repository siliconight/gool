# scripts/player_controller.gd
#
# Local player. WASD to move; left-click to fire; Q to cycle
# weapons. Mouse-look is intentionally NOT implemented — the demo
# is about audio, not first-person camera handling. The Camera3D
# in the scene is a fixed third-person follow.
#
# Footsteps are emitted on a distance-traveled timer so the rate
# matches movement speed (faster movement = more frequent steps).
# This is the recommended pattern from docs/multiplayer.md §13:
# generate footsteps locally from movement, never RPC them.

class_name PlayerController
extends CharacterBody3D

@export var move_speed: float = 4.5

# Step-distance threshold: emit a footstep every N meters of
# horizontal travel. Tune to match the character's gait.
@export var step_distance_meters: float = 0.85

@onready var weapon: Weapon = $Weapon
@onready var footsteps: FootstepSurfacePlayer = $FootstepSurfacePlayer

var _distance_since_step: float = 0.0
var _weapon_kinds := [Weapon.WeaponKind.PISTOL,
                       Weapon.WeaponKind.RIFLE,
                       Weapon.WeaponKind.SHOTGUN]
var _current_weapon_idx: int = 1   # start on rifle

func _ready() -> void:
    weapon.kind     = _weapon_kinds[_current_weapon_idx]
    weapon.is_local = true

    # Footstep prefab needs a surface_sounds map. Ours is uniform
    # stone for the whole demo.
    footsteps.surface_sounds = {
        "stone": ["step_stone_a", "step_stone_b", "step_stone_c"],
    }
    footsteps.default_surface = "stone"

func _physics_process(delta: float) -> void:
    var input := Vector3.ZERO
    if Input.is_action_pressed("move_forward"): input.z -= 1.0
    if Input.is_action_pressed("move_back"):    input.z += 1.0
    if Input.is_action_pressed("move_left"):    input.x -= 1.0
    if Input.is_action_pressed("move_right"):   input.x += 1.0
    input = input.limit_length(1.0)

    velocity = input * move_speed
    move_and_slide()

    # Step accumulator — emit footstep on every step_distance traveled.
    var horizontal_speed := Vector2(velocity.x, velocity.z).length()
    if horizontal_speed > 0.1:
        _distance_since_step += horizontal_speed * delta
        if _distance_since_step >= step_distance_meters:
            _distance_since_step = 0.0
            footsteps.step()

func _unhandled_input(event: InputEvent) -> void:
    if event.is_action_pressed("fire_weapon"):
        weapon.try_fire()
    elif event.is_action_pressed("cycle_weapon"):
        _cycle_weapon()

func _cycle_weapon() -> void:
    _current_weapon_idx = (_current_weapon_idx + 1) % _weapon_kinds.size()
    weapon.kind = _weapon_kinds[_current_weapon_idx]
    var Gool := get_node("/root/Gool")
    if Gool.is_initialized():
        Gool.play_3d("ui_select", global_position, 100)
        print("[player] weapon -> ", weapon.get_kind_name())
