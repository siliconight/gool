# stb

This directory is where `stb_vorbis.c` lives when the Ogg Vorbis decoder is
enabled. The audio engine does **not** vendor it in-tree to keep the core
repo small. `stb_vorbis.c` is public domain.

Note: `stb_vorbis.c` is named `.c` upstream because it ships as a single C
file that doubles as a header. The audio engine includes it from a single
C++ TU under an `extern "C"` block; no `.h` rename is needed.

## Three ways to get stb_vorbis.c here

### 1. CMake's FetchContent (easiest)

When `AUDIO_ENGINE_DECODERS_OGG=ON` and `AUDIO_ENGINE_FETCH_DECODERS=ON`
(default), CMake pulls stb from GitHub on first configure:

```bash
cmake -S . -B build -DAUDIO_ENGINE_DECODERS_OGG=ON
cmake --build build -j
```

### 2. Run the fetch script

```bash
./scripts/fetch_decoders.sh         # Linux/macOS
.\scripts\fetch_decoders.bat        # Windows
```

### 3. Drop the file in by hand

Copy `stb_vorbis.c` into this directory. CMake detects its presence and
prefers the vendored copy over FetchContent.

## License

stb is dual-licensed (public domain or MIT). See the header preamble of
`stb_vorbis.c`.

## Pinning a version

Add `-DAE_STB_TAG=<git-ref>` at configure time to pin FetchContent to a
specific commit or tag.
