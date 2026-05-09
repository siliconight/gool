# miniaudio

This directory is where `miniaudio.h` lives when the miniaudio backend is
enabled. The audio engine does **not** vendor miniaudio in-tree to keep the
core repo small and the upstream license boundary clear (miniaudio is
public domain / MIT-0 either way, but pulling a 95k-line header into every
checkout is friction).

## Three ways to get miniaudio.h here

### 1. CMake's FetchContent (easiest)

When `AUDIO_ENGINE_BACKEND_MINIAUDIO=ON` and `AUDIO_ENGINE_FETCH_MINIAUDIO=ON`
(default), CMake will pull miniaudio from GitHub on first configure:

```bash
cmake -S . -B build -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON
cmake --build build -j
```

Nothing else to do. The fetched header is placed under `build/_deps/`.

### 2. Helper script (works without CMake)

```bash
./scripts/fetch_miniaudio.sh         # macOS / Linux / WSL
scripts\fetch_miniaudio.bat          # Windows cmd
```

Both download the latest `miniaudio.h` from GitHub and drop it into this
directory. After that, the file is on the include path for both CMake and
raw g++/clang/cl builds.

### 3. Manual (for offline / pinned builds)

Download `miniaudio.h` from <https://github.com/mackron/miniaudio> and place
it next to this README. For production builds, pin to a specific tag rather
than tracking `master`.

## Verifying the install

```bash
ls third_party/miniaudio/miniaudio.h    # should print the path
```

If the file is present, `cmake -DAUDIO_ENGINE_BACKEND_MINIAUDIO=ON` will use
it directly (skipping FetchContent), and `examples/playback` becomes
buildable.

## License

miniaudio is dual-licensed: public domain (Unlicense) or MIT-0, your choice.
This project does not modify or redistribute miniaudio's source; you are
fetching it directly from upstream.
