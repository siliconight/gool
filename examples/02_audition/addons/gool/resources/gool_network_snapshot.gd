# addons/gool/resources/gool_network_snapshot.gd
#
# Resource class capturing gool runtime state relevant to host
# migration. Parallel to GoolMixSnapshot, but for the network
# backend's host-migration scenario described in the network arch
# doc: the host periodically sends a sync state packet, and when a
# new host is selected the new host applies this state to its
# gool runtime before resuming.
#
# Expected to handle ~5 seconds of migration with 5–10 seconds of
# acceptable lost runtime state. We don't try to reconstruct every
# in-flight emitter or every sample-accurate playback position —
# the goal is "gool comes back online in a sensible-enough state
# that the next tick of gameplay doesn't feel jarring."
#
# Tier 1 (v0.46.0): captures the cheap, well-bounded state that's
# already exposed through the autoload — voice players, mix state,
# music state, current simulation tick.
#
# Tier 2 (v0.47.0+, needs new C++ bindings): active emitter
# handles, in-flight prediction IDs, current ambient-zone
# subscriptions. Defer until the lead's host-migration design is
# closer to landed.

class_name GoolNetworkSnapshot
extends Resource

## Simulation tick at the moment of capture. The new host resumes
## from this tick rather than starting from 0. Bridge applies this
## via advance_tick on the new host's first frame.
@export var simulation_tick: int = 0

## Server time (ms since some agreed-upon epoch) at capture. Used
## by the bridge's staleness checks and by gool's event timing on
## the new host.
@export var server_time_ms: int = 0

## Player IDs of registered voice sources at capture. New host
## re-registers each via Gool.register_voice_source on apply.
## Doesn't restore mid-stream voice buffers — those drop with the
## old host, and the jitter buffer rebuilds within ~200ms once
## packets resume to the new host.
@export var voice_player_ids: PackedInt32Array = PackedInt32Array()

## Mix state at capture — fader positions, effect parameters, etc.
## Captured by Gool.capture_mix_snapshot, applied by the same path
## on the new host. Optional; null means "don't restore mix state."
@export var mix_snapshot: GoolMixSnapshot = null

## Current music state name (if a MusicStateController was active).
## Optional; empty means "no music state to restore." Host code is
## responsible for re-entering the named state via its
## MusicStateController on apply.
@export var music_state_name: String = ""

## Free-form label for the snapshot. Set this if you're caching
## multiple snapshots — e.g. "pre-objective" / "during-tank-fight"
## — for easier identification in saved .tres files.
@export var label: String = ""
