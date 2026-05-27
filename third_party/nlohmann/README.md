# nlohmann/json

This directory is where `json.hpp` lives. The audio engine uses
[nlohmann/json](https://github.com/nlohmann/json) (v3.11.3 pinned) for
JSON string-escape decoding in two parsers — `bus_config_loader.cpp`
and `sound_bank.cpp`. The library is not vendored in-tree because the
single header is ~25k lines and pulling it into every checkout is
friction; the same convention as miniaudio and the dr_libs decoders.

## Background

Prior to v0.80.0 those two parsers were hand-rolled and missing several
JSON spec escape sequences (`\u` in both; `\b` and `\f` in
bus_config_loader). The shipping FPS template contained `\u2014` and
`\u2192` in a `_comment` field, which made first-run installs fail for
users who selected the FPS preset. v0.80.0 delegates string-escape
decoding to nlohmann/json; the bug class is now structurally
impossible because the library is spec-compliant and CI exercises
every shipped JSON artifact through the loader.

See `CHANGELOG.md` v0.80.0 entry for the full incident note.

## Three ways to get json.hpp here

### 1. CMake's FetchContent (easiest)

With `AUDIO_ENGINE_FETCH_NLOHMANN_JSON=ON` (the default), CMake pulls
nlohmann/json from GitHub on first configure:

```bash
cmake -S . -B build
cmake --build build -j
```

Nothing else to do. The fetched header lands under `build/_deps/`.

### 2. Helper script (works without CMake)

```bash
./scripts/fetch_nlohmann_json.sh         # macOS / Linux / WSL
scripts\fetch_nlohmann_json.bat          # Windows cmd
```

Both download the pinned `v3.11.3` `json.hpp` from GitHub and drop it
into this directory. After that the file is on the include path for
both CMake and raw g++/clang/cl builds.

### 3. Manual (for offline / pinned builds)

Download `single_include/nlohmann/json.hpp` from
<https://github.com/nlohmann/json/releases/tag/v3.11.3> and place it
next to this README. For production builds, pin to a specific tag
rather than tracking `develop`.

## Verifying the install

```bash
ls third_party/nlohmann/json.hpp    # should print the path
```

If the file is present, CMake will use it directly (skipping
FetchContent).

## License

nlohmann/json is MIT-licensed. This project does not modify or
redistribute the source; you are fetching it directly from upstream.
