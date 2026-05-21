# addons/gool/resources/gool_audio_material.gd
#
# Designer-authored AudioMaterial tag for a piece of level geometry.
# Saved as a .tres so it's diffable, reusable across multiple nodes,
# and re-skinnable (one resource per surface kind in your game,
# referenced from every collider that uses that surface).
#
# Usage:
#   1. In the FileSystem dock, right-click → New Resource →
#      GoolAudioMaterial. Save it (e.g.
#      res://materials/audio/concrete.tres).
#   2. Set `material` to one of the Gool.MATERIAL_* constants
#      (Default, Air, Glass, Wood, Drywall, Concrete, Metal,
#      Curtain, Foliage, Meat). The inspector shows the value as an
#      int but you can match it against the constants in code.
#   3. In your level scene, select a CollisionObject3D / Area3D /
#      StaticBody3D representing a surface. In its inspector,
#      Add Metadata → name `gool_audio_material`, value: drag the
#      resource you just saved into the field.
#   4. At runtime, `Gool.material_from_collider(node)` reads back
#      the material int, and `Gool.play_impact_sound(name, pos,
#      material)` picks the right variant from your sound bank's
#      by_material group.
#
# Reusability:
#   Save one .tres per material (concrete.tres, wood.tres, etc.)
#   and reference the same resource from every collider that uses
#   it. Changing the resource later updates every collider that
#   points at it — no per-collider edits.
#
# Why a Resource instead of just an int metadata?
#   - Discoverable: shows up in the FileSystem dock as a recognized
#     type with the GoolAudioMaterial label.
#   - Reusable: one resource shared across many nodes, vs. a free
#     int metadata field per node that's easy to typo.
#   - Inspectable: the inspector can render a friendly picker once
#     editor tooling for it lands in a future release.
#
# Both paths (int metadata, resource metadata, or
# audio_material:Concrete group) are accepted by
# Gool.material_from_collider so designers can mix and match per
# project preferences.

@tool
class_name GoolAudioMaterial
extends Resource

## The AudioMaterial value. Use one of the Gool.MATERIAL_*
## constants (Default=0, Air=1, Glass=2, Wood=3, Drywall=4,
## Concrete=5, Metal=6, Curtain=7, Foliage=8, Meat=9). Out-of-range
## values are treated as MATERIAL_DEFAULT at lookup time.
@export var material: int = 0
