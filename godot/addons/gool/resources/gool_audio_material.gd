# addons/gool/resources/gool_audio_material.gd
#
# Designer-authored AudioMaterial tag for a piece of level geometry.
# Saved as a .tres so it's diffable, reusable across multiple nodes,
# and re-skinnable (one resource per surface kind in your game,
# referenced from every collider that uses that surface).
#
# Usage:
#   1. In the FileSystem dock, right-click → New Resource →
#      GoolAudioMaterial. Save it (e.g.
#      res://materials/audio/concrete.tres).
#   2. Set `material` to one of the Gool.MATERIAL_* constants
#      (Default, Air, Glass, Wood, Drywall, Concrete, Metal,
#      Curtain, Foliage, Meat, Cardboard, Rubber, Liquid).
#   3. In your level scene, select a CollisionObject3D / Area3D /
#      StaticBody3D representing a surface. In its inspector,
#      Add Metadata → name `gool_audio_material`, value: drag the
#      resource you just saved into the field.
#   4. At runtime, `Gool.material_from_collider(node)` reads back
#      the material int, and `Gool.play_impact_sound(name, pos,
#      material)` picks the right variant from your sound bank's
#      by_material group.
#
# v0.60.0 (Phase 6.E.1 — Option B): per-instance EQ curve overrides.
#
#   When `override_enabled` is true, the resource's per-band fields
#   (low_freq_hz / low_gain_db / mid_freq_hz / mid_gain_db / mid_q /
#   high_freq_hz / high_gain_db) take effect instead of the engine's
#   built-in curve for the selected `material`. This lets designers
#   tweak the curve for one specific surface without forking the
#   engine table or affecting other uses of the same material.
#
#   To engage the override path, callers must pass the Resource
#   (not just the int) into APIs that accept it — primarily
#   `Gool.play_impact_sound(name, pos, resource_or_int)` and
#   `Gool.material_resource_from_collider(node)` (which returns the
#   Resource when one is set as collider metadata, falling back to
#   the int for non-resource cases).
#
#   When override_enabled=false the resource is bit-equivalent to a
#   pre-v0.60.0 file: zero-overhead, uses the engine table directly.
#   This is also the default for new resources, so backward compat
#   is perfect.
#
# Reusability:
#   Save one .tres per material (concrete.tres, wood.tres, etc.)
#   and reference the same resource from every collider that uses
#   it. Changing the resource later updates every collider that
#   points at it — no per-collider edits.
#
# Why a Resource instead of just an int metadata?
#   - Discoverable: shows up in the FileSystem dock as a recognized
#     type with the GoolAudioMaterial label.
#   - Reusable: one resource shared across many nodes, vs. a free
#     int metadata field per node that's easy to typo.
#   - Inspectable: the inspector renders the EQ curve plot for any
#     material (read-only since v0.59.2), with audition since v0.59.3,
#     and per-instance editable handles since v0.60.0.
#
# Both paths (int metadata, resource metadata, or
# audio_material:Concrete group) are accepted by
# Gool.material_from_collider so designers can mix and match per
# project preferences. The resource path is the only one that
# preserves overrides; the int and group paths look up the engine
# table directly.

@tool
class_name GoolAudioMaterial
extends Resource

## The AudioMaterial value. Use one of the Gool.MATERIAL_*
## constants (Default=0, Air=1, Glass=2, Wood=3, Drywall=4,
## Concrete=5, Metal=6, Curtain=7, Foliage=8, Meat=9,
## Cardboard=10, Rubber=11, Liquid=12). Out-of-range values
## are treated as MATERIAL_DEFAULT at lookup time.
##
## When override_enabled=false (the default) this int picks the
## engine's built-in curve for playback. When override_enabled=true
## the per-band fields below take effect instead, but `material`
## is still used as the "category label" for sound-bank by_material
## variant lookup (concrete impact sounds vs wood impact sounds).
@export var material: int = 0

## v0.60.0: when true, the per-band fields below override the
## engine's built-in EQ curve for this resource. When false (the
## default), the resource behaves as in v0.59.x — the engine table
## for `material` is used directly with zero overhead.
##
## On the false → true transition, the override fields auto-populate
## with the engine's current values for `material`, so the curve
## doesn't snap to defaults when override is engaged. The designer's
## starting point is "exactly what the engine has", and they tweak
## from there.
@export var override_enabled: bool = false:
	set(value):
		var was_off := not override_enabled
		override_enabled = value
		if value and was_off:
			# False → true transition: seed override fields from the
			# engine table for `material` so the curve doesn't change
			# audibly the moment override is toggled on. The designer
			# can then deviate from this starting point with drag
			# handles in the inspector.
			_seed_overrides_from_engine_table()
		# Notify any inspector / mixer dock that overrides changed,
		# so the visualizer can re-read the effective curve.
		emit_changed()

## v0.60.0: low-shelf parameters. Knee frequency in Hz; gain in dB.
## Shelf Q is fixed at 1.0 (the "no resonance" cookbook default,
## matching what the runtime impact / listener EQ uses).
@export_range(20.0, 2000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var low_freq_hz: float = 200.0 :
	set(value):
		low_freq_hz = clampf(value, 20.0, 20000.0)
		emit_changed()

@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var low_gain_db: float = 0.0 :
	set(value):
		low_gain_db = clampf(value, -24.0, 24.0)
		emit_changed()

## v0.60.0: peaking band parameters. Center frequency, gain, Q.
## Q is the "sharpness" of the band — 0.5 is broad, 1.0 moderate,
## 2.0+ surgical.
@export_range(50.0, 12000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var mid_freq_hz: float = 1000.0 :
	set(value):
		mid_freq_hz = clampf(value, 20.0, 20000.0)
		emit_changed()

@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var mid_gain_db: float = 0.0 :
	set(value):
		mid_gain_db = clampf(value, -24.0, 24.0)
		emit_changed()

@export_range(0.1, 10.0, 0.05)
var mid_q: float = 0.7 :
	set(value):
		mid_q = clampf(value, 0.1, 10.0)
		emit_changed()

## v0.60.0: high-shelf parameters. Same conventions as the low shelf.
@export_range(1000.0, 20000.0, 1.0, "or_greater", "or_less", "suffix:Hz")
var high_freq_hz: float = 8000.0 :
	set(value):
		high_freq_hz = clampf(value, 20.0, 20000.0)
		emit_changed()

@export_range(-12.0, 12.0, 0.1, "suffix:dB")
var high_gain_db: float = 0.0 :
	set(value):
		high_gain_db = clampf(value, -24.0, 24.0)
		emit_changed()


## v0.60.0: return the effective EQ curve as a Dictionary in the same
## shape Gool.get_material_eq_for_material() returns.
##
##   keys: low_gain_db, low_freq_hz, mid_gain_db, mid_freq_hz,
##         mid_q, high_gain_db, high_freq_hz, is_neutral
##
## When override_enabled=false: looks up the engine table via the
## Gool autoload (when reachable) or returns a neutral curve as
## fallback. When override_enabled=true: returns the override fields.
##
## Editor-context callers (the inspector) should use the engine-table
## fallback embedded in the inspector plugin itself (which mirrors
## the C++ table), not call this method, since the Gool autoload
## isn't reachable in editor.
func get_curve() -> Dictionary:
	if override_enabled:
		var is_neutral := (absf(low_gain_db)  < 0.01
				and        absf(mid_gain_db)  < 0.01
				and        absf(high_gain_db) < 0.01)
		return {
			"low_gain_db":  low_gain_db,
			"low_freq_hz":  low_freq_hz,
			"mid_gain_db":  mid_gain_db,
			"mid_freq_hz":  mid_freq_hz,
			"mid_q":        mid_q,
			"high_gain_db": high_gain_db,
			"high_freq_hz": high_freq_hz,
			"is_neutral":   is_neutral,
		}
	# Fall through to the engine table via the autoload, if it's
	# available (game session). In editor context the autoload
	# isn't reachable; callers handle that path themselves.
	if Engine.has_singleton("Gool"):
		var gool: Object = Engine.get_singleton("Gool")
		if gool.has_method("get_material_eq_for_material"):
			return gool.get_material_eq_for_material(material)
	# Final fallback: neutral. The inspector replaces this with its
	# own hardcoded engine-table mirror so it has something to draw.
	return {
		"low_gain_db":  0.0, "low_freq_hz":  200.0,
		"mid_gain_db":  0.0, "mid_freq_hz":  1000.0, "mid_q": 0.7,
		"high_gain_db": 0.0, "high_freq_hz": 8000.0,
		"is_neutral":   true,
	}


## v0.60.0: reset all override fields to the engine table for the
## current `material`. Useful as a "revert" button alongside the
## inspector's drag handles.
##
## Sets override_enabled = true as a side effect (the user clicked
## "reset overrides", they want the override path active).
func reset_overrides_to_engine_table() -> void:
	_seed_overrides_from_engine_table()
	override_enabled = true
	emit_changed()


# Seed the per-band fields from the engine table for `material`.
# Called on the override_enabled false → true transition (so the
# curve doesn't snap) and explicitly by reset_overrides_to_engine_table.
#
# Uses the Gool autoload when available (game session); falls back
# to a small hardcoded mirror of the engine table when not (editor
# context, where autoload isn't reachable). The mirror values match
# include/audio_engine/geometry_query.h's MaterialEqByMaterial() as
# of v0.60.0; the inspector also carries the same mirror, and a CI
# test pins the C++ side to the same numbers.
func _seed_overrides_from_engine_table() -> void:
	var curve: Dictionary = {}
	if Engine.has_singleton("Gool"):
		var gool: Object = Engine.get_singleton("Gool")
		if gool.has_method("get_material_eq_for_material"):
			curve = gool.get_material_eq_for_material(material)
	if curve.is_empty():
		curve = _engine_table_fallback(material)
	low_freq_hz  = float(curve.get("low_freq_hz", 200.0))
	low_gain_db  = float(curve.get("low_gain_db", 0.0))
	mid_freq_hz  = float(curve.get("mid_freq_hz", 1000.0))
	mid_gain_db  = float(curve.get("mid_gain_db", 0.0))
	mid_q        = float(curve.get("mid_q",       0.7))
	high_freq_hz = float(curve.get("high_freq_hz", 8000.0))
	high_gain_db = float(curve.get("high_gain_db", 0.0))


# Hardcoded mirror of the C++ MaterialEqByMaterial() table for use
# when the Gool autoload isn't reachable (editor context primarily).
# Kept in sync with include/audio_engine/geometry_query.h as of
# v0.60.0. Same table the inspector embeds — single source of
# duplication, easy to fix in lockstep if the engine values change.
func _engine_table_fallback(mat: int) -> Dictionary:
	match mat:
		2:  # Glass
			return {"low_gain_db": -0.5, "low_freq_hz": 200.0,
					"mid_gain_db": -0.5, "mid_freq_hz": 1000.0, "mid_q": 1.0,
					"high_gain_db": 3.5, "high_freq_hz": 6000.0}
		3:  # Wood
			return {"low_gain_db": 2.0, "low_freq_hz": 250.0,
					"mid_gain_db": 1.5, "mid_freq_hz": 500.0, "mid_q": 0.7,
					"high_gain_db": -1.5, "high_freq_hz": 6000.0}
		4:  # Drywall
			return {"low_gain_db": 0.0, "low_freq_hz": 200.0,
					"mid_gain_db": -1.0, "mid_freq_hz": 1000.0, "mid_q": 0.7,
					"high_gain_db": -1.0, "high_freq_hz": 8000.0}
		5:  # Concrete
			return {"low_gain_db": 1.0, "low_freq_hz": 200.0,
					"mid_gain_db": 2.5, "mid_freq_hz": 1500.0, "mid_q": 1.0,
					"high_gain_db": 2.0, "high_freq_hz": 6000.0}
		6:  # Metal
			return {"low_gain_db": 0.0, "low_freq_hz": 200.0,
					"mid_gain_db": 2.0, "mid_freq_hz": 2000.0, "mid_q": 1.5,
					"high_gain_db": 4.0, "high_freq_hz": 7000.0}
		7:  # Curtain
			return {"low_gain_db": 0.0, "low_freq_hz": 200.0,
					"mid_gain_db": -2.0, "mid_freq_hz": 800.0, "mid_q": 0.5,
					"high_gain_db": -4.0, "high_freq_hz": 4000.0}
		8:  # Foliage
			return {"low_gain_db": 0.0, "low_freq_hz": 200.0,
					"mid_gain_db": -1.5, "mid_freq_hz": 1000.0, "mid_q": 0.4,
					"high_gain_db": -2.0, "high_freq_hz": 6000.0}
		9:  # Meat
			return {"low_gain_db": 1.5, "low_freq_hz": 250.0,
					"mid_gain_db": -1.0, "mid_freq_hz": 800.0, "mid_q": 0.5,
					"high_gain_db": -3.5, "high_freq_hz": 5000.0}
		10:  # Cardboard
			return {"low_gain_db": -0.5, "low_freq_hz": 250.0,
					"mid_gain_db": -0.5, "mid_freq_hz": 1500.0, "mid_q": 0.6,
					"high_gain_db": -2.0, "high_freq_hz": 7000.0}
		11:  # Rubber
			return {"low_gain_db": 0.5, "low_freq_hz": 300.0,
					"mid_gain_db": -1.5, "mid_freq_hz": 1200.0, "mid_q": 0.7,
					"high_gain_db": -4.0, "high_freq_hz": 5000.0}
		12:  # Liquid
			return {"low_gain_db": 2.5, "low_freq_hz": 200.0,
					"mid_gain_db": -1.0, "mid_freq_hz": 600.0, "mid_q": 0.5,
					"high_gain_db": -6.0, "high_freq_hz": 4000.0}
	# Default + Air + unknown → neutral.
	return {"low_gain_db": 0.0, "low_freq_hz": 200.0,
			"mid_gain_db": 0.0, "mid_freq_hz": 1000.0, "mid_q": 0.7,
			"high_gain_db": 0.0, "high_freq_hz": 8000.0}
