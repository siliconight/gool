# scripts/weapon.gd
#
# Weapon component. Attach as a child of a character; the parent's
# global_transform is used as the firing position.
#
# This is a deliberately thin component. It owns the cooldown
# timer, picks the right sound for local-vs-remote firing, and
# emits a `fired` signal that the CombatMusicDirector listens to.
# Real games would extend this with ammo, range, accuracy spread,
# damage application, etc.
#
# Networking: in this single-host demo the weapon plays the sound
# directly via Gool. In a real multiplayer setup, replace the
# direct play call with a NetworkedAudioEvent.play() call to get
# server-authoritative validation + RPC fan-out (see
# docs/multiplayer.md and addons/gool/prefabs/networked_audio_event.gd).
# The architecture is the same; only the playback verb changes.

class_name Weapon
extends Node3D

enum WeaponKind { PISTOL, RIFLE, SHOTGUN }

@export var kind: WeaponKind = WeaponKind.RIFLE
## True if this weapon belongs to the locally-controlled player.
## Drives sound selection (loud near-field for local, distance
## attenuation for remote).
@export var is_local: bool = true

# Per-weapon stats. Tuned so the three feel distinct on press-and-hold.
# Sound names are BASE names — the actual sound played has a
# "_local" or "_remote" suffix appended at play time, picked based
# on `is_local`. The two variants share the same audio asset but
# route to different buses (LocalSfx vs RemoteSfx) so the multi-
# tier sidechain ducking config in gool/config.json triggers
# correctly: the local player's gun ducks both music AND remote
# teammates' guns.
const _STATS := {
    WeaponKind.PISTOL:  { "cooldown": 0.18, "fire_sound": "pistol_fire",  "tail": "pistol_tail" },
    WeaponKind.RIFLE:   { "cooldown": 0.12, "fire_sound": "rifle_fire",   "tail": "rifle_tail" },
    WeaponKind.SHOTGUN: { "cooldown": 0.85, "fire_sound": "shotgun_fire", "tail": "shotgun_tail" },
}

var _cooldown_remaining: float = 0.0

## Emitted whenever the weapon fires. CombatMusicDirector listens to
## this on every weapon in the scene to drive the combat-intensity
## RTPC. Carries `is_local` so the director can decide how hard to
## bump intensity (local shots dominate).
signal fired(is_local: bool, kind: int)

func _process(delta: float) -> void:
    if _cooldown_remaining > 0.0:
        _cooldown_remaining = max(0.0, _cooldown_remaining - delta)

# Returns true if the weapon actually fired (i.e. cooldown was
# clear). Returns false if the trigger was pulled too fast.
func try_fire() -> bool:
    if _cooldown_remaining > 0.0:
        return false
    _cooldown_remaining = _STATS[kind]["cooldown"]
    _play_fire()
    fired.emit(is_local, kind)
    return true

func get_kind_name() -> String:
    match kind:
        WeaponKind.PISTOL:  return "pistol"
        WeaponKind.RIFLE:   return "rifle"
        WeaponKind.SHOTGUN: return "shotgun"
    return "unknown"

func _play_fire() -> void:
    var Gool := get_node("/root/Gool")
    if not Gool.is_initialized():
        return
    var pos := global_position
    var stats: Dictionary = _STATS[kind]
    var suffix: String = "_local" if is_local else "_remote"

    if is_local:
        # Local fire: full-volume near-field, routed to LocalSfx bus
        # (the sidechain key signal). Priority is high so this
        # never gets evicted under voice cap pressure.
        Gool.play_3d(stats["fire_sound"] + suffix, pos, 220)
    else:
        # Remote fire: routed to RemoteSfx (which ducks under
        # LocalSfx in the bus config). Distance attenuation handles
        # falloff naturally; the slightly delayed "tail" emulates
        # propagation distance.
        Gool.play_3d(stats["fire_sound"] + suffix, pos, 160)
        get_tree().create_timer(0.04).timeout.connect(
            func(): Gool.play_3d(stats["tail"] + suffix, pos, 140))
