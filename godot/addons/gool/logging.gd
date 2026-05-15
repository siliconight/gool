# addons/gool/logging.gd
#
# GoolLog — gool's structured-logging helper. Replaces ad-hoc
# print() / push_warning() / push_error() calls throughout the
# addon with a single API that supports:
#
#   - Five severity levels: TRACE, DEBUG, INFO, WARN, ERROR
#   - Per-category filtering (e.g. mixer at TRACE while voice at WARN)
#   - Structured key=value fields, parseable by log analyzers
#   - Project Settings integration for per-project defaults
#   - Optional file sink for after-session review
#   - Maps to Godot's native print/push_warning/push_error so the
#     Output panel's color-coding still works
#
# Usage:
#
#   GoolLog.info("emitter", "play", {"sound": name, "pos": position})
#   → Output: [gool/emitter INFO] play  sound=sfx/click pos=(1.2, 0, 3.4)
#
#   GoolLog.warn("decoder", "format unsupported", {"file": path})
#   → push_warning (Godot shows it in yellow): [gool/decoder WARN] format unsupported  file=res://...
#
# Project-level configuration via Project Settings (Editor → Project
# Settings → General → "addons/gool/logging/..."):
#
#   addons/gool/logging/global_level       String, default "info"
#     One of: trace, debug, info, warn, error, silent
#
#   addons/gool/logging/categories         String, default ""
#     Comma-separated category:level pairs that override the global
#     level for specific categories. Example:
#       "mixer:trace,decoder:warn,voice:silent"
#
#   addons/gool/logging/file_sink_enabled  bool,   default false
#     If true, all log lines also write to user://gool.log
#
# Runtime overrides via the static API are also available (see
# set_global_level / set_category_level / enable_file_sink below).
#
# Category names are free-form strings; the convention is single-
# lowercase-word identifiers matching gool's subsystems. Standard
# categories used by gool's own code:
#
#   runtime    — autoload init, shutdown, version, device, config
#   emitter    — AudioEmitter3D create/play/stop
#   mixer      — render-thread health, voice activity
#   loader     — GoolSoundBankLoader registration
#   bank       — GoolFolderSoundBank scanning, format detection
#   decoder    — codec failures, sample-buffer issues
#   voice      — voice chat jitter, packet loss, registration
#   net        — networked event broadcast/receive
#
# Your own game code can use any category names you want. Categories
# are created lazily — no need to pre-register them.

class_name GoolLog
extends Object

# Severity levels. Higher numbers = more severe. SILENT suppresses
# everything; useful for production builds or temporarily quieting
# a category. FATAL is reserved for catastrophic failure — events
# that mean the system cannot continue operating (audio backend
# crashed, mandatory asset missing, ...). Currently routes to the
# same Godot output as ERROR (push_error → red in Output panel),
# but semantically distinct in the log line and JSON output so
# analyzers can prioritize FATAL incidents.
enum Level {
    TRACE   = 0,   # noisy per-frame / per-tick detail
    DEBUG   = 1,   # development-time state changes
    INFO    = 2,   # significant lifecycle events (init, shutdown)
    WARN    = 3,   # recoverable problems
    ERROR   = 4,   # unrecoverable problems
    FATAL   = 5,   # v0.23.4: catastrophic failure
    SILENT  = 6,   # suppress all logging
}

# v0.23.4: verbosity preset. A single dial that drives several
# logging settings in concert — chosen for the common cases so
# users don't have to think through 4 individual toggles to "set
# up production logging" or "max-detail diagnostics."
#
# AUTO resolves at init time based on build type:
#   - In the editor   (OS.has_feature("editor"))  → DEV
#   - Exported debug  (OS.is_debug_build())       → DEBUG
#   - Exported release                            → SHIP
#
# CUSTOM disables preset behavior and uses the individual
# global_level / include_source / include_timestamps / file_sink
# settings explicitly — for advanced users who want fine control.
#
# All preset values can be runtime-overridden via set_global_level
# etc; the preset is init-time defaults, not a permanent gate.
enum Verbosity {
    AUTO       = 0,   # build-type-aware defaults (default)
    SHIP       = 1,   # WARN+, no source, no timestamps
    DEV        = 2,   # INFO+, with source, no timestamps
    DEBUG      = 3,   # DEBUG+, with source, no timestamps
    DIAGNOSTIC = 4,   # TRACE+, with source, with timestamps, file sink on
    CUSTOM     = 5,   # use individual settings (no preset applied)
}

# Human-readable level names, indexed by Level enum value.
# v0.23.5: was `const _LEVEL_NAMES: PackedStringArray = PackedStringArray([...])`
# but Godot's parser rejects that — the constructor call isn't a constant
# expression. A plain Array literal IS constant in GDScript, and indexing
# by integer (level) works identically. The 7-entry size makes the
# memory-packing benefit of PackedStringArray irrelevant.
const _LEVEL_NAMES = [
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "SILENT"]

# Project Settings paths. These are read once at first-use time and
# cached. Runtime API calls (set_global_level etc) override the
# cached value without re-reading the settings file.
const _PS_GLOBAL_LEVEL: String = "addons/gool/logging/global_level"
const _PS_CATEGORIES: String   = "addons/gool/logging/categories"
const _PS_FILE_SINK: String    = "addons/gool/logging/file_sink_enabled"
const _PS_FILE_PATH: String    = "addons/gool/logging/file_path"
const _PS_INCLUDE_TS: String   = "addons/gool/logging/include_timestamps"
# v0.23.3:
const _PS_FORMAT: String       = "addons/gool/logging/format"
const _PS_INCLUDE_SOURCE: String = "addons/gool/logging/include_source"
# v0.23.4:
const _PS_VERBOSITY: String    = "addons/gool/logging/verbosity"

# Output format options. "human" (default) is the v0.23.2 format
# styled for readability in Godot's Output panel; "json" emits one
# JSON object per line, suitable for shipping to log aggregators
# (Loki/Splunk/Elastic) or grep'ing with `jq`. In JSON mode,
# timestamps and source are always present regardless of the
# include_timestamps / include_source toggles (structured logs
# without timestamps are useless to analyzers).
enum Format {
    HUMAN = 0,
    JSON  = 1,
}

# Default file sink path. Lives in user:// so it's writable on every
# Godot-supported platform without permission gymnastics.
const _DEFAULT_FILE_PATH: String = "user://gool.log"

# ─── state ───────────────────────────────────────────────────

# Cached configuration. Loaded from Project Settings on first use
# (see _ensure_initialized); subsequent reads use these cached
# values to avoid repeated ProjectSettings lookups in hot logging
# paths.
static var _initialized: bool = false
static var _global_level: int = Level.INFO
static var _category_levels: Dictionary = {}    # String -> int (Level)
static var _file_sink_enabled: bool = false
static var _file_path: String = _DEFAULT_FILE_PATH
static var _include_timestamps: bool = false
static var _file: FileAccess = null

# v0.23.3: format + source-location toggles
static var _format: int = Format.HUMAN
static var _include_source: bool = true

# v0.23.4: verbosity preset (resolved to a concrete Verbosity value
# at init time — never AUTO at runtime). Used for diagnostic
# reporting; the actual logging behavior is driven by the resolved
# _global_level / _include_source / _include_timestamps / file
# sink settings that the preset configured.
static var _verbosity: int = Verbosity.DEV

# ─── public API: configuration ───────────────────────────────

# Set the default log level for all categories that don't have a
# specific override. Takes either a Level enum value or a case-
# insensitive string name ("trace", "DEBUG", etc).
static func set_global_level(level) -> void:
    _ensure_initialized()
    _global_level = _coerce_level(level)

# Override the level for a specific category. Categories below their
# threshold are silently dropped. Pass Level.SILENT (or "silent") to
# fully mute a category without touching the global level.
static func set_category_level(category: String, level) -> void:
    _ensure_initialized()
    _category_levels[category] = _coerce_level(level)

# Enable mirroring of all log output to a file in addition to the
# Godot Output panel. Path defaults to user://gool.log. The file is
# opened in WRITE mode (truncates on each session start); if you want
# append behavior, manage the file yourself and disable this.
static func enable_file_sink(path: String = "") -> void:
    _ensure_initialized()
    var target_path: String = path if path != "" else _file_path
    if _file != null:
        _file.close()
        _file = null
    _file = FileAccess.open(target_path, FileAccess.WRITE)
    if _file == null:
        push_warning(
            "[GoolLog] could not open file sink at %s "
            % target_path
            + "(FileAccess error %d). Log output continues to "
            % FileAccess.get_open_error()
            + "Godot's Output panel only."
        )
        _file_sink_enabled = false
        return
    _file_path = target_path
    _file_sink_enabled = true
    info("logging", "file sink opened", {"path": target_path})

# Disable the file sink (closes the open file handle). Subsequent
# log calls go to the Output panel only.
static func disable_file_sink() -> void:
    if _file != null:
        _file.close()
        _file = null
    _file_sink_enabled = false

# v0.23.3: choose log line format at runtime. Accepts either a
# Format enum value or a case-insensitive string ("human" / "json").
# JSON format is recommended for sessions you plan to analyze
# post-hoc (jq-piped logs from a multiplayer session, for example);
# human format is recommended for live debugging in the Output panel.
static func set_format(format) -> void:
    _ensure_initialized()
    _format = _coerce_format(format)

# v0.23.4: switch verbosity preset at runtime. Re-applies the
# preset's defaults (writes back into the individual Project
# Settings + cached state). Pass Verbosity.CUSTOM to "freeze"
# current settings — switching to CUSTOM doesn't change anything
# immediately but prevents future calls to this from being
# clobbered by a preset.
#
# Accepts either a Verbosity enum value or a case-insensitive
# string ("auto", "ship", "dev", "debug", "diagnostic", "custom").
static func set_verbosity(verbosity) -> void:
    _ensure_initialized()
    var v: int = _coerce_verbosity(verbosity)
    if v == Verbosity.AUTO:
        v = _resolve_auto_verbosity()
    _verbosity = v
    _apply_verbosity_preset(v)
    # Refresh the cached state from the (just-updated) settings so
    # the new preset takes effect immediately for subsequent logs.
    _global_level = _coerce_level(ProjectSettings.get_setting(
        _PS_GLOBAL_LEVEL, "info"))
    _include_source = ProjectSettings.get_setting(_PS_INCLUDE_SOURCE, true)
    _include_timestamps = ProjectSettings.get_setting(_PS_INCLUDE_TS, false)
    var new_file_sink: bool = ProjectSettings.get_setting(_PS_FILE_SINK, false)
    if new_file_sink and not _file_sink_enabled:
        enable_file_sink(_file_path)
    elif _file_sink_enabled and not new_file_sink:
        disable_file_sink()

# v0.23.4: query the currently-resolved verbosity. Returns one of
# the concrete Verbosity values (never AUTO at runtime — that's
# always resolved to a concrete preset during init).
static func get_verbosity() -> int:
    _ensure_initialized()
    return _verbosity

# v0.23.4: create a bound logging context with pre-baked category
# and (optionally) label. The returned GoolLogContext object has
# the same five severity methods (trace/debug/info/warn/error)
# plus fatal(), but doesn't require repeating the category and
# label on every call.
#
# Usage pattern (typically once per file, e.g. at scene-script
# scope):
#
#   var _log_ctx := GoolLog.create_context("emitter", "audio_emitter_3d.gd")
#
#   func play() -> void:
#       _log_ctx.info("play", {"sound": sound_name})
#       ...
#
# Per-call overrides are still possible — pass a custom label as
# the optional 3rd arg to bypass the bound one.
static func create_context(category: String, label: String = "") -> GoolLogContext:
    var ctx := GoolLogContext.new()
    ctx.category = category
    ctx.label = label
    return ctx

# Query the resolved level for a category (after global / per-
# category override resolution). Useful when a caller wants to gate
# expensive log-message construction:
#
#   if GoolLog.is_enabled("mixer", GoolLog.Level.DEBUG):
#       GoolLog.debug("mixer", build_expensive_state_summary())
static func is_enabled(category: String, level: int) -> bool:
    _ensure_initialized()
    var threshold: int = _category_levels.get(category, _global_level)
    return level >= threshold

# ─── public API: logging ─────────────────────────────────────

# Five severity-specific entry points. Each takes a category, a
# short human-readable message, an optional Dictionary of structured
# fields (rendered as key=value pairs after the message), and an
# optional label that distinguishes sources within the same category.
#
# Fields are best for state that varies per-call. Avoid baking
# variable state into the message itself — that defeats grep'ing
# log lines by message and forces analyzers to do regex.
#
# Good:   info("emitter", "play", {"sound": name, "pos": pos})
# Bad:    info("emitter", "play %s at %s" % [name, pos])
#
# Label is the "which source within this category" tag (e.g.
# "player_gun" vs "enemy_gun" both under category="emitter"). The
# auto-captured source location (file:line) gives you the code
# site; label gives you the semantic identity. Use it when source
# alone doesn't disambiguate (a single call site that fires for
# multiple actors, for example).
static func trace(category: String, msg: String,
                  fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.TRACE, category, msg, fields, label)

static func debug(category: String, msg: String,
                  fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.DEBUG, category, msg, fields, label)

static func info(category: String, msg: String,
                 fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.INFO, category, msg, fields, label)

static func warn(category: String, msg: String,
                 fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.WARN, category, msg, fields, label)

static func error(category: String, msg: String,
                  fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.ERROR, category, msg, fields, label)

# v0.23.4: FATAL is a separate severity for catastrophic failures —
# events that mean a subsystem (or the whole runtime) cannot continue.
# Routed via push_error like ERROR, but tagged distinctly in both the
# human and JSON formats so analyzers can prioritize FATAL incidents
# above mere ERROR events. Use sparingly; if everything is FATAL,
# nothing is.
static func fatal(category: String, msg: String,
                  fields: Dictionary = {}, label: String = "") -> void:
    _log(Level.FATAL, category, msg, fields, label)

# ─── internals ───────────────────────────────────────────────

# Core dispatcher. Threshold-checks, formats, routes to the
# appropriate Godot output function (print/push_warning/push_error),
# and mirrors to the file sink if enabled.
static func _log(level: int, category: String,
                 msg: String, fields: Dictionary,
                 label: String = "") -> void:
    _ensure_initialized()
    var threshold: int = _category_levels.get(category, _global_level)
    if level < threshold:
        return
    # Capture source location BEFORE building the formatted line so
    # the stack frame for THIS function and the user's info()/warn()/
    # etc. wrapper aren't part of the trace. get_stack() returns
    # frames innermost-first; index 2 is the user's caller (0=_log,
    # 1=info/warn/etc, 2=caller).
    var source: String = ""
    if _include_source:
        source = _get_source_location(2)
    # Build a formatted line per the configured format. The two
    # formatters take the same inputs but produce wildly different
    # outputs (one for humans reading Output panel; one for tools).
    var formatted: String
    if _format == Format.JSON:
        formatted = _format_line_json(level, category, msg, fields, source, label)
    else:
        formatted = _format_line_human(level, category, msg, fields, source, label)
    # Route to native Godot output by severity. push_warning gives us
    # the yellow icon in the Output panel; push_error gives red.
    # TRACE/DEBUG/INFO all share plain print since Godot doesn't have
    # lower visual tiers — the level tag in the line distinguishes them.
    # v0.23.4: FATAL also routes via push_error (same red), but the
    # level tag in the message + JSON output distinguishes it.
    match level:
        Level.FATAL, Level.ERROR:
            push_error(formatted)
        Level.WARN:
            push_warning(formatted)
        _:
            print(formatted)
    # Optional file mirror. In JSON mode the line already contains
    # the timestamp inside the object; in human mode we prepend an
    # extra ISO 8601 timestamp for unambiguous time-ordering when
    # the human format's include_timestamps is disabled.
    if _file_sink_enabled and _file != null:
        if _format == Format.JSON:
            _file.store_line(formatted)
        else:
            var ts: String = Time.get_datetime_string_from_system(false, true)
            _file.store_line("%s %s" % [ts, formatted])
        _file.flush()

# v0.23.3: human-readable formatter. Output looks like:
#
#   [2026-05-14 17:32:45.123] INFO [gool/mixer]: registered sound | name="sfx/click" handle=42 (gool_sound_bank_loader.gd:167)
#
# Timestamp is included when ProjectSettings → include_timestamps
# is true; source (file:line) is included when include_source is
# true AND we're in a debug build (get_stack() returns frames).
static func _format_line_human(level: int, category: String,
                                msg: String, fields: Dictionary,
                                source: String, label: String = "") -> String:
    var parts: PackedStringArray = PackedStringArray()
    if _include_timestamps:
        parts.append("[%s]" % _human_timestamp())
    parts.append(_LEVEL_NAMES[level])
    # v0.23.4: label augments category. Format: [gool/category:label]
    # when a label is present; [gool/category] when empty. Same column
    # discipline either way.
    if label != "":
        parts.append("[gool/%s:%s]:" % [category, label])
    else:
        parts.append("[gool/%s]:" % category)
    if msg != "":
        parts.append(msg)
    var header: String = " ".join(parts)
    var fields_str: String = _format_fields_human(fields)
    if fields_str != "":
        header += " | " + fields_str
    if source != "":
        header += " (%s)" % source
    return header

# v0.23.3: JSON formatter. Emits one JSON object per line:
#
#   {"timestamp":"2026-05-14T17:32:45.123Z","level":"INFO",
#    "category":"mixer","msg":"registered sound",
#    "source":"addons/gool/prefabs/gool_sound_bank_loader.gd:167",
#    "data":{"name":"sfx/click","handle":42}}
#
# Field order in the output is deterministic (insertion order in
# the Dictionary literal below). Values in `data` are coerced via
# _coerce_for_json so Godot-native types (Vector3, Color, etc) get
# JSON-friendly representations instead of fail-to-serialize errors.
static func _format_line_json(level: int, category: String,
                               msg: String, fields: Dictionary,
                               source: String, label: String = "") -> String:
    # Always include timestamp and source in JSON mode — structured
    # logs without these are useless to analyzers, so the
    # include_timestamps / include_source toggles only affect the
    # human format. (If a user wants smaller JSON output, they can
    # post-process; we won't drop required structure here.)
    var obj: Dictionary = {
        "timestamp": _iso_timestamp(),
        "level": _LEVEL_NAMES[level],
        "category": category,
    }
    # v0.23.4: label is a top-level field (peer of category) so it
    # can be filtered/grouped in analyzers as easily as category.
    if label != "":
        obj["label"] = label
    obj["msg"] = msg
    if source != "":
        obj["source"] = source
    if not fields.is_empty():
        obj["data"] = _coerce_dict_for_json(fields)
    return JSON.stringify(obj)

# v0.23.3: render Dictionary as `key=value key2="value2"` for the
# human format. Differs from v0.23.2's _format_fields:
#   - String values get double quotes (matches logfmt strict)
#   - Numbers/bools/null are unquoted
#   - Godot types (Vector3 etc) render with their natural str() form
#     wrapped in quotes (since they contain spaces/parens)
static func _format_fields_human(fields: Dictionary) -> String:
    if fields.is_empty():
        return ""
    var parts: PackedStringArray = PackedStringArray()
    for key in fields:
        var v: Variant = fields[key]
        parts.append("%s=%s" % [str(key), _format_value_for_human(v)])
    return " ".join(parts)

# Per-value rendering for the human format. Numbers/bools/null
# unquoted; everything else stringified and quoted. The quoting
# distinguishes "42" the number from "42" the string in log
# readers, and tolerates fields containing spaces or '=' without
# breaking the key=value structure.
static func _format_value_for_human(v: Variant) -> String:
    if v == null:
        return "null"
    if v is bool:
        return "true" if v else "false"
    if v is int or v is float:
        return str(v)
    # All other types (String, Vector3, Color, custom objects, etc)
    # → stringify and wrap in double quotes. Internal double quotes
    # are backslash-escaped so the line stays parseable.
    var s: String = str(v)
    s = s.replace("\\", "\\\\").replace("\"", "\\\"")
    return "\"%s\"" % s

# Walk a Dictionary and coerce each value to a type JSON.stringify
# can serialize. Godot's JSON only supports: null, bool, int, float,
# String, Array, Dictionary. Anything else (Vector3, Color,
# Object, NodePath, ...) gets fallback-stringified, but with helpful
# special cases for the geometric types game code passes most often.
static func _coerce_dict_for_json(d: Dictionary) -> Dictionary:
    var out: Dictionary = {}
    for key in d:
        out[str(key)] = _coerce_for_json(d[key])
    return out

static func _coerce_for_json(v: Variant) -> Variant:
    if v == null:
        return null
    if v is bool or v is int or v is float or v is String:
        return v
    if v is Dictionary:
        return _coerce_dict_for_json(v)
    if v is Array:
        var arr: Array = []
        for item in v:
            arr.append(_coerce_for_json(item))
        return arr
    # Geometric types: emit as plain arrays so analyzers can parse
    # them without per-type knowledge. JSON has no notion of Vector3,
    # so [x,y,z] is the cleanest round-trip.
    if v is Vector2:
        return [v.x, v.y]
    if v is Vector3:
        return [v.x, v.y, v.z]
    if v is Vector4:
        return [v.x, v.y, v.z, v.w]
    if v is Color:
        return [v.r, v.g, v.b, v.a]
    if v is Quaternion:
        return [v.x, v.y, v.z, v.w]
    if v is StringName or v is NodePath:
        return str(v)
    # Last-resort: stringify whatever this is.
    return str(v)

# v0.23.3: walk get_stack() to find the first frame outside this
# logging script. The 'skip' parameter tells us how many internal
# frames to ignore (the typical call chain is user_code → info() →
# _log() → _get_source_location(), so from _log we skip 1 to get
# past info() and into the user's frame).
#
# Returns a string like "addons/gool/runtime_singleton.gd:118" or
# empty string in release builds (where get_stack() returns []) or
# if we somehow couldn't find a frame outside this script.
static func _get_source_location(skip_frames: int) -> String:
    # get_stack() is only available in debug builds. Returns an
    # empty array otherwise. We don't want to penalize release
    # builds with a missing-source string, so just emit empty.
    var stack: Array = get_stack()
    if stack.is_empty():
        return ""
    # Find the first frame whose source file isn't this logging
    # script. We can't just trust the skip count because Godot's
    # call dispatch sometimes adds intermediate frames depending on
    # whether the call was direct, lambda, or call_deferred.
    for frame in stack:
        var src: String = frame.get("source", "")
        if src == "":
            continue
        # Skip frames inside logging.gd itself. Hardcoded path
        # check (instead of get_script() which isn't available in
        # static methods).
        if src.ends_with("addons/gool/logging.gd"):
            continue
        var line: int = frame.get("line", 0)
        # Strip the res:// prefix for compactness in human format
        # AND JSON. The path-prefixed-not-bare-filename form keeps
        # disambiguation (e.g. addons/gool/runtime_singleton.gd vs
        # your project's runtime_singleton.gd if you had one).
        var rel: String = src
        if rel.begins_with("res://"):
            rel = rel.substr(6)  # len("res://") == 6
        return "%s:%d" % [rel, line]
    return ""

# ISO 8601 timestamp with millisecond precision and Z suffix
# (UTC). Format for JSON; consumed by log analyzers expecting a
# parseable timestamp field.
static func _iso_timestamp() -> String:
    var ts: float = Time.get_unix_time_from_system()
    var ms: int = int((ts - floor(ts)) * 1000.0)
    var base: String = Time.get_datetime_string_from_unix_time(int(ts), false)
    # Time returns "YYYY-MM-DDTHH:MM:SS"; we append .mmmZ
    return "%s.%03dZ" % [base, ms]

# Human-readable timestamp with millisecond precision in the local
# timezone. Format for the human log lines. Uses local time because
# the Output panel is consumed by a human reader who thinks in their
# local clock, not UTC.
static func _human_timestamp() -> String:
    var dt: Dictionary = Time.get_datetime_dict_from_system()
    var ts: float = Time.get_unix_time_from_system()
    var ms: int = int((ts - floor(ts)) * 1000.0)
    return "%04d-%02d-%02d %02d:%02d:%02d.%03d" % [
        dt.year, dt.month, dt.day,
        dt.hour, dt.minute, dt.second, ms]

# Coerce a Format enum value or a case-insensitive string ("human"
# / "json") to the integer Format. Unknown strings default to HUMAN
# with a warning. The string form lets users configure via Project
# Settings as a plain string.
static func _coerce_format(format) -> int:
    if format is int:
        if format == Format.HUMAN or format == Format.JSON:
            return format
        push_warning(
            "[GoolLog] format int %d not recognized; using HUMAN" % format)
        return Format.HUMAN
    if format is String:
        match format.to_lower():
            "human": return Format.HUMAN
            "json":  return Format.JSON
        push_warning(
            "[GoolLog] unknown format name '%s'; using human. " % format
            + "Valid: human, json.")
        return Format.HUMAN
    push_warning(
        "[GoolLog] format must be int or String; got %s. Using HUMAN."
        % typeof(format))
    return Format.HUMAN

# Coerce a Level enum value or a case-insensitive string name to
# the integer Level. Unknown strings default to INFO with a warning.
static func _coerce_level(level) -> int:
    if level is int:
        if level < 0 or level > Level.SILENT:
            push_warning(
                "[GoolLog] level int %d out of range; using INFO" % level)
            return Level.INFO
        return level
    if level is String:
        var name: String = level.to_upper()
        var idx: int = _LEVEL_NAMES.find(name)
        if idx < 0:
            push_warning(
                "[GoolLog] unknown level name '%s'; using INFO. " % level
                + "Valid: trace, debug, info, warn, error, silent.")
            return Level.INFO
        return idx
    push_warning(
        "[GoolLog] level must be int or String; got %s. Using INFO."
        % typeof(level))
    return Level.INFO

# Lazy first-use init. Reads Project Settings, populates cached
# values, and registers the settings as properties so they appear in
# the editor's Project Settings dialog with sensible defaults.
static func _ensure_initialized() -> void:
    if _initialized:
        return
    _initialized = true
    _register_project_settings()
    # v0.23.4: read verbosity FIRST. The preset determines defaults
    # for several other settings (global_level, include_source,
    # include_timestamps, file_sink_enabled), which are then
    # overridable via the per-setting Project Settings or runtime
    # API calls. AUTO resolves to one of the concrete presets
    # based on the current build type.
    var verbosity_str: String = ProjectSettings.get_setting(
        _PS_VERBOSITY, "auto")
    _verbosity = _coerce_verbosity(verbosity_str)
    if _verbosity == Verbosity.AUTO:
        _verbosity = _resolve_auto_verbosity()
    _apply_verbosity_preset(_verbosity)
    # Now read the individual settings. These override the preset
    # whenever they're explicitly set; if a user picked a preset and
    # didn't customize, the individual settings reflect the preset's
    # values (because _apply_verbosity_preset wrote them via
    # set_setting). When verbosity == CUSTOM, the preset application
    # is a no-op and the individual settings stand alone.
    var global_str: String = ProjectSettings.get_setting(
        _PS_GLOBAL_LEVEL, "info")
    _global_level = _coerce_level(global_str)
    var cats_str: String = ProjectSettings.get_setting(
        _PS_CATEGORIES, "")
    _parse_category_overrides(cats_str)
    _file_sink_enabled = ProjectSettings.get_setting(
        _PS_FILE_SINK, false)
    _file_path = ProjectSettings.get_setting(
        _PS_FILE_PATH, _DEFAULT_FILE_PATH)
    _include_timestamps = ProjectSettings.get_setting(
        _PS_INCLUDE_TS, false)
    # v0.23.3
    _format = _coerce_format(ProjectSettings.get_setting(
        _PS_FORMAT, "human"))
    _include_source = ProjectSettings.get_setting(
        _PS_INCLUDE_SOURCE, true)
    if _file_sink_enabled:
        enable_file_sink(_file_path)

# v0.23.4: resolve the AUTO verbosity to a concrete preset based
# on the current build type. The three-way split matches game-
# development reality: editor sessions want dev-level signal,
# exported debug builds (for QA) want more, release builds want
# silence by default.
static func _resolve_auto_verbosity() -> int:
    if OS.has_feature("editor"):
        return Verbosity.DEV
    if OS.is_debug_build():
        return Verbosity.DEBUG
    return Verbosity.SHIP

# v0.23.4: apply a verbosity preset by overwriting the individual
# settings. Called from _ensure_initialized BEFORE individual
# settings are read, so the individuals always reflect the active
# state (preset's values when a preset is in effect; their own
# values when verbosity == CUSTOM).
#
# CUSTOM is a no-op: the user controls every setting individually.
static func _apply_verbosity_preset(v: int) -> void:
    if v == Verbosity.CUSTOM:
        return
    var level: String
    var include_src: bool
    var include_ts: bool
    var file_sink: bool
    match v:
        Verbosity.SHIP:
            level = "warn"
            include_src = false
            include_ts = false
            file_sink = false
        Verbosity.DEV:
            level = "info"
            include_src = true
            include_ts = false
            file_sink = false
        Verbosity.DEBUG:
            level = "debug"
            include_src = true
            include_ts = false
            file_sink = false
        Verbosity.DIAGNOSTIC:
            level = "trace"
            include_src = true
            include_ts = true
            file_sink = true
        _:
            return
    ProjectSettings.set_setting(_PS_GLOBAL_LEVEL, level)
    ProjectSettings.set_setting(_PS_INCLUDE_SOURCE, include_src)
    ProjectSettings.set_setting(_PS_INCLUDE_TS, include_ts)
    ProjectSettings.set_setting(_PS_FILE_SINK, file_sink)

# Coerce a Verbosity enum value or a case-insensitive string name
# to the integer Verbosity. Unknown strings default to AUTO with a
# warning.
static func _coerce_verbosity(v) -> int:
    if v is int:
        if v >= Verbosity.AUTO and v <= Verbosity.CUSTOM:
            return v
        push_warning(
            "[GoolLog] verbosity int %d out of range; using AUTO" % v)
        return Verbosity.AUTO
    if v is String:
        match v.to_lower():
            "auto":       return Verbosity.AUTO
            "ship":       return Verbosity.SHIP
            "dev":        return Verbosity.DEV
            "debug":      return Verbosity.DEBUG
            "diagnostic": return Verbosity.DIAGNOSTIC
            "custom":     return Verbosity.CUSTOM
        push_warning(
            "[GoolLog] unknown verbosity '%s'; using AUTO. " % v
            + "Valid: auto, ship, dev, debug, diagnostic, custom.")
        return Verbosity.AUTO
    push_warning(
        "[GoolLog] verbosity must be int or String; got %s. Using AUTO."
        % typeof(v))
    return Verbosity.AUTO

# Register the Project Settings entries so they show up in the
# editor's settings dialog with proper types, defaults, and hints.
# Idempotent — set_setting on an existing key just updates the value,
# which is fine since we use it to ENSURE the default exists.
static func _register_project_settings() -> void:
    if not ProjectSettings.has_setting(_PS_GLOBAL_LEVEL):
        ProjectSettings.set_setting(_PS_GLOBAL_LEVEL, "info")
        ProjectSettings.add_property_info({
            "name": _PS_GLOBAL_LEVEL,
            "type": TYPE_STRING,
            "hint": PROPERTY_HINT_ENUM,
            "hint_string": "trace,debug,info,warn,error,silent",
        })
        ProjectSettings.set_initial_value(_PS_GLOBAL_LEVEL, "info")
    if not ProjectSettings.has_setting(_PS_CATEGORIES):
        ProjectSettings.set_setting(_PS_CATEGORIES, "")
        ProjectSettings.add_property_info({
            "name": _PS_CATEGORIES,
            "type": TYPE_STRING,
            "hint": PROPERTY_HINT_PLACEHOLDER_TEXT,
            "hint_string": "mixer:trace,decoder:warn,voice:silent",
        })
        ProjectSettings.set_initial_value(_PS_CATEGORIES, "")
    if not ProjectSettings.has_setting(_PS_FILE_SINK):
        ProjectSettings.set_setting(_PS_FILE_SINK, false)
        ProjectSettings.add_property_info({
            "name": _PS_FILE_SINK,
            "type": TYPE_BOOL,
        })
        ProjectSettings.set_initial_value(_PS_FILE_SINK, false)
    if not ProjectSettings.has_setting(_PS_FILE_PATH):
        ProjectSettings.set_setting(_PS_FILE_PATH, _DEFAULT_FILE_PATH)
        ProjectSettings.add_property_info({
            "name": _PS_FILE_PATH,
            "type": TYPE_STRING,
        })
        ProjectSettings.set_initial_value(_PS_FILE_PATH, _DEFAULT_FILE_PATH)
    if not ProjectSettings.has_setting(_PS_INCLUDE_TS):
        ProjectSettings.set_setting(_PS_INCLUDE_TS, false)
        ProjectSettings.add_property_info({
            "name": _PS_INCLUDE_TS,
            "type": TYPE_BOOL,
        })
        ProjectSettings.set_initial_value(_PS_INCLUDE_TS, false)
    # v0.23.3
    if not ProjectSettings.has_setting(_PS_FORMAT):
        ProjectSettings.set_setting(_PS_FORMAT, "human")
        ProjectSettings.add_property_info({
            "name": _PS_FORMAT,
            "type": TYPE_STRING,
            "hint": PROPERTY_HINT_ENUM,
            "hint_string": "human,json",
        })
        ProjectSettings.set_initial_value(_PS_FORMAT, "human")
    if not ProjectSettings.has_setting(_PS_INCLUDE_SOURCE):
        ProjectSettings.set_setting(_PS_INCLUDE_SOURCE, true)
        ProjectSettings.add_property_info({
            "name": _PS_INCLUDE_SOURCE,
            "type": TYPE_BOOL,
        })
        ProjectSettings.set_initial_value(_PS_INCLUDE_SOURCE, true)
    # v0.23.4: verbosity preset
    if not ProjectSettings.has_setting(_PS_VERBOSITY):
        ProjectSettings.set_setting(_PS_VERBOSITY, "auto")
        ProjectSettings.add_property_info({
            "name": _PS_VERBOSITY,
            "type": TYPE_STRING,
            "hint": PROPERTY_HINT_ENUM,
            "hint_string": "auto,ship,dev,debug,diagnostic,custom",
        })
        ProjectSettings.set_initial_value(_PS_VERBOSITY, "auto")

# Parse the comma-separated category-overrides string from Project
# Settings into the _category_levels dictionary.
# Format: "category:level,category2:level2"
# Example: "mixer:trace,decoder:warn"
# Whitespace around tokens is tolerated; malformed entries are
# warned and skipped, so a typo in one entry doesn't disable the
# others.
static func _parse_category_overrides(s: String) -> void:
    _category_levels.clear()
    if s.strip_edges() == "":
        return
    for raw in s.split(",", false):
        var pair: String = raw.strip_edges()
        if pair == "":
            continue
        var bits: PackedStringArray = pair.split(":")
        if bits.size() != 2:
            push_warning(
                "[GoolLog] malformed category override '%s' "
                % pair
                + "(expected 'category:level'); skipping. Full "
                + "override string was: %s" % s)
            continue
        var cat: String = bits[0].strip_edges()
        var lvl_name: String = bits[1].strip_edges()
        if cat == "":
            push_warning(
                "[GoolLog] empty category name in override '%s'; skipping"
                % pair)
            continue
        _category_levels[cat] = _coerce_level(lvl_name)
