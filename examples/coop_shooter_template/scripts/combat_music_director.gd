# scripts/combat_music_director.gd
#
# Watches every Weapon in the scene and decides what music state
# the MusicStateController should be in:
#
#   no recent gunfire           -> "explore"   (calm pad)
#   gunfire in last 6 sec       -> "suspicion" (chord pad)
#   sustained gunfire           -> "combat"    (rhythmic punctuated pad)
#
# Also drives the "combat_intensity" RTPC so music attenuates
# (ducks) under heavy gunfire activity. Local shots bump intensity
# harder than remote shots — your own gun should make the music
# step back further than someone else's.
#
# This is the demo's stand-in for the multi-tier sidechain ducking
# in the C++ multi_tier_ducking example. The two mechanisms produce
# the same audible result for a player but use different DSP:
#   - sidechain compression: the music BUS is processed by an
#     envelope follower keyed off the SFX bus's signal
#   - RTPC ducking (this demo): a gameplay parameter modulates the
#     music sound's volume directly via SoundDefinition's RTPC binding
# RTPC ducking is exposed to GDScript today; sidechain bus
# compression requires the bus-config GDScript bindings that are
# on the roadmap. See README.

class_name CombatMusicDirector
extends Node

## Reference to the MusicStateController prefab in the scene. Set
## from main.gd at scene init.
var music: Node = null

## Combat-intensity decay per second. At intensity 1.0 with no new
## fire events, intensity falls to ~0 in 1 / decay seconds.
@export var intensity_decay_per_sec: float = 0.6

## How much each LOCAL fire event adds to intensity. Capped at 1.0.
@export var local_fire_intensity_bump: float = 0.55

## How much each REMOTE fire event adds. Lower than local — your
## own gun should win the mix more than a teammate's.
@export var remote_fire_intensity_bump: float = 0.25

## How recent a fire event must be to count toward the suspicion
## threshold (in seconds).
@export var suspicion_window_sec: float = 6.0

## Sustained-fire threshold for combat state: this many fire events
## within `combat_window_sec` to escalate from suspicion to combat.
@export var combat_threshold_count: int = 5
@export var combat_window_sec:        float = 3.0

# State.
var _intensity: float = 0.0
var _recent_fires: Array[float] = []   # timestamps in seconds since startup
var _current_state: String = "explore"
var _runtime_seconds: float = 0.0

func _ready() -> void:
    # Connect to every weapon already in the scene tree. New weapons
    # added later (e.g. picked up by a character) would need to call
    # `register_weapon` themselves.
    for w in get_tree().get_nodes_in_group("weapons"):
        register_weapon(w)

func register_weapon(weapon: Weapon) -> void:
    weapon.fired.connect(_on_weapon_fired)

func _on_weapon_fired(is_local: bool, _kind: int) -> void:
    var bump := local_fire_intensity_bump if is_local else remote_fire_intensity_bump
    _intensity = min(1.0, _intensity + bump)
    _recent_fires.append(_runtime_seconds)

func _process(delta: float) -> void:
    _runtime_seconds += delta

    # Decay intensity. The RTPC's smoothing_ms (300 ms in the
    # binding) handles the audible slew — we set the target every
    # frame and the engine smoothly tracks toward it.
    _intensity = max(0.0, _intensity - intensity_decay_per_sec * delta)

    # Push to RTPC. The "combat_intensity" parameter is bound to
    # music volume in audio_setup.gd.
    var Gool := get_node("/root/Gool")
    if Gool.is_initialized():
        Gool.set_rtpc("combat_intensity", _intensity)

    # State transition. Pop any fire events that fell outside the
    # suspicion window.
    while not _recent_fires.is_empty() and \
          _recent_fires[0] < _runtime_seconds - suspicion_window_sec:
        _recent_fires.pop_front()

    var fires_in_combat_window := 0
    for ts in _recent_fires:
        if ts >= _runtime_seconds - combat_window_sec:
            fires_in_combat_window += 1

    var target_state := "explore"
    if fires_in_combat_window >= combat_threshold_count:
        target_state = "combat"
    elif not _recent_fires.is_empty():
        target_state = "suspicion"

    if target_state != _current_state and music != null:
        # Crossfade durations chosen so transitions match the
        # narrative urgency:
        #   explore   -> suspicion: 1500 ms (gentle ramp-up)
        #   suspicion -> combat:    600 ms  (tight, things-just-got-serious)
        #   combat    -> suspicion: 2000 ms (relax slowly)
        #   anything  -> explore:   3000 ms (longest, calm again)
        var fade_ms := 1500.0
        match [_current_state, target_state]:
            ["suspicion", "combat"]:                    fade_ms = 600.0
            ["combat",    "suspicion"]:                 fade_ms = 2000.0
            ["combat",    "explore"], \
            ["suspicion", "explore"]:                   fade_ms = 3000.0
        music.set_state(target_state, fade_ms)
        _current_state = target_state
