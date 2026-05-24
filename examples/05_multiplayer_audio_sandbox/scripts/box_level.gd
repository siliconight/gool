extends Node3D

# BoxLevel
#
# Session 2+3: spawns one FpsPlayer per connected peer (replacing
# session 1's static peer cubes), starts looping music on each
# client locally, and provides a small arena for spatial audio
# testing.
#
# Spawn strategy: same as session 1 — server owns spawning,
# MultiplayerSpawner replicates spawn events to clients. Each
# spawned player has its multiplayer authority set to its owning
# peer so the owning client drives input + position, and the
# MultiplayerSynchronizer inside the player scene replicates
# transform to other peers.
#
# Music strategy: every client (host + remote) independently
# starts the music loop in its own _ready(). Music is generated
# programmatically by AudioSetup so it's identical on every
# client, and since the loop is sample-rate-locked all clients
# stay closely in sync without an authoritative server step.
# In a real game with recorded music you'd typically have the
# host send a "start music NOW" RPC with a sync timestamp.

const FPS_PLAYER_SCENE: PackedScene = preload("res://scenes/fps_player.tscn")

# Range of starting positions so two players don't spawn on top
# of each other in this small arena.
const SPAWN_X_RANGE: Vector2 = Vector2(-6.0, 6.0)
const SPAWN_Z_RANGE: Vector2 = Vector2(-6.0, 6.0)
const SPAWN_Y: float = 0.0

var _music_handle: int = -1


func _ready() -> void:
	print("[BoxLevel] ready (is_server=%s, my_peer_id=%d)"
			% [str(multiplayer.is_server()), multiplayer.get_unique_id()])

	_start_music()

	if multiplayer.is_server():
		_spawn_player(multiplayer.get_unique_id())
		NetworkManager.peer_joined.connect(_on_peer_joined)
		NetworkManager.peer_left.connect(_on_peer_left)


func _exit_tree() -> void:
	# Stop music when leaving the level.
	if _music_handle >= 0:
		Gool.destroy_emitter(_music_handle, 200.0)
		_music_handle = -1


func _spawn_player(peer_id: int) -> void:
	assert(multiplayer.is_server(), "spawning must happen on the server")
	var player: Node3D = FPS_PLAYER_SCENE.instantiate()
	player.name = "peer_%d" % peer_id
	player.set_multiplayer_authority(peer_id)
	player.position = Vector3(
		randf_range(SPAWN_X_RANGE.x, SPAWN_X_RANGE.y),
		SPAWN_Y,
		randf_range(SPAWN_Z_RANGE.x, SPAWN_Z_RANGE.y)
	)
	$Players.add_child(player, true)
	print("[BoxLevel] spawned peer_%d at %s" % [peer_id, player.position])


func _on_peer_joined(peer_id: int) -> void:
	_spawn_player(peer_id)


func _on_peer_left(peer_id: int) -> void:
	var node_name := "peer_%d" % peer_id
	var existing := $Players.get_node_or_null(node_name)
	if existing != null:
		print("[BoxLevel] removing %s" % node_name)
		existing.queue_free()


func _start_music() -> void:
	if not Gool.is_initialized():
		push_warning("[BoxLevel] Gool not initialized; music not started")
		return
	_music_handle = Gool.create_emitter("music", Vector3.ZERO, true, 250.0)
	if _music_handle < 0:
		push_warning("[BoxLevel] failed to start music (handle=%d)" % _music_handle)
	else:
		print("[BoxLevel] music started (handle=%d)" % _music_handle)
