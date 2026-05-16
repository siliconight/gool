extends Node

# NetworkManager
#
# Tiny autoload that wraps Godot's MultiplayerAPI with an ENet-
# specific transport. Exposes a transport-AGNOSTIC interface —
# host_game(), join_game(), leave_game(), plus signals — so that
# call sites elsewhere in this project never reach into ENet
# specifics directly.
#
# This means swapping to SteamMultiplayerPeer (or to your eventual
# real dual-transport networking module) doesn't require changing
# anything except the bodies of host_game() / join_game().
#
# Design choices:
#
#   - Port 9999 is hardcoded as the default. Override with the
#     `port` argument to host_game() / join_game().
#   - MAX_CLIENTS = 4 because that's the target player count for
#     the eventual real game. ENet enforces this on the host side.
#   - Signals re-emit Godot's MultiplayerAPI signals 1:1 so callers
#     don't have to know about Godot's MultiplayerAPI surface at
#     all. If we ever want to add wrapping logic (rate limiting,
#     filtering, etc.) we have one place to do it.
#   - leave_game() is idempotent: safe to call when not connected.

signal hosting_started
signal joined_server
signal connection_failed
signal disconnected
signal peer_joined(peer_id: int)
signal peer_left(peer_id: int)

const DEFAULT_PORT: int = 9999
const MAX_CLIENTS: int = 4

# Track whether we've connected the MultiplayerAPI signals so
# repeated host/join calls don't double-connect.
var _signals_connected: bool = false


func host_game(port: int = DEFAULT_PORT) -> int:
    # Tear down any previous connection cleanly first so this is
    # callable from any state.
    leave_game()
    var peer := ENetMultiplayerPeer.new()
    var err := peer.create_server(port, MAX_CLIENTS)
    if err != OK:
        push_error("[NetworkManager] host failed (port=%d): %s"
                % [port, error_string(err)])
        return err
    multiplayer.multiplayer_peer = peer
    _ensure_signals_connected()
    hosting_started.emit()
    print("[NetworkManager] hosting on port %d, max %d clients"
            % [port, MAX_CLIENTS])
    return OK


func join_game(ip: String, port: int = DEFAULT_PORT) -> int:
    leave_game()
    var peer := ENetMultiplayerPeer.new()
    var err := peer.create_client(ip, port)
    if err != OK:
        push_error("[NetworkManager] join failed (%s:%d): %s"
                % [ip, port, error_string(err)])
        return err
    multiplayer.multiplayer_peer = peer
    _ensure_signals_connected()
    print("[NetworkManager] connecting to %s:%d" % [ip, port])
    return OK


func leave_game() -> void:
    if multiplayer.multiplayer_peer != null:
        multiplayer.multiplayer_peer.close()
    multiplayer.multiplayer_peer = null


func is_hosting() -> bool:
    return multiplayer.multiplayer_peer != null and multiplayer.is_server()


func is_connected_as_client() -> bool:
    if multiplayer.multiplayer_peer == null:
        return false
    return not multiplayer.is_server() \
            and multiplayer.multiplayer_peer.get_connection_status() \
                    == MultiplayerPeer.CONNECTION_CONNECTED


func get_my_peer_id() -> int:
    if multiplayer.multiplayer_peer == null:
        return 0
    return multiplayer.get_unique_id()


# Connect Godot's MultiplayerAPI signals to our re-emit wrappers.
# Idempotent: safe to call multiple times across host/join cycles.
func _ensure_signals_connected() -> void:
    if _signals_connected:
        return
    multiplayer.connected_to_server.connect(_on_connected_to_server)
    multiplayer.connection_failed.connect(_on_connection_failed)
    multiplayer.server_disconnected.connect(_on_disconnected)
    multiplayer.peer_connected.connect(_on_peer_connected)
    multiplayer.peer_disconnected.connect(_on_peer_disconnected)
    _signals_connected = true


# ---- Signal re-emit handlers ----

func _on_connected_to_server() -> void:
    print("[NetworkManager] connected to server (peer_id=%d)"
            % multiplayer.get_unique_id())
    joined_server.emit()

func _on_connection_failed() -> void:
    push_warning("[NetworkManager] connection failed")
    connection_failed.emit()

func _on_disconnected() -> void:
    push_warning("[NetworkManager] disconnected from server")
    disconnected.emit()

func _on_peer_connected(peer_id: int) -> void:
    print("[NetworkManager] peer joined: peer_id=%d" % peer_id)
    peer_joined.emit(peer_id)

func _on_peer_disconnected(peer_id: int) -> void:
    print("[NetworkManager] peer left: peer_id=%d" % peer_id)
    peer_left.emit(peer_id)
