# addons/gool/prefabs/gool_listener_3d.gd
#
# Drag-and-drop 3D audio listener. Parent this under your camera
# (or any Node3D whose transform represents "where the player's
# ears are") and gool tracks that transform every frame, feeding
# position + forward + velocity into the engine.
#
# Replaces the otherwise-mandatory hand-rolled _process loop that
# every gool project needs:
#
#     # before:
#     func _process(delta):
#         var fwd = -$Camera3D.global_transform.basis.z
#         Gool.set_listener_transform($Camera3D.global_position, fwd)
#
#     # after:
#     # (drop a GoolListener3D under Camera3D, done)
#
# Note the name: Godot 4 already has a built-in `AudioListener3D`
# for its own audio system. The `Gool` prefix avoids the collision
# and signals which engine owns this listener.
#
# A scene with multiple GoolListener3D nodes is supported but only
# one "wins" per frame (whichever node's _process runs last). The
# node warns on _ready if it sees siblings, since this is almost
# always a bug.

@tool
class_name GoolListener3D
extends Node3D

## When true (default), drives the gool runtime's listener pose
## from this node's global_transform every frame. Disable to take
## scripted control without removing the node — useful if you
## want to lerp the listener separately from the camera (e.g.
## for cinematic transitions).
@export var enabled: bool = true

## When true, computes the listener's linear velocity from
## frame-to-frame position delta and feeds it to the engine for
## Doppler. Disable if the parent's transform teleports or has
## interpolation artifacts that would produce false Doppler
## shifts (e.g. a free-look camera with mouse smoothing on a
## stationary character).
@export var track_velocity: bool = true

# Internal: previous-frame position used for velocity derivation.
# `_have_prev` guards the first frame, where there's no delta to
# compute against.
var _prev_position: Vector3 = Vector3.ZERO
var _have_prev: bool = false

var _runtime: Node = null
var _warned_multiple: bool = false

const _LISTENER_GROUP: StringName = &"gool_listeners"

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning(
			"GoolListener3D: /root/Gool autoload not found. The gool "
			+ "plugin is installed but not enabled. Fix: open Project "
			+ "Settings → Plugins, find 'gool' in the list, tick the "
			+ "Enable checkbox."
		)
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	add_to_group(_LISTENER_GROUP)
	_check_for_duplicates()

func _check_for_duplicates() -> void:
	var listeners := get_tree().get_nodes_in_group(_LISTENER_GROUP)
	if listeners.size() > 1 and not _warned_multiple:
		push_warning(
			"GoolListener3D: %d listener nodes found in the scene. "
			% listeners.size()
			+ "gool supports a single active listener; the last node "
			+ "to call set_listener_transform per frame wins. "
			+ "Remove duplicates, or disable all but one via the "
			+ "`enabled` property to make the active listener "
			+ "unambiguous."
		)
		_warned_multiple = true

func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if not enabled or _runtime == null:
		return
	var pos: Vector3 = global_transform.origin
	# Godot convention: -Z is forward for Camera3D and the engine's
	# default node orientation. Matches the convention used by
	# AudioEmitter3D's forward computation.
	var fwd: Vector3 = -global_transform.basis.z
	var vel: Vector3 = Vector3.ZERO
	if track_velocity and _have_prev and delta > 0.0:
		vel = (pos - _prev_position) / delta
	_prev_position = pos
	_have_prev = true
	_runtime.set_listener_transform(pos, fwd, vel)

func _exit_tree() -> void:
	if is_in_group(_LISTENER_GROUP):
		remove_from_group(_LISTENER_GROUP)
