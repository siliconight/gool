# Gool Multiplayer Audio Sandbox

A minimal Godot project for validating gool's networked audio chain
end-to-end with two clients. Built incrementally over a series of
small sessions, each producing something testable.

## Status

| Session | Goal | State |
|---|---|---|
| **1** | Project scaffolding + ENet host/join, see placeholder cubes per peer | **active** |
| 2 | CharacterBody3D player + transform sync, players walk around | planned |
| 3 | Networked gunshot SFX via `Gool.play_networked()` — the actual audio test | planned |
| 4 | Voice chat + multi-emitter stress (deferred to release 3 milestone) | deferred |

## What this validates

This rig is purpose-built to exercise gool's networked audio under
realistic-ish conditions. It uses Godot's vanilla high-level
multiplayer API (`@rpc`, `MultiplayerSpawner`) over
`ENetMultiplayerPeer`. The networking layer is intentionally
transport-agnostic so this rig keeps working when the eventual real
networking module (Steam P2P + ENet with listen-server architecture)
replaces ENet here. Same Godot multiplayer API underneath = same gool
audio behavior on top.

What this rig is NOT:
- A game (no gameplay loop, no win condition)
- A network module reference (uses Godot stock multiplayer, not the
  dual-transport Steam/ENet module your networking person is building)
- A scalability test (4-player stress is session 4 territory)

## Setup

### Prerequisites

- Godot **4.6.2.stable** or compatible
- The gool addon installed in `addons/gool/`

### Installing the gool addon

This example folder does NOT ship a copy of the gool addon, to avoid
the maintenance hazard of a stale duplicated addon source tree (see
`examples/coop_shooter_template/` for the cautionary tale). Pick one
of these options to install gool:

**Option A — Quick install (Windows):**

Drop `scripts/gool-install.cmd` (from the gool repo) into this folder
and double-click. The installer downloads the latest gool release
from GitHub and extracts `addons/gool/` here.

**Option B — Dev copy from local gool checkout:**

From `examples/multiplayer_audio_sandbox/`:

```bash
# Unix:
cp -r ../../godot/addons/gool addons/gool
cp ../../godot/gool.gdextension addons/gool/

# Windows PowerShell:
Copy-Item -Recurse ..\..\godot\addons\gool addons\gool
Copy-Item ..\..\godot\gool.gdextension addons\gool\
```

You also need to build the GDExtension binary and place it at
`addons/gool/bin/`. See `SETUP.md` in the gool repo root for the
build instructions. (Option A skips the build step entirely by
downloading the prebuilt binary.)

### Open + run

1. Open this folder (`examples/multiplayer_audio_sandbox/`) as a
   Godot project — File > Open > pick the folder
2. Enable the gool plugin: Project > Project Settings > Plugins,
   enable "gool"
3. Press F5. The lobby scene loads.

## How to test session 1

Two Godot instances on the same machine (simplest):

1. Open this project in Godot. F5 to launch — lobby appears
2. Click **Host**. A "Hosting..." status flashes, then the box level
   loads. You see one placeholder cube (your peer)
3. Open ANOTHER instance of Godot (Godot supports running multiple
   editor windows on the same project, but the cleanest way is to
   launch a second instance of `godot.exe` and open the project in
   the new window — or use the running game window without opening
   the editor at all)
4. F5 the second instance. Lobby appears. IP defaults to `127.0.0.1`
5. Click **Join**. After a moment, the box level loads. You see TWO
   cubes — yours and the host's

If that works → session 1 done. No audio yet.

### Testing across two physical machines (LAN)

Replace `127.0.0.1` in the Join input with the host machine's LAN
IP (something like `192.168.1.x`). The Windows host needs to allow
inbound UDP traffic on port 9999 (Windows Firewall will prompt the
first time).

## Architecture notes

**Authority model:** The host is the multiplayer authority (Godot
peer_id 1). The MultiplayerSpawner replicates peer cube spawning
from server to clients. This is integrated listen-server, not the
separate-process architecture the real game will use — that's a
networking-module concern that doesn't affect gool's audio
behavior, so we don't replicate it here.

**Transport:** `ENetMultiplayerPeer`. Swapping to
`SteamMultiplayerPeer` later is mechanical because everything above
the `MultiplayerAPI` layer is transport-agnostic.

**Authority of cube nodes:** Each peer cube gets
`set_multiplayer_authority(peer_id)` so the peer who owns it can
later drive its position (session 2). For session 1 the cubes are
static.

## Files

```
multiplayer_audio_sandbox/
├── README.md          (this file)
├── project.godot      (Godot project file)
├── icon.svg           (placeholder project icon)
├── scenes/
│   ├── lobby.tscn         (host/join UI)
│   ├── box_level.tscn     (a small arena)
│   └── peer_cube.tscn     (placeholder cube spawned per peer)
└── scripts/
    ├── network_manager.gd  (autoload — ENet host/client wrapper)
    ├── lobby.gd            (lobby UI logic)
    ├── box_level.gd        (spawn cubes per peer)
    └── peer_cube.gd        (placeholder cube logic, mostly empty)
```

## Troubleshooting

- **"Failed to host"** — port 9999 may already be in use. Edit
  `network_manager.gd` to use a different port.
- **"Connection failed"** — host isn't running, or firewall is
  blocking, or wrong IP.
- **Lobby loads but cubes don't spawn after connecting** — check
  the Output panel for errors. The `MultiplayerSpawner` is fiddly
  about node paths; the `spawn_path` must exist at the time
  `add_child` is called on the server.
- **Plugin errors mentioning `GoolLog` or `runtime_singleton`** —
  the gool addon isn't installed correctly. Verify
  `addons/gool/runtime_singleton.gd` exists. If you installed via
  the dev-copy method, did you also build the GDExtension binary?
