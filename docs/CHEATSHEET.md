# gool cheatsheet

The 10 most common things you'll do with gool, as paste-able
code snippets. Keep this page open while you're building.

For each operation, the snippet assumes:

- gool is installed (`addons/gool/` is in your project, plugin
  enabled — see [godot_quickstart.md](godot_quickstart.md))
- The `Gool` autoload is registered (the plugin does this for you)
- A `gool/config.json` exists at the project root, defining at
  least a Master bus and a Sfx bus (the FPS template covers this)

## 1. Play a sound at a position

The "I just want to make a noise" call. Fire and forget — the sound
auto-frees when it finishes.

```gdscript
Gool.register_sound_from_file("blip", "res://sfx/blip.ogg")
Gool.play_one_shot("blip", Vector3(5, 0, 0))
```

`register_sound_from_file` is idempotent — calling it twice with
the same name is fine, it just won't reload. Do registrations
once at startup (e.g. in your audio_setup autoload's `_ready`).

`play_one_shot` is a v0.71.0 convenience wrapper around the more
general `create_emitter`. Use `create_emitter` when you need
looping, fade-in, or to keep the handle for later destruction.

## 2. Play a looping sound and stop it later

The third arg is `looping`. Keep the handle to destroy later.

```gdscript
var handle: int = Gool.create_emitter("engine_hum", Vector3.ZERO, true)
# ...sometime later, e.g. when the engine turns off:
Gool.destroy_emitter(handle, 0.3)   # 0.3 = fade-out in seconds
```

If you destroy a handle that's already dead, it's a no-op — safe.

## 3. Move a sound around in 3D

For positioned sounds that follow a moving object, update each
frame (or whenever the position changes).

```gdscript
func _process(_delta: float) -> void:
    Gool.set_emitter_transform(_engine_handle, $Vehicle.global_position)
```

## 4. Set a bus volume from script

For "mute music when in a menu," "duck SFX during a cutscene,"
"slider in your settings UI."

```gdscript
Gool.set_bus_gain_db("Music", -6.0)   # quieter by 6 dB
Gool.set_bus_gain_db("Music",  0.0)   # back to baseline
```

## 5. Add a reverb zone to a scene

Drag the `ReverbZone` prefab into your scene (it's a node — search
"ReverbZone" in **Add Child Node**). Set its `Area3D` shape to
cover the room. In the inspector, pick a preset from the dropdown
(`SMALL_ROOM`, `CATHEDRAL`, `CAVE`, etc.) or define custom params.

When your `Listener3D` enters that Area3D, the bus the zone
targets gets the reverb preset applied. When it leaves, the
preset blends back to the previous zone.

No script needed — it's a drag-and-drop node.

## 6. Position the listener (a.k.a. "ears")

Add a `Listener3D` node as a child of your camera (or wherever
"the player's ears" are in your scene). That's it. Gool's spatial
math uses its world transform automatically.

```
Camera3D
└── Listener3D      ← drag in from Add Child Node
```

No script. If you have multiple Listener3D nodes, the highest-
priority one (set in the inspector) wins.

## 7. Crossfade between music tracks

Use the `MusicStateController` prefab. Add it to your scene,
populate the `states` array in the inspector with `state_name`
→ sound mappings, then call:

```gdscript
$MusicStateController.transition_to("combat", 2.0)  # 2-second crossfade
```

The controller handles the dual-emitter, gain-ramp, hand-off
mechanics so your gameplay code only thinks about "what mood am I
in right now."

## 8. React to a sound finishing

```gdscript
var handle: int = Gool.create_emitter("vo_line_42", Vector3.ZERO)
await Gool.emitter_finished
# (use the autoload's signal — it fires for every emitter that
#  finishes; check `arguments[0] == handle` if you care which)
```

For dialogue specifically, the `DialogueDirector` autoload gives
you `current_line_finished` + a priority queue — usually nicer
than wiring this manually.

## 9. Check if a sound exists before you try to play it

Defensive code that prevents the "silent failure" trap. New in
v0.66.0.

```gdscript
if not Gool.has_sound("blip"):
    push_warning("Sound 'blip' not registered — did you forget audio_setup?")
    return
Gool.create_emitter("blip", Vector3.ZERO)
```

For larger projects, hold this guard at your audio-event entry
points so a missing-asset bug becomes a visible warning instead
of "huh, why didn't the gun fire sound play?"

## 10. Dump a session log for debugging

If something audio-related is wrong and you want to send the dev
a "what was the engine doing right before this happened" snapshot,
press **Ctrl+Shift+G** at any point during play. A JSONL file
appears in your `user://` directory and your OS file manager
opens with it highlighted.

From script:

```gdscript
var path: String = Gool.dump_session_log()
print("session log: ", path)
```

Default ring buffer is 4096 entries; configurable via
**Project Settings → Addons → gool → logging → session_buffer_size**.

---

## Common gotchas

**"My sound doesn't play."**

1. Did you call `register_sound_from_file` before `create_emitter`?
   `create_emitter` with an unknown name silently no-ops (v0.66.0+
   warns, but only if you wired the warning).
2. Is the Sfx bus muted or has its `gain_db` set to a very low
   value? Open the gool mixer dock (**Project → Tools → gool
   Mixer**) and look at bus levels.
3. Is there a `Listener3D` somewhere in your scene tree? Without
   one, gool has nowhere to render spatial audio.

**"Sound plays but feels really loud / quiet."**

Check the Master bus volume in the mixer dock first. Then the bus
that sound routes through (most commonly Sfx). Then the per-emitter
volume if you passed one.

**"How do I know what bus my sound is going to?"**

The sound's bus is set in `register_sound_from_file` (default is
the bus named in your config's `category_routing`). To override
per-call, use `Gool.create_emitter_on_bus(name, position, bus_name)`.

**"My ReverbZone doesn't seem to do anything."**

The most common cause: the `bus_name` property on the ReverbZone
points at a bus that doesn't exist in your `gool/config.json`, or
points at a bus that has no `reverb` effect in its chain. Open
the mixer dock and confirm the named bus has a `reverb` effect to
modulate.

## Where to go from here

- [`cookbook.md`](cookbook.md) — recipes longer than the snippets above
- [`asset_pipeline.md`](asset_pipeline.md) — how to register large
  sound banks at startup
- [`terminology.md`](terminology.md) — what these words mean
- [`../examples/README.md`](../examples/README.md) — runnable demos
