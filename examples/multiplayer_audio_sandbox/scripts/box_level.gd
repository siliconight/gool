extends Node3D

# BoxLevel
#
# A tiny arena that spawns one placeholder cube per peer.
#
# Spawn strategy: the SERVER owns spawning. When a peer joins, the
# server adds a peer_cube child under the "Players" node. The
# MultiplayerSpawner sibling watches that path and auto-replicates
# the spawn to all connected clients.
#
# Session 1 scope: cubes are static. Each cube has its multiplayer
# authority set to its owning peer's peer_id so that in session 2
# we can drive position from the owning client and replicate to
# others via a MultiplayerSynchronizer on the cube.
#
# Things that would normally go here but don't yet:
#   - Player input handling (session 2)
#   - Networked SFX (session 3)
#   - Voice chat (session 4 / deferred)
#   - GoolListener3D placement (session 2, when we have a camera)

const PEER_CUBE_SCENE: PackedScene = preload("res://scenes/peer_cube.tscn")

# Range of starting positions so two cubes don't visually overlap.
const SPAWN_X_RANGE: Vector2 = Vector2(-3.0, 3.0)
const SPAWN_Z_RANGE: Vector2 = Vector2(-3.0, 3.0)
const SPAWN_Y: float = 0.5  # half a unit above the floor


func _ready() -> void:
    print("[BoxLevel] ready (is_server=%s, my_peer_id=%d)"
            % [str(multiplayer.is_server()), multiplayer.get_unique_id()])

    if multiplayer.is_server():
        # Spawn the host's own cube immediately.
        _spawn_peer_cube(multiplayer.get_unique_id())
        # And spawn a cube for each peer that joins from now on.
        NetworkManager.peer_joined.connect(_on_peer_joined)
        NetworkManager.peer_left.connect(_on_peer_left)


# Server-only. Spawning a child of $Players triggers the
# MultiplayerSpawner sibling to replicate it to all clients.
func _spawn_peer_cube(peer_id: int) -> void:
    assert(multiplayer.is_server(), "spawning must happen on the server")
    var cube: Node3D = PEER_CUBE_SCENE.instantiate()
    # Name = peer_id so we can find/remove cubes by peer on departure.
    # Using set_name AFTER instantiate but BEFORE add_child is the
    # canonical Godot pattern for predictable node paths under
    # MultiplayerSpawner.
    cube.name = "peer_%d" % peer_id
    # The peer who owns this cube is the multiplayer authority for it.
    # Session 2 will use this so each client drives its own cube's
    # position and the server (and other clients) receive the sync.
    cube.set_multiplayer_authority(peer_id)
    # Spread spawn position so cubes don't all stack at the origin.
    cube.position = Vector3(
        randf_range(SPAWN_X_RANGE.x, SPAWN_X_RANGE.y),
        SPAWN_Y,
        randf_range(SPAWN_Z_RANGE.x, SPAWN_Z_RANGE.y)
    )
    $Players.add_child(cube, true)
    print("[BoxLevel] spawned peer_%d at %s" % [peer_id, cube.position])


func _on_peer_joined(peer_id: int) -> void:
    _spawn_peer_cube(peer_id)


func _on_peer_left(peer_id: int) -> void:
    var node_name := "peer_%d" % peer_id
    var existing := $Players.get_node_or_null(node_name)
    if existing != null:
        print("[BoxLevel] removing %s" % node_name)
        existing.queue_free()
