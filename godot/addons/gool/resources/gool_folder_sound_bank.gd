# addons/gool/resources/gool_folder_sound_bank.gd
#
# Auto-populating sound bank that scans a folder of audio files
# and registers each with the gool runtime. Use this for the
# "drop a file in, it works" workflow — no inspector authoring,
# no Dictionary maintenance, no script changes when adding audio
# assets. The folder structure encodes the categorization.
#
# Typical workflow for music producers:
#
#   1. Bounce stems / SFX / music tracks from your DAW
#   2. Drop them into res://sounds/{category}/<filename>.ogg
#      e.g.:
#        res://sounds/music/explore_loop.ogg
#        res://sounds/sfx/gunshot.wav
#        res://sounds/voice/narrator_intro.ogg
#        res://sounds/ambience/wind_loop.ogg
#        res://sounds/ui/menu_click.wav
#   3. Create a GoolFolderSoundBank.tres pointing at res://sounds,
#      drop a GoolSoundBankLoader in your main scene with the bank
#      assigned. F5.
#
# That's it. New files dropped into the folder are picked up on
# next project load. No code changes.
#
# Inherits from GoolSoundBank so GoolSoundBankLoader doesn't need
# to know which kind of bank it has — it just iterates `sounds`.
# The folder scan populates `sounds` in `_init()` so the dict is
# ready before any loader sees it.

@tool
class_name GoolFolderSoundBank
extends GoolSoundBank

## Root folder to scan for audio files. Subdirectories of this
## path encode the AudioCategory (when `category_from_subfolder`
## is true): "music" → Music, "sfx" → SFX, "voice" → Voice,
## "ambience" → Ambience, "ui" → UI, "dialogue" → Dialogue. Files
## directly in folder_path or in subdirectories with unrecognized
## names fall back to SFX.
@export_dir var folder_path: String = "res://sounds"

## When true (default), the scan recurses into subdirectories.
## Disable to scan only direct children of folder_path.
@export var recursive: bool = true

## Naming strategy for the registered sound names. Three modes:
##
##   - filename:             "gunshot"             (basename only)
##   - subfolder/filename:   "sfx/gunshot"         (default — gives natural namespacing)
##   - snake_case_path:      "sfx_gunshot"         (Wwise-style flat names)
##
## Choose once per project and stay consistent — emitter prefabs
## reference sounds by these names, so changing the style mid-
## project breaks every existing reference.
@export_enum("filename", "subfolder/filename", "snake_case_path")
var naming_style: int = 1

## When true (default), the first subfolder under folder_path is
## interpreted as the AudioCategory. Standard layout:
##
##   sounds/music/*       → Music    (looping=true)
##   sounds/sfx/*         → SFX      (looping=false)
##   sounds/voice/*       → Voice    (looping=false)
##   sounds/ambience/*    → Ambience (looping=true)
##   sounds/ui/*          → UI       (looping=false)
##   sounds/dialogue/*    → Dialogue (looping=false)
##
## Files in unrecognized subfolders fall back to SFX. Disable this
## and the bank's `default_category` (from GoolSoundBank) applies
## to all entries uniformly.
@export var category_from_subfolder: bool = true

## Apply category-conventional defaults during registration:
## music/ambience entries are registered with looping=true, others
## one-shot. Disable for full manual control via per-emitter
## settings.
@export var apply_category_defaults: bool = true

# File extensions we recognize as audio. Matches the format-detection
# the runtime's register_sound_from_bytes() will perform on the bytes.
const _AUDIO_EXTENSIONS: Array[String] = ["wav", "ogg", "mp3", "flac", "opus"]

# Category subfolder name → AudioCategory enum value. The category
# constants live on the Gool autoload (CATEGORY_SFX, CATEGORY_MUSIC,
# etc.) but we duplicate the values here as integer literals to avoid
# a load-time dependency on the autoload (the bank may be evaluated
# in @tool mode before /root/Gool exists).
const _CATEGORY_BY_SUBFOLDER: Dictionary = {
    "sfx":      0,   # CATEGORY_SFX
    "voice":    1,   # CATEGORY_VOICE
    "music":    2,   # CATEGORY_MUSIC
    "ambience": 3,   # CATEGORY_AMBIENCE
    "ui":       4,   # CATEGORY_UI
    "dialogue": 5,   # CATEGORY_DIALOGUE
}

# Per-entry category, indexed by the same string keys as `sounds`.
# Loaders read this and pass to register_sound_definition. Populated
# alongside sounds in _scan_folder.
var sounds_category: Dictionary = {}

# Per-entry looping flag, same indexing. True for music/ambience
# when apply_category_defaults is on; false otherwise.
var sounds_looping: Dictionary = {}

func _init() -> void:
    # Scan happens at construction. For runtime, this populates the
    # parent's `sounds` Dictionary before GoolSoundBankLoader iterates
    # it. For editor (@tool mode), this also runs so the inspector
    # can show the discovered count — DirAccess is fast (sub-100ms
    # for typical project sizes) so we don't gate on editor vs runtime.
    _scan_folder()

func _scan_folder() -> void:
    sounds.clear()
    sounds_category.clear()
    sounds_looping.clear()
    if folder_path == "":
        return
    if not DirAccess.dir_exists_absolute(folder_path):
        # Don't push_warning during @tool execution at project load;
        # too noisy. The Loader emits a runtime warning when it tries
        # to register from an empty bank.
        return
    _scan_recursive(folder_path, "")

func _scan_recursive(absolute_path: String, relative_from_root: String) -> void:
    var dir: DirAccess = DirAccess.open(absolute_path)
    if dir == null:
        return
    dir.list_dir_begin()
    while true:
        var entry: String = dir.get_next()
        if entry == "":
            break
        if entry.begins_with("."):
            continue
        # Skip Godot's own import-cache directory which sometimes
        # shows up as a sibling of the audio files.
        if entry == ".godot" or entry.ends_with(".import") or entry.ends_with(".uid"):
            continue
        var child_absolute: String = absolute_path.path_join(entry)
        var child_relative: String
        if relative_from_root == "":
            child_relative = entry
        else:
            child_relative = relative_from_root.path_join(entry)
        if dir.current_is_dir():
            if recursive:
                _scan_recursive(child_absolute, child_relative)
            continue
        if not _is_audio_file(entry):
            continue
        # Try to load the file as an AudioStream. Godot imports audio
        # files (wav, ogg, mp3 — flac depends on plugin support) into
        # AudioStreamWAV / AudioStreamOggVorbis / AudioStreamMP3
        # automatically. If load() returns null, the file isn't yet
        # imported — skip with a quiet warning.
        var stream: Resource = load(child_absolute)
        if stream == null or not (stream is AudioStream):
            push_warning(
                "GoolFolderSoundBank: %s is not a loadable AudioStream "
                % child_absolute
                + "(perhaps not yet imported by Godot). Skipping."
            )
            continue
        var sound_name: String = _derive_name(child_relative)
        sounds[sound_name] = stream
        # Categorize. The "category" comes from the FIRST path segment
        # in relative_from_root when category_from_subfolder is on.
        if category_from_subfolder:
            var first_segment: String = child_relative.split("/")[0].to_lower()
            if _CATEGORY_BY_SUBFOLDER.has(first_segment):
                sounds_category[sound_name] = _CATEGORY_BY_SUBFOLDER[first_segment]
            else:
                sounds_category[sound_name] = default_category
        else:
            sounds_category[sound_name] = default_category
        # Apply category defaults: music + ambience loop by default.
        if apply_category_defaults:
            var cat: int = sounds_category[sound_name]
            sounds_looping[sound_name] = (cat == 2 or cat == 3)  # Music or Ambience
        else:
            sounds_looping[sound_name] = false
    dir.list_dir_end()

func _is_audio_file(filename: String) -> bool:
    var ext: String = filename.get_extension().to_lower()
    return ext in _AUDIO_EXTENSIONS

func _derive_name(relative_path: String) -> String:
    # "music/explore_loop.ogg" → derived per naming_style.
    var without_ext: String = relative_path.get_basename()
    match naming_style:
        0: return without_ext.get_file()            # "explore_loop"
        1: return without_ext                       # "music/explore_loop"
        2: return without_ext.replace("/", "_").to_lower()  # "music_explore_loop"
        _: return without_ext

## Returns the count of discovered sounds. Useful for editor
## inspectors and loader prefabs to display "X sounds will be
## registered from this bank".
func get_sound_count() -> int:
    return sounds.size()

## Forces a fresh scan of the folder. Call this from script after
## changing folder_path or naming_style at runtime. In the editor,
## the resource re-scans automatically when reloaded.
func rescan() -> void:
    _scan_folder()
