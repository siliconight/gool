# addons/gool/prefabs/reverb_zone.gd
#
# Area3D that adjusts the reverb mix when a listener enters or
# leaves. Detects the listener via group ("gool_listener" by
# default — your player character should be in that group).
#
# Each zone defines target reverb parameters (room size, damping,
# wet level). On listener entry, the zone smoothly ramps the
# active reverb bus toward those parameters; on exit, it ramps
# back to the runtime's default.
#
# This is a thin coordinator — the actual DSP is the gool reverb
# bus effect already in the engine. The zone just commands
# parameter changes via SetBusParameter.

@tool
class_name ReverbZone
extends Area3D

## Reverb room size in [0..1]: 0 = small room, 1 = cathedral.
@export_range(0.0, 1.0, 0.01) var room_size: float = 0.6

## High-frequency damping in [0..1]: 0 = bright, 1 = muffled.
@export_range(0.0, 1.0, 0.01) var damping: float = 0.5

## Wet-mix level in dB (negative = quieter).
@export_range(-60.0, 0.0, 0.5, "suffix:dB") var wet_gain_db: float = -12.0

## Smoothing time for the parameter ramp on entry/exit (ms).
@export_range(0.0, 5000.0, 1.0, "suffix:ms") var transition_ms: float = 800.0

## Group that the listener (player) is expected to be in.
@export var listener_group: String = "gool_listener"

signal listener_entered
signal listener_exited

var _runtime: Node = null
var _occupied: bool = false

func _ready() -> void:
    if Engine.is_editor_hint():
        return
    _runtime = get_node_or_null("/root/Gool")
    if _runtime == null:
        push_warning("ReverbZone: /root/Gool autoload not found")
        return
    body_entered.connect(_on_body_entered)
    body_exited.connect(_on_body_exited)

func _on_body_entered(body: Node) -> void:
    if not body.is_in_group(listener_group):
        return
    if _occupied:
        return
    _occupied = true
    listener_entered.emit()
    _apply_zone_settings()

func _on_body_exited(body: Node) -> void:
    if not body.is_in_group(listener_group):
        return
    if not _occupied:
        return
    _occupied = false
    listener_exited.emit()
    _restore_default_settings()

func _apply_zone_settings() -> void:
    # NOTE: Calling SetBusParameter on the underlying engine isn't
    # exposed through the v0 binding yet. For now, this prefab
    # emits the listener_entered signal so the host can update bus
    # parameters directly via a small C++ helper, OR just record
    # the desired room_size/damping/wet for use by the host's
    # custom reverb logic.
    #
    # When the binding adds set_bus_parameter() (planned), this
    # method calls into it directly and the zone becomes fully
    # self-contained.
    pass

func _restore_default_settings() -> void:
    pass
