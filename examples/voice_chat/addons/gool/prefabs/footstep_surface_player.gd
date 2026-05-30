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

# addons/gool/prefabs/footstep_surface_player.gd
#
## Plays footstep sounds at the parent's feet position. Surface is
## determined by raycasting downward and looking up the result's
## collision group in surface_sounds. Supports per-surface random
## variants to avoid repetitive playback.
#
## Usage:
##   1. Add as a child of your character node.
##   2. Configure surface_sounds in the inspector.
##   3. Call step() from your locomotion code (e.g. on a step
##      animation key, or every N units of horizontal movement).

@tool
class_name FootstepSurfacePlayer
extends Node3D

## Maps surface group name -> Array[String] of registered sound
## names. The plugin picks one at random per step. Example:
##   { "stone": ["step_stone_a", "step_stone_b", "step_stone_c"],
##     "grass": ["step_grass_a", "step_grass_b"] }
@export var surface_sounds: Dictionary = {}

## Default surface to use if the raycast misses or the hit body
## isn't in any known group.
@export var default_surface: String = "stone"

## How far down to raycast for surface detection (meters).
@export_range(0.1, 10.0, 0.1, "suffix:m") var raycast_distance: float = 1.5

## Footstep volume reduction relative to bus master (dB).
@export_range(-40.0, 0.0, 0.5, "suffix:dB") var step_gain_db: float = -6.0

var _runtime: Node = null
var _last_played: Dictionary = {}    # surface -> last sound index

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning("FootstepSurfacePlayer: /root/Gool autoload not found. The gool plugin is installed but not enabled. Fix: open Project Settings → Plugins, find 'gool' in the list, tick the Enable checkbox. (If gool is not in the list, the addon folder is missing — see https://github.com/siliconight/gool for install instructions.)")

# Call this from your locomotion code at each footfall.
func step() -> void:
	if _runtime == null:
		return
	var surface := _detect_surface()
	var variants: Array = surface_sounds.get(surface,
												surface_sounds.get(default_surface, []))
	if variants.is_empty():
		return

	var idx := randi() % variants.size()
	# Avoid immediate repeat when more than one variant exists.
	if variants.size() > 1 and idx == _last_played.get(surface, -1):
		idx = (idx + 1) % variants.size()
	_last_played[surface] = idx

	_runtime.play_sound_at_location(variants[idx], global_transform.origin)

func _detect_surface() -> String:
	var space := get_world_3d().direct_space_state
	var from := global_transform.origin
	var to := from + Vector3.DOWN * raycast_distance
	var query := PhysicsRayQueryParameters3D.create(from, to)
	var hit := space.intersect_ray(query)
	if hit.is_empty():
		return default_surface
	var collider: Node = hit["collider"]
	for surface_name in surface_sounds.keys():
		if collider.is_in_group(surface_name):
			return surface_name
	return default_surface
