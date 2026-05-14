# addons/gool/editor/sound_name_inspector.gd
#
# Editor inspector plugin (v0.22.0). For any node that has a
# `sound_name: String` @export property — AudioEmitter3D,
# NetworkedAudioEvent, NetworkedAudioEmitter3D, MusicStateController —
# replaces the default String text input with a dropdown of sound
# names discovered from every GoolSoundBank / GoolFolderSoundBank
# resource in the project.
#
# UX goal: drop a NetworkedAudioEvent into a scene, click its
# sound_name field in the inspector, see a dropdown of every sound
# the project knows about. No more typing names from memory and
# discovering at runtime that you spelled it wrong.
#
# Behavior:
#   * If the project contains zero sound banks, falls back to the
#     default String editor (free-form text).
#   * If sound banks exist, shows an OptionButton with all discovered
#     names plus a "(custom)" option that re-enables free-form text
#     for advanced cases (programmatically-registered sounds the
#     designer needs to reference).
#   * Scans resources on first inspector display, caches results,
#     and refreshes on a configurable interval (default: every time
#     the inspector reopens, which is cheap).
#
# This is an editor-only feature; it doesn't affect runtime behavior.
# The underlying property is still a plain String — typed or chosen
# via dropdown, both produce the same field value.

@tool
extends EditorInspectorPlugin

# Properties this plugin replaces. If a node has any of these as a
# @export String property, we provide the dropdown. Keeping this as
# an explicit list rather than "any sound_name" so adding a property
# in a different prefab doesn't accidentally get the dropdown (e.g.,
# if some future class has a sound_name that takes raw file paths).
const _ASSISTED_PROPERTY_NAMES: Array[String] = [
    "sound_name",
]

# Cached sound names. Lazily populated on first request. Cleared
# via clear_cache() to force a rescan on next access.
#
# v0.22.3: clear_cache() is now called by plugin.gd whenever the
# editor's EditorFileSystem.filesystem_changed signal fires (any
# project file added/removed/moved/reimported). So the cache stays
# fresh automatically — dropping a new audio file or adding a new
# GoolSoundBank invalidates the cache, and the next inspector
# render re-scans and shows the new names. No plugin toggle or
# editor restart needed anymore.
static var _cached_names: PackedStringArray = PackedStringArray()
static var _cache_valid: bool = false

func _can_handle(object: Object) -> bool:
    # Returning true means "I want a chance at this object's properties
    # via _parse_property". The actual property-by-property decision
    # happens there.
    return object != null

func _parse_property(object: Object, type: int, name: String,
                       hint_type: int, hint_string: String,
                       usage_flags: int, wide: bool) -> bool:
    # type comes from Variant.Type; for String properties, type == TYPE_STRING.
    if type != TYPE_STRING:
        return false
    if not (name in _ASSISTED_PROPERTY_NAMES):
        return false
    # Refresh the sound-name cache if it's been invalidated. The
    # cache is invalidated on two triggers:
    #   1. First access (cache starts invalid)
    #   2. Any project filesystem change (plugin.gd calls
    #      clear_cache() from the filesystem_changed handler —
    #      v0.22.3)
    # The actual project scan only runs here, lazily, when an
    # inspector with a sound_name property is rendered — so a burst
    # of filesystem_changed signals during an import collapses to
    # at most one rescan, on the next render.
    if not _cache_valid:
        _refresh_cache()
    if _cached_names.is_empty():
        # No banks found anywhere in the project. Let the default
        # String editor handle it — the user can still type a name,
        # they just don't get autocomplete assistance.
        return false
    var editor := SoundNameOptionEditor.new(_cached_names)
    add_property_editor(name, editor)
    return true

# Walks res:// recursively, finds .tres files whose contents declare
# them as GoolSoundBank or GoolFolderSoundBank subclasses, loads each,
# and aggregates the sound names. Stores results in _cached_names.
#
# Two important details about scanning .tres files:
#   1. We check the FIRST FEW LINES of the file (cheap, no full load)
#      to identify candidates before calling load(), which is expensive.
#      A real "[gd_resource ... script=class_path]" line tells us
#      whether this is a bank-like resource.
#   2. For GoolFolderSoundBank, _init() runs at load time and
#      populates sounds dict via DirAccess — no special handling
#      needed here, the inherited `sounds` field reads correctly.
static func _refresh_cache() -> void:
    var names_set: Dictionary = {}    # use as a set; preserves insertion order in 4.x
    _scan_for_banks("res://", names_set)
    _cached_names = PackedStringArray(names_set.keys())
    _cached_names.sort()
    _cache_valid = true

static func _scan_for_banks(absolute_path: String, names_set: Dictionary) -> void:
    var dir := DirAccess.open(absolute_path)
    if dir == null:
        return
    dir.list_dir_begin()
    while true:
        var entry := dir.get_next()
        if entry == "":
            break
        if entry.begins_with("."):
            continue
        # Skip Godot's internal cache and the addon itself (the addon's
        # own example tres files would add noise; we want USER banks).
        if entry in [".godot", ".import", "addons"]:
            continue
        var child := absolute_path.path_join(entry)
        if dir.current_is_dir():
            _scan_for_banks(child, names_set)
            continue
        if not entry.ends_with(".tres"):
            continue
        # Quick header check: peek at first ~1KB to see if this is a
        # GoolSoundBank-flavored resource before doing a full load().
        # Avoids loading every Resource in the project just to check
        # its type.
        if not _is_sound_bank_tres(child):
            continue
        var bank: Resource = load(child)
        if bank == null:
            continue
        # Read the `sounds` Dictionary. Both GoolSoundBank and
        # GoolFolderSoundBank expose this field with the same shape.
        var sounds_dict: Variant = bank.get("sounds")
        if sounds_dict == null or not (sounds_dict is Dictionary):
            continue
        for key in sounds_dict.keys():
            if typeof(key) == TYPE_STRING:
                names_set[key] = true
    dir.list_dir_end()

# Cheap pre-load type check. Peeks at the first 256 bytes of the .tres
# file looking for the "GoolSoundBank" or "GoolFolderSoundBank" class
# name marker that Godot writes for typed resources. Returns false
# for any .tres that isn't a sound bank, which avoids the full load()
# cost on unrelated resources.
static func _is_sound_bank_tres(path: String) -> bool:
    var f := FileAccess.open(path, FileAccess.READ)
    if f == null:
        return false
    var header := f.get_buffer(512).get_string_from_utf8()
    f.close()
    # The .tres header has lines like:
    #   [gd_resource type="Resource" script_class="GoolSoundBank" ...]
    # or referenced via ext_resource pointing at the script path.
    return header.contains("GoolSoundBank") \
        or header.contains("GoolFolderSoundBank") \
        or header.contains("gool_sound_bank.gd") \
        or header.contains("gool_folder_sound_bank.gd")

## Public API used by plugin.gd on enable/disable to force a fresh
## scan. The cache is static so it persists across plugin enables;
## clearing it ensures the first inspector render after re-enabling
## the plugin gets a clean read.
static func clear_cache() -> void:
    _cached_names = PackedStringArray()
    _cache_valid = false


# ---------------------------------------------------------------------
# The actual EditorProperty widget that replaces the default String
# input. It's an OptionButton with the discovered sound names plus a
# "(custom)" trailing option that lets the user type any string.
# ---------------------------------------------------------------------

class SoundNameOptionEditor extends EditorProperty:
    var _option_button: OptionButton
    var _custom_input: LineEdit
    var _names: PackedStringArray
    var _CUSTOM_INDEX: int = -1   # set when (custom) option is added

    func _init(names: PackedStringArray) -> void:
        _names = names
        _option_button = OptionButton.new()
        _option_button.size_flags_horizontal = SIZE_EXPAND_FILL
        # Special "(none)" first entry maps to empty string. Lets the
        # user clear the field via the dropdown rather than typing.
        _option_button.add_item("(none)", 0)
        for i in _names.size():
            _option_button.add_item(_names[i], i + 1)
        # Trailing "(custom)" option — selecting this swaps the
        # OptionButton out for a LineEdit so the user can type a
        # name not in the discovered set.
        _CUSTOM_INDEX = _names.size() + 1
        _option_button.add_item("(custom: type below)", _CUSTOM_INDEX)
        _option_button.item_selected.connect(_on_option_selected)
        add_child(_option_button)
        # Custom-input LineEdit is hidden by default; revealed when
        # the user picks (custom) from the dropdown.
        _custom_input = LineEdit.new()
        _custom_input.placeholder_text = "Type a custom sound name"
        _custom_input.visible = false
        _custom_input.text_changed.connect(_on_custom_text_changed)
        add_child(_custom_input)

    func _update_property() -> void:
        # Called by the inspector when the underlying property's value
        # changes (e.g., loaded from .tscn). Sync the widget's display
        # to match.
        var current: String = get_edited_object().get(get_edited_property())
        if current == null:
            current = ""
        # Find current value in dropdown, else fall back to (custom)
        # mode with the value pre-filled.
        var found_index: int = -1
        if current == "":
            found_index = 0
        else:
            for i in _names.size():
                if _names[i] == current:
                    found_index = i + 1
                    break
        if found_index >= 0:
            _option_button.select(found_index)
            _custom_input.visible = false
        else:
            # Not in our known set — switch to custom mode.
            _option_button.select(_option_button.get_item_count() - 1)
            _custom_input.text = current
            _custom_input.visible = true

    func _on_option_selected(idx: int) -> void:
        var item_id: int = _option_button.get_item_id(idx)
        if item_id == 0:
            # (none) selected
            _custom_input.visible = false
            emit_changed(get_edited_property(), "")
        elif item_id == _CUSTOM_INDEX:
            # Switching to custom mode — reveal the LineEdit and let
            # the user type. Don't emit a change yet; they need to type
            # something first.
            _custom_input.visible = true
            _custom_input.grab_focus()
        else:
            # Known name selected.
            _custom_input.visible = false
            var name_index: int = item_id - 1   # offset by 1 for (none)
            if name_index >= 0 and name_index < _names.size():
                emit_changed(get_edited_property(), _names[name_index])

    func _on_custom_text_changed(new_text: String) -> void:
        emit_changed(get_edited_property(), new_text)
