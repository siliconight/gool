# Copyright 2026 Brannen Graves
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing permissions
# and limitations under the License.

# addons/gool/resources/gool_music_state.gd
#
## A single named music state in the adaptive music state machine.
## Each instance pairs a logical state name (used by `set_state()`
## calls in your game code) with the actual sound to play and the
## crossfade duration to use when transitioning INTO this state.
#
## TWO AUTHORING PATHS
## ===================
#
## Music states can be defined two ways. Both produce the same
## runtime behavior; pick whichever fits your team:
#
##   1. **Declarative (inspector / .tres files)** — Create
##      `GoolMusicState` resources in the FileSystem panel, drag
##      them into your `MusicStateController.states_at_startup`
##      array. The controller hydrates these into its runtime
##      state dictionary during `_ready()`. Good for music states
##      that are stable across the game's lifetime.
#
##   2. **Imperative (code)** — Call
##      `music_state_controller.add_state(name, sound_name, fade_ms)`
##      from your game code. Good for dynamically-loaded music
##      (per-level themes, DLC tracks, conditional variations).
#
## The two paths coexist. Declarative states are added first
## during `_ready()`; imperative `add_state()` calls add to the
## same dictionary at any time and can also overwrite a
## declarative entry by name if you want to re-author a state at
## runtime.
#
## WHY THIS IS MINIMAL (THREE FIELDS)
## ==================================
#
## A music state has fewer parameters than you might expect.
## Things you might think belong here but don't:
#
##   - **Looping**: a property of the sound, not the state. Set
##     loop points / loop-enable on the entry in your
##     `GoolSoundBank` resource. A music state pointing at a
##     non-looping sound just plays to the end and stops; pointing
##     at a looping sound loops indefinitely until `set_state()` is
##     called with another state.
#
##   - **Crossfade curve**: gool's engine uses equal-power
##     crossfade — the energy-preserving transition that avoids
##     the perceptual dip mid-fade of a linear cross. There's no
##     curve choice today; if you need linear or S-curve crossfade,
##     that's an engine change, not a state parameter.
#
##   - **Beat / bar quantization**: also not implemented. Music
##     transitions today fire immediately on `set_state()` and the
##     crossfade runs sample-accurately from that point. If you
##     need beat-aware quantization, that's a future engine
##     feature (call it "MusicState.transition_quantize" when it
##     lands), not a v0.81.x concern.
#
## So the data model is: name, sound_name, fade_ms. Three fields.
## If a future patch adds new fields here, this doc comment gets
## updated in lockstep with the addition.

@tool
class_name GoolMusicState
extends Resource


## State identifier used by `MusicStateController.set_state(name)`.
## Must be unique within a single controller's state set.
## Convention: lowercase snake_case ("explore", "combat", "boss",
## "victory"). Validation: empty names are rejected at hydration
## with a warning.
@export var name: String = ""


## Name of the sound to play when this state is active. Must be a
## sound registered in your `GoolSoundBank` (or registered
## imperatively via `Gool.register_pcm_sound()` /
## `Gool.register_streaming_sound()`). The MusicStateController
## itself doesn't validate the name exists in the bank — if it
## doesn't, the engine logs a missing-sound warning at the time
## `set_state()` is called.
@export var sound_name: String = ""


## Crossfade duration, in milliseconds, used when transitioning
## INTO this state from another state. 1500ms (default) is a
## common smooth-but-not-sluggish value. Use shorter (300-600ms)
## for combat-on transitions where you want the change to feel
## decisive; use longer (3000-5000ms) for victory / debrief
## transitions where you want the music to swell in.
##
## Note: this is the IN fade for THIS state. The previous state's
## OUT fade is also `fade_ms` (equal-power crossfade pairs the
## incoming and outgoing volumes symmetrically), so 1500ms here
## means a full 1500ms transition, not 3000ms.
@export_range(0.0, 10000.0, 50.0, "suffix:ms") var fade_ms: float = 1500.0


## Returns true if this state's required fields are filled in.
## Used by `MusicStateController` during hydration to skip
## misconfigured entries with a clear warning.
func is_valid() -> bool:
	return name != "" and sound_name != ""
