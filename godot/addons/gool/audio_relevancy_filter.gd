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

# addons/gool/audio_relevancy_filter.gd
#
# Helper for audio-event relevancy culling. Given a position and a
# priority, returns the list of multiplayer peer ids that should
# receive the corresponding RPC.
#
# Without filtering, replicated audio events bombard every peer
# regardless of whether they could possibly hear the sound. For a
# 32-player game in a 200-meter map with footsteps at 5 m audible
# range, naive RPC produces ~30× the bandwidth of the actual
# audible content. This filter is the cheap-but-effective fix.
#
# Two filters compose:
#   - Distance:  peer must be within `audible_radius` of the source
#   - Team:      optional; same team or non-team-locked sound only
#
# Bandwidth budgeting (priority-driven dropping) is a third axis,
# handled by NetworkedAudioEvent itself rather than here.
#
# This is a host-side helper. The host owns "where is each peer?"
# and "what team is each peer on?" — it tells us, we filter.

class_name AudioRelevancyFilter
extends RefCounted

## Per-peer context the filter needs to make decisions.
class PeerInfo:
	var peer_id: int
	var position: Vector3
	var team: int = 0          # 0 = no team / global
	func _init(pid: int, pos: Vector3, t: int = 0) -> void:
		peer_id = pid
		position = pos
		team = t

## Currently-known peers. Caller updates this from their multiplayer
## peer-tracking code. Map: peer_id -> PeerInfo.
var peers: Dictionary = {}

## Default audible radius (meters). Per-event override can be passed
## to filter().
var default_audible_radius: float = 50.0

## Update or add a peer's info. Call when peers join, change team,
## or move (typically every network tick for moving peers, or
## on-change-only for static ones).
func update_peer(peer_id: int, position: Vector3, team: int = 0) -> void:
	if peers.has(peer_id):
		var p: PeerInfo = peers[peer_id]
		p.position = position
		p.team = team
	else:
		peers[peer_id] = PeerInfo.new(peer_id, position, team)

func remove_peer(peer_id: int) -> void:
	peers.erase(peer_id)

## Return the list of peer_ids that should receive a sound event at
## `source_position`. Pass `audible_radius` to override the default
## (e.g. an explosion has 100 m radius, a footstep has 5 m).
##
## If `source_team` > 0, only peers on the same team will receive
## (e.g. team comms voice). Pass 0 for non-team-locked sounds.
##
## If `exclude_peer_id` is non-zero, that peer is omitted (used by
## the source to avoid sending an RPC back to themselves).
func filter(source_position: Vector3,
			  audible_radius: float = -1.0,
			  source_team: int = 0,
			  exclude_peer_id: int = 0) -> PackedInt32Array:
	var radius := audible_radius if audible_radius >= 0.0 else default_audible_radius
	var radius_sq := radius * radius
	var out := PackedInt32Array()
	for peer_id in peers.keys():
		if peer_id == exclude_peer_id:
			continue
		var p: PeerInfo = peers[peer_id]
		if source_team > 0 and p.team != source_team:
			continue
		var d_sq := source_position.distance_squared_to(p.position)
		if d_sq <= radius_sq:
			out.push_back(peer_id)
	return out
