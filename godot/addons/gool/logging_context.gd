# addons/gool/logging_context.gd
#
# GoolLogContext — a small wrapper around GoolLog's static methods
# that pre-binds a category and (optionally) a label, so calling
# code doesn't have to repeat them on every log line.
#
# Typical pattern: declare one context per file at script scope.
#
#   var _log_ctx := GoolLog.create_context("emitter", "audio_emitter_3d.gd")
#
#   func play() -> void:
#       _log_ctx.info("play", {"sound": sound_name, "pos": global_transform.origin})
#       ...
#
#   func stop() -> void:
#       _log_ctx.debug("stop", {})
#
# Each context call delegates to the corresponding GoolLog static
# method, supplying the bound category and label. Per-call label
# override is supported by passing a non-empty string as the
# optional 3rd argument; pass "" (default) to use the bound label.
#
# Contexts are cheap — Object instances with two fields. Create
# them at script load or _ready; don't allocate them per-call.

class_name GoolLogContext
extends Object

# Category and label baked at create_context() time. Mutable so the
# caller can rebind them on a long-lived context (e.g. a music
# director that changes category when entering different game
# states), but the typical pattern is to set them once.
var category: String = ""
var label: String = ""

func trace(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.trace(category, msg, fields, _effective_label(override_label))

func debug(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.debug(category, msg, fields, _effective_label(override_label))

func info(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.info(category, msg, fields, _effective_label(override_label))

func warn(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.warn(category, msg, fields, _effective_label(override_label))

func error(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.error(category, msg, fields, _effective_label(override_label))

func fatal(msg: String, fields: Dictionary = {}, override_label: String = "") -> void:
    GoolLog.fatal(category, msg, fields, _effective_label(override_label))

# Returns the label that should be attached to the next log call:
# the per-call override if non-empty, otherwise the bound label.
# Empty bound + empty override = no label (and the formatters
# render the line without a label tag).
func _effective_label(override_label: String) -> String:
    return override_label if override_label != "" else label
