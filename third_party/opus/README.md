# libopus vendoring slot

The audio engine's CMake voice-codec resolution checks **this directory first** when
`AUDIO_ENGINE_VOICE_OPUS=ON`. If it finds a `CMakeLists.txt` here it pulls the
library in via `add_subdirectory()`. The `opus` target (libopus's standard CMake
target) becomes a normal link dependency of `audio_engine`.

## Three ways to populate

### 1. Vendor the source tree (offline-friendly, fully reproducible)

Clone xiph/opus directly under this directory:

```sh
./scripts/fetch_opus.sh        # POSIX shell
scripts\fetch_opus.bat         # Windows
```

This drops the full libopus source under `third_party/opus/` so subsequent CMake
configures resolve the library locally without network access.

The script clones the upstream `master` branch by default; pin a release tag
(e.g. `v1.5.2`) by editing the `OPUS_REF` variable at the top of the script if
you want a fixed version.

### 2. System package

Skip vendoring entirely. The CMake fallback chain after this directory tries
`find_package(Opus)` then `pkg_check_modules(opus)`. On Debian-derived systems:

```sh
sudo apt install libopus-dev
```

On macOS via Homebrew:

```sh
brew install opus
```

### 3. FetchContent

If neither vendored sources nor a system package are present, CMake's
`FetchContent` clones xiph/opus at configure time (this requires network access
and is gated by `AUDIO_ENGINE_FETCH_OPUS=ON`, which defaults to ON).

## Why libopus isn't shipped under `third_party/` like dr_libs

Unlike dr_libs and stb (single-header public-domain), libopus is:
- Multiple translation units organised under its own `CMakeLists.txt`.
- BSD-3-Clause licensed (compatible with most projects, but you should confirm
  for your distribution).
- Around ~50,000 lines of C not appropriate to copy into a host repo
  unconditionally.

The vendoring slot exists for hosts that want a hermetic build without
depending on either a system package or network access at configure time.

## License

libopus is BSD-3-Clause. See `COPYING` in the upstream repository.
