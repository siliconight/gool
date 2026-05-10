# scripts/audio_setup.gd
#
# Generates and registers all sounds used by the demo. Called once
# from main.gd:_ready(). The synthesis here is deliberately
# minimal — these are placeholder sounds meant to demonstrate the
# audio architecture, not finished SFX. Replace with real assets by
# editing the calls below to point at an asset bank instead of
# synthesized PCM.
#
# Naming convention: gameplay-facing names ("rifle_fire", not
# "Rifle_Fire_03_Layered.wav"). Gameplay code should never see
# asset names — see docs/multiplayer.md §16.
#
# All sounds are mono unless noted. The runtime spatializes mono
# sources via the configured spatializer (default: equal-power pan
# with distance attenuation).

class_name AudioSetup
extends RefCounted

const SR := 48000

# Public entry point. Call once before any play/emitter calls.
static func register_all(Gool: Node) -> void:
    _register_weapons(Gool)
    _register_footsteps(Gool)
    _register_music(Gool)
    _register_ambience(Gool)
    _register_ui(Gool)
    _bind_combat_intensity_rtpc(Gool)

# ---------------------------------------------------------------------------
# Weapons (3 types). Each weapon has an attack/body/decay shape:
#   pistol  - short, bright, snappy (snap-decay 60 ms)
#   rifle   - medium, mid-high, more body (decay 120 ms)
#   shotgun - heavy, low-broad, longest body (decay 200 ms)
#
# Each weapon also ships a remote-distance "tail" — a quieter, low-
# pass-flavored cousin used for bots' shots when they're far from
# the listener. A real production setup would interpolate between
# near/far layers; for the demo a separate registered sound + the
# weapon code picking which to play is sufficient.
# ---------------------------------------------------------------------------

static func _register_weapons(Gool: Node) -> void:
    # Each weapon registers TWO definitions per sound — one targeted
    # at LocalSfx (for the local player's own weapon) and one at
    # RemoteSfx (for AI bots / remote teammates). The bus config in
    # gool/config.json puts a sidechain compressor on RemoteSfx
    # keyed off LocalSfx, so the local player's gun audibly ducks
    # remote gunshots. Same audio asset, different bus routing.
    var pistol_fire_pcm    := _make_weapon(0.06, 6000.0, 0.85)
    Gool.register_pcm_sound("pistol_fire_local",  pistol_fire_pcm, SR, 1)
    Gool.register_pcm_sound("pistol_fire_remote", pistol_fire_pcm, SR, 1)
    Gool.register_sound_definition("pistol_fire_local",  true, false, 1.0, 1.0, 80.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("pistol_fire_remote", true, false, 1.0, 1.0, 80.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    var pistol_tail_pcm    := _make_weapon(0.10, 2200.0, 0.45)
    Gool.register_pcm_sound("pistol_tail_local",  pistol_tail_pcm, SR, 1)
    Gool.register_pcm_sound("pistol_tail_remote", pistol_tail_pcm, SR, 1)
    Gool.register_sound_definition("pistol_tail_local",  true, false, 1.0, 30.0, 120.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("pistol_tail_remote", true, false, 1.0, 30.0, 120.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    var rifle_fire_pcm     := _make_weapon(0.12, 4500.0, 0.90)
    Gool.register_pcm_sound("rifle_fire_local",   rifle_fire_pcm, SR, 1)
    Gool.register_pcm_sound("rifle_fire_remote",  rifle_fire_pcm, SR, 1)
    Gool.register_sound_definition("rifle_fire_local",  true, false, 1.0, 1.0, 100.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("rifle_fire_remote", true, false, 1.0, 1.0, 100.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    var rifle_tail_pcm     := _make_weapon(0.20, 1600.0, 0.50)
    Gool.register_pcm_sound("rifle_tail_local",   rifle_tail_pcm, SR, 1)
    Gool.register_pcm_sound("rifle_tail_remote",  rifle_tail_pcm, SR, 1)
    Gool.register_sound_definition("rifle_tail_local",  true, false, 1.0, 40.0, 150.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("rifle_tail_remote", true, false, 1.0, 40.0, 150.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    var shotgun_fire_pcm   := _make_weapon(0.20, 2200.0, 1.0)
    Gool.register_pcm_sound("shotgun_fire_local",  shotgun_fire_pcm, SR, 1)
    Gool.register_pcm_sound("shotgun_fire_remote", shotgun_fire_pcm, SR, 1)
    Gool.register_sound_definition("shotgun_fire_local",  true, false, 1.0, 1.0, 120.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("shotgun_fire_remote", true, false, 1.0, 1.0, 120.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    var shotgun_tail_pcm   := _make_weapon(0.32, 900.0, 0.60)
    Gool.register_pcm_sound("shotgun_tail_local",  shotgun_tail_pcm, SR, 1)
    Gool.register_pcm_sound("shotgun_tail_remote", shotgun_tail_pcm, SR, 1)
    Gool.register_sound_definition("shotgun_tail_local",  true, false, 1.0, 50.0, 180.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.register_sound_definition("shotgun_tail_remote", true, false, 1.0, 50.0, 180.0,
                                     Gool.CATEGORY_SFX, "RemoteSfx")

    # Reload sound — same audio for all weapons in the demo. Local-
    # player reload routes to LocalSfx (it's "your" gun sound).
    Gool.register_pcm_sound("weapon_reload",
                              _make_reload_click(0.5), SR, 1)
    Gool.register_sound_definition("weapon_reload", true, false, 0.8, 1.0, 50.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")

# ---------------------------------------------------------------------------
# Footsteps — three variants per surface, picked at random by the
# FootstepSurfacePlayer prefab. The demo only uses one surface
# ("stone") since the scene is a single CSG floor. To add more
# surfaces, register additional variants and set
# `surface_sounds = { "stone": [...], "grass": [...] }` on the
# prefab.
# ---------------------------------------------------------------------------

static func _register_footsteps(Gool: Node) -> void:
    # Player + bot footsteps both go to LocalSfx in this single-host
    # demo. In a true multiplayer build you'd register two variants
    # (step_stone_*_local / step_stone_*_remote) and pick at play time
    # based on whether it's the local player walking or a remote peer.
    Gool.register_pcm_sound("step_stone_a", _make_step(0.05, 0.55, 800.0), SR, 1)
    Gool.register_sound_definition("step_stone_a", true, false, 0.8, 1.0, 30.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")

    Gool.register_pcm_sound("step_stone_b", _make_step(0.05, 0.65, 700.0), SR, 1)
    Gool.register_sound_definition("step_stone_b", true, false, 0.8, 1.0, 30.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")

    Gool.register_pcm_sound("step_stone_c", _make_step(0.05, 0.50, 900.0), SR, 1)
    Gool.register_sound_definition("step_stone_c", true, false, 0.8, 1.0, 30.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")

# ---------------------------------------------------------------------------
# Music — three states forming a tension arc:
#   explore   - quiet pad, mostly fundamentals
#   suspicion - middle pad with added fifth, slightly busier
#   combat    - punctuated rhythm + fifth + brighter top
#
# The MusicStateController prefab handles the equal-power crossfade
# between states. The CombatMusicDirector script triggers state
# transitions based on gunfire activity.
#
# All music sounds are looping (registered with looping=true) and
# get a small loop_crossfade_ms via SoundDefinition to mask the
# join when the loop wraps.
# ---------------------------------------------------------------------------

static func _register_music(Gool: Node) -> void:
    Gool.register_pcm_sound("music_explore",
                              _make_pad(110.0, 4.0, 0.30, 0), SR, 1)
    Gool.register_sound_definition("music_explore", false, true, 1.0, 50.0, 100.0,
                                     Gool.CATEGORY_MUSIC, "Music")

    Gool.register_pcm_sound("music_suspicion",
                              _make_pad(110.0, 4.0, 0.35, 1), SR, 1)
    Gool.register_sound_definition("music_suspicion", false, true, 1.0, 50.0, 100.0,
                                     Gool.CATEGORY_MUSIC, "Music")

    Gool.register_pcm_sound("music_combat",
                              _make_pad(110.0, 4.0, 0.40, 2), SR, 1)
    Gool.register_sound_definition("music_combat", false, true, 1.0, 50.0, 100.0,
                                     Gool.CATEGORY_MUSIC, "Music")

# ---------------------------------------------------------------------------
# Ambience — single low-passed noise bed. Looping, registered so the
# demo plays a continuous "wind / room tone" under everything else
# without the listener having to drift in and out of any
# particular zone. Real games would route per-zone ambience through
# AudioEmitter3D prefabs at zone centers.
# ---------------------------------------------------------------------------

static func _register_ambience(Gool: Node) -> void:
    Gool.register_pcm_sound("ambient_wind",
                              _make_ambience(8.0, 200.0), SR, 1)
    Gool.register_sound_definition("ambient_wind", false, true, 0.4, 50.0, 100.0,
                                     Gool.CATEGORY_AMBIENCE, "Ambient")

# ---------------------------------------------------------------------------
# UI — short tone for weapon-cycle feedback. Tagged with the UI
# audio category so it's routed via category_routing in
# config.json (default → Master, no ducking applied).
# ---------------------------------------------------------------------------

static func _register_ui(Gool: Node) -> void:
    Gool.register_pcm_sound("ui_select", _make_blip(0.10, 1200.0), SR, 1)
    Gool.register_sound_definition("ui_select", false, false, 0.6, 0.0, 0.0,
                                     Gool.CATEGORY_UI, "")

# ---------------------------------------------------------------------------
# RTPC — bind music volume to the "combat_intensity" parameter so
# that as the CombatMusicDirector raises intensity (0-1), the
# music attenuates from full volume down to ~30% gain. This is the
# RTPC-driven analog to the multi-tier sidechain ducking in the
# multi_tier_ducking C++ example. See README for the difference.
#
# Curve: scurve so intensity bumps don't pump the music too hard
# at low intensities (small bumps stay subtle).
# Smoothing: 300 ms so the music gain doesn't snap on rising/falling
# intensity edges.
# ---------------------------------------------------------------------------

static func _bind_combat_intensity_rtpc(Gool: Node) -> void:
    var binding := {
        "parameter":   "combat_intensity",
        "target":      "volume",
        "curve":       "scurve",
        "min_value":   0.0, "max_value": 1.0,
        "min_output":  1.0, "max_output": 0.30,
        "smoothing_ms": 300.0,
    }
    for state_sound in ["music_explore", "music_suspicion", "music_combat"]:
        Gool.bind_rtpc(state_sound, binding)
    # Initial value: 0 means "no combat", so music sits at full volume.
    Gool.set_rtpc("combat_intensity", 0.0)

# ===========================================================================
# Synthesis helpers. Math is intentionally simple — these are
# placeholders. The README explains how to swap in real assets.
# ===========================================================================

# Generic weapon shot: a short HP-filtered noise burst with an
# exponential envelope. `decay` is the time constant (smaller =
# faster snap). `tone_hz` shifts the spectral center via a 1-pole
# resonant filter. `amp` is peak.
static func _make_weapon(decay_secs: float, tone_hz: float, amp: float) -> PackedFloat32Array:
    var dur := decay_secs * 4.0  # tail out 4 time constants
    var n := int(dur * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    var prev := 0.0
    var coeff := exp(-2.0 * PI * tone_hz / float(SR))
    for i in range(n):
        var t := float(i) / float(SR)
        var env := exp(-t / decay_secs)
        # White noise + simple resonant emphasis at tone_hz via leaky
        # integrator. Crude but characterful.
        var raw := (randf() * 2.0 - 1.0) * env
        prev = raw * (1.0 - coeff) + prev * coeff
        # Mix raw + filtered for a bit of brightness on top.
        v[i] = (raw * 0.5 + prev * 1.5) * amp
    return v

# Footstep: short noise pulse with a weak resonant body.
static func _make_step(secs: float, amp: float, body_hz: float) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    var prev := 0.0
    var coeff := exp(-2.0 * PI * body_hz / float(SR))
    for i in range(n):
        var t := float(i) / float(SR)
        var env := exp(-t * 60.0)
        var raw := (randf() * 2.0 - 1.0) * env
        prev = raw * (1.0 - coeff) + prev * coeff
        v[i] = (raw * 0.4 + prev * 1.2) * amp
    return v

# Music pad. `chord_kind`:
#   0 - explore: fundamental + octave
#   1 - suspicion: fundamental + octave + fifth
#   2 - combat: fundamental + octave + fifth + slightly detuned
#   third + amplitude pulse every 0.5s (rhythmic energy)
static func _make_pad(freq: float, secs: float, amp: float, chord_kind: int) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    var freq_oct := freq * 2.0
    var freq_fifth := freq * 1.5
    var freq_third := freq * 1.26      # detuned major third (audible in combat)
    for i in range(n):
        var t := float(i) / float(SR)
        var s := 0.6 * sin(TAU * freq * t)
        s += 0.30 * sin(TAU * freq_oct * t)
        if chord_kind >= 1:
            s += 0.20 * sin(TAU * freq_fifth * t)
        if chord_kind >= 2:
            s += 0.18 * sin(TAU * freq_third * t)
            # half-second pulse imparts rhythmic combat energy.
            var pulse_phase := fmod(t, 0.5) / 0.5
            var pulse_env   := exp(-pulse_phase * 6.0)
            s += 0.20 * sin(TAU * freq * 4.0 * t) * pulse_env
        v[i] = s * amp
    return v

# Ambient wind: long-period filtered noise loop.
static func _make_ambience(secs: float, cutoff_hz: float) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    var prev := 0.0
    var coeff := exp(-2.0 * PI * cutoff_hz / float(SR))
    for i in range(n):
        var raw := (randf() * 2.0 - 1.0)
        prev = raw * (1.0 - coeff) + prev * coeff
        v[i] = prev * 0.50
    return v

# Reload click: descending-pitch tonal blip then shorter rising one,
# evokes "magazine ejected, magazine inserted."
static func _make_reload_click(secs: float) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    for i in range(n):
        var t := float(i) / float(SR)
        # Descending click in the first half, ascending in the second.
        var phase := t / secs
        var freq := 0.0
        var env := 0.0
        if phase < 0.5:
            freq = lerp(800.0, 400.0, phase * 2.0)
            env = exp(-(phase * 2.0) * 4.0) * 0.6
        else:
            var p := (phase - 0.5) * 2.0
            freq = lerp(500.0, 900.0, p)
            env = exp(-p * 3.0) * 0.5
        v[i] = sin(TAU * freq * t) * env
    return v

# UI tap.
static func _make_blip(secs: float, freq: float) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    for i in range(n):
        var t := float(i) / float(SR)
        v[i] = sin(TAU * freq * t) * exp(-t * 30.0) * 0.5
    return v
