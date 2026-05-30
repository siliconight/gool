# gool 4P Coop Validation Example

A minimal Godot project that validates gool's full audio stack
end-to-end without requiring any final audio assets, art, or
gameplay tuning. Open it, hit play, see PASS/FAIL within 2 seconds.

This is a **kick-the-tires tool** for developers building 4-player
FPS PvE games on gool. It tells you whether the engine works as
you need it to *before* you commit to building your real game on
top of it. If everything passes here, you're solid; if something
fails, the error message points at what to fix.

## What you'll see when you press play

1. A small 3D scene loads — gray ground plane, four colored test-
   marker cubes around the player at cardinal directions, one
   translucent cyan cube to the northeast (a ReverbZone).

2. **Within 2 seconds**, a banner appears at the top:
   - ✓ **VALIDATION PASSED** (green) — gool is working end-to-end
   - ✗ **VALIDATION FAILED** (red) — see the status label for the
     specific failure (most common: gool plugin not enabled)

3. The FPSCoopAudio diagnostic overlay shows in the top-left
   corner: per-category event counts, listener state, sound bank
   state.

After auto-validation completes, the scene drops into **manual
mode** where you can play with the audio system.

## What auto-validation actually checks

| Check | Pass criterion |
|---|---|
| Engine boot | `/root/Gool` autoload resolved |
| Sound bank registration | 5 procedural sounds registered via `Gool.register_pcm_sound()` |
| FPSCoopAudio instantiation | Prefab `_ready()` completes without errors |
| Listener attachment | `set_local_player()` succeeded; diagnostic shows `listener: ✓ Player` |
| Per-category dispatch | Each of WEAPONS, ENEMIES, WORLD, MOVEMENT, HUD has `fired >= 1` in the diagnostic |

If all 5 categories show `fired >= 1` in the overlay and the
listener is attached, validation passes. The audio system is
fundamentally working.

## Manual mode

After validation, you can interact with the scene:

- **WASD / arrow keys** — move the player capsule. The camera and
  audio listener follow. Sounds fired at fixed positions change
  apparent direction as you turn (left ear vs. right ear).
- **Buttons at the bottom of the screen** or **number keys 1-5** —
  fire each of the five FPSCoopAudio categories at a random test
  marker.
  - `1` weapon fire   — client-predicted, 100m radius
  - `2` enemy growl   — server-authoritative, 80m radius
  - `3` world boom    — server-authoritative, 200m radius
  - `4` footstep      — client-authoritative, 30m radius
  - `5` hud beep      — local only, no replication
- **Walk into the cyan cube** — entering the ReverbZone changes
  the wet-bus send so subsequent sounds get spatial reverb. Walk
  out, reverb tail decays.

## Multiplayer testing (manual, multi-instance)

The scene includes Host and Join buttons in the top-right. To
exercise RPC event replication:

1. Launch a **second instance** of this Godot project. In the
   Godot editor: Project → Run Project (it'll open another window).
2. In one window, click **Host (port 7777)**.
3. In the other window, click **Join localhost:7777**.
4. Either window can fire events. Watch the diagnostic overlay
   in *both* windows: when peer A fires `play_weapon`, peer B's
   `Weapons recv` count should increment.

`fired` vs `recv` is the key signal:
- Both should be matched within ~1 second of fire (network RTT)
- If `fired` goes up on the sender but `recv` stays flat on the
  receiver → multiplayer wiring is broken
- If counts match but you don't hear anything → the sound bank
  wasn't loaded on that peer (re-check registration)

## What the scene does NOT validate

Things you'll need to test separately once you have a real game:

- **Production multiplayer wiring** — matchmaking, NAT punching,
  authority transfer, cheat protection. This example uses ENet
  on localhost, which is the simplest possible case.
- **Real audio quality** — placeholder sounds are procedurally
  generated tones; they're audible and category-distinguishable
  but obviously not what you'd ship. Swap in your real `.wav`/
  `.ogg`/`.opus` assets via a GoolSoundBank resource.
- **Performance under load** — five categories firing 1× each is
  not a stress test. Use the bench targets in the main gool repo
  (`tests/bench/`) for that.
- **Voice chat** — separate prefab (`VoiceChatPlayer`). Wire it
  per player when you're ready for voice.

## Swapping in real assets

When you're ready to move beyond placeholders:

1. **Create a `GoolSoundBank` resource** in Godot (right-click in
   the FileSystem panel → New Resource → GoolSoundBank).
2. **Register your real sounds** in the bank (one entry per
   sound, each pointing at a .wav/.ogg/.opus file).
3. **Replace `_register_procedural_sounds()` in `main.gd`** with
   assigning your bank to the FPSCoopAudio's `sound_bank` export.
   The `GoolSoundBankLoader` child will register the bank's
   entries automatically.
4. **Call `play_weapon("your_sound_name", pos)`** with your real
   sound names instead of the placeholders.

The semantic API doesn't change — only the underlying sound files do.

## Troubleshooting

**"FAILED — /root/Gool autoload missing"** — open Project Settings
→ Plugins, find "gool" in the list, tick the Enable checkbox.

**"FAILED — listener not attached"** — should be impossible in this
example since `_bind_listener()` runs deferred. If you see this,
check the Godot console for warnings about `FPSCoopAudio` failing
to load.

**"FAILED — weapons: expected fired >= 1, got 0"** — the
`play_weapon()` call failed silently. Most likely the
NetworkedAudioEvent child wasn't created (check console for the
"Couldn't load NetworkedAudioEvent script" error — means the addon
files aren't where the script expects them).

**No sound at all** — first check the diagnostic overlay. If
`fired` counts are going up but you hear nothing: open your OS
audio mixer, make sure Godot isn't muted, check that the validation
passed (silent failure is rare but possible). If `fired` counts
aren't going up: the buttons aren't reaching FPSCoopAudio — most
likely a scene-load issue.

## File layout

```
coop_4p_minimal/
├── project.godot           # Godot project file (autoload + plugin enabled)
├── main.tscn               # Single entry scene (just a Node with main.gd)
├── main.gd                 # Builds the entire scene at runtime
├── icon.svg                # Project icon
├── README.md               # This file
└── addons/gool/            # Subset of the canonical addon needed for this example
    ├── plugin.cfg
    ├── plugin.gd
    ├── runtime_singleton.gd
    ├── audio_relevancy_filter.gd
    ├── multiplayer_bridge.gd
    ├── prefabs/
    │   ├── fps_coop_audio.gd
    │   ├── networked_audio_event.gd
    │   ├── gool_listener_3d.gd
    │   ├── gool_sound_bank_loader.gd
    │   └── reverb_zone.gd
    └── resources/
        └── gool_sound_bank.gd
```

Everything that builds the scene lives in **one file**, `main.gd`.
No .tscn surgery, no spawning glue between scripts, no per-component
configuration. Read it end-to-end in five minutes.
