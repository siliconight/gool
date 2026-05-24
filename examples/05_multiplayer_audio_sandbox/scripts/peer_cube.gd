extends Node3D

# PeerCube
#
# Session 1: a static placeholder. Visually distinguishes local vs
# remote so the user can verify the multiplayer spawn worked
# correctly (their own cube is a different color than other
# players').
#
# Session 2: this scene will be promoted to CharacterBody3D with
# a player controller, GoolListener3D on a child camera, and a
# MultiplayerSynchronizer for position sync. For now: just a
# colored cube.

@onready var mesh: MeshInstance3D = $MeshInstance3D


func _ready() -> void:
	# Apply a color based on whether this is the local peer's cube.
	var my_peer_id := multiplayer.get_unique_id()
	var owning_peer_id := get_multiplayer_authority()
	var is_mine := (owning_peer_id == my_peer_id)

	var mat := StandardMaterial3D.new()
	if is_mine:
		# Local peer's cube: bright cyan (matches the icon).
		mat.albedo_color = Color(0.49, 0.78, 0.89)
	else:
		# Remote peer's cube: warm orange so it stands out.
		mat.albedo_color = Color(0.95, 0.55, 0.25)
	mesh.material_override = mat

	print("[peer_%d] ready (is_mine=%s)"
			% [owning_peer_id, str(is_mine)])
