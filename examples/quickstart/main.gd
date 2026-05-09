# examples/quickstart/main.gd
#
# The quickstart demo. Exercises every prefab in one scene:
#
#   - Adaptive music: state controller crossfades from "explore" to
#     "combat" every 6 seconds and back.
#   - Spatial audio: a moving emitter circles the listener, audible
#     panning + Doppler.
#   - Footsteps: a fake walker triggers footstep sounds on a timer.
#   - Reverb: a reverb zone covers half the play area; the listener
#     drifts in and out of it (signal logged to console).
#   - Voice chat: registers a mock voice source and submits a few
#     fake packets so the runtime exercises the voice path. (Real
#     mic capture is platform-specific; this demo proves the hookup
#     works without requiring an actual second player.)
#
# All sounds are synthesized procedurally on _ready so the demo has
# zero asset dependencies — just open the project, press Play, and
# you should hear it within ~1 second.

extends Node3D

# Synthesized sound parameters.
const SR := 48000
const TAU_F := 6.2831853

var music: MusicStateController
var moving_emitter: AudioEmitter3D
var voice: VoiceChatPlayer
var listener_node: Node3D

var _state_timer: float = 0.0
var _orbit_timer: float = 0.0
var _step_timer: float = 0.0
var _voice_seq: int = 0

func _ready() -> void:
    # Wait for the autoload to come up before we touch any audio.
    var Gool = get_node("/root/Gool")
    if not Gool.is_initialized():
        await Gool.ready_to_play

    _register_synth_sounds(Gool)

    # ---- listener ----
    # Drift the listener on a slow path so the spatial emitter has
    # something to pan against and the reverb zone has something to
    # enter/exit.
    listener_node = $Listener
    listener_node.add_to_group("gool_listener")

    # ---- adaptive music ----
    music = $Music
    music.add_state("explore", "music_explore", 1500.0)
    music.add_state("combat",  "music_combat",  600.0)
    music.set_state("explore")

    # ---- moving spatial emitter ----
    moving_emitter = $MovingEmitter
    moving_emitter.sound_name = "spatial_drone"
    moving_emitter.looping = true
    moving_emitter.loop_crossfade_ms = 50.0
    moving_emitter.autoplay = true

    # ---- voice chat (mock) ----
    voice = $Voice
    voice.player_id = 1
    # Wait one frame so the voice source is registered.
    await get_tree().process_frame
    _send_mock_voice_packet()

    print("[quickstart] running. you should hear:")
    print("  - low pad (music_explore), crossfading to brighter pad every 6s")
    print("  - drone circling the listener (spatial)")
    print("  - footstep clicks every 0.5s (when walker moves)")

func _process(delta: float) -> void:
    var Gool = get_node("/root/Gool")
    if not Gool.is_initialized():
        return

    # listener transform updates Doppler + panning
    listener_node.position.x = sin(Time.get_ticks_msec() * 0.0003) * 4.0
    Gool.set_listener_transform(listener_node.global_transform.origin,
                                  -listener_node.global_transform.basis.z,
                                  Vector3.ZERO)

    # state machine: ping-pong between explore and combat every 6s
    _state_timer += delta
    if _state_timer >= 6.0:
        _state_timer = 0.0
        if music.current_state == "explore":
            music.set_state("combat")
        else:
            music.set_state("explore")

    # orbit the moving emitter
    _orbit_timer += delta
    var radius := 5.0
    moving_emitter.position = Vector3(
        cos(_orbit_timer * 0.8) * radius,
        0.0,
        sin(_orbit_timer * 0.8) * radius)

    # footsteps
    _step_timer += delta
    if _step_timer >= 0.45:
        _step_timer = 0.0
        $Walker.step()

    # mock voice traffic every ~50ms
    _voice_seq += 1
    if _voice_seq % 3 == 0:
        _send_mock_voice_packet()

func _send_mock_voice_packet() -> void:
    var fake_bytes := PackedByteArray()
    fake_bytes.resize(40)            # opus packet typical length
    for i in range(40):
        fake_bytes[i] = i
    var Gool = get_node("/root/Gool")
    Gool.submit_voice_packet(1, fake_bytes, _voice_seq,
                                Time.get_ticks_msec() - 20,
                                Time.get_ticks_msec())

func _register_synth_sounds(Gool: Node) -> void:
    # 1-second 100 Hz pad for the explore state — low and warm.
    Gool.register_pcm_sound("music_explore",
                              _make_pad(100.0, 1.0, 0.3), SR, 1)
    Gool.register_sound_definition("music_explore", false, true, 1.0, 50.0, 100.0)

    # 1-second pad with extra harmonics for combat — brighter.
    Gool.register_pcm_sound("music_combat",
                              _make_pad(140.0, 1.0, 0.35), SR, 1)
    Gool.register_sound_definition("music_combat", false, true, 1.0, 50.0, 100.0)

    # 1-second 220 Hz spatial drone — registered as spatialized.
    Gool.register_pcm_sound("spatial_drone",
                              _make_pad(220.0, 1.0, 0.25), SR, 1)
    Gool.register_sound_definition("spatial_drone", true, true, 1.0, 30.0, 50.0)

    # short click for footsteps.
    Gool.register_pcm_sound("step_a", _make_click(0.05, 0.6), SR, 1)
    Gool.register_sound_definition("step_a", true, false, 1.0, 20.0, 0.0)
    Gool.register_pcm_sound("step_b", _make_click(0.05, 0.55), SR, 1)
    Gool.register_sound_definition("step_b", true, false, 1.0, 20.0, 0.0)
    Gool.register_pcm_sound("step_c", _make_click(0.05, 0.65), SR, 1)
    Gool.register_sound_definition("step_c", true, false, 1.0, 20.0, 0.0)

    # configure the walker's surface map
    $Walker.surface_sounds = {
        "stone": ["step_a", "step_b", "step_c"],
    }
    $Walker.default_surface = "stone"

func _make_pad(freq: float, secs: float, amp: float) -> PackedFloat32Array:
    # Detuned-stack pad: fundamental + 2 detuned octaves at lower
    # amplitude. Long enough that loop_crossfade_ms covers the join.
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    for i in range(n):
        var t := float(i) / float(SR)
        var s := 0.6 * sin(TAU_F * freq * t)
        s += 0.25 * sin(TAU_F * freq * 2.0 * t)
        s += 0.15 * sin(TAU_F * freq * 3.01 * t)   # detuned slightly
        v[i] = s * amp
    return v

func _make_click(secs: float, amp: float) -> PackedFloat32Array:
    var n := int(secs * SR)
    var v := PackedFloat32Array()
    v.resize(n)
    for i in range(n):
        var t := float(i) / float(SR)
        # exponentially-decaying noise pulse — sounds like a footfall.
        var env := exp(-t * 80.0)
        var s := (randf() * 2.0 - 1.0) * env
        v[i] = s * amp
    return v
