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

# addons/gool/resources/gool_sound_bank.gd
#
# Designer-authored mapping from gameplay sound names to Godot
# AudioStream resources. Stored as a .tres so it's diffable, shows
# up in version control, and supports inheritance / override via
# Godot's resource pipeline.
#
# Usage:
#   1. In the FileSystem dock, right-click → New Resource → GoolSoundBank.
#      Save it (e.g. res://sounds/main_bank.tres).
#   2. Open the resource. In the inspector, click `sounds` → Add Element
#      and add string/stream pairs:
#        "coin_pickup"  → res://sfx/coin.wav
#        "engine_loop"  → res://sfx/engine.ogg
#        "music_calm"   → res://music/exploration.ogg
#   3. Drop a GoolSoundBankLoader node into your main scene, assign
#      this resource to its `bank` field. On scene ready, every
#      entry gets registered with the runtime.
#   4. Reference the names from AudioEmitter3D.sound_name, etc.
#
# Why a Dictionary instead of a typed Array of entry resources?
# Two reasons:
#   * Godot 4.2's typed-Dictionary support is partial; a plain
#     Dictionary with @export gives the cleanest inspector UX
#     without forcing a separate Entry resource type.
#   * The intent is "sound name as key, stream as value", which
#     Dictionary models directly.
#
# The cost is no compile-time type check on the value side — a
# designer can drop something that isn't an AudioStream and find
# out at load time. GoolSoundBankLoader emits an actionable error
# (with the offending key name) rather than silently skipping or
# crashing.

@tool
class_name GoolSoundBank
extends Resource

## Mapping from gameplay sound name (String) to Godot AudioStream
## resource. The string keys become the names AudioEmitter3D and
## other prefabs reference via their `sound_name` properties.
##
## Supported AudioStream types: AudioStreamWAV, AudioStreamOggVorbis,
## AudioStreamMP3, and any other stream whose `resource_path` is
## set (i.e. backed by a .wav, .ogg, .mp3, or .flac file in the
## project). Runtime-generated streams (AudioStreamGenerator,
## AudioStreamPolyphonic) cannot be registered through this path
## — for those, call `Gool.register_pcm_sound()` from a script.
@export var sounds: Dictionary = {}

## Default spatialization setting applied to every entry on
## registration. Override per-emitter via the prefab's properties
## if you need finer control.
@export var default_spatialized: bool = true

## Default category routing. SFX (0) is the safe default; pick
## MUSIC (2), VOICE (1), AMBIENCE (3), UI (4), or DIALOGUE (5)
## if every entry in this bank belongs to one category. For
## mixed-category banks, leave this at SFX and route via
## `target_bus_name` on emitters or via the per-category routing
## in `res://gool/config.json`.
@export_enum("SFX", "Voice", "Music", "Ambience", "UI", "Dialogue")
var default_category: int = 0
