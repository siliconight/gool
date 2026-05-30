# gool 4P Coop Validation Example

A minimal Godot project that validates gool's full audio stack
end-to-end without requiring any final audio assets, art, or
gameplay tuning. Once installed, you open it, hit play, and see
PASS/FAIL within 2 seconds.

This is a **kick-the-tires tool** for developers building 4-player
FPS PvE games on gool. It tells you whether the engine works as
you need it to *before* you commit to building your real game on
top of it.

## Prerequisites

- **Godot 4.2 or newer**
- **The gool GDExtension binary** installed under
  `examples/coop_4p_minimal/addons/gool/bin/` for your platform.
  This example ships only the GDScript wrappers — the compiled
  C++ engine they call into has to be present separately.

If the binary isn't there, the project still opens in Godot but
hitting play produces `GoolAudioRuntime class not registered`
errors in the console (the GDExtension failed to load because
its shared library isn't where Godot expects). The validation
banner will read **✗ VALIDATION FAILED** and the status label
will name the missing autoload.

There are two ways to get the binary into place.

### Track A — Prebuilt (recommended)

If you just want to validate gool, use the prebuilt addon from
the latest GitHub release. The one-line installer drops a
working `addons/gool/` (including the binary for your platform)
straight into a Godot project:

**Windows (PowerShell):**

```powershell
cd path\to\gool\examples\coop_4p_minimal
iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1 | iex
```

**Linux / macOS:**

```bash
cd path/to/gool/examples/coop_4p_minimal
curl -sSL https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.sh | bash
```

The script overwrites the example's bundled `addons/gool/` with
the latest released addon archive — same GDScript wrappers, plus
the binary. After it finishes, open the project in Godot.

Track A works on Linux x86_64, Windows x86_64, and macOS arm64.
See [`../../SETUP.md`](../../SETUP.md) for the full install
documentation including troubleshooting.

### Track B — Build from source

Required if you're modifying gool, working on a platform Track A
doesn't cover, or contributing patches. See
[`../../SETUP.md`](../../SETUP.md) for full build instructions —
short version:

```bash
# From the repo root:
cmake -B build -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON
cmake --build build --config Release

# Then copy the produced binary into the example:
cp build/godot/gool_godot.<platform>.<arch>.dll \
   examples/coop_4p_minimal/addons/gool/bin/
```

The exact binary path varies by platform and CMake generator;
`SETUP.md` has the table.

## What you'll see when validation passes

1. A small 3D scene loads — gray ground plane, four colored test-
   marker cubes around the player at cardinal directions, one
   translucent cyan cube to the northeast (a `ReverbZone`).

2. **Within 2 seconds**, a banner appears at the top:
   - ✓ **VALIDATION PASSED** (green) — gool is working end-to-end
   - ✗ **VALIDATION FAILED** (red) — see the status label below
     the diagnostic overlay for the specific failure

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
| Per-category dispatch | Each of WEAPONS, ENEMIES, WORLD, MOVEMENT, HUD has `fired >= 1` |

If all five categories show `fired >= 1` in the overlay and the
listener is attached, validation passes. The audio system is
fundamentally working — you can move on to building your real
game on top of it.

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
- **Walk into the cyan cube** — entering the `ReverbZone` changes
  the wet-bus send so subsequent sounds get spatial reverb. Walk
  out, reverb tail decays.

## Multiplayer testing (manual, multi-instance)

The scene includes Host and Join buttons in the top-right. To
exercise RPC event replication:

1. Launch a **second instance** of this Godot project. In the
   Godot editor: Debug → Run Multiple Instances → 2 instances.
2. In one window, click **Host (port 7777)**.
3. In the other window, click **Join localhost:7777**.
4. Either window can fire events. Watch the diagnostic overlay
   in **both** windows: when peer A fires `play_weapon`, peer B's
   `Weapons recv` count should increment within ~1 second.

`fired` vs `recv` is the key signal:

- Both should be matched within a network RTT of fire
- `fired` going up but `recv` flat on the receiver → multiplayer
  wiring is broken (check the Godot console for ENet errors)
- Counts match but nothing audible → the sound bank wasn't loaded
  on that peer

## What the scene does NOT validate

Things you'll need to test separately once you have a real game:

- **Production multiplayer wiring** — matchmaking, NAT punching,
  authority transfer, cheat protection. This example uses ENet
  on localhost.
- **Real audio quality** — placeholder sounds are procedurally-
  generated tones; audible and category-distinguishable but
  obviously not shippable. Swap in your real `.wav`/`.ogg`/`.opus`
  assets via a `GoolSoundBank` resource.
- **Performance under load** — five categories firing once each
  is not a stress test. The main repo's `tests/bench/` targets
  cover that.
- **Voice chat** — separate prefab (`VoiceChatPlayer`), demonstrated
  in `examples/voice_chat/`.

## Swapping in real assets

When you're ready to move beyond placeholders:

1. Create a `GoolSoundBank` resource in Godot (right-click in
   the FileSystem panel → New Resource → `GoolSoundBank`).
2. Register your real sounds in the bank (one entry per sound,
   each pointing at a .wav/.ogg/.opus file).
3. Replace `_register_procedural_sounds()` in `main.gd` with
   assigning your bank to `FPSCoopAudio.sound_bank`. The
   `GoolSoundBankLoader` child will register the bank's entries
   automatically.
4. Call `play_weapon("your_sound_name", pos)` etc. with your real
   sound names instead of the placeholders.

The semantic API doesn't change — only the underlying sound
files do.

## Troubleshooting

**Banner shows ✗ VALIDATION FAILED, status: `/root/Gool autoload
missing`** — the gool plugin isn't enabled. Open Project Settings
→ Plugins, find "gool" in the list, tick the Enable checkbox.
If gool isn't in the list at all, the addon directory is
incomplete — re-run the quickinstall script.

**Console shows `GoolAudioRuntime class not registered`** — the
GDExtension binary isn't loading. Most common causes: missing
`addons/gool/bin/<your-platform>.dll` (or `.so`/`.dylib`),
mismatched Godot version (you need 4.2+), or the binary was
built for a different architecture than Godot is running as.

**Banner shows ✗ with `listener not attached`** — should be
impossible in this example since `_bind_listener()` runs
deferred. If you see it, check the console for warnings about
`FPSCoopAudio` failing to load.

**Banner shows ✗ with `weapons: expected fired >= 1, got 0`** —
the `play_weapon()` call failed silently. Most likely the
`NetworkedAudioEvent` child wasn't created (look for "Couldn't
load NetworkedAudioEvent script" in the console — the addon
files aren't where the script expects them).

**No sound at all but validation passes** — first check the
diagnostic overlay. If `fired` counts are going up but you hear
nothing: open your OS audio mixer, make sure Godot isn't muted,
check that the validation banner says PASSED. Silent-but-passing
is rare but possible.

## File layout

```
coop_4p_minimal/
├── project.godot           # Godot project file
├── main.tscn               # Single entry scene (just a Node + main.gd)
├── main.gd                 # Builds the entire scene at runtime
├── icon.svg                # Project icon
├── README.md               # This file
└── addons/gool/            # Subset of the canonical addon
    ├── plugin.{cfg,gd}
    ├── runtime_singleton.gd
    ├── audio_relevancy_filter.gd
    ├── multiplayer_bridge.gd
    ├── prefabs/
    │   ├── fps_coop_audio.{gd,svg}
    │   ├── networked_audio_event.{gd,svg}
    │   ├── gool_listener_3d.{gd,svg}
    │   ├── gool_sound_bank_loader.{gd,svg}
    │   └── reverb_zone.{gd,svg}
    └── resources/
        └── gool_sound_bank.gd
```

The example ships only the GDScript wrappers. The compiled
GDExtension binary lives at `addons/gool/bin/<platform>/` and is
populated by either the quickinstall script (Track A) or your
own `cmake --build` (Track B). See `../../SETUP.md` for the
canonical install documentation.

Everything that builds the scene lives in **one file**, `main.gd`.
No .tscn surgery, no spawning glue between scripts. Read it
end-to-end in five minutes.
