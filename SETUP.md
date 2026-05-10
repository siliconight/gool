# Setting up gool

This is the one document you should follow to go from "I downloaded
this repo" to "I have gool running in a Godot project." If anything
here doesn't work, that's a bug — please open an issue with your OS,
shell, and the failing command.

## Pick a path

There are two ways to get gool into a Godot project:

| Path                                | When to use                                                | Status              |
|-------------------------------------|------------------------------------------------------------|---------------------|
| **Track A — Use prebuilt addon**    | You just want to use gool in your game                     | Available from v0.11.4 onward |
| **Track B — Build from source**     | You want to modify gool, or your platform isn't covered    | Works today         |

Track A is the fastest path for most adopters. It works on Linux
x86_64, Windows x86_64, and macOS — the platforms the release
pipeline builds binaries for. macOS support is **new in v0.11.6**;
it builds clean against Apple Clang now but hasn't been smoke-tested
across Apple Silicon and Intel as thoroughly as the other platforms.
Please file an issue if you hit anything.

Track B is required if you want to modify gool, contribute changes,
or build for a platform the release pipeline doesn't cover yet
(macOS, ARM64, embedded). It also works as a fallback if Track A
hits a binary-compatibility issue with your specific Godot version.

---

## Track A — Use the prebuilt addon

This is the fastest path. No C++ compiler, no SCons, no godot-cpp
build. Suitable for any Godot 4.2+ project on Linux x86_64,
Windows x86_64, or macOS. (macOS support is new in v0.11.6 — please
file an issue if you hit anything unexpected.)

1. Go to the [Releases page](https://github.com/siliconight/gool/releases).

2. Download the addon archive matching your platform from the
   latest release:

   - **Linux:** `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`
   - **Windows:** `gool-X.Y.Z-godot-addon-windows-x86_64.zip`
   - **macOS (Apple Silicon):** `gool-X.Y.Z-godot-addon-macos-arm64.tar.gz`

   *(Note the `-godot-addon-` segment in the filename. The other
   archive without that segment is the C++ static library — useful
   only if you're embedding the engine in a C++ build.)*

3. Extract the archive. Inside, you'll find an `addons/gool/`
   directory containing all the GDScript files, the prefab tree,
   the `gool.gdextension` manifest, and a prebuilt binary in
   `addons/gool/bin/`.

4. Copy the entire `addons/gool/` directory into your Godot
   project's root, alongside your `project.godot`:

   ```
   <my_godot_project>/
   ├── project.godot
   └── addons/
       └── gool/
           ├── bin/
           │   └── libgool_godot.so       (or gool_godot.dll)
           ├── prefabs/
           ├── runtime_singleton.gd
           ├── audio_relevancy_filter.gd
           ├── plugin.cfg
           ├── plugin.gd
           └── gool.gdextension
   ```

5. Open the project in Godot 4.2 or later.

6. **Project Settings → Plugins → gool → Enable.** This wires up
   the editor plugin (which writes a default `gool/config.json`)
   and adds the `Gool` autoload.

7. (Optional) **Project Settings → Autoload** — verify `Gool` is
   listed and pointing at `res://addons/gool/runtime_singleton.gd`.
   The plugin should add this automatically; if it didn't, add it
   manually.

Skip to [§Verifying it worked](#verifying-it-worked) below.

### Don't want to wait for a tagged release?

The `nightly` GitHub Actions workflow produces an addon archive on
every push to `main`. Get the latest by:

1. Go to the project's [Actions tab → Nightly](https://github.com/siliconight/gool/actions/workflows/nightly.yml)
2. Click the most recent successful run
3. Scroll to "Artifacts" at the bottom
4. Download `gool-nightly-godot-addon-<platform>` for your platform

These are signed off only by the CI's "the build compiled" — they
haven't been smoke-tested before being uploaded. Use tagged releases
for production projects; nightlies for following along with active
development.

---

## Track B — Build from source

Three phases:

1. Install platform prerequisites (one-time per machine)
2. Build the GDExtension binary
3. Install the addon into your Godot project

The first phase is the longest. The other two each take a few
minutes once everything's installed.

### The fast path: `scripts/bootstrap.sh` / `bootstrap.ps1`

After the platform prerequisites are installed (Phase 1 below),
phases 2 and 3 collapse into one command:

**Linux / macOS:**

```bash
./scripts/bootstrap.sh                         # build only
./scripts/bootstrap.sh --install-to ~/MyGame   # build + install into your project
```

**Windows** (from the **x64 Native Tools Command Prompt for VS 2022**):

```powershell
scripts\bootstrap.ps1
scripts\bootstrap.ps1 -InstallTo C:\path\to\my_godot_project
```

What it does:

1. Verifies prerequisites are on PATH (git / cmake / python / scons / C++ compiler)
2. Runs the `fetch_*.sh` / `.bat` scripts to get the single-header
   dependencies (miniaudio, dr_libs, stb_vorbis)
3. Clones godot-cpp at the pinned ref (default: `4.2`, override
   with `GODOT_CPP_REF=4.3`) into `third_party/godot-cpp/` and
   builds it with SCons
4. Configures and builds gool's GDExtension via CMake
5. (If `--install-to` / `-InstallTo` is given) copies the addon
   files and the built binary into the target Godot project

The script is **idempotent** — every step checks if its work is
already done before repeating it. Re-running is cheap.

If the bootstrap script fails partway through, the manual walkthrough
below documents what each step does, so you can resume from where
the script left off.

---

### Phase 1: Install prerequisites

You need:

- A C++20 compiler (g++ 10+, Clang 11+, or MSVC 19.29+)
- CMake 3.20 or later
- Python 3.7 or later
- SCons (the build tool godot-cpp uses)
- Git

Pick your platform and copy-paste the commands.

#### Windows

The most reliable path on Windows is the **Visual Studio 2022 Build
Tools** for the C++ compiler, plus standalone CMake / Python / Git.

**Using `winget` (Windows 10 21H2+ and Windows 11):**

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
winget install --id Kitware.CMake
winget install --id Python.Python.3.12
winget install --id Git.Git
```

After installing Python, open a **new** PowerShell or Command Prompt
window (so the updated `PATH` is picked up) and install SCons:

```powershell
pip install scons
```

**Using `choco` (Chocolatey):**

```powershell
choco install -y visualstudio2022buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
choco install -y cmake python git
pip install scons
```

After install, all CMake / SCons / git commands below should be run
in the **"x64 Native Tools Command Prompt for VS 2022"** (start menu)
or in PowerShell after running `vcvars64.bat`. This is the only way
the MSVC compiler shows up on `PATH` correctly.

#### macOS

```bash
xcode-select --install              # C++ compiler + git (Apple Clang)
brew install cmake python           # Homebrew
pip3 install scons
```

If you don't have Homebrew, install it first via the one-liner at
[brew.sh](https://brew.sh).

macOS support landed in v0.11.6. The build cleanly handles Apple
Clang's pragma differences from GCC. If you hit any compile error
that's specific to your macOS version or Xcode toolchain, please
file an issue.

#### Linux — Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 python3-pip pkg-config git
pip3 install scons
```

#### Linux — Fedora / RHEL

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y cmake python3-pip pkgconf-pkg-config git
pip3 install scons
```

#### Linux — Arch / Manjaro

```bash
sudo pacman -S --needed base-devel cmake python python-pip git scons
```

---

### Phase 2: Build the GDExtension

This phase has three sub-steps:

1. Clone gool (the repo you're reading) and fetch its single-header
   dependencies.
2. Clone and build `godot-cpp` (the C++ bindings for Godot's GDExtension API).
3. Build gool's GDExtension binding against godot-cpp.

#### 2.1 — Clone gool and fetch its dependencies

```bash
git clone https://github.com/siliconight/gool.git
cd gool
```

The C++ engine has three single-header dependencies that aren't
vendored in the repo. Run the fetch scripts to grab them:

**Linux / macOS:**

```bash
./scripts/fetch_miniaudio.sh
./scripts/fetch_decoders.sh
```

**Windows (PowerShell or cmd):**

```powershell
scripts\fetch_miniaudio.bat
scripts\fetch_decoders.bat
```

These scripts download:

- `third_party/miniaudio/miniaudio.h` (cross-platform audio device I/O)
- `third_party/dr_libs/dr_wav.h` (WAV decoder)
- `third_party/dr_libs/dr_flac.h` (FLAC decoder)
- `third_party/stb/stb_vorbis.c` (Ogg Vorbis decoder)

You only need to run these once per checkout. They use `curl` /
`wget` / PowerShell `Invoke-WebRequest` (whichever is available),
and set the downloaded files read-only so accidental edits are
visible.

#### 2.2 — Clone and build godot-cpp

`godot-cpp` is the C++ bindings for Godot's GDExtension API. It's
~100 MB and tracks Godot's release cadence, so we don't vendor it —
you check out a branch matching your Godot version.

For Godot 4.2+:

```bash
git clone --branch 4.2 https://github.com/godotengine/godot-cpp.git
cd godot-cpp
scons platform=linux target=template_release -j$(nproc)
cd ..
```

Substitute the platform:

| Your platform | `platform=` argument |
|---------------|-----------------------|
| Linux         | `linux`              |
| macOS         | `macos`              |
| Windows       | `windows`            |

On Windows, replace `-j$(nproc)` with `-j%NUMBER_OF_PROCESSORS%`.

The first build takes 5–20 minutes depending on your machine. It
produces a static library at `godot-cpp/bin/libgodot-cpp.<platform>.template_release.<arch>.a`.
Subsequent builds (if you upgrade godot-cpp) are incremental and fast.

If you're targeting a different Godot version, swap the branch:
`--branch 4.3` for Godot 4.3, etc. The branch name on godot-cpp
matches the Godot minor version exactly.

#### 2.3 — Build gool's GDExtension

From the `gool` repo root (assuming you cloned `godot-cpp/` next to it):

```bash
cmake -S godot -B build-godot \
    -DGODOT_CPP_PATH=../godot-cpp \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-godot --config Release -j
```

On Windows, run this from the **x64 Native Tools Command Prompt for
VS 2022**:

```powershell
cmake -S godot -B build-godot -DGODOT_CPP_PATH=..\godot-cpp -DCMAKE_BUILD_TYPE=Release
cmake --build build-godot --config Release -j
```

The build produces the GDExtension shared library:

| Platform | Output file                                   |
|----------|-----------------------------------------------|
| Linux    | `build-godot/libgool_godot.so`               |
| macOS    | `build-godot/libgool_godot.dylib`            |
| Windows  | `build-godot/Release/gool_godot.dll`         |

---

### Phase 3: Install the addon into your Godot project

1. In your Godot project (the directory containing `project.godot`),
   create the addons directory:

   ```bash
   mkdir -p addons/gool/bin
   ```

2. Copy the addon GDScript files (from gool's repo into your project):

   ```bash
   cp -r /path/to/gool/godot/addons/gool/* /path/to/your/project/addons/gool/
   ```

   This copies all the prefabs, the runtime singleton, the editor
   plugin, and the `gool.gdextension` manifest. It does NOT include
   a `bin/` directory — that's where the binary you just built goes.

3. Copy the GDExtension binary into the addon's bin directory:

   ```bash
   # Linux:
   cp /path/to/gool/build-godot/libgool_godot.so /path/to/your/project/addons/gool/bin/

   # macOS:
   cp /path/to/gool/build-godot/libgool_godot.dylib /path/to/your/project/addons/gool/bin/

   # Windows:
   copy "C:\path\to\gool\build-godot\Release\gool_godot.dll" "C:\path\to\your\project\addons\gool\bin\"
   ```

4. Open the project in Godot 4.2 or later. The first time, the
   editor will scan the new GDExtension. Watch the output panel for
   any errors loading `gool_godot`.

5. **Project Settings → Plugins → gool → Enable.** This wires up
   the editor plugin (which writes a default `gool/config.json`)
   and the `Gool` autoload.

6. (Optional) **Project Settings → Autoload** — verify `Gool` is
   listed and pointing at `res://addons/gool/runtime_singleton.gd`.
   The plugin should add this automatically; if it didn't, add it
   manually.

---

## Verifying it worked

The fastest way to confirm everything is wired up is to run the
quickstart example project:

1. In Godot, open the project at `examples/quickstart/` (inside the
   gool repo)
2. Copy your built binary into `examples/quickstart/addons/gool/bin/`
   the same way you did for your own project
3. Press Play

You should hear synthesized test tones routed through the seven
prefab nodes (adaptive music crossfading, spatial drone, footsteps,
mock voice chat, reverb zone, networked event, networked emitter).
The output panel should show no errors and the `[gool] runtime
initialized` log line.

For a richer demonstration with actual gameplay (one playable
character + three AI bots, three weapon types, multi-tier sidechain
ducking via the v0.10+ bus config), open `examples/coop_shooter_template/`
and press Play.

---

## Optional features

The engine has several build flags. As of v0.11.9, the audio
backend + WAV/OGG/FLAC decoders **default ON** because that's what
Godot adopters and most C++ embedders need. Opus codecs default OFF
because they require system packages (libopusfile is autotools, no
FetchContent fallback). The C++ static library archive on the
Releases page is built with everything OFF for embedders who prefer
minimal.

| CMake flag                       | Default | What it enables                         | Extra dependency             |
|----------------------------------|---------|------------------------------------------|------------------------------|
| `AUDIO_ENGINE_BACKEND_MINIAUDIO` | **ON**  | Cross-platform audio device output      | `miniaudio.h` (fetch script) |
| `AUDIO_ENGINE_DECODERS_WAV`      | **ON**  | WAV file decoding                        | `dr_wav.h` (fetch script)    |
| `AUDIO_ENGINE_DECODERS_OGG`      | **ON**  | Ogg Vorbis decoding                      | `stb_vorbis.c` (fetch script)|
| `AUDIO_ENGINE_DECODERS_FLAC`     | **ON**  | FLAC decoding                            | `dr_flac.h` (fetch script)   |
| `AUDIO_ENGINE_DECODERS_OPUS`     | OFF     | Ogg Opus file decoding                   | `libopusfile` (system pkg)   |
| `AUDIO_ENGINE_VOICE_OPUS`        | OFF     | Opus codec for voice chat                | `libopus` (system / fetch)   |
| `AUDIO_ENGINE_BUILD_TESTS`       | ON      | Build unit tests                         | none                         |
| `AUDIO_ENGINE_BUILD_EXAMPLES`    | ON      | Build C++ examples                       | none                         |
| `AUDIO_ENGINE_SHARED`            | OFF     | Build engine as shared library           | none                         |

The fetched headers (`miniaudio.h`, `dr_wav.h`, `dr_flac.h`,
`stb_vorbis.c`) come from `scripts/fetch_*.{sh,bat}` if you've run
them, or from CMake's `FetchContent` fallback (controlled by
`AUDIO_ENGINE_FETCH_*=ON`, default) if you haven't. The fetch
scripts are friendlier to repeat builds; FetchContent is what runs
on a fresh checkout via `cmake -S . -B build` with no flags.

For air-gapped builds where FetchContent can't reach the network,
either run the fetch scripts manually first OR pass
`-DAUDIO_ENGINE_FETCH_*=OFF` and supply the headers yourself.

For a Godot-side build with the v0.11.9 defaults, no audio flags
need to be passed:

```bash
cmake -S godot -B build-godot \
    -DGODOT_CPP_PATH=../godot-cpp \
    -DCMAKE_BUILD_TYPE=Release
```

The miniaudio backend + WAV/OGG/FLAC decoders are picked up
automatically. Add `-DAUDIO_ENGINE_DECODERS_OPUS=ON
-DAUDIO_ENGINE_VOICE_OPUS=ON` if you have libopusfile + libopus
installed and want Opus support.

### Adding Opus support

Opus has two independent integration points:

- **`AUDIO_ENGINE_DECODERS_OPUS`** — decode `.opus` files at runtime
  (game music, SFX). Needs `libopusfile`.
- **`AUDIO_ENGINE_VOICE_OPUS`** — encode/decode voice chat packets.
  Needs `libopus`.

Most projects want both, or neither. They're independent flags so
you can mix and match.

#### Installing libopusfile (for `.opus` file playback)

| Platform        | Command                                                     |
|-----------------|-------------------------------------------------------------|
| Windows (vcpkg) | `vcpkg install opusfile:x64-windows-static-md`              |
| Windows (other) | Build from [xiph/opusfile](https://github.com/xiph/opusfile) |
| macOS           | `brew install opusfile`                                     |
| Ubuntu / Debian | `sudo apt install libopusfile-dev`                          |
| Fedora          | `sudo dnf install opusfile-devel`                           |
| Arch            | `sudo pacman -S opusfile`                                   |

For Windows, the `x64-windows-static-md` triplet links libopusfile
statically into your addon binary while keeping the dynamic CRT
(matches godot-cpp's default), so the resulting DLL has no extra
runtime dependencies. The Godot release pipeline uses the same
triplet — adopters downloading Track A's Windows addon archive don't
need vcpkg installed.

If you're building locally on Windows with `bootstrap.ps1`, add the
toolchain file to your cmake invocation:

```powershell
cmake -S godot -B build-godot `
    -DGODOT_CPP_PATH=../godot-cpp `
    -DCMAKE_BUILD_TYPE=Release `
    -DAUDIO_ENGINE_DECODERS_OPUS=ON `
    -DAUDIO_ENGINE_VOICE_OPUS=ON `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
```

After installing, add `-DAUDIO_ENGINE_DECODERS_OPUS=ON` to your CMake
invocation. CMake will find libopusfile via `find_package(OpusFile)`
or `pkg-config opusfile` (whichever your install registered with),
or via the vcpkg toolchain file on Windows.

#### Installing libopus (for voice chat)

| Platform        | Command                                                     |
|-----------------|-------------------------------------------------------------|
| Windows (vcpkg) | `vcpkg install opus`                                        |
| Windows (fetch) | `scripts\fetch_opus.bat` clones the source for FetchContent |
| macOS           | `brew install opus`                                         |
| Ubuntu / Debian | `sudo apt install libopus-dev`                              |
| Fedora          | `sudo dnf install opus-devel`                               |
| Arch            | `sudo pacman -S opus`                                       |

`scripts/fetch_opus.sh` / `.bat` exist as a fallback that clones
xiph/opus into `third_party/opus/`; CMake adds it as a subdirectory
when present. Use this only if your platform's package manager
doesn't have libopus.

After installing, add `-DAUDIO_ENGINE_VOICE_OPUS=ON` to your CMake
invocation.

---

## Troubleshooting

### "Set GODOT_CPP_PATH to a built godot-cpp checkout"

You skipped Phase 2.2 or your `-DGODOT_CPP_PATH=...` argument doesn't
point at a directory containing a built godot-cpp. Verify:

```bash
ls /path/to/godot-cpp/include/godot_cpp/godot.hpp
ls /path/to/godot-cpp/bin/                       # should contain libgodot-cpp.*.a
```

If `bin/` is empty, godot-cpp isn't built yet — go back to Phase 2.2
and run `scons platform=...`.

### "miniaudio.h: No such file or directory"

You have `AUDIO_ENGINE_BACKEND_MINIAUDIO=ON` but didn't run the
fetch script. From the gool repo root:

```bash
./scripts/fetch_miniaudio.sh        # or .bat on Windows
```

Then re-run cmake and the build.

### "dr_wav.h / dr_flac.h / stb_vorbis.c: No such file or directory"

Same problem with the decoder libraries. Run:

```bash
./scripts/fetch_decoders.sh         # or .bat on Windows
```

### "Compiler does not support C++20"

Your compiler is too old. Required minimums:

- g++ 10 or later (`g++ --version`)
- Clang 11 or later (`clang --version`)
- MSVC 19.29 or later (Visual Studio 2019 16.10+, or VS 2022)

On Ubuntu 20.04, default g++ is 9 — install g++-10 explicitly:

```bash
sudo apt install g++-10
export CXX=g++-10
```

### Windows: "cl is not recognized" / "MSBuild not found"

You're not in a Visual Studio developer environment. Either:

- Open the **"x64 Native Tools Command Prompt for VS 2022"** from the
  Start menu (recommended), OR
- In a regular PowerShell, run:
  `& "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
  before any cmake/scons commands

### "scons: command not found"

You installed Python but `pip install scons` didn't put scons on
PATH. On Windows you may need to open a fresh terminal after
installing Python. On Linux/macOS:

```bash
pip3 install --user scons
export PATH="$HOME/.local/bin:$PATH"        # add to ~/.bashrc / ~/.zshrc
```

### Godot says "Failed to load GDExtension" or the addon doesn't appear

Most common causes:

1. **Wrong binary architecture.** If you built for `x86_64` but
   Godot is `arm64` (or vice versa), the binary won't load. Check
   that your built `libgool_godot.*` matches Godot's architecture.
2. **Wrong Godot version.** The `.gdextension` manifest declares
   `compatibility_minimum = "4.2"`. Godot 4.0 / 4.1 won't load it.
3. **Binary in wrong location.** It must be at
   `addons/gool/bin/libgool_godot.so` (or `.dylib` / `gool_godot.dll`).
   The `.gdextension` manifest points at exactly these paths.
4. **godot-cpp version mismatch.** If you built godot-cpp from
   branch `4.3` but you're running Godot 4.2, the ABI may differ.
   Match the godot-cpp branch to your Godot version.

Open the Godot output panel (bottom of the editor); the load failure
message usually names the specific issue.

### "GoolAudioRuntime class not registered"

The GDExtension is loading but the class wasn't picked up. Either:

- The binding initialization function failed silently. Run Godot
  from a terminal so you see stderr output during load.
- The `.gdextension` manifest's `entry_symbol = "gool_godot_init"`
  doesn't match what the binary exports. If you've modified the
  binding source, verify the export.

### macOS-specific issues

The Apple Clang pragma issue that broke macOS builds before v0.11.6
is fixed. If you hit a different macOS-specific compile error after
that:

- **Make sure Xcode CLI is installed** (`xcode-select --install`).
  The system compiler must be Apple Clang, not GCC from Homebrew —
  the binding code is written for the system toolchain.
- **Check your Apple Clang version**: `clang --version`. The C++20
  features used (`std::span`, designated initializers) need Apple
  Clang 14+ (Xcode 14 / June 2022 or later). On macOS 11 with an
  older Xcode, you may hit "no member named 'span' in namespace
  'std'" — upgrade Xcode.
- **Apple Silicon vs Intel**: the release pipeline ships a
  `macos-arm64` binary built on macos-latest (which is Apple
  Silicon). It loads in Apple Silicon Godot. **If you're on Intel
  Mac, this binary won't load** — Track A doesn't ship an Intel
  binary yet; use Track B (build from source) to produce one. A
  proper universal binary (Intel + Apple Silicon) is a follow-up
  that will use `lipo` to combine separately-built arch slices.

If the failure is reproducible and you can capture the compile
error, please open an issue. macOS support is new and the team
needs concrete reproductions to harden it.

---

## What's next

Once gool is loaded in your project:

- The [README](README.md) covers what the API looks like (`Gool.play_3d`,
  RTPC bindings, music transitions).
- `examples/quickstart/` is the seven-prefab demo — open it to see
  every feature wired up at once.
- `examples/coop_shooter_template/` is the gameplay-shaped demo with
  weapons, AI, and multi-tier sidechain ducking.
- `docs/multiplayer.md` is the design doc for the
  replicate-events / play-locally pattern that defines how to wire
  audio in a networked game.

If you hit anything not covered here, please open an issue. The goal
of this document is that nobody hits a wall during setup — every
gap is a doc bug.
