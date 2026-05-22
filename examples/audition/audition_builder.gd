# scripts/audition_builder.gd
#
# Audition scene for gool v0.43.0 + v0.44.0 features.
#
# Programmatically builds a single room containing:
#
#   1. Material gallery — 13 panels in a row, one per AudioMaterial
#      enum value. Each panel is a shootable StaticBody3D tagged
#      with an AudioMaterialTag prefab and labeled with the material
#      name. Shoot the panels to hear impact sounds with per-material
#      occlusion + EQ + reverb behavior.
#
#   2. Reverb rooms — 5 walkable rectangular zones along the back
#      wall, each containing a ReverbZone driving a different
#      preset from GoolPresets.REVERB_*. Walk into a room and the
#      diegetic Sfx bus's reverb shifts to match the space.
#
#   3. Live demos:
#       - B key: fires a DialogueDirector bark. With the v0.43.0
#         config.json update, this ducks both Music and Sfx via
#         the sidechain compressors. Visually verifiable on the
#         editor's mixer dock.
#       - K key: captures the current bus state into a
#         GoolMixSnapshot held in this script.
#       - L key: applies the held snapshot, restoring the captured
#         state. Useful after fiddling faders on the mixer dock.
#
#   4. Live observability:
#       - F3 in-game: toggles GoolDebugOverlay (added automatically)
#       - In editor: the v0.44.0 Live Stats panel at the bottom of
#         the mixer dock shows voice count + master peak + drops +
#         per-player VOIP jitter while F5 is running.
#
# This is a sandbox audition tool, NOT a product. Gool is the
# product; this exists so I can validate v0.43.0 + v0.44.0
# behavior against real geometry before shipping a release.

extends Node3D

# ─── Material gallery configuration ────────────────────────────

const MATERIAL_CATALOG := [
	{ "id": 0,  "name": "Default",   "color": Color(0.45, 0.45, 0.45) },
	{ "id": 1,  "name": "Air",       "color": Color(0.85, 0.92, 1.0, 0.35) },
	{ "id": 2,  "name": "Glass",     "color": Color(0.7, 0.9, 1.0, 0.6) },
	{ "id": 3,  "name": "Wood",      "color": Color(0.55, 0.32, 0.15) },
	{ "id": 4,  "name": "Drywall",   "color": Color(0.85, 0.82, 0.78) },
	{ "id": 5,  "name": "Concrete",  "color": Color(0.6, 0.6, 0.6) },
	{ "id": 6,  "name": "Metal",     "color": Color(0.55, 0.58, 0.63), "metallic": 0.85 },
	{ "id": 7,  "name": "Curtain",   "color": Color(0.55, 0.3, 0.4) },
	{ "id": 8,  "name": "Foliage",   "color": Color(0.25, 0.55, 0.22) },
	{ "id": 9,  "name": "Meat",      "color": Color(0.65, 0.22, 0.18) },
	{ "id": 10, "name": "Cardboard", "color": Color(0.78, 0.66, 0.45) },
	{ "id": 11, "name": "Rubber",    "color": Color(0.13, 0.13, 0.15) },
	{ "id": 12, "name": "Liquid",    "color": Color(0.3, 0.5, 0.8, 0.7) },
]

const GALLERY_PANEL_WIDTH: float = 2.0
const GALLERY_PANEL_HEIGHT: float = 2.5
const GALLERY_PANEL_DEPTH: float = 0.5
# v0.47.1: bumped 0.4 → 1.2 so the FPS player can walk between
# panels on their way to the cathedral. The old 0.4m gap was a
# tight squeeze that effectively walled off the hub's north exit.
const GALLERY_PANEL_GAP: float = 1.2
const GALLERY_Z: float = -5.0   # v0.47.1: pulled in from -10 so it
                                # doesn't block the cathedral corridor

# ─── Reverb rooms configuration ────────────────────────────────
#
# v0.47.1: hub-and-spokes layout instead of the v0.43+ row-of-tiles.
# Five reverb presets now live in DISTINCT spatial environments with
# real walls so occlusion actually fires (the old layout had only
# Area3D triggers — walking between presets crossed invisible
# boundaries with no physical barrier between sound and listener).
#
# Layout (top-down, +Z is into the screen):
#
#                            ┌──────────────┐
#                            │  CATHEDRAL   │       z = -22 (center)
#                            │  14×9×14 m   │
#                            └──────┬───────┘
#                                doorway (south wall)
#                                   │
#                            ┌──────┴────────┐
#       ┌────────┐           │               │           ┌─────────┐
#       │BATHROOM│           │   HUB         │           │  CAVE   │
#       │  TILE  │── door ─→│  (open area,  │←── door ──│  10×5×12│
#       │ 5×3×5  │           │   gallery,    │           │         │
#       │        │           │  outdoor rev) │           │         │
#       └────────┘           │  16×16        │           └─────────┘
#       x = -14              │               │           x = +16
#                            └──────┬────────┘
#                                doorway (north wall)
#                                   │
#                            ┌──────┴───────┐
#                            │  SMALL ROOM  │            z = +14 (center)
#                            │   6×3×6 m    │
#                            └──────────────┘
#
# Walking between rooms requires going through doorways — sounds
# inside a room are occluded by walls when the listener is outside.
# Sounds in the hub can leak through doorways into rooms (and vice
# versa) at attenuated level, which is exactly the perceptual
# experience we want to test.
const REVERB_ROOMS := [
	{ "name": "Cathedral",
	  "preset_key": "REVERB_CATHEDRAL",
	  "center": Vector3(0, 0, -22),
	  "size": Vector3(14, 9, 14),
	  "doorway_wall": "south",
	  "wall_color": Color(0.55, 0.45, 0.50),     # stone-purple
	  "floor_color": Color(0.40, 0.30, 0.45),
	  # v0.47.1: stone walls + wooden pews — the classic cathedral
	  # acoustic surfaces, very different impact characters.
	  "targets": ["Concrete", "Wood"] },
	{ "name": "Cave",
	  "preset_key": "REVERB_CAVE",
	  "center": Vector3(16, 0, 0),
	  "size": Vector3(10, 5, 12),
	  "doorway_wall": "west",
	  "wall_color": Color(0.30, 0.30, 0.35),     # dark stone
	  "floor_color": Color(0.22, 0.22, 0.28),
	  # Concrete = bare rock; Foliage = wet moss / mineral growth.
	  "targets": ["Concrete", "Foliage"] },
	{ "name": "Bathroom Tile",
	  "preset_key": "REVERB_BATHROOM_TILE",
	  "center": Vector3(-14, 0, 0),
	  "size": Vector3(5, 3, 5),
	  "doorway_wall": "east",
	  "wall_color": Color(0.92, 0.95, 1.0),      # bright tile white
	  "floor_color": Color(0.85, 0.92, 1.0),
	  # Glass = mirror, Metal = faucet/fixtures. Both ring bright in
	  # the tile reverb's tight, short tail.
	  "targets": ["Glass", "Metal"] },
	{ "name": "Small Room",
	  "preset_key": "REVERB_SMALL_ROOM",
	  "center": Vector3(0, 0, 14),
	  "size": Vector3(6, 3, 6),
	  "doorway_wall": "north",
	  "wall_color": Color(0.65, 0.55, 0.40),     # warm wood
	  "floor_color": Color(0.55, 0.45, 0.30),
	  # Typical small-room finishes: wooden furniture + drywall.
	  "targets": ["Wood", "Drywall"] },
]

# Outdoor Open is the HUB itself — no walls, just an Area3D
# covering the central open space and a reverb zone configured
# from REVERB_OUTDOOR_OPEN. Defined separately because it doesn't
# have the same construction shape as the enclosed rooms.
#
# Sized generously (30×30 footprint) so the Area3D reaches all
# four doorways — without this, there's a small "dead strip" in
# each doorway where neither hub nor room Area3D fires. The hub's
# visual floor patch is smaller (16×16) so it stays
# legible from above.
const HUB_SIZE: Vector3 = Vector3(30, 6, 30)
const HUB_FLOOR_VISUAL_SIZE: Vector2 = Vector2(16, 16)
const HUB_CENTER: Vector3 = Vector3(0, 0, 0)

# Construction parameters shared across all enclosed rooms.
const WALL_THICKNESS: float = 0.3
const DOORWAY_WIDTH: float = 2.0
const DOORWAY_HEIGHT: float = 2.5

# ─── Snapshot demo state ───────────────────────────────────────

# Held snapshot — populated by K, applied by L. null until first K.
var _held_snapshot: GoolMixSnapshot = null

# ─── Lifecycle ─────────────────────────────────────────────────

func _ready() -> void:
	_build_environment()
	_build_floor()
	_build_material_gallery()
	_build_reverb_rooms()
	_register_test_bark()
	_add_debug_overlay()
	_add_usage_billboard()

func _input(event: InputEvent) -> void:
	if not (event is InputEventKey and event.pressed and not event.echo):
		return
	match event.keycode:
		KEY_B: _on_bark_key_pressed()
		KEY_K: _on_snapshot_capture()
		KEY_L: _on_snapshot_apply()

# ─── Builders ──────────────────────────────────────────────────

func _build_environment() -> void:
	# Sky + ambient — borrowed from box_level's style so the
	# audition reads visually similar to the rest of the sandbox.
	var world_env := WorldEnvironment.new()
	var env := Environment.new()
	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_horizon_color = Color(0.65, 0.65, 0.7)
	sky_mat.ground_horizon_color = Color(0.45, 0.45, 0.5)
	var sky := Sky.new()
	sky.sky_material = sky_mat
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_color = Color(0.3, 0.3, 0.35)
	env.ambient_light_energy = 0.6
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	world_env.environment = env
	add_child(world_env)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, -30, 0)
	light.shadow_enabled = true
	light.light_energy = 0.8
	add_child(light)

func _build_floor() -> void:
	# v0.47.1: bigger floor — 60×60 to comfortably contain the
	# hub-and-spokes layout (cathedral at z=-22, small room at z=+14,
	# bathroom at x=-14, cave at x=+16, each with room footprint
	# extending several more meters outward).
	var floor_size := Vector2(60, 60)
	var body := StaticBody3D.new()
	body.name = "AuditionFloor"
	body.position = Vector3(0, -0.05, 0)

	var mesh_inst := MeshInstance3D.new()
	var mesh := PlaneMesh.new()
	mesh.size = floor_size
	mesh_inst.mesh = mesh
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.18, 0.18, 0.22)
	mesh_inst.material_override = floor_mat
	body.add_child(mesh_inst)

	var col := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = Vector3(floor_size.x, 0.1, floor_size.y)
	col.shape = shape
	body.add_child(col)

	add_child(body)

# Map preset key string → const Dictionary from GoolPresets. Done
# via match because GoolPresets is a class_name (not instance),
# so dict-style dynamic lookup isn't available.
func _lookup_reverb_preset(key: String) -> Dictionary:
	match key:
		"REVERB_SMALL_ROOM":    return GoolPresets.REVERB_SMALL_ROOM
		"REVERB_MEDIUM_ROOM":   return GoolPresets.REVERB_MEDIUM_ROOM
		"REVERB_LARGE_HALL":    return GoolPresets.REVERB_LARGE_HALL
		"REVERB_CATHEDRAL":     return GoolPresets.REVERB_CATHEDRAL
		"REVERB_CAVE":          return GoolPresets.REVERB_CAVE
		"REVERB_BATHROOM_TILE": return GoolPresets.REVERB_BATHROOM_TILE
		"REVERB_OUTDOOR_OPEN":  return GoolPresets.REVERB_OUTDOOR_OPEN
		"REVERB_UNDERWATER":    return GoolPresets.REVERB_UNDERWATER
		_:                      return {}

func _build_material_gallery() -> void:
	# Lay the panels out along the X axis at GALLERY_Z, centered
	# on x=0. Each panel: StaticBody3D + CollisionShape3D +
	# MeshInstance3D + AudioMaterialTag + Label3D.
	var n := MATERIAL_CATALOG.size()
	var step := GALLERY_PANEL_WIDTH + GALLERY_PANEL_GAP
	var x_start := -float(n - 1) * step * 0.5
	for i in range(n):
		var entry: Dictionary = MATERIAL_CATALOG[i]
		var x := x_start + float(i) * step
		_build_material_panel(entry, Vector3(x, GALLERY_PANEL_HEIGHT * 0.5, GALLERY_Z))

	# Section title hovering above the gallery
	var title := Label3D.new()
	title.text = "Material Gallery — shoot a panel to hear its impact character"
	title.position = Vector3(0, GALLERY_PANEL_HEIGHT + 1.5, GALLERY_Z)
	title.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	title.modulate = Color(0.95, 0.95, 1.0)
	title.font_size = 36
	title.outline_size = 6
	add_child(title)

func _build_material_panel(entry: Dictionary, position: Vector3) -> void:
	var body := StaticBody3D.new()
	body.name = "Mat_%s" % entry.name
	body.position = position

	# Mesh
	var mesh_inst := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(GALLERY_PANEL_WIDTH, GALLERY_PANEL_HEIGHT, GALLERY_PANEL_DEPTH)
	mesh_inst.mesh = mesh
	var std_mat := StandardMaterial3D.new()
	std_mat.albedo_color = entry.color
	# Transparency for Glass / Air / Liquid (alpha < 1.0)
	if entry.color.a < 1.0:
		std_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	if entry.has("metallic"):
		std_mat.metallic = entry.metallic
		std_mat.roughness = 0.3
	mesh_inst.material_override = std_mat
	body.add_child(mesh_inst)

	# Collision
	var col := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = Vector3(GALLERY_PANEL_WIDTH, GALLERY_PANEL_HEIGHT, GALLERY_PANEL_DEPTH)
	col.shape = shape
	body.add_child(col)

	# AudioMaterialTag — the v0.43.0 prefab. The tag's @tool _ready
	# sets the gool_audio_material metadata on the parent (this
	# StaticBody3D), which is what the impact-sound lookup paths
	# read at fire time.
	var tag_script := load("res://addons/gool/prefabs/audio_material_tag.gd")
	var tag = tag_script.new()
	tag.name = "AudioMaterialTag"
	tag.material = entry.id
	body.add_child(tag)

	# Label3D above the panel
	var label := Label3D.new()
	label.text = entry.name
	label.position = Vector3(0, GALLERY_PANEL_HEIGHT * 0.5 + 0.6, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.outline_size = 5
	label.font_size = 28
	body.add_child(label)

	add_child(body)

func _build_reverb_rooms() -> void:
	# v0.47.1: hub-and-spokes layout. First builds the central hub
	# (open area + OUTDOOR_OPEN reverb zone), then four enclosed
	# rooms branching off via doorways.
	_build_hub_reverb()
	for room in REVERB_ROOMS:
		_build_enclosed_room(room)

	var title := Label3D.new()
	title.text = "Reverb Audition — walk through doorways to switch presets"
	title.position = Vector3(0, 3.5, HUB_CENTER.z + HUB_SIZE.z * 0.5 - 0.5)
	title.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	title.modulate = Color(0.95, 0.95, 1.0)
	title.font_size = 32
	title.outline_size = 6
	add_child(title)

# Hub: open area with Outdoor Open reverb. No walls, no ceiling —
# the hub is the "ambient world" between the four enclosed rooms.
# Gallery panels live here so material impacts can be auditioned
# against the outdoor reverb baseline.
func _build_hub_reverb() -> void:
	var zone_script := load("res://addons/gool/prefabs/reverb_zone.gd")
	var zone = zone_script.new()
	zone.name = "ReverbZone_OutdoorHub"
	zone.position = HUB_CENTER + Vector3(0, HUB_SIZE.y * 0.5, 0)
	var zone_col := CollisionShape3D.new()
	var zone_shape := BoxShape3D.new()
	zone_shape.size = HUB_SIZE
	zone_col.shape = zone_shape
	zone.add_child(zone_col)
	_apply_preset_to_zone(zone, "REVERB_OUTDOOR_OPEN")
	add_child(zone)

	# Floor-marker patch so the hub is visually distinct from the
	# default gray floor. Just a colored plane on top of the main
	# floor — same trick the v0.43+ tile-row layout used per-room.
	# Sized smaller than the Area3D so it stays a legible "central
	# courtyard" visually even though the reverb zone reaches out
	# to each doorway.
	var floor_marker := MeshInstance3D.new()
	floor_marker.name = "HubFloorMarker"
	floor_marker.position = HUB_CENTER + Vector3(0, 0.02, 0)
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = HUB_FLOOR_VISUAL_SIZE
	floor_marker.mesh = floor_mesh
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.45, 0.55, 0.45)   # outdoor green-gray
	floor_marker.material_override = floor_mat
	add_child(floor_marker)

	var label := Label3D.new()
	label.text = "Outdoor (hub)"
	label.position = HUB_CENTER + Vector3(0, HUB_SIZE.y + 0.5, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.outline_size = 5
	label.font_size = 28
	add_child(label)

# Build one enclosed room: floor patch + 4 walls (one with a
# doorway gap) + ceiling + interior ReverbZone + label.
func _build_enclosed_room(room: Dictionary) -> void:
	var room_root := Node3D.new()
	room_root.name = "Room_%s" % String(room.name).replace(" ", "_")
	room_root.position = room.center
	add_child(room_root)

	var size: Vector3 = room.size
	var half_x: float = size.x * 0.5
	var half_z: float = size.z * 0.5
	var wall_color: Color = room.wall_color

	# Floor patch — colored to identify the room from above.
	var floor_marker := MeshInstance3D.new()
	floor_marker.name = "Floor"
	floor_marker.position = Vector3(0, 0.02, 0)
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(size.x, size.z)
	floor_marker.mesh = floor_mesh
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = room.floor_color
	floor_marker.material_override = floor_mat
	room_root.add_child(floor_marker)

	# Ceiling — full coverage. Important: without a ceiling, sound
	# from inside the room would have direct line-of-sight to the
	# sky and listener occlusion raycasts would mostly miss.
	_build_wall(room_root, "Ceiling",
			Vector3(0, size.y, 0),
			Vector3(size.x, WALL_THICKNESS, size.z),
			wall_color)

	# Four walls — one of them has the doorway gap.
	var wall_dir: String = String(room.doorway_wall)
	# North wall: at z = -half_z (away from hub if room is south,
	# toward hub if room is north). Build wall along the X axis.
	_build_wall_or_doorway(
			room_root, "WallNorth",
			Vector3(0, size.y * 0.5, -half_z),
			Vector3(size.x + WALL_THICKNESS, size.y, WALL_THICKNESS),
			wall_color, wall_dir == "north", true)
	# South wall: at z = +half_z. Build wall along the X axis.
	_build_wall_or_doorway(
			room_root, "WallSouth",
			Vector3(0, size.y * 0.5, half_z),
			Vector3(size.x + WALL_THICKNESS, size.y, WALL_THICKNESS),
			wall_color, wall_dir == "south", true)
	# East wall: at x = +half_x. Build wall along the Z axis.
	_build_wall_or_doorway(
			room_root, "WallEast",
			Vector3(half_x, size.y * 0.5, 0),
			Vector3(WALL_THICKNESS, size.y, size.z),
			wall_color, wall_dir == "east", false)
	# West wall: at x = -half_x. Build wall along the Z axis.
	_build_wall_or_doorway(
			room_root, "WallWest",
			Vector3(-half_x, size.y * 0.5, 0),
			Vector3(WALL_THICKNESS, size.y, size.z),
			wall_color, wall_dir == "west", false)

	# ReverbZone Area3D filling the interior — slightly inset from
	# the walls so the trigger fires when the player is solidly
	# inside, not while pressed against a wall.
	var zone_script := load("res://addons/gool/prefabs/reverb_zone.gd")
	var zone = zone_script.new()
	zone.name = "ReverbZone"
	zone.position = Vector3(0, size.y * 0.5, 0)
	var zone_col := CollisionShape3D.new()
	var zone_shape := BoxShape3D.new()
	zone_shape.size = Vector3(size.x - 0.5, size.y - 0.2, size.z - 0.5)
	zone_col.shape = zone_shape
	zone.add_child(zone_col)
	_apply_preset_to_zone(zone, String(room.preset_key))
	room_root.add_child(zone)

	# Label hovering above the doorway side so the player can read
	# it from inside the hub.
	var label := Label3D.new()
	label.text = "%s\n(%s)" % [room.name, String(room.preset_key).replace("REVERB_", "")]
	label.position = _label_position_for_doorway(wall_dir, size)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.outline_size = 5
	label.font_size = 28
	label.modulate = Color(1.0, 1.0, 1.0)
	room_root.add_child(label)

	# v0.47.1: per-room impact targets. Two material panels inside
	# each room so the player can shoot in-room and hear the same
	# impact with that room's reverb + (when shot from outside the
	# room through the doorway) with wall occlusion.
	_build_room_targets(room_root, room)

# Place 1–N material panels inside a room. Reads room.targets
# (list of material names from MATERIAL_CATALOG). Positions are
# along the X axis at the room's local origin, evenly spaced.
# Panels are smaller than the hub-gallery ones so they fit
# comfortably in the smallest room (5×5 bathroom).
const ROOM_TARGET_SIZE: Vector3 = Vector3(0.8, 1.4, 0.25)
const ROOM_TARGET_SPACING: float = 2.4   # meters between target centers

func _build_room_targets(room_root: Node3D, room: Dictionary) -> void:
	var target_names: Array = room.get("targets", [])
	if target_names.is_empty():
		return
	var n: int = target_names.size()
	var x_start: float = -float(n - 1) * ROOM_TARGET_SPACING * 0.5
	for i in range(n):
		var name_str: String = String(target_names[i])
		var entry: Dictionary = _find_material_entry(name_str)
		if entry.is_empty():
			push_warning("[Audition] room target material '%s' not in MATERIAL_CATALOG" % name_str)
			continue
		var local_pos := Vector3(
				x_start + float(i) * ROOM_TARGET_SPACING,
				ROOM_TARGET_SIZE.y * 0.5,
				0.0)
		_build_room_target(room_root, entry, local_pos)

# Build a single in-room material target. Same shape as
# _build_material_panel but smaller, and parented under the room
# (so its position is local to the room's center).
func _build_room_target(parent: Node3D, entry: Dictionary, local_pos: Vector3) -> void:
	var body := StaticBody3D.new()
	body.name = "Target_%s" % entry.name
	body.position = local_pos
	parent.add_child(body)

	var mesh_inst := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = ROOM_TARGET_SIZE
	mesh_inst.mesh = mesh
	var std_mat := StandardMaterial3D.new()
	std_mat.albedo_color = entry.color
	if entry.color.a < 1.0:
		std_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	if entry.has("metallic"):
		std_mat.metallic = entry.metallic
		std_mat.roughness = 0.3
	mesh_inst.material_override = std_mat
	body.add_child(mesh_inst)

	var col := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = ROOM_TARGET_SIZE
	col.shape = shape
	body.add_child(col)

	# AudioMaterialTag — same prefab the gallery panels use. Sets
	# the gool_audio_material metadata on the parent StaticBody3D
	# at _ready so impact-sound lookup at fire time picks up the
	# material id and routes to the right per-material EQ + impact
	# sound bank entry.
	var tag_script := load("res://addons/gool/prefabs/audio_material_tag.gd")
	var tag = tag_script.new()
	tag.name = "AudioMaterialTag"
	tag.material = entry.id
	body.add_child(tag)

	# Small label above the target.
	var label := Label3D.new()
	label.text = entry.name
	label.position = Vector3(0, ROOM_TARGET_SIZE.y * 0.5 + 0.3, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.outline_size = 4
	label.font_size = 22
	body.add_child(label)

# Linear search through MATERIAL_CATALOG by name. Called once per
# target during scene build, so the O(N×M) cost is negligible
# (M=2-3 targets per room, N=13 catalog entries).
func _find_material_entry(material_name: String) -> Dictionary:
	for entry in MATERIAL_CATALOG:
		if entry.name == material_name:
			return entry
	return {}

# Build either a solid wall or a wall-with-doorway depending on
# the has_doorway flag. The horizontal flag tells us whether the
# wall runs along X (north/south walls) or Z (east/west walls).
func _build_wall_or_doorway(parent: Node3D, name_prefix: String,
		center: Vector3, full_size: Vector3, color: Color,
		has_doorway: bool, horizontal: bool) -> void:
	if not has_doorway:
		_build_wall(parent, name_prefix, center, full_size, color)
		return
	# Split the wall into three pieces around a doorway hole at the
	# wall's midpoint along its length axis. Pieces:
	#   - "_Left"   : from one end up to the doorway opening
	#   - "_Right"  : from doorway opening to the other end
	#   - "_Header" : above the doorway, full doorway width × (ceiling - doorway_height)
	# 'horizontal' picks whether 'length' is X (true) or Z (false).
	var length: float = full_size.x if horizontal else full_size.z
	var thickness: float = full_size.z if horizontal else full_size.x
	var height: float = full_size.y
	var side_length: float = (length - DOORWAY_WIDTH) * 0.5
	var header_height: float = max(0.0, height - DOORWAY_HEIGHT)

	# Left segment — at length-axis position = -(DOORWAY_WIDTH*0.5 + side_length*0.5)
	var left_pos := -DOORWAY_WIDTH * 0.5 - side_length * 0.5
	var right_pos := DOORWAY_WIDTH * 0.5 + side_length * 0.5
	if horizontal:
		# Wall along X, length-axis = X. Left/right are -X/+X side.
		_build_wall(parent, name_prefix + "_Left",
				Vector3(center.x + left_pos, center.y, center.z),
				Vector3(side_length, height, thickness), color)
		_build_wall(parent, name_prefix + "_Right",
				Vector3(center.x + right_pos, center.y, center.z),
				Vector3(side_length, height, thickness), color)
		if header_height > 0.0:
			_build_wall(parent, name_prefix + "_Header",
					Vector3(center.x,
							DOORWAY_HEIGHT + header_height * 0.5,
							center.z),
					Vector3(DOORWAY_WIDTH, header_height, thickness),
					color)
	else:
		# Wall along Z, length-axis = Z. Left/right are -Z/+Z side.
		_build_wall(parent, name_prefix + "_Left",
				Vector3(center.x, center.y, center.z + left_pos),
				Vector3(thickness, height, side_length), color)
		_build_wall(parent, name_prefix + "_Right",
				Vector3(center.x, center.y, center.z + right_pos),
				Vector3(thickness, height, side_length), color)
		if header_height > 0.0:
			_build_wall(parent, name_prefix + "_Header",
					Vector3(center.x,
							DOORWAY_HEIGHT + header_height * 0.5,
							center.z),
					Vector3(thickness, header_height, DOORWAY_WIDTH),
					color)

# Solid wall = StaticBody3D + MeshInstance3D + CollisionShape3D.
# StaticBody3D is what gool's occlusion raycasts hit when the
# listener and emitter are on opposite sides.
func _build_wall(parent: Node3D, wall_name: String,
		position: Vector3, size: Vector3, color: Color) -> void:
	var body := StaticBody3D.new()
	body.name = wall_name
	body.position = position
	parent.add_child(body)

	var mesh_inst := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = size
	mesh_inst.mesh = box
	var mat := StandardMaterial3D.new()
	mat.albedo_color = color
	mesh_inst.material_override = mat
	body.add_child(mesh_inst)

	var col := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = size
	col.shape = shape
	body.add_child(col)

# Compute a label position just outside the doorway, hovering
# above the room so the player reads it from inside the hub.
func _label_position_for_doorway(wall_dir: String, size: Vector3) -> Vector3:
	var y: float = size.y + 0.6
	match wall_dir:
		"north": return Vector3(0, y, -size.z * 0.5 - 0.5)
		"south": return Vector3(0, y,  size.z * 0.5 + 0.5)
		"east":  return Vector3( size.x * 0.5 + 0.5, y, 0)
		"west":  return Vector3(-size.x * 0.5 - 0.5, y, 0)
		_:       return Vector3(0, y, 0)

# Pull preset values from GoolPresets and apply to a ReverbZone.
# Centralized so the hub + each enclosed room use identical
# preset-application logic. v0.46.1 added predelay_ms, v0.47.0
# added send_hpf_hz + return_lpf_hz.
func _apply_preset_to_zone(zone: Node, preset_key: String) -> void:
	var preset: Dictionary = _lookup_reverb_preset(preset_key)
	if preset.is_empty():
		push_warning("[Audition] preset key '%s' not found on GoolPresets"
				% preset_key)
		return
	zone.decay       = float(preset.get("decay",       zone.decay))
	zone.lf_damping  = float(preset.get("lf_damping",  zone.lf_damping))
	zone.hf_damping  = float(preset.get("hf_damping",  zone.hf_damping))
	zone.diffusion   = float(preset.get("diffusion",   zone.diffusion))
	zone.wet_gain_db = float(preset.get("wet_gain_db", zone.wet_gain_db))
	if "predelay_ms" in zone:
		zone.predelay_ms = float(preset.get("predelay_ms", 30.0))
	if "send_hpf_hz" in zone:
		zone.send_hpf_hz = float(preset.get("send_hpf_hz", 0.0))
	if "return_lpf_hz" in zone:
		zone.return_lpf_hz = float(preset.get("return_lpf_hz", 22000.0))

func _register_test_bark() -> void:
	# Procedural "bark" — a 400 Hz tone pulse with a quick decay,
	# clearly distinguishable from the gunshot's noise burst.
	# Registered on the Dialogue bus so it triggers the sidechain
	# compressors on Music + Sfx (the v0.43.0 config.json update).
	if not Gool.is_initialized():
		push_warning("[Audition] Gool not initialized; bark not registered")
		return

	const DURATION_S: float = 1.0
	const SR: int = 48000
	var n := int(DURATION_S * SR)
	var samples := PackedFloat32Array()
	samples.resize(n)
	var freq := 400.0
	var two_pi_freq_dt := 2.0 * PI * freq / float(SR)
	for i in range(n):
		var t := float(i) / float(SR)
		var env := exp(-3.0 * t)  # exponential decay
		samples[i] = sin(two_pi_freq_dt * float(i)) * env * 0.6

	Gool.register_pcm_sound("test_bark", samples, SR, 1)
	# v0.25.x sound definition step: tells the engine spatialization
	# rules, distance falloff, category routing, and target bus. We
	# explicitly set target_bus_name="Dialogue" so the bark routes
	# to the Dialogue bus regardless of whether category_routing is
	# configured — more robust for an audition that's exercising
	# the dialogue sidechain compressors specifically.
	if Gool.has_method("register_sound_definition"):
		Gool.register_sound_definition(
			"test_bark",
			true,                       # spatialized — plays at the speaker's 3D position
			false,                      # looping — one-shot
			1.0,                        # min_distance (defaults)
			50.0,                       # max_distance
			0.0,                        # loop_crossfade_ms
			Gool.CATEGORY_DIALOGUE,     # category — for telemetry/staleness rules
			"Dialogue",                 # target_bus_name — explicit override of category routing
			false)                      # occlusion_enabled — keep the bark crisp; we're auditioning the duck, not occlusion

func _add_debug_overlay() -> void:
	# GoolDebugOverlay is a CanvasLayer prefab — adding it makes
	# F3 in-game toggle the overlay showing live engine stats.
	var overlay_script := load("res://addons/gool/prefabs/gool_debug_overlay.gd")
	var overlay = overlay_script.new()
	overlay.name = "GoolDebugOverlay"
	add_child(overlay)

func _add_usage_billboard() -> void:
	# Floating instructions for the audition. Placed back near the
	# spawn point so the player sees it on first frame.
	var label := Label3D.new()
	label.text = ("AUDITION — gool v0.47.0\n\n"
			+ "You're in the HUB (outdoor reverb).\n"
			+ "Four rooms branch off: Cathedral (N),\n"
			+ "Cave (E), Bathroom (W), Small Room (S).\n\n"
			+ "WASD + Mouse: move + look\n"
			+ "LMB: shoot (hear material impact + room reverb)\n"
			+ "B: DialogueDirector bark (ducks Music + Sfx)\n"
			+ "K / L: capture / apply mix snapshot\n"
			+ "F3: toggle debug overlay\n\n"
			+ "Listen for: reverb tail changes across doorways,\n"
			+ "occlusion when shooting from outside a room,\n"
			+ "predelay differences between rooms.")
	label.position = Vector3(0, 3.5, 8.0)   # in the hub, behind the player so it doesn't block sightlines forward
	label.position = Vector3(0, 3.5, 4.0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.font_size = 24
	label.outline_size = 5
	label.modulate = Color(1.0, 0.95, 0.7)
	add_child(label)

# ─── Input handlers ────────────────────────────────────────────

func _on_bark_key_pressed() -> void:
	if DialogueDirector == null:
		push_warning("[Audition] DialogueDirector autoload not present")
		return
	var player := get_node_or_null("FpsPlayer") as Node3D
	var pos: Vector3 = player.global_position if player != null else Vector3.ZERO
	# Audition uses a single speaker_id so re-pressing B from a
	# single instance interrupts the previous bark (showing the
	# same-speaker policy in action).
	var ok: bool = DialogueDirector.bark("audition_speaker", "test_bark",
			200, 1.0, pos)
	print("[Audition] DialogueDirector.bark() → %s" % ok)

func _on_snapshot_capture() -> void:
	if Gool == null or not Gool.is_initialized():
		push_warning("[Audition] Gool not initialized; snapshot not captured")
		return
	# Capture the buses we have effects on — Music + Sfx — so
	# tweaking those effects' parameters via the mixer dock and
	# then pressing L restores them.
	var buses := PackedStringArray(["Music", "Sfx"])
	_held_snapshot = Gool.capture_mix_snapshot(buses)
	if _held_snapshot != null:
		_held_snapshot.label = "audition_capture"
		print("[Audition] Snapshot captured (%d buses)"
				% _held_snapshot.bus_states.size())
	else:
		push_warning("[Audition] capture_mix_snapshot returned null")

func _on_snapshot_apply() -> void:
	if _held_snapshot == null:
		print("[Audition] No held snapshot — press K to capture first")
		return
	var ok: bool = Gool.apply_mix_snapshot(_held_snapshot)
	print("[Audition] Snapshot applied → %s" % ok)
