# addons/gool/resources/gool_master_fx_preset.gd
#
# v0.63.0 — Phase 7 Master FX Lite preset.
#
# A saved bundle of master-bus chain settings. Designer drops a
# GoolMasterFxProfile node, picks one of these from the dropdown
# (5 built-in profiles ship with gool; user profiles save to
# res://gool/master_fx_presets/), and the node pushes the preset's
# values to the MasterControl effect on the Master bus.
#
# What this controls (matches MasterControlConfig in
# audio_engine/dsp/master_control.h, three audible stages):
#
#   1. GLUE COMPRESSOR — gentle bus cohesion. Low ratio, soft
#      knee, slow attack. Makes disparate sources feel like one
#      mix without flattening them.
#
#   2. GAIN RIDER (LUFS-targeted) — slow follower toward an
#      integrated LUFS target. Cinematic moments stay quiet,
#      combat stays loud, but the SESSION-AVERAGE settles where
#      the target says. Time constant is multi-second so short
#      loud events still feel loud relative to average.
#
#   3. TRUE-PEAK LIMITER — brickwall ceiling at -1 dBTP default.
#      Lookahead so transients keep their shape. Non-negotiable
#      clip protection — the mix WON'T break.
#
# A LUFS meter taps the signal between stages 1 and 2 and feeds
# telemetry to the dock + the rider. Always on; not configurable.
#
# Storage:
#   res://addons/gool/master_fx_presets/<Name>.tres    built-in
#   res://gool/master_fx_presets/<sanitized_name>.tres  user
#
# Same dual-directory convention as v0.61.3 EQ presets and
# v0.62.0 acoustic profiles.

@tool
class_name GoolMasterFxPreset
extends Resource


## Human-readable label shown in the profile dropdown.
@export var preset_name: String = ""


## Optional one-line description. Shown in tooltips.
@export var description: String = ""


## Schema version. v0.63.0 is version 1. Bump when this Resource
## gains fields so loaders default missing values gracefully.
@export var preset_schema_version: int = 1


# --- Stage enables ---
#
# Each audible stage can be individually disabled. With all three
# off, the chain is bypass — the "None / bypass" preset uses this
# state. Useful for A/B against an authored mix without the chain.

@export var glue_enabled: bool = true
@export var rider_enabled: bool = true
@export var limiter_enabled: bool = true


# --- Stage 1: Glue compressor ---
#
# Conservative settings. -12 dB threshold + 2:1 ratio + 6 dB
# soft knee means the compressor does ~3 dB of work at hot
# moments and is effectively invisible at normal levels. Slow
# attack lets transients through; slow release smooths recovery.

@export_range(-30.0, 0.0, 0.5, "suffix:dB") var glue_threshold_db: float = -12.0
@export_range(1.0, 10.0, 0.1) var glue_ratio: float = 2.0
@export_range(1.0, 100.0, 1.0, "suffix:ms") var glue_attack_ms: float = 10.0
@export_range(10.0, 2000.0, 5.0, "suffix:ms") var glue_release_ms: float = 250.0
@export_range(0.0, 24.0, 0.5, "suffix:dB") var glue_knee_db: float = 6.0
@export_range(-12.0, 12.0, 0.1, "suffix:dB") var glue_makeup_db: float = 0.0


# --- Stage 3: Gain rider (LUFS-targeted) ---
#
# Per the design doc Section 5 Phase B target profiles:
#   Cinema:               -23 LUFS
#   Default / Standard FPS: -16 LUFS
#   Streaming:            -14 LUFS
#   Competitive clarity:  -12 LUFS
#   Headphones:           -18 LUFS
#   Night mode:           -22 LUFS

@export_range(-30.0, -8.0, 0.5, "suffix:LUFS") var rider_target_lufs: float = -16.0
@export_range(500.0, 10000.0, 100.0, "suffix:ms") var rider_time_constant_ms: float = 3000.0
@export_range(0.0, 12.0, 0.5, "suffix:dB") var rider_max_gain_db: float = 6.0
@export_range(-12.0, 0.0, 0.5, "suffix:dB") var rider_min_gain_db: float = -6.0
## How far below the target current LUFS can fall before the rider
## freezes its gain. Prevents noise-floor amplification on silence.
@export_range(-24.0, 0.0, 1.0, "suffix:LU") var rider_freeze_below_lufs: float = -6.0


# --- Stage 4: True-peak limiter ---
#
# -1 dBTP default matches broadcast / cert convention. Gives
# downstream consumers (lossy codecs, DAC reconstruction) 1 dB of
# headroom against intersample peaks. 5 ms lookahead = 240 samples
# @ 48 kHz; adds 5 ms latency to master (one-time, at scene load).

@export_range(-6.0, 0.0, 0.1, "suffix:dBTP") var limiter_ceiling_dbtp: float = -1.0
@export_range(10.0, 500.0, 5.0, "suffix:ms") var limiter_release_ms: float = 50.0
## Lookahead length in ms. Change requires effect re-init; designers
## won't typically tune this live. 5 ms is the industry-typical
## "transparent transient preservation" sweet spot.
@export_range(0.0, 10.0, 0.5, "suffix:ms") var limiter_lookahead_ms: float = 5.0
