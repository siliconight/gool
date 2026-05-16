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
# That's it. New files dropped into the folder are picked up
# automatically:
#   - At runtime: the scan runs once in _init() before the
#     GoolSoundBankLoader iterates `sounds`.
#   - In the editor (v0.22.3+): the bank subscribes to Godot's
#     EditorFileSystem.filesystem_changed signal and re-scans
#     automatically whenever the project's filesystem changes —
#     so dropping a .wav into res://sounds/ updates the bank
#     with no manual rescan, no folder_path re-type, no editor
#     restart. The rescan is debounced against redundant signal
#     bursts (Godot fires filesystem_changed several times during
#     a single import).
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

# v0.22.3: editor-only filesystem-watch state. When running inside
# the Godot editor (@tool mode), the bank subscribes to
# EditorFileSystem.filesystem_changed so dropping a new audio file
# into the watched folder triggers an automatic rescan. These
# fields are unused at runtime (the signal source doesn't exist
# outside the editor).
var _fs_watch_connected: bool = false
# Debounce guard: Godot fires filesystem_changed multiple times
# during a single import operation (once when the raw file lands,
# again when the .import sidecar is written, etc). Without
# debouncing we'd re-scan the whole folder 3-5x per dropped file.
# We coalesce bursts by deferring the actual rescan to the next
# idle frame and collapsing repeated requests in between.
var _rescan_queued: bool = false

# Emitted after an editor-triggered rescan completes. The inspector
# plugin (or any other tool script) can connect to this to refresh
# its own caches. Carries the new sound count for convenience.
signal rescanned(sound_count: int)

func _init() -> void:
	# Scan happens at construction. For runtime, this populates the
	# parent's `sounds` Dictionary before GoolSoundBankLoader iterates
	# it. For editor (@tool mode), this also runs so the inspector
	# can show the discovered count — DirAccess is fast (sub-100ms
	# for typical project sizes) so we don't gate on editor vs runtime.
	_scan_folder()
	# In the editor, wire up live filesystem watching so the bank
	# stays in sync with the project folder without manual rescans.
	# Guarded by is_editor_hint() because EditorInterface only
	# exists inside the editor process — at runtime this is a no-op.
	if Engine.is_editor_hint():
		_connect_filesystem_watch()

# Subscribes to the editor's filesystem-changed signal. Safe to
# call more than once — the _fs_watch_connected guard prevents
# duplicate connections (which Godot would otherwise allow,
# resulting in N rescans per change).
func _connect_filesystem_watch() -> void:
	if _fs_watch_connected:
		return
	# EditorInterface is a static-access singleton in Godot 4.2+.
	# get_resource_filesystem() returns the EditorFileSystem whose
	# filesystem_changed signal fires after any project file is
	# added, removed, moved, or reimported.
	var efs := EditorInterface.get_resource_filesystem()
	if efs == null:
		# Defensive: should never happen inside the editor, but if
		# the API surface changes we degrade to "manual rescan only"
		# rather than erroring.
		GoolLog.warn("bank", "EditorFileSystem unavailable",
			{"effect": "live folder-watching disabled",
			 "fix": "re-open the project or re-set folder_path to rescan manually"})
		return
	efs.filesystem_changed.connect(_on_filesystem_changed)
	_fs_watch_connected = true

# Signal handler for EditorFileSystem.filesystem_changed. Rather
# than rescanning immediately (the signal fires several times per
# import), we queue a single deferred rescan and collapse repeat
# signals until it runs.
func _on_filesystem_changed() -> void:
	if _rescan_queued:
		return
	_rescan_queued = true
	# call_deferred pushes the actual rescan to the end of the
	# current idle frame. Multiple filesystem_changed signals
	# within the same frame all set _rescan_queued = true but only
	# the first schedules the deferred call.
	_do_deferred_rescan.call_deferred()

func _do_deferred_rescan() -> void:
	_rescan_queued = false
	var before := sounds.size()
	_scan_folder()
	var after := sounds.size()
	# Only emit + log when something actually changed. A
	# filesystem_changed signal for an unrelated file (a script
	# edit, a scene save) shouldn't spam the output.
	if before != after:
		# v0.23.2: routed via GoolLog. DEBUG-level — this fires
		# whenever the user drops a file into res://sounds/, and
		# is interesting during dev but noisy in steady state.
		GoolLog.debug("bank", "folder rescanned",
			{"before": before, "after": after, "delta": after - before})
	rescanned.emit(after)

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
		# files (wav, ogg, mp3, flac) into AudioStreamWAV /
		# AudioStreamOggVorbis / AudioStreamMP3 / AudioStreamFLAC
		# automatically. If load() returns null, the file isn't a
		# form Godot can import.
		#
		# v0.22.4: when this fails we now peek the file's first 64
		# bytes and tell the user what the file actually is plus
		# how to fix it. Previously the warning was generic ("not a
		# loadable AudioStream — perhaps not yet imported"); the
		# user was left to figure out whether the file was just
		# un-reimported, or was in a format Godot doesn't support,
		# or had a wrong extension. The codec-detection path here
		# turns a 30-minute detective session into a one-line
		# warning that names the actual codec and points at the
		# fix.
		var stream: Resource = load(child_absolute)
		if stream == null or not (stream is AudioStream):
			var classification: Dictionary = _classify_audio_file(child_absolute)
			GoolLog.warn("bank", "could not register audio file", {
				"path": child_absolute,
				"detected_format": classification.codec,
				"fix": classification.advice,
			})
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

# v0.22.4: peek a file's first 64 bytes to identify what audio
# codec/container it actually contains. Used to produce specific
# error messages when Godot's load() rejects a file. Returns a
# Dictionary with two keys:
#   - codec:   human-readable description of what was found
#   - advice:  what the user should do to make it importable
#
# Detection covers: Opus-in-Ogg (the Logic Pro X gotcha), Vorbis-
# in-Ogg, RIFF/WAV, FLAC, MP3 (ID3-tagged and raw MPEG frame),
# and a fallback for anything else. The byte sniffing is the same
# kind of magic-number check Godot's own importers do internally,
# just exposed here as actionable feedback.
func _classify_audio_file(path: String) -> Dictionary:
	var f: FileAccess = FileAccess.open(path, FileAccess.READ)
	if f == null:
		return {
			"codec": "unreadable",
			"advice": "Cannot open the file — check permissions and that the path exists."
		}
	var header_bytes: PackedByteArray = f.get_buffer(64)
	f.close()
	if header_bytes.size() < 4:
		return {
			"codec": "empty or truncated",
			"advice": "File is too small to be valid audio. Re-export from your DAW."
		}
	# Build an ASCII-only string from the bytes for pattern
	# matching. Non-printable bytes become '.' which is fine —
	# we're looking for ASCII signatures like "OpusHead", "vorbis",
	# "RIFF", "fLaC", "ID3".
	var header: String = ""
	for b in header_bytes:
		if b >= 32 and b < 127:
			header += char(b)
		else:
			header += "."
	# Ogg container detection. The "OggS" magic is at byte 0;
	# the codec identification (OpusHead or "vorbis") appears in
	# the first page's payload, which lives within the first ~60
	# bytes of the file.
	if header.begins_with("OggS"):
		if header.find("OpusHead") >= 0:
			return {
				"codec": "Opus (in Ogg container)",
				"advice":
					"Godot 4.x has no built-in Opus importer. "
					+ "Re-export from your DAW as WAV (always works) or "
					+ "as Ogg Vorbis. **Logic Pro X note**: Logic's Ogg "
					+ "export defaults to Opus — bounce as WAV instead, "
					+ "or convert with: "
					+ "ffmpeg -i input.wav -c:a libvorbis -q:a 6 output.ogg"
			}
		if header.find("vorbis") >= 0:
			return {
				"codec": "Vorbis (in Ogg container)",
				"advice":
					"File contains valid Vorbis data but Godot couldn't "
					+ "import it. Possible causes: (1) Godot hasn't "
					+ "reimported the file yet — right-click in the "
					+ "FileSystem dock and select Reimport. (2) The "
					+ "Vorbis stream is corrupted or uses an unsupported "
					+ "variant — try re-encoding from WAV: "
					+ "ffmpeg -i input.wav -c:a libvorbis -q:a 6 output.ogg"
			}
		return {
			"codec": "Unknown codec inside Ogg container",
			"advice":
				"Ogg container detected but the codec inside is neither "
				+ "Vorbis nor Opus. Re-export from your DAW as WAV."
		}
	# RIFF/WAV detection: "RIFF" at byte 0, "WAVE" at byte 8.
	if header.begins_with("RIFF") and header.find("WAVE") >= 0:
		return {
			"codec": "RIFF/WAV",
			"advice":
				"WAV file detected but Godot couldn't import it. Possible "
				+ "causes: (1) Not yet imported — right-click in "
				+ "FileSystem and Reimport. (2) Unsupported WAV variant "
				+ "(e.g., 32-bit float or A-law/μ-law encoding) — "
				+ "re-export as 16-bit or 24-bit PCM."
		}
	# FLAC: "fLaC" magic at byte 0.
	if header.begins_with("fLaC"):
		return {
			"codec": "FLAC",
			"advice":
				"FLAC file detected. Right-click in the FileSystem dock "
				+ "and select Reimport. If Reimport doesn't help, the "
				+ "FLAC stream may be corrupted — re-export from your DAW."
		}
	# MP3: ID3v2 header is "ID3" at byte 0; raw MPEG frames start
	# with the 11-bit sync word 0xFFE0.
	if header.begins_with("ID3"):
		return {
			"codec": "MP3 (with ID3 tags)",
			"advice":
				"MP3 file detected. Right-click in the FileSystem dock "
				+ "and select Reimport. MP3 import requires Godot's "
				+ "standard MP3 importer (enabled by default)."
		}
	if header_bytes.size() >= 2 \
			and header_bytes[0] == 0xFF \
			and (header_bytes[1] & 0xE0) == 0xE0:
		return {
			"codec": "MP3 (raw MPEG frame)",
			"advice":
				"MP3 file detected (no ID3 tags). Right-click in "
				+ "FileSystem and Reimport."
		}
	return {
		"codec": "Unknown format",
		"advice":
			"File header doesn't match any known audio container "
			+ "(Ogg, RIFF/WAV, FLAC, MP3). The file may not actually "
			+ "contain audio data, or may use an unsupported format. "
			+ "First 16 bytes (hex): %s. " % _bytes_to_hex(header_bytes, 16)
			+ "Re-export from your DAW as WAV — it's the most reliable "
			+ "format for Godot's importer."
	}

# Helper for the diagnostic above. Returns space-separated hex of
# the first `count` bytes, useful when the format detector can't
# identify anything.
func _bytes_to_hex(bytes: PackedByteArray, count: int) -> String:
	var hex: String = ""
	var n: int = min(count, bytes.size())
	for i in n:
		if i > 0:
			hex += " "
		hex += "%02X" % bytes[i]
	return hex

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
## changing folder_path or naming_style at runtime.
##
## In the editor (v0.22.3+) you rarely need to call this manually —
## the bank auto-rescans on any project filesystem change via
## EditorFileSystem.filesystem_changed. This method remains useful
## for: runtime folder_path changes, forcing a rescan after a
## naming_style change (which filesystem_changed won't catch since
## no file changed), or scripted tooling.
func rescan() -> void:
	_scan_folder()
	if Engine.is_editor_hint():
		rescanned.emit(sounds.size())
