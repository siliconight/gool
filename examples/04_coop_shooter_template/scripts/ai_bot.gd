# scripts/ai_bot.gd
#
# AI bot. Wanders to random points within a configurable radius,
# pauses, fires a burst of weapon shots, repeats. Stand-in for the
# 3 remote players in a real co-op session — the audio
# architecture treats their gunfire/footsteps the same way as a
# remote multiplayer peer's would.
#
# Bots fire bursts (not single shots) so the combat-intensity
# accumulator reliably crosses the "combat" threshold and shows
# the music state transition in action.

class_name AiBot
extends CharacterBody3D

@export var wander_radius: float = 8.0
@export var move_speed: float = 2.5
@export var pause_seconds: float = 1.5

## How many shots in a fire-burst.
@export var burst_min: int = 3
@export var burst_max: int = 6

## Idle delay between bursts.
@export var burst_idle_min_sec: float = 2.5
@export var burst_idle_max_sec: float = 6.0

## Which weapon this bot uses. Set at scene-init time so we can
## have one of each in the lineup.
@export var weapon_kind: Weapon.WeaponKind = Weapon.WeaponKind.RIFLE

@onready var weapon: Weapon = $Weapon
@onready var footsteps: FootstepSurfacePlayer = $FootstepSurfacePlayer

var _origin: Vector3
var _target: Vector3
var _state: String = "moving"
var _state_timer: float = 0.0
var _burst_remaining: int = 0
var _burst_shot_timer: float = 0.0
var _distance_since_step: float = 0.0

func _ready() -> void:
    _origin = global_position
    _target = _pick_wander_target()

    weapon.kind     = weapon_kind
    weapon.is_local = false

    footsteps.surface_sounds = {
        "stone": ["step_stone_a", "step_stone_b", "step_stone_c"],
    }
    footsteps.default_surface = "stone"

func _physics_process(delta: float) -> void:
    _state_timer += delta

    match _state:
        "moving":
            _do_move(delta)
        "pausing":
            velocity = Vector3.ZERO
            move_and_slide()
            if _state_timer >= pause_seconds:
                _enter_burst()
        "firing":
            _do_burst(delta)
        "idle":
            velocity = Vector3.ZERO
            move_and_slide()
            if _state_timer >= randf_range(burst_idle_min_sec, burst_idle_max_sec):
                _enter_moving()

func _do_move(delta: float) -> void:
    var to_target := _target - global_position
    to_target.y = 0.0
    var dist := to_target.length()

    if dist < 0.3:
        _enter_pausing()
        return

    var dir := to_target / dist
    velocity = dir * move_speed
    move_and_slide()

    _distance_since_step += move_speed * delta
    if _distance_since_step >= 0.85:
        _distance_since_step = 0.0
        footsteps.step()

func _do_burst(delta: float) -> void:
    velocity = Vector3.ZERO
    move_and_slide()
    _burst_shot_timer -= delta
    if _burst_shot_timer <= 0.0:
        if _burst_remaining > 0:
            if weapon.try_fire():
                _burst_remaining -= 1
        # Slight jitter in shot spacing so multiple bots don't
        # synchronize and produce machine-gun-grade pulses.
        _burst_shot_timer = randf_range(0.10, 0.25)
    if _burst_remaining <= 0:
        _enter_idle()

func _enter_moving() -> void:
    _state = "moving"
    _state_timer = 0.0
    _target = _pick_wander_target()

func _enter_pausing() -> void:
    _state = "pausing"
    _state_timer = 0.0

func _enter_burst() -> void:
    _state = "firing"
    _state_timer = 0.0
    _burst_remaining = randi_range(burst_min, burst_max)
    _burst_shot_timer = 0.0

func _enter_idle() -> void:
    _state = "idle"
    _state_timer = 0.0

func _pick_wander_target() -> Vector3:
    var ang := randf() * TAU
    var r   := randf_range(2.0, wander_radius)
    return _origin + Vector3(cos(ang) * r, 0.0, sin(ang) * r)
