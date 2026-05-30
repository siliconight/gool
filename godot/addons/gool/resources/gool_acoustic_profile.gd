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

# addons/gool/resources/gool_acoustic_profile.gd
#
# v0.62.0 — Phase 6.E.4 follow-up: scene-level acoustic profile.
#
# A saved bundle of reverb characteristics that describe how a
# space sounds — decay tail, damping, diffusion, predelay, wet
# level. The complementary GoolSceneProfile prefab applies one
# of these to the reverb bus at scene load.
#
# Concept positioning:
#
#   - GoolMaterialEqPreset (v0.61.2/.3): per-SURFACE EQ shape.
#     Answers "what does an impact on tile sound like?"
#
#   - GoolAcousticProfile (v0.62.0): per-SPACE reverb shape.
#     Answers "what does this room reverberate like?"
#
#   - These are orthogonal. A single level uses surface presets
#     on impact buses + one scene-wide acoustic profile + zero
#     or more ReverbZones for sub-region overrides. Each
#     concern stays in its own resource type.
#
# Why no occlusion / ambient-bus EQ / reverb-by-material in this
# Resource: the simplicity test (does this make a designer's
# workflow simpler, or just more flexible?) failed for all three.
# Occlusion already has reasonable defaults in the engine table;
# ambient-bus EQ doubles the cognitive load of authoring a profile;
# reverb-by-material doubles up with ReverbZones (which already
# cover spatial reverb). See docs/audio_design/acoustic_presence_eq.md
# Phase 6.E.4 status section for the full rationale.
#
# Storage convention:
#   res://addons/gool/acoustic_profiles/<Name>.tres   built-in
#   res://gool/acoustic_profiles/<sanitized_name>.tres  user
#
# Same dual-directory convention as v0.61.3 EQ presets. Built-ins
# ship with gool; users save tweaks alongside.

@tool
class_name GoolAcousticProfile
extends Resource


## Human-readable label shown in the prefab's profile dropdown.
@export var profile_name: String = ""


## Optional one-line description. Shown in tooltips / detail.
@export var description: String = ""


## Schema version. v0.62.0 is version 1. Bump if/when this Resource
## gains fields so loaders can default missing values gracefully.
@export var profile_schema_version: int = 1


# --- Reverb characteristics (mirrors GoolReverbZone fields) ---
#
# Ranges and defaults match ReverbZone's @export ranges so a
# designer who already knows the ReverbZone interface needs no
# new mental model for these. Both ultimately push the same
# audio::EffectParameter::Reverb_* params to the same reverb
# effect on the reverb bus.

## Reverb decay (0 = dry, 1 = very long tail). 0.6 ≈ medium room.
@export_range(0.0, 1.0, 0.01) var reverb_decay: float = 0.6

## Low-frequency damping inside the reverb tail.
@export_range(0.0, 1.0, 0.01) var reverb_lf_damping: float = 0.1

## High-frequency damping inside the reverb tail. Higher = warmer.
@export_range(0.0, 1.0, 0.01) var reverb_hf_damping: float = 0.3

## Diffusion (0 = colored echoes, 1 = smooth wash). 0.625 default
## matches the Dattorro plate that the engine ships with.
@export_range(0.0, 1.0, 0.01) var reverb_diffusion: float = 0.625

## Predelay before the reverb tail begins. Higher = larger room feel.
@export_range(0.0, 200.0, 1.0, "suffix:ms") var reverb_predelay_ms: float = 30.0

## Reverb wet level in dB. Lower = drier; higher = wetter.
@export_range(-60.0, 0.0, 0.5, "suffix:dB") var reverb_wet_gain_db: float = -12.0
