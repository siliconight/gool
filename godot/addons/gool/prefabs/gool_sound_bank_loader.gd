# addons/gool/prefabs/gool_sound_bank_loader.gd
#
## Drag-and-drop sound registration. Assign a GoolSoundBank
## resource to the `bank` field and every (name → AudioStream)
## entry gets registered with the gool runtime on _ready, no
## script required.
#
## Replaces the boilerplate every gool project otherwise needs:
#
##     # before:
##     func _ready():
##         await Gool.ready_to_play
##         Gool.register_sound_from_file("coin", "res://sfx/coin.wav")
##         Gool.register_sound_from_file("engine", "res://sfx/engine.ogg")
##         ...20 more lines for 20 more sounds...
#
##     # after:
##     # (drop a GoolSoundBankLoader, assign main_bank.tres, done)
#
## The loader reads each AudioStream's `resource_path` and feeds
## the bytes to `Gool.register_sound_from_bytes()` with FORMAT_AUTO
## detection. Supported formats: WAV, OGG Vorbis, FLAC. Opus
## requires AUDIO_ENGINE_DECODERS_OPUS=ON at build time.
#
## Multiple loaders per scene are supported and additive — useful
## for level-specific banks layered on top of a global bank.
## Duplicate names across banks: last-loaded wins (the engine
## overwrites the previous registration).

@tool
class_name GoolSoundBankLoader
extends Node

## Sound bank resource to register on _ready. If null, the loader
## logs a warning and does nothing — useful for scenes where the
## node exists but the bank is assigned dynamically from script
## before adding to the tree.
@export var bank: GoolSoundBank = null

## When true (default), emits a warning if `bank` is null on
## _ready. Disable to silence the warning for the "assigned later
## from script" pattern.
@export var warn_if_unassigned: bool = true

## Emitted after every entry has been processed. The dictionary
## maps each registered name to its handle (truthy on success,
## 0 on failure). Connect this if your scene wants to wait for
## registration to complete before instantiating emitters
## dynamically.
signal registration_complete(results: Dictionary)

var _runtime: Node = null

func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_runtime = get_node_or_null("/root/Gool")
	if _runtime == null:
		push_warning(
			"GoolSoundBankLoader: /root/Gool autoload not found. "
			+ "The gool plugin is installed but not enabled. Fix: "
			+ "open Project Settings → Plugins, find 'gool' in the "
			+ "list, tick the Enable checkbox."
		)
		return
	if bank == null:
		if warn_if_unassigned:
			push_warning(
				"GoolSoundBankLoader: `bank` is unassigned. Assign a "
				+ "GoolSoundBank resource in the inspector, or set "
				+ "warn_if_unassigned=false if the bank is assigned "
				+ "later from script before the node enters the tree."
			)
		return
	if not _runtime.is_initialized():
		await _runtime.ready_to_play
	_register_all()

func _register_all() -> void:
	var results: Dictionary = {}
	for key in bank.sounds.keys():
		if typeof(key) != TYPE_STRING:
			push_warning(
				"GoolSoundBankLoader: skipping non-string key %s in "
				% str(key)
				+ "bank. Sound names must be strings."
			)
			continue
		var name: String = key
		var stream: Variant = bank.sounds[key]
		if stream == null:
			push_warning(
				"GoolSoundBankLoader: entry '%s' has a null stream. "
				% name
				+ "Open the bank resource and assign an AudioStream."
			)
			results[name] = 0
			continue
		if not (stream is AudioStream):
			push_warning(
				"GoolSoundBankLoader: entry '%s' is not an AudioStream "
				% name
				+ "(got %s). Skipping."
				% stream.get_class()
			)
			results[name] = 0
			continue
		# Delegate to the C++ binding's register_sound_from_stream,
		# which handles both file-backed streams (the 95% case) and
		# AudioStreamWAV with in-memory PCM. Returns the AudioSoundId
		# on success, 0 on failure.
		var handle: int = _runtime.register_sound_from_stream(name, stream)
		if handle == 0:
			push_warning(
				"GoolSoundBankLoader: register_sound_from_stream "
				+ "failed for '%s'. Common causes: stream is a "
				% name
				+ "procedural type (Randomizer/Polyphonic/Generator) "
				+ "that can't be reduced to a single PCM asset, or "
				+ "the underlying file format's decoder isn't "
				+ "compiled in (AUDIO_ENGINE_DECODERS_* in CMake)."
			)
			results[name] = 0
			continue
		# Apply category + looping per-entry. GoolFolderSoundBank
		# populates `sounds_category` and `sounds_looping` dictionaries
		# based on the source subfolder (sfx/music/voice/etc); the plain
		# GoolSoundBank has neither and falls back to the bank-wide
		# defaults. Loader uses whatever is available.
		var entry_category: int = bank.default_category
		var entry_looping: bool = false
		if bank.get("sounds_category") != null \
				and bank.sounds_category.has(name):
			entry_category = bank.sounds_category[name]
		if bank.get("sounds_looping") != null \
				and bank.sounds_looping.has(name):
			entry_looping = bank.sounds_looping[name]
		_runtime.register_sound_definition(
			name,
			bank.default_spatialized,
			entry_looping,    # looping (per-entry from folder bank, else false)
			1.0,              # min_distance
			50.0,             # max_distance
			0.0,              # loop_crossfade_ms
			entry_category,
			"")               # target_bus_name (use category routing)
		results[name] = handle
	# v0.22.4: registration summary. The loader silently succeeded
	# before — registering N sounds with no output, making it
	# impossible to tell "loader ran and worked" from "loader didn't
	# run at all." This single log line is decisive diagnostic data
	# for the "I added sounds to the bank but emitters don't see
	# them" failure mode.
	var ok_count: int = 0
	var failed_names: PackedStringArray = PackedStringArray()
	for entry_name in results:
		if results[entry_name] != 0:
			ok_count += 1
		else:
			failed_names.append(entry_name)
	var bank_label: String = bank.resource_path if bank.resource_path != "" else "<unsaved bank>"
	var name_list: PackedStringArray = PackedStringArray()
	for entry_name in results:
		if results[entry_name] != 0:
			name_list.append(entry_name)
	if ok_count > 0:
		# v0.23.2: routed via GoolLog. INFO-level so the success
		# summary stays visible (this is once-per-bank-load, not
		# per-frame, so it's not noisy).
		GoolLog.info("loader", "registered sounds", {
			"ok": ok_count,
			"total": results.size(),
			"bank": bank_label,
			"names": "[%s]" % ", ".join(name_list),
		})
	if failed_names.size() > 0:
		GoolLog.warn("loader", "failed to register sounds", {
			"count": failed_names.size(),
			"bank": bank_label,
			"names": "[%s]" % ", ".join(failed_names),
			"note": "see earlier warnings for each entry's specific cause",
		})
	registration_complete.emit(results)
