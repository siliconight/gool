extends Node

# AudioSetup
#
# Autoload that generates programmatic placeholder sounds at
# startup and registers them with Gool. Used by the multiplayer
# audio sandbox so the example "just works" without requiring
# the user to provide their own .wav/.ogg assets.
#
# Two sounds:
#
#   - "gunshot": short white noise burst with exponential decay
#       ~0.15 seconds, mono, 48kHz. Sharp transient suitable for
#       triggering ducking + spatial position tests.
#
#   - "music":   sustained drone loop with three sine harmonics
#       and a slow amplitude LFO, 4 seconds long (designed to
#       loop seamlessly — the duration is an integer number of
#       cycles of both the fundamental and the LFO).
#
# Both are generated as PackedFloat32Array and registered via
# Gool.register_pcm_sound() during _ready. Sample rate matches
# the runtime's configured rate (48kHz; if you change that in
# gool/config.json, change SAMPLE_RATE here too).
#
# In a real game you'd register actual recorded audio files via
# Gool.register_sound_from_file() or via a SoundBank resource.
# This file is a stand-in for testing the multiplayer audio
# chain without distribution-asset complications.

const SAMPLE_RATE: int = 48000

# Volume scaling factor applied to ALL generated samples to keep
# things polite. The sidechain compressor in gool/config.json
# expects normalized levels; over-loud raw signals push the
# limiter too hard and the ducking effect becomes inaudible.
const OUTPUT_GAIN: float = 0.5


func _ready() -> void:
	# Defer one frame to make sure Gool autoload's _ready has run
	# and the runtime is fully initialized before we register.
	await get_tree().process_frame
	_register_sounds()


func _register_sounds() -> void:
	if not Gool.is_initialized():
		push_warning("[AudioSetup] Gool not initialized; sounds not registered. "
				+ "Check Gool autoload order in project.godot.")
		return

	var t0 := Time.get_ticks_msec()

	# --- Gunshot: 0.15s white noise with exponential decay ---
	var gunshot_samples := _generate_gunshot()
	Gool.register_pcm_sound("gunshot", gunshot_samples, SAMPLE_RATE, 1)
	# Hint gool that this category is sfx_local by default — when
	# triggered via Gool.play_networked(), the firing peer plays it
	# locally (routed to LocalSfx → triggers ducker), while the
	# RPC fanout to other peers plays it on RemoteSfx (no duck).
	# This routing is handled at the playback call site, not here.

	# --- Music: 4s harmonic drone with LFO, loopable ---
	var music_samples := _generate_music_loop()
	Gool.register_pcm_sound("music", music_samples, SAMPLE_RATE, 1)

	var elapsed := Time.get_ticks_msec() - t0
	print("[AudioSetup] registered 2 sounds (gunshot, music) in %dms" % elapsed)


# White noise burst with ~50ms exponential decay. Suitable for a
# placeholder gunshot — sharp transient, energetic, but doesn't
# overload the bus chain.
func _generate_gunshot() -> PackedFloat32Array:
	var duration_s: float = 0.15
	var n: int = int(SAMPLE_RATE * duration_s)
	var samples := PackedFloat32Array()
	samples.resize(n)
	# Decay time constant: amplitude falls to ~37% over this duration.
	# 50ms gives a snappy attack-and-decay profile.
	var decay_samples: float = SAMPLE_RATE * 0.05
	for i in n:
		var envelope: float = exp(-float(i) / decay_samples)
		var noise: float = randf() * 2.0 - 1.0
		samples[i] = noise * envelope * 0.9 * OUTPUT_GAIN
	return samples


# Sustained drone with three sine harmonics + slow amplitude LFO.
# Designed to be seamlessly loopable: 4s @ 110Hz fundamental
# yields 440 complete cycles; the 0.25Hz LFO completes exactly
# 1 cycle, so the start/end samples match.
func _generate_music_loop() -> PackedFloat32Array:
	var duration_s: float = 4.0
	var n: int = int(SAMPLE_RATE * duration_s)
	var samples := PackedFloat32Array()
	samples.resize(n)

	# A2 fundamental, perfect fifth, and one octave above. Simple
	# triad-like timbre that's pleasant for sustained listening.
	var fundamental_hz: float = 110.0
	var harmonic_2: float = fundamental_hz * 1.5   # perfect fifth
	var harmonic_3: float = fundamental_hz * 2.0   # octave

	# 0.25Hz LFO = one full cycle per 4 seconds, matching loop length.
	var lfo_hz: float = 0.25

	for i in n:
		var t: float = float(i) / SAMPLE_RATE
		var fundamental_amp: float = sin(t * fundamental_hz * TAU) * 0.45
		var fifth_amp: float = sin(t * harmonic_2 * TAU) * 0.25
		var octave_amp: float = sin(t * harmonic_3 * TAU) * 0.15
		# LFO ranges 0..1, multiplied into amplitude for tremolo.
		var lfo: float = sin(t * lfo_hz * TAU) * 0.5 + 0.5
		var combined: float = (fundamental_amp + fifth_amp + octave_amp) * lfo
		samples[i] = combined * OUTPUT_GAIN
	return samples
