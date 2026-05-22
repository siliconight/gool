# addons/gool/resources/gool_mix_snapshot.gd
#
# Captures whole-bus effect parameter state into a savable Resource
# and applies it back later in one call. The "named mix state" pattern
# that FMOD/Wwise call Snapshots — a one-tap swap between "combat
# mix", "stealth mix", "incapacitated mix" without authoring the
# transition by hand each time.
#
# Tier 1 (this release): instant apply. Most gain-type parameters
# in gool already ramp ~5 ms internally at the C++ level, so the
# perceived transition is smooth in practice for the most
# ducking-sensitive parameters. A controllable crossfade is Tier 2
# (would need either per-parameter tween loops in GDScript — costly
# on large snapshots — or a new C++ side capability we don't have).
#
# Typical usage:
#
#   # Author once (in editor or at game start)
#   var combat_mix := GoolMixSnapshot.capture_from(
#           ["Music", "LocalSfx", "RemoteSfx", "Dialogue"])
#   ResourceSaver.save(combat_mix, "res://mixes/combat.tres")
#
#   # Apply at runtime
#   Gool.apply_mix_snapshot(load("res://mixes/combat.tres"))
#
# Or one-step (capture + immediate apply later in the same session):
#
#   var pre_pause := Gool.capture_mix_snapshot(["Music", "LocalSfx"])
#   # ... game does its thing
#   Gool.apply_mix_snapshot(pre_pause)   # restore
#
# Snapshots are not signed or versioned — if you change the bus
# graph (add/remove effects, reorder them), saved snapshots may
# apply to the wrong effects. The implementation walks effect
# indices in order; missing effects produce push_warning and skip
# rather than throwing.

class_name GoolMixSnapshot
extends Resource

## Per-bus effect parameter state captured at `capture_from` time.
## Shape: { bus_name (String) → Array of effect-state dicts }
## Each effect-state dict has the shape
##   { "kind": int, "kind_name": String, "params": { param_id: float } }
## which matches what Gool.get_bus_effects() returns.
@export var bus_states: Dictionary = {}

## Human-readable label for inspector display / debugging. Optional;
## not used by apply(). Set this when authoring snapshots in code
## ("combat", "stealth", "incapacitated", "pause") so the .tres
## file is easier to identify.
@export var label: String = ""

## Capture the current state of the named buses into a new
## GoolMixSnapshot instance. The returned snapshot is a fresh
## Resource; save it with ResourceSaver.save(snap, "path.tres")
## or just keep it in a variable for in-session reuse.
##
## If any bus name doesn't exist, that bus is skipped (with a
## push_warning) — the snapshot still captures the buses that
## DO exist.
static func capture_from(bus_names: PackedStringArray) -> GoolMixSnapshot:
	var snap := GoolMixSnapshot.new()
	# Gool is the autoload — we look it up via the tree because
	# Resource-derived classes can't safely use autoload references
	# during their own construction in all contexts (e.g. editor).
	var gool := Engine.get_singleton("Gool") if Engine.has_singleton("Gool") else null
	if gool == null:
		# Fall back to the standard path. Most callers will be in
		# runtime where the autoload is reachable as a global.
		var tree := Engine.get_main_loop() as SceneTree
		if tree != null and tree.root != null:
			gool = tree.root.get_node_or_null("/root/Gool")
	if gool == null:
		push_warning("[GoolMixSnapshot] capture_from: no Gool autoload "
				+ "reachable — returning empty snapshot")
		return snap
	for bus in bus_names:
		var effects = gool.get_bus_effects(bus)
		if effects.is_empty():
			push_warning("[GoolMixSnapshot] capture_from: bus '%s' "
					+ "has no effects or doesn't exist — skipped" % bus)
			continue
		snap.bus_states[bus] = effects
	return snap

## Apply this snapshot's captured state back to the live bus graph.
## Returns true if every parameter applied successfully, false if
## any individual call failed or the runtime isn't initialized.
##
## "Apply" here is per-parameter set_effect_parameter — the C++ side
## handles its own ramp-time (~5 ms for gain-type params) so the
## perceived transition is smooth for most use cases. If you need a
## guaranteed-smooth long crossfade (e.g. 2-second mood change), this
## is a Tier-2 capability gool doesn't yet have.
func apply() -> bool:
	var gool := Engine.get_singleton("Gool") if Engine.has_singleton("Gool") else null
	if gool == null:
		var tree := Engine.get_main_loop() as SceneTree
		if tree != null and tree.root != null:
			gool = tree.root.get_node_or_null("/root/Gool")
	if gool == null:
		push_warning("[GoolMixSnapshot] apply: no Gool autoload "
				+ "reachable — snapshot not applied")
		return false
	var all_ok: bool = true
	for bus_name in bus_states:
		var effects = bus_states[bus_name]
		if not (effects is Array):
			continue
		for effect_idx in range(effects.size()):
			var effect = effects[effect_idx]
			if not (effect is Dictionary) or not effect.has("params"):
				continue
			var params = effect["params"]
			for param_id in params:
				var ok: bool = gool.set_effect_parameter(
						bus_name, effect_idx,
						int(param_id), float(params[param_id]))
				if not ok:
					push_warning(("[GoolMixSnapshot] apply: "
							+ "set_effect_parameter failed for "
							+ "bus='%s' effect=%d param=%d — "
							+ "snapshot may be stale relative to "
							+ "the current bus graph")
							% [bus_name, effect_idx, int(param_id)])
					all_ok = false
	return all_ok
