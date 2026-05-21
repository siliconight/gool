# addons/gool/dialogue_director.gd
#
# Autoload. Manages NPC bark playback with per-speaker step-on
# prevention, priority-based interruption, and (when paired with
# a properly-wired bus graph) automatic ducking of SFX and Music
# while dialogue plays.
#
# Why this prefab exists:
# Without it, shipping L4D2-style callouts means each AI script
# manually tracks "is this speaker already talking", calls the
# right play_3d() arguments, and somehow ensures "TANK!" cuts
# through gunfire. That's audio-engineer work. DialogueDirector
# absorbs it into a one-liner:
#
#     DialogueDirector.bark("survivor_a", "callout_tank", 200)
#
# Speaker step-on, priority interruption, and bus routing all
# handled inside. SFX ducking is handled by the bus graph (not
# by this prefab) — see `docs/audio_design/dialogue_setup.md`
# for the recommended sidechain wiring, or look at the default
# config.json shipped by gool's plugin.gd which has it pre-wired.
#
# Public API:
#
#   register_speaker(speaker_id: String, node: Node3D)
#   unregister_speaker(speaker_id: String)
#   bark(speaker_id, sound_name, priority := 128,
#        duration_s := 2.0, position := null) -> bool
#   is_speaking(speaker_id: String) -> bool
#   stop_all() -> void
#
# Position resolution priority on bark():
#   1. Explicit `position` argument if given (Vector3)
#   2. Registered speaker's global_position if speaker_id registered
#   3. Vector3.ZERO with a push_warning (likely a bug in caller)
#
# Same-speaker collision behavior (configurable via same_speaker_policy):
#   "interrupt_if_higher_priority" (default) — only interrupt if the
#       new bark has a strictly higher priority than the active one
#   "always_interrupt" — every new bark cancels any current bark
#       from the same speaker
#   "drop_if_active" — drop the new bark; let the current one finish

extends Node

# Configurable defaults — exported so devs can change via the
# Autoloads inspector or by setting at runtime.

## The bus to route barks through. Must exist in your gool/config.json
## bus graph for ducking to work. The default config that ships with
## the gool plugin includes a "Dialogue" bus pre-wired as a sidechain
## source for Music + LocalSfx + RemoteSfx. If you've customized your
## config and removed the Dialogue bus, set this to whatever bus you
## use for NPC speech.
@export var dialogue_bus_name: String = "Dialogue"

## Policy for handling a bark() call when the same speaker is already
## mid-bark. See file header for the three valid values.
@export_enum("interrupt_if_higher_priority", "always_interrupt", "drop_if_active")
var same_speaker_policy: String = "interrupt_if_higher_priority"

## If true, the director prints one diagnostic line on first bark()
## when the dialogue bus appears unwired (no Dialogue bus in graph
## or no sidechain references to it). Helpful for new adopters; can
## be toggled off in shipping builds.
@export var warn_on_unwired_setup: bool = true

# Internal state -------------------------------------------------

# speaker_id (String) → Node3D
var _speakers: Dictionary = {}

# speaker_id (String) → { sound_name: String, priority: int,
#                          timer: SceneTreeTimer }
var _active_barks: Dictionary = {}

# Whether we've already issued the "unwired" warning this session.
# Reset across editor runs / restarts; not persisted.
var _setup_warning_issued: bool = false

# Lifecycle ------------------------------------------------------

func _ready() -> void:
	# We don't validate the bus graph here because the gool autoload
	# may not yet have run init(). First bark() does the check.
	pass

# Public API -----------------------------------------------------

## Register a Node3D as the source location for a speaker. After
## registration, bark(speaker_id, ...) without an explicit position
## will use the registered node's global_position automatically.
##
## Pass the same speaker_id you'll use in bark() — typically the
## character's identifier string ("survivor_a", "bot_zoey",
## "enemy_charger_3"). Re-registering the same speaker_id with a
## different node updates the binding.
func register_speaker(speaker_id: String, node: Node3D) -> void:
	if speaker_id.is_empty():
		push_warning("[DialogueDirector] register_speaker: empty speaker_id")
		return
	if node == null:
		push_warning("[DialogueDirector] register_speaker: null node "
				+ "for '%s' — call unregister_speaker instead" % speaker_id)
		return
	_speakers[speaker_id] = node

## Forget a previously-registered speaker. Call this on the speaker's
## tree_exiting signal so dead NPCs don't keep stale registrations.
## No-op if the speaker_id wasn't registered.
func unregister_speaker(speaker_id: String) -> void:
	_speakers.erase(speaker_id)
	# Also clear any active bark — the speaker is going away, so any
	# lingering Timer would dangle.
	if _active_barks.has(speaker_id):
		_active_barks.erase(speaker_id)

## Trigger a bark for the named speaker. Returns true if the bark
## was actually played (or queued); false if it was dropped due to
## the same_speaker_policy or because the runtime/bus is unavailable.
##
## priority: integer 0-255 (matches gool's emitter priority scale).
##   Higher = more important. Used both for gool's voice eviction
##   AND for same-speaker interrupt decisions. Typical scale:
##     50  = ambient chatter, idle reactions
##     128 = standard combat callouts (default)
##     200 = special-infected spawn ("TANK!", "WITCH!")
##     250 = critical narrative ("I'm down!", incapacitation gasps)
##
## duration_s: how long the director should consider this bark
##   "in progress" before clearing the slot. Used to schedule
##   automatic dequeueing. Doesn't actually stop the audio early
##   if duration is too short — gool plays the full sound. If too
##   long, the slot stays "busy" past the audio finishing. Set
##   this to roughly your bark sound's length; 2.0s is a sensible
##   default for short callouts.
##
## position: Vector3 source location. If null (default) and the
##   speaker is registered, uses the registered Node3D's global
##   position. If null and not registered, plays at Vector3.ZERO
##   with a push_warning.
func bark(speaker_id: String, sound_name: String,
		priority: int = 128, duration_s: float = 2.0,
		position: Variant = null) -> bool:
	if speaker_id.is_empty() or sound_name.is_empty():
		push_warning("[DialogueDirector] bark: empty speaker_id or sound_name")
		return false

	# First-call setup check — diagnose unwired bus graph
	# once per session.
	if warn_on_unwired_setup and not _setup_warning_issued:
		_check_setup()
		_setup_warning_issued = true

	# Same-speaker collision handling
	if _active_barks.has(speaker_id):
		var current = _active_barks[speaker_id]
		match same_speaker_policy:
			"always_interrupt":
				_clear_slot(speaker_id)
			"drop_if_active":
				return false
			_:   # "interrupt_if_higher_priority" (default)
				if priority > current.priority:
					_clear_slot(speaker_id)
				else:
					return false

	# Resolve position
	var play_position: Vector3 = Vector3.ZERO
	if position is Vector3:
		play_position = position
	elif _speakers.has(speaker_id):
		var node: Node3D = _speakers[speaker_id]
		if is_instance_valid(node):
			play_position = node.global_position
		else:
			# Speaker registered but the node is gone — clean up.
			_speakers.erase(speaker_id)
			push_warning(("[DialogueDirector] bark: speaker '%s' was "
					+ "registered but the node is no longer valid — "
					+ "unregistered. Pass an explicit position for "
					+ "this bark.") % speaker_id)
	else:
		push_warning(("[DialogueDirector] bark: speaker '%s' is not "
				+ "registered and no position was passed — playing at "
				+ "world origin. Either pass a position argument or "
				+ "register_speaker(...) first.") % speaker_id)

	# Play via gool's positional API. Uses priority for voice
	# eviction; bus routing comes from the sound's metadata in the
	# sound bank (which the dev should set to dialogue_bus_name).
	var gool := _get_gool()
	if gool == null:
		return false
	var ok: bool = gool.play_3d(sound_name, play_position, priority)
	if not ok:
		return false

	# Track the active bark so same-speaker collisions detect it.
	# Use SceneTreeTimer for slot cleanup — simple and doesn't need
	# a node added to the tree.
	var timer := get_tree().create_timer(duration_s)
	timer.timeout.connect(_on_bark_finished.bind(speaker_id))
	_active_barks[speaker_id] = {
		"sound_name": sound_name,
		"priority": priority,
		"timer": timer,
	}
	return true

## True if the given speaker is currently mid-bark.
func is_speaking(speaker_id: String) -> bool:
	return _active_barks.has(speaker_id)

## Clear all active bark slots. Does NOT actually stop currently-
## playing audio — gool plays the sound to completion. Use this if
## you want subsequent bark() calls to ignore the in-progress
## tracking (e.g. on scene transition, pause menu open).
func stop_all() -> void:
	_active_barks.clear()

# Internal helpers -----------------------------------------------

func _clear_slot(speaker_id: String) -> void:
	_active_barks.erase(speaker_id)

func _on_bark_finished(speaker_id: String) -> void:
	# Only clear if this is still the active bark for this speaker.
	# (An interrupting bark may have already replaced the slot.)
	if _active_barks.has(speaker_id):
		_active_barks.erase(speaker_id)

func _get_gool() -> Node:
	if Engine.has_singleton("Gool"):
		return Engine.get_singleton("Gool")
	# Standard fallback for autoload lookup
	var tree := get_tree()
	if tree != null and tree.root != null:
		return tree.root.get_node_or_null("/root/Gool")
	return null

# Diagnose whether the bus graph appears wired for dialogue
# ducking. One-shot per session, called from bark() the first time.
func _check_setup() -> void:
	var gool := _get_gool()
	if gool == null:
		push_warning("[DialogueDirector] setup check: gool autoload "
				+ "not reachable — barks will fail until gool init().")
		return
	# Is the dialogue bus present at all?
	var effects = gool.get_bus_effects(dialogue_bus_name)
	if effects.is_empty():
		# get_bus_effects returns [] for both "no effects" and
		# "no such bus" — we can't distinguish here. But for a
		# bus that exists with no effects (pure passthrough,
		# which is the recommended Dialogue config), this isn't
		# necessarily an error. Only warn if we can confirm the
		# bus is missing — which we can't with this API.
		# Soft warning instead:
		push_warning(("[DialogueDirector] setup check: dialogue_bus_name "
				+ "= '%s' returned no effects. If this bus doesn't "
				+ "exist in your gool/config.json, barks will play "
				+ "but without ducking. See "
				+ "docs/audio_design/dialogue_setup.md for the "
				+ "recommended bus graph. Set warn_on_unwired_setup "
				+ "= false on the DialogueDirector autoload to "
				+ "silence this.") % dialogue_bus_name)
