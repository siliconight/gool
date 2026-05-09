# dr_libs

This directory is where `dr_wav.h` and `dr_flac.h` live when the WAV / FLAC
decoders are enabled. The audio engine does **not** vendor these in-tree to
keep the core repo small. Both files are public domain.

## Three ways to get the headers here

### 1. CMake's FetchContent (easiest)

When `AUDIO_ENGINE_DECODERS_WAV=ON` (or `_FLAC=ON`) and
`AUDIO_ENGINE_FETCH_DECODERS=ON` (default), CMake pulls dr_libs from GitHub
on first configure:

```bash
cmake -S . -B build -DAUDIO_ENGINE_DECODERS_WAV=ON -DAUDIO_ENGINE_DECODERS_FLAC=ON
cmake --build build -j
```

### 2. Run the fetch script

```bash
./scripts/fetch_decoders.sh         # Linux/macOS
.\scripts\fetch_decoders.bat        # Windows
```

This downloads the latest `dr_wav.h` and `dr_flac.h` from
[github.com/mackron/dr_libs](https://github.com/mackron/dr_libs) into this
directory.

### 3. Drop the headers in by hand

Copy `dr_wav.h` and/or `dr_flac.h` into this directory. CMake will detect
either file's presence and prefer the vendored copy over FetchContent.

## License

dr_libs is dual-licensed under public domain (Unlicense) or MIT-0 pick
whichever your jurisdiction prefers. See the header preamble in each file.

## Pinning a version

Add `-DAE_DRLIBS_TAG=<git-ref>` at configure time to pin FetchContent to a
specific commit or tag.
