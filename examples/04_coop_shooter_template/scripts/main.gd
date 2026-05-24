# scripts/main.gd
#
# Top-level scene controller. Wires up:
#   - audio bootstrap (synthesize + register all sounds)
#   - listener-tracking (camera/player follows for spatial)
#   - music state controller + combat music director
#   - ambience emitter (long-lived loop)
#
# This script is the demo's "what to wire in this scene" hub. The
# corresponding scene (main.tscn) just instantiates the right
# nodes and assigns scripts. Running logic lives here.

extends Node3D

@onready var player:        PlayerController         = $Player
@onready var bot_pistol:    AiBot                    = $BotPistol
@onready var bot_rifle:     AiBot                    = $BotRifle
@onready var bot_shotgun:   AiBot                    = $BotShotgun
@onready var music:         MusicStateController     = $Music
@onready var music_director: CombatMusicDirector     = $MusicDirector
@onready var ambience:      AudioEmitter3D           = $Ambience
@onready var camera:        Camera3D                 = $Camera

func _ready() -> void:
    var Gool := get_node("/root/Gool")
    if not Gool.is_initialized():
        await Gool.ready_to_play

    AudioSetup.register_all(Gool)

    # Bot weapon assignment — one of each kind so all three
    # weapons get exercised.
    bot_pistol.weapon_kind  = Weapon.WeaponKind.PISTOL
    bot_rifle.weapon_kind   = Weapon.WeaponKind.RIFLE
    bot_shotgun.weapon_kind = Weapon.WeaponKind.SHOTGUN

    # MusicStateController setup. Each state name maps to a
    # registered sound + crossfade duration. The director picks
    # which one to set based on combat activity.
    music.add_state("explore",   "music_explore",   1500.0)
    music.add_state("suspicion", "music_suspicion", 1500.0)
    music.add_state("combat",    "music_combat",     600.0)
    music.set_state("explore")

    # CombatMusicDirector wiring — connect every weapon to its
    # `fired` signal. Group-based so adding/removing weapons at
    # runtime stays cheap.
    music_director.music = music
    for w in get_tree().get_nodes_in_group("weapons"):
        music_director.register_weapon(w)

    # Ambience layer — long-lived 3D emitter looping at the world
    # origin with a wide max-distance so it's audible everywhere
    # in the scene. Real games would use multiple AudioEmitter3D
    # nodes per ambient zone.
    ambience.sound_name        = "ambient_wind"
    ambience.looping           = true
    ambience.loop_crossfade_ms = 250.0
    ambience.autoplay          = true

    print("[coop_shooter_template] running. controls:")
    print("  WASD       - move local player")
    print("  Left click - fire current weapon")
    print("  Q          - cycle weapon (pistol/rifle/shotgun)")
    print("  bots wander and fire periodically")
    print("  music adapts: explore -> suspicion -> combat")

func _process(_delta: float) -> void:
    var Gool := get_node("/root/Gool")
    if not Gool.is_initialized():
        return
    # Listener follows the player. Camera transform feeds the
    # forward vector so panning matches what's visually on screen.
    Gool.set_listener_transform(
        player.global_transform.origin,
        -camera.global_transform.basis.z,
        Vector3.ZERO)
