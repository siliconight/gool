extends Control

# Lobby
#
# Tiny UI for choosing host or join. On either success the lobby
# transitions to the box_level scene where peer cubes are spawned.
#
# Wire-up: NetworkManager autoload's signals drive scene transitions.
# We don't transition synchronously from the button press because:
#
#   - host_game() succeeds instantly (server peer is created), so
#     hosting_started fires inside the same frame
#   - join_game() needs a network round-trip for the server to
#     accept the connection, so joined_server fires later
#
# Treating both via signals keeps the code path uniform.

const BOX_LEVEL_SCENE: String = "res://scenes/box_level.tscn"
const DEFAULT_HOST_ADDRESS: String = "127.0.0.1"

@onready var host_button: Button = $CenterContainer/VBox/HostButton
@onready var join_button: Button = $CenterContainer/VBox/JoinHBox/JoinButton
@onready var ip_input: LineEdit = $CenterContainer/VBox/JoinHBox/IpInput
@onready var status_label: Label = $CenterContainer/VBox/StatusLabel


func _ready() -> void:
    ip_input.text = DEFAULT_HOST_ADDRESS
    host_button.pressed.connect(_on_host_pressed)
    join_button.pressed.connect(_on_join_pressed)

    NetworkManager.hosting_started.connect(_on_hosting_started)
    NetworkManager.joined_server.connect(_on_joined_server)
    NetworkManager.connection_failed.connect(_on_connection_failed)
    NetworkManager.disconnected.connect(_on_disconnected)

    status_label.text = "Idle"


func _on_host_pressed() -> void:
    _set_buttons_disabled(true)
    var err := NetworkManager.host_game()
    if err != OK:
        status_label.text = "Host failed: %s" % error_string(err)
        _set_buttons_disabled(false)
        return
    # hosting_started signal will fire and trigger the transition.


func _on_join_pressed() -> void:
    var ip := ip_input.text.strip_edges()
    if ip == "":
        status_label.text = "Enter an IP first"
        return
    _set_buttons_disabled(true)
    var err := NetworkManager.join_game(ip)
    if err != OK:
        status_label.text = "Join failed: %s" % error_string(err)
        _set_buttons_disabled(false)
        return
    status_label.text = "Connecting to %s..." % ip


func _on_hosting_started() -> void:
    status_label.text = "Hosting — entering level..."
    # Defer scene change a frame so the status text actually paints.
    call_deferred("_transition_to_level")


func _on_joined_server() -> void:
    status_label.text = "Connected — entering level..."
    call_deferred("_transition_to_level")


func _on_connection_failed() -> void:
    status_label.text = "Connection failed."
    _set_buttons_disabled(false)


func _on_disconnected() -> void:
    # If we somehow get disconnected at the lobby (e.g. host crashed
    # immediately) reset to idle so the user can try again.
    status_label.text = "Disconnected. Try again."
    _set_buttons_disabled(false)


func _transition_to_level() -> void:
    var err := get_tree().change_scene_to_file(BOX_LEVEL_SCENE)
    if err != OK:
        push_error("[Lobby] scene change failed: %s" % error_string(err))
        status_label.text = "Scene change failed."
        _set_buttons_disabled(false)


func _set_buttons_disabled(disabled: bool) -> void:
    host_button.disabled = disabled
    join_button.disabled = disabled
    ip_input.editable = not disabled
