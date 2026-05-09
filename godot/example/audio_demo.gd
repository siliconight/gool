# godot/example/audio_demo.gd
#
# Drop this on a Node in a Godot scene to verify the binding.
# Plays a synthesized 1 kHz tone after a 1-second warmup, then
# crossfades to a 600 Hz tone, then stops with a fade.
#
# This is the simplest possible "is the binding working" check; for
# a real project you'd register sounds from .gpak/.wav files via the
# host C++ side and just play() them by name from GDScript.

extends Node

var runtime: GoolAudioRuntime
var music: GoolMusicChannel

func _ready():
    runtime = GoolAudioRuntime.new()
    add_child(runtime)
    if not runtime.init():
        push_error("audio engine init failed")
        return

    # Synthesize two short looping tones in Godot, register both.
    var sr := 48000
    var samples_a := PackedFloat32Array()
    var samples_b := PackedFloat32Array()
    samples_a.resize(sr)
    samples_b.resize(sr)
    var TAU := PI * 2.0
    for i in range(sr):
        samples_a[i] = 0.4 * sin(TAU * 440.0 * float(i) / float(sr))
        samples_b[i] = 0.4 * sin(TAU * 880.0 * float(i) / float(sr))
    runtime.register_pcm_sound("track_a", samples_a, sr, 1)
    runtime.register_pcm_sound("track_b", samples_b, sr, 1)

    music = GoolMusicChannel.new()
    add_child(music)
    music.attach(runtime)

    music.play("track_a", 200.0)

    await get_tree().create_timer(2.0).timeout
    music.play("track_b", 1000.0)        # crossfade A -> B over 1 second

    await get_tree().create_timer(2.5).timeout
    music.stop(500.0)

func _process(delta):
    if runtime and runtime.is_initialized():
        runtime.update(delta)

func _exit_tree():
    if runtime and runtime.is_initialized():
        runtime.shutdown()
