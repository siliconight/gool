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
# a category.
enum Level {
    TRACE  = 0,   # noisy per-frame / per-tick detail
    DEBUG  = 1,   # development-time state changes
    INFO  = 2,   # significant lifecycle events (init, shutdown)
    WARN  = 3,   # recoverable problems
    ERROR = 4,   # unrecoverable problems
    SILENT = 5,  # suppress all logging
}

# Human-readable level names, indexed by Level enum value.
const _LEVEL_NAMES: PackedStringArray = PackedStringArray([
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "SILENT"])

# Project Settings paths. These are read once at first-use time and
# cached. Runtime API calls (set_global_level etc) override the
# cached value without re-reading the settings file.
const _PS_GLOBAL_LEVEL: String = "addons/gool/logging/global_level"
const _PS_CATEGORIES: String   = "addons/gool/logging/categories"
const _PS_FILE_SINK: String    = "addons/gool/logging/file_sink_enabled"
const _PS_FILE_PATH: String    = "addons/gool/logging/file_path"
const _PS_INCLUDE_TS: String   = "addons/gool/logging/include_timestamps"

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
# short human-readable message, and an optional Dictionary of
# structured fields (rendered as key=value pairs after the message).
#
# Fields are best for state that varies per-call. Avoid baking
# variable state into the message itself — that defeats grep'ing
# log lines by message and forces analyzers to do regex.
#
# Good:   info("emitter", "play", {"sound": name, "pos": pos})
# Bad:    info("emitter", "play %s at %s" % [name, pos])
static func trace(category: String, msg: String, fields: Dictionary = {}) -> void:
    _log(Level.TRACE, category, msg, fields)

static func debug(category: String, msg: String, fields: Dictionary = {}) -> void:
    _log(Level.DEBUG, category, msg, fields)

static func info(category: String, msg: String, fields: Dictionary = {}) -> void:
    _log(Level.INFO, category, msg, fields)

static func warn(category: String, msg: String, fields: Dictionary = {}) -> void:
    _log(Level.WARN, category, msg, fields)

static func error(category: String, msg: String, fields: Dictionary = {}) -> void:
    _log(Level.ERROR, category, msg, fields)

# ─── internals ───────────────────────────────────────────────

# Core dispatcher. Threshold-checks, formats, routes to the
# appropriate Godot output function (print/push_warning/push_error),
# and mirrors to the file sink if enabled.
static func _log(level: int, category: String,
                 msg: String, fields: Dictionary) -> void:
    _ensure_initialized()
    var threshold: int = _category_levels.get(category, _global_level)
    if level < threshold:
        return
    var formatted: String = _format_line(level, category, msg, fields)
    # Route to native Godot output by severity. push_warning gives us
    # the yellow icon in the Output panel; push_error gives red.
    # TRACE/DEBUG/INFO all share plain print since Godot doesn't have
    # lower visual tiers — the level tag in the line distinguishes them.
    match level:
        Level.ERROR:
            push_error(formatted)
        Level.WARN:
            push_warning(formatted)
        _:
            print(formatted)
    # Optional file mirror. Includes timestamps even if the Output
    # panel format doesn't, since file logs are reviewed asynchronously
    # and the time context matters.
    if _file_sink_enabled and _file != null:
        var ts: String = Time.get_datetime_string_from_system(false, true)
        _file.store_line("%s %s" % [ts, formatted])
        _file.flush()

# Format a log line into the standard `[gool/CATEGORY LEVEL] message
# field=value field2=value2` shape used throughout gool's logs.
# Timestamps are prepended only if include_timestamps is enabled in
# Project Settings — useful for session captures, but noisy for
# day-to-day Output panel work.
static func _format_line(level: int, category: String,
                          msg: String, fields: Dictionary) -> String:
    var prefix: String = "[gool/%s %s]" % [category, _LEVEL_NAMES[level]]
    var body: String = msg if msg != "" else ""
    var fields_str: String = _format_fields(fields)
    var ts_str: String = ""
    if _include_timestamps:
        ts_str = Time.get_datetime_string_from_system(false, true) + " "
    if body == "" and fields_str == "":
        return ts_str + prefix
    if fields_str == "":
        return "%s%s %s" % [ts_str, prefix, body]
    if body == "":
        return "%s%s  %s" % [ts_str, prefix, fields_str]
    return "%s%s %s  %s" % [ts_str, prefix, body, fields_str]

# Render a Dictionary as a sequence of "key=value" tokens separated
# by single spaces. Values are coerced to string via Godot's default
# stringification (Vector3 → "(x, y, z)", float → "1.234", etc).
# Keys are emitted in iteration order, which for Dictionary in Godot
# 4 is insertion order — predictable for log readers.
static func _format_fields(fields: Dictionary) -> String:
    if fields.is_empty():
        return ""
    var parts: PackedStringArray = PackedStringArray()
    for key in fields:
        var v: Variant = fields[key]
        # Strings get wrapped in single-quotes only if they contain
        # whitespace or '=' — otherwise the bare value is easier to
        # read. This is the convention from logfmt.
        var rendered: String = ""
        if v is String:
            var s: String = v
            if s.contains(" ") or s.contains("="):
                rendered = "'%s'" % s
            else:
                rendered = s
        else:
            rendered = str(v)
        parts.append("%s=%s" % [str(key), rendered])
    return " ".join(parts)

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
    if _file_sink_enabled:
        enable_file_sink(_file_path)

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
