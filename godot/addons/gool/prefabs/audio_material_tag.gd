# addons/gool/prefabs/audio_material_tag.gd
#
## Drop-in Node that tags its parent with an AudioMaterial value
## via the `gool_audio_material` metadata key. The metadata key is
## what gool's geometry-query and impact-sound code paths look up
## when picking per-material reverb / EQ / occlusion behavior.
#
## Why this prefab exists:
## Without it, telling gool "this StaticBody3D is Concrete" requires
## either authoring a GoolAudioMaterial resource (.tres file) and
## assigning it, OR setting the metadata key by hand in the inspector
## (which most Godot devs don't know about). This prefab gives a
## third path: drop AudioMaterialTag as a child node, pick "Concrete"
## from the dropdown, done. No .tres files, no metadata-key knowledge,
## no code.
#
## Usage:
##   StaticBody3D (your wall)
##   └─ CollisionShape3D
##   └─ AudioMaterialTag (material = "Concrete")
#
## At _ready() (in both editor and runtime, since this is a @tool
## script), the tag pushes its `material` integer to the parent's
## `gool_audio_material` metadata. Changing the dropdown in the
## inspector updates the metadata immediately — no rerun needed.
#
## If you delete the AudioMaterialTag node, the metadata key
## remains on the parent (Godot doesn't reliably notify @tool
## scripts on editor-delete). Clear it manually via the Inspector's
## Metadata section if you care; it's harmless to leave because
## the lookup paths gracefully default to MATERIAL_DEFAULT for
## unknown values.

@tool
class_name AudioMaterialTag
extends Node

## The acoustic material of the parent collision body. Picked
## from the same set as Gool.MATERIAL_* constants. Default = 0
## (Default — no special material behavior).
@export_enum("Default", "Air", "Glass", "Wood", "Drywall",
		"Concrete", "Metal", "Curtain", "Foliage", "Meat",
		"Cardboard", "Rubber", "Liquid")
var material: int = 0 :
	set(value):
		material = value
		_push_to_parent()

func _ready() -> void:
	_push_to_parent()

# Push the material value to the parent's gool_audio_material
# metadata key. Safe to call at any time — handles missing parent
# (e.g. during scene-tree teardown) by silently no-op'ing.
func _push_to_parent() -> void:
	var p := get_parent()
	if p == null:
		return
	p.set_meta("gool_audio_material", material)
