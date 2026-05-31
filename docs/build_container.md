# Container dependencies for building gool from source

This document lists the dependencies a container needs to build gool
end-to-end — the C++ audio engine library, the Godot GDExtension
binary, and all the dev/CI tooling around them. Useful if you want
to:

- Set up a reproducible build environment (Docker, Podman, CI)
- Verify your local environment has everything needed
- Audit what's actually required vs. nice-to-have

If you just want to USE gool (drop the addon into a Godot project,
no compilation), you don't need any of this — see `SETUP.md` Track A.

---

## What actually builds

When you build gool from source, three artifacts get produced:

| Artifact | What it is | Where it lands |
|---|---|---|
| `audio_engine.lib` / `libaudio_engine.a` | The C++ audio runtime — pure C++20, no Godot dependency | `build/Release/` or `build/` |
| `gool_godot.dll` / `libgool_godot.so` / `libgool_godot.dylib` | The Godot GDExtension binary that bridges the engine to Godot | `build-godot/` (or `build-godot/Release/` on Windows) |
| `godot-cpp` static library | Godot 4.x bindings, built from source. Transitive — needed to link the GDExtension. | `third_party/godot-cpp/bin/` |

The audio engine itself is a standalone C++ library; the GDExtension
is a thin layer on top. Sanitizer builds (ASan, TSan, UBSan, libFuzzer)
add more artifacts but are off by default.

---

## Tier 1 — Minimum dependencies (audio engine only)

Builds `audio_engine.lib` / `libaudio_engine.a` and runs the unit
tests. Skips the Godot bindings entirely. Smallest viable container.

| Dependency | Minimum version | Purpose |
|---|---|---|
| C++20 compiler | GCC 10+, Clang 12+, MSVC 19.30+ | Engine source is C++20 (`CMAKE_CXX_STANDARD 20`) |
| CMake | 3.20+ | Build system (`cmake_minimum_required(VERSION 3.20)`) |
| git | any recent | CMake reads `git rev-parse` for the build's commit SHA |
| pthread (POSIX) or Win32 threads | n/a — system | `find_package(Threads REQUIRED)` |
| curl OR wget | any recent | `scripts/fetch_*.sh` downloads header-only deps |
| make / ninja | any recent | CMake generator's underlying build tool |
| pkg-config | 0.29+ | libopus discovery on Linux |
| libopus | 1.3+ | Voice chat codec (header `opus/opus.h`) |
| libopusfile | 0.12+ | Opus file decoding |

### Linux package install (Debian/Ubuntu)

```bash
apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    pkg-config \
    libopus-dev \
    libopusfile-dev \
    ninja-build
```

### Linux package install (Fedora/RHEL)

```bash
dnf install -y \
    gcc-c++ \
    cmake \
    git \
    curl \
    pkgconf-pkg-config \
    opus-devel \
    opusfile-devel \
    ninja-build
```

### macOS (Homebrew)

```bash
brew install cmake git curl pkg-config opus opusfile ninja
# Xcode Command Line Tools (provides Apple Clang + Make) — install via:
# xcode-select --install
```

### Windows

Windows uses **vcpkg** for Opus (not pkg-config). Both `opusfile`
and `opus` should be installed with the `x64-windows-static-md`
triplet so they link cleanly into a single shared library without
dragging extra DLLs into the addon archive:

```cmd
vcpkg install opusfile:x64-windows-static-md opus:x64-windows-static-md
```

You also need:
- Visual Studio 2022 (or 2019) with the C++ workload
- MSVC 19.30+ (ships with VS 2022)
- CMake (VS Installer can include it, or install separately)
- Git

The `windows-latest` GitHub Actions runner already has CMake, MSVC,
Git, and vcpkg installed; only `vcpkg install` is needed for Opus.

### Verification at this tier

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DAUDIO_ENGINE_BUILD_EXAMPLES=OFF \
    -DAUDIO_ENGINE_BUILD_TESTS=OFF \
    -DAUDIO_ENGINE_BACKEND_MINIAUDIO=OFF
cmake --build build --config Release --target audio_engine
```

If `audio_engine.lib` (Windows) or `libaudio_engine.a` (Linux/macOS)
appears in `build/`, the Tier 1 toolchain is complete.

---

## Tier 2 — Full GDExtension build

Adds the Godot extension binary on top of Tier 1. Adds these deps:

| Dependency | Minimum version | Purpose |
|---|---|---|
| Python 3 | 3.8+ | Runtime for SCons (godot-cpp's build tool) |
| pip | any recent | Installs SCons |
| SCons | 4.0+ | godot-cpp's build system; installed via `pip install scons` |
| Network access to github.com | n/a | Clones `godotengine/godot-cpp` at build time |

`godot-cpp` is **NOT** vendored in the repo. The build clones it
fresh from `https://github.com/godotengine/godot-cpp` at the branch
specified by the `GODOT_CPP_REF` environment variable (default `4.4`
matching `gool.gdextension`'s `compatibility_minimum`). The clone +
build takes ~5 minutes on a fresh machine; CI caches the built result
under `third_party/godot-cpp/`.

### Linux additions to Tier 1

```bash
apt-get install -y python3 python3-pip
pip3 install scons
```

### macOS additions to Tier 1

```bash
brew install python
pip3 install scons
```

### Windows additions to Tier 1

```cmd
pip install scons
```
(Python 3 is preinstalled on `windows-latest`.)

### Verification at this tier

```bash
# Set the godot-cpp branch (matches gool.gdextension)
export GODOT_CPP_REF=4.4

# Fetch the header-only audio deps (Tier 1 doesn't need this unless
# building with miniaudio backend enabled; the GDExtension does)
bash scripts/fetch_miniaudio.sh
bash scripts/fetch_decoders.sh

# Clone + build godot-cpp (skip if already cached)
if [ ! -d third_party/godot-cpp ]; then
    git clone --depth 1 --branch "$GODOT_CPP_REF" \
        https://github.com/godotengine/godot-cpp third_party/godot-cpp
    cd third_party/godot-cpp
    scons platform=linux target=template_release -j$(nproc)
    cd ../..
fi

# Build the GDExtension binary
cmake -S . -B build-godot \
    -DCMAKE_BUILD_TYPE=Release \
    -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON \
    -DAUDIO_ENGINE_DECODERS_WAV=ON \
    -DAUDIO_ENGINE_DECODERS_OGG=ON \
    -DAUDIO_ENGINE_DECODERS_FLAC=ON
cmake --build build-godot --config Release --target gool_godot
```

If `libgool_godot.so` (Linux) or `gool_godot.dll` (Windows) appears
in `build-godot/`, the Tier 2 toolchain is complete.

---

## Header-only dependencies (fetched at build time, NOT pre-installable)

These are downloaded by `scripts/fetch_miniaudio.sh` and
`scripts/fetch_decoders.sh` from their canonical upstream repos.
The container needs `curl` or `wget` to fetch them; nothing to
pre-install:

| Header | Upstream | What it provides |
|---|---|---|
| `miniaudio.h` | github.com/mackron/miniaudio | Cross-platform audio backend (WASAPI / CoreAudio / ALSA / PulseAudio) |
| `dr_wav.h` | github.com/mackron/dr_libs | WAV decoder |
| `dr_flac.h` | github.com/mackron/dr_libs | FLAC decoder |
| `stb_vorbis.c` | github.com/nothings/stb | Ogg Vorbis decoder |

The fetch scripts pin to `master` by default but accept a `REF=`
env var for reproducible builds. For container reproducibility,
either pin via `REF=<sha>` or vendor the headers into
`third_party/{miniaudio,dr_libs,stb}/` before building.

Three other deps come via CMake `FetchContent` automatically:

| Dependency | Source | Pinned version | Purpose |
|---|---|---|---|
| nlohmann/json | github.com/nlohmann/json | v3.11.3 | Bus config parsing |
| Opus (optional, source build) | github.com/xiph/opus | latest | Fallback if system Opus unavailable |
| miniaudio (optional, source build) | github.com/mackron/miniaudio | latest | Fallback if not fetched via shell script |

These also need network access at first build; CMake's
`FetchContent_MakeAvailable` clones into `build/_deps/`. Subsequent
builds use the cached clone.

---

## Tier 3 — Optional CI / dev tooling

These match what the GitHub Actions CI runs. Not needed to build,
but needed to replicate the full check matrix locally or in a CI
container.

### Static analysis & lint

| Tool | Install | Purpose |
|---|---|---|
| clang-tidy 15 | `apt install clang-tidy-15` | C++ static analysis (strict mode in CI) |
| cppcheck | `apt install cppcheck` | Additional C++ static analysis |
| lizard | `pip install lizard` | Cyclomatic complexity gates |
| gdtoolkit 4.x | `pip install "gdtoolkit==4.*"` | GDScript lint + format |

### Test runners

Built-in to CMake's test discovery — no separate framework install
needed. Tests build under `cmake --build build --target tests`.

### Sanitizers (Clang/GCC builds only)

ASan, UBSan, TSan are compiler-provided — no separate install.
libFuzzer is Clang-only and requires Clang 12+ (already in Tier 1):

```bash
cmake -S . -B build-asan -DAUDIO_ENGINE_SANITIZE_ASAN=ON
cmake -S . -B build-tsan -DAUDIO_ENGINE_SANITIZE_TSAN=ON
cmake -S . -B build-fuzz -DAUDIO_ENGINE_FUZZ=ON  # clang-only
```

### Documentation scanners

The repo includes 7 Python scanners that CI runs on every push.
None require third-party deps beyond Python 3 standard library —
just `python3`:

- `scripts/check_version_sync.sh` (bash)
- `scripts/check_addon_autoload_safety.py`
- `scripts/check_license_canonical.sh` (bash)
- `scripts/check_notice_canonical.sh` (bash)
- `scripts/apply_apache_headers.py`
- `scripts/check_addon_drift.py`
- `scripts/check_scene_references.py`

---

## Working Dockerfile example

Tier 2 (full GDExtension build) on Ubuntu 24.04. Builds the Linux
GDExtension binary. ~700MB image after build; ~250MB with multi-stage
trimming.

```dockerfile
# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV GODOT_CPP_REF=4.4

# Tier 1: minimum build deps
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        curl \
        pkg-config \
        libopus-dev \
        libopusfile-dev \
    && rm -rf /var/lib/apt/lists/*

# Tier 2: Python + SCons for godot-cpp
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 \
        python3-pip \
    && pip3 install --break-system-packages scons \
    && rm -rf /var/lib/apt/lists/*

# Optional Tier 3: lint / static-analysis tools
# Uncomment if you want CI parity in this image (~200MB extra)
# RUN apt-get update && apt-get install -y --no-install-recommends \
#         clang-tidy-15 \
#         cppcheck \
#     && ln -sf "$(which clang-tidy-15)" /usr/local/bin/clang-tidy \
#     && pip3 install --break-system-packages lizard "gdtoolkit==4.*" \
#     && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Fetch header-only audio deps
RUN bash scripts/fetch_miniaudio.sh \
    && bash scripts/fetch_decoders.sh

# Build godot-cpp (one-time, ~5min)
RUN git clone --depth 1 --branch "${GODOT_CPP_REF}" \
        https://github.com/godotengine/godot-cpp third_party/godot-cpp \
    && cd third_party/godot-cpp \
    && scons platform=linux target=template_release -j$(nproc)

# Build the GDExtension binary
RUN cmake -S . -B build-godot \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON \
        -DAUDIO_ENGINE_DECODERS_WAV=ON \
        -DAUDIO_ENGINE_DECODERS_OGG=ON \
        -DAUDIO_ENGINE_DECODERS_FLAC=ON \
    && cmake --build build-godot --target gool_godot --parallel

# Verify build artifacts exist
RUN test -f build-godot/libgool_godot.so \
    && echo "OK: GDExtension binary built successfully"

# Optional: thin runtime image with just the artifact
FROM ubuntu:24.04 AS runtime
COPY --from=builder /src/build-godot/libgool_godot.so /opt/gool/
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libopus0 \
        libopusfile0 \
    && rm -rf /var/lib/apt/lists/*
```

Build it:

```bash
docker build -t gool-builder .
```

Extract the GDExtension binary:

```bash
docker create --name gool-extract gool-builder
docker cp gool-extract:/opt/gool/libgool_godot.so ./
docker rm gool-extract
```

---

## What's NOT needed in a build container

For clarity on what to skip:

- **Godot Engine itself.** godot-cpp is the bindings library; the
  Godot editor / runtime is needed only for actually USING the
  GDExtension, not for building it. CI's `godot-headless-smoke`
  job runs Godot but it's a separate concern from the build
  matrix.

- **SCons for gool itself.** gool uses CMake. SCons is only needed
  to build godot-cpp (which is a transitive dep). The build
  scripts handle this; you don't need to invoke SCons directly.

- **A specific Godot version pinned in the container.** The
  GDExtension is forward-compatible with any Godot ≥ the
  `compatibility_minimum` in `gool.gdextension` (currently 4.4).
  No Godot binary needed in the build container.

- **node.js / npm.** No JavaScript anywhere in gool's build.

- **Rust / Go / Java / other language runtimes.** Pure C++ +
  Python (for SCons) + bash (for shell scripts).

- **GPU / OpenGL / Vulkan SDKs.** gool is audio-only; no graphics
  pipeline. Headless server containers are a perfectly fine
  target.

---

## Network access requirements

The build needs network access during these specific steps:

| Step | URL | Reason |
|---|---|---|
| `apt install` / package install | Distro mirrors | System deps |
| `scripts/fetch_miniaudio.sh` | `raw.githubusercontent.com/mackron/miniaudio` | miniaudio.h |
| `scripts/fetch_decoders.sh` | `raw.githubusercontent.com/{mackron/dr_libs,nothings/stb}` | dr_wav.h, dr_flac.h, stb_vorbis.c |
| `git clone godot-cpp` | `github.com/godotengine/godot-cpp` | Godot 4.x bindings source |
| CMake `FetchContent` | `github.com/nlohmann/json` (and optionally Opus, miniaudio) | Source-built deps |
| (Windows only) vcpkg | `github.com/Kitware/CMake/releases` for bootstrapping | vcpkg internal — bit me in v0.82.3 CI when GitHub returned 502 |

**For air-gapped builds**: vendor `miniaudio.h`, `dr_wav.h`,
`dr_flac.h`, `stb_vorbis.c` into `third_party/{miniaudio,dr_libs,stb}/`
manually, vendor `godot-cpp` into `third_party/godot-cpp/`, and pre-build
godot-cpp once. After that the build needs no network.

The v0.82.3 release.yml CI run failed on a transient 502 from
`github.com/Kitware/CMake/releases` when vcpkg tried to bootstrap
itself — the failure was nothing to do with gool's code, just
GitHub returning 502 for a third-party download. A pre-warmed cache
or vendored toolchain sidesteps this entirely.

---

## Image size reference

Approximate sizes for the Dockerfile above:

| Stage | Size | Contents |
|---|---|---|
| Tier 1 base (Ubuntu 24.04 + build tools) | ~600MB | gcc, cmake, libopus, libopusfile |
| Tier 2 (+ Python + SCons) | ~700MB | + Python 3, SCons |
| Tier 3 (+ clang-tidy + cppcheck + gdtoolkit) | ~900MB | + static analysis tools |
| After full build (engine + GDExtension) | ~1.2GB | + build/, build-godot/, godot-cpp/ |
| Runtime-only (just `libgool_godot.so`) | ~80MB | minimal Ubuntu + libopus + libopusfile + the .so |

Multi-stage build trimming is straightforward — the GDExtension
binary itself is ~2MB; everything else is build-time only.

---

## See also

- `SETUP.md` — Track A (prebuilt) and Track B (build) instructions
  for end users (not container authors)
- `RELEASING.md` — Maintainer guide for the release.yml workflow
- `.github/workflows/release.yml` — Authoritative source for what
  CI actually does
- `.github/workflows/ci.yml` — Authoritative source for what static
  analysis runs
- `CMakeLists.txt` — Authoritative source for build options
