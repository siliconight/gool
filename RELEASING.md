# Releasing gool

This is the procedure for cutting a tagged release. The release
workflow (`.github/workflows/release.yml`) does the artifact build
automatically when a `v*` tag is pushed; the steps below get the
repo into a state where pushing the tag is safe.

## Versioning policy

Semantic Versioning. While the major version is `0`, minor bumps may
include backward-incompatible API changes — consult the release's
`Changed` section in `CHANGELOG.md` before upgrading. Once we ship
`1.0.0`, normal SemVer guarantees apply.

The version is recorded in three places that **must** stay in sync:

1. `include/audio_engine/version.h` — the `kVersionMajor` /
   `kVersionMinor` / `kVersionPatch` / `kVersionString` /
   `kVersionFull` constants
2. `CMakeLists.txt` — `project(audio_engine VERSION X.Y.Z ...)`
3. `tests/unit/version_test.cpp` — pinned values asserted by the
   test (catches drift between the above two)

The git tag (`vX.Y.Z`) is the fourth source of truth and is what
`release.yml` reads to name artifacts. The leading `v` is stripped;
the rest must equal the project version.

## Six-step release procedure

Substitute the version you're cutting for `X.Y.Z`.

### 1. Bump version in source

Edit all four locations:

```cpp
// include/audio_engine/version.h
constexpr int  kVersionMajor = X;
constexpr int  kVersionMinor = Y;
constexpr int  kVersionPatch = Z;
constexpr const char* kVersionString = "X.Y.Z";
constexpr const char* kVersionFull   = "X.Y.Z";
```

```cmake
# CMakeLists.txt
project(audio_engine
    VERSION X.Y.Z
    ...)
```

```cpp
// tests/unit/version_test.cpp
assert(v.major == X);
assert(v.minor == Y);
assert(v.patch == Z);
assert(std::strcmp(v.full, "X.Y.Z") == 0);
```

### 2. Update CHANGELOG.md

Move items from `## [Unreleased]` into a new `## [X.Y.Z] - YYYY-MM-DD`
section above it. Keep `## [Unreleased]` empty for the next cycle.

Add the version-link footnote at the bottom:

```markdown
[X.Y.Z]: https://github.com/siliconight/gool/releases/tag/vX.Y.Z
```

Update the `[Unreleased]` link to compare against the new tag.

### 3. Run the full test suite locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

All tests must pass. The `version_test` will catch any drift between
the three source-of-truth locations.

### 4. Commit + push

```bash
git add CHANGELOG.md CMakeLists.txt include/audio_engine/version.h \
        tests/unit/version_test.cpp
git commit -m "Release vX.Y.Z"
git push
```

Wait for CI on `main` to go green (Linux + Windows). If it's red,
fix forward with another commit before tagging.

### 5. Tag and push the tag

```bash
git tag -a vX.Y.Z -m "Release X.Y.Z"
git push origin vX.Y.Z
```

The leading `v` matters — `release.yml` triggers on `v*` and strips
the `v` to derive the artifact filename suffix.

### 6. Verify the GitHub Release

The release workflow runs on every `v*` tag and:

1. Builds `audio_engine` static library on each OS in the matrix
   (Linux, Windows; macOS deferred)
2. Stages headers + library + LICENSE + README
3. Archives as `gool-X.Y.Z-<platform>.<ext>`
4. Creates a GitHub Release named `vX.Y.Z` and attaches the
   archives

Open https://github.com/siliconight/gool/releases and confirm:

- The release page shows the new tag
- All expected platforms have artifacts attached
- The release notes auto-generated from the tag message look right
  (or paste the relevant `CHANGELOG.md` section if not)

## What to do when the release breaks

If `release.yml` fails (build break on a platform CI didn't catch,
artifact upload error, etc.), the safe recovery is:

1. Delete the tag locally and remotely:
   ```bash
   git tag -d vX.Y.Z
   git push origin :refs/tags/vX.Y.Z
   ```
2. Delete the partial GitHub Release (if one was created) via the
   web UI
3. Fix the issue with another commit on `main`
4. Re-tag and re-push

The version number stays — no need to bump again unless the fix
itself constitutes a backward-incompatible change.

## What goes in which version bump

This is a guide, not a rule. Use judgment.

| Bump type | When                                                            |
|-----------|-----------------------------------------------------------------|
| `+0.0.1`  | Bug fixes, doc updates, internal refactors, build-system tweaks |
| `+0.1.0`  | New public API, new prefab, new subsystem; non-breaking          |
| `+1.0.0`  | Renamed/removed public API, ABI break, behavior change that requires user code edits |

Pre-1.0 (which is where we are): minor bumps may include breaks
that would normally be major. Document them prominently in
CHANGELOG.md under `Changed` or `Removed`.

## Pre-releases

For larger work that wants to ship for evaluation before the stable
tag:

```cpp
// version.h
constexpr const char* kVersionSuffix = "-rc.1";
constexpr const char* kVersionFull   = "X.Y.Z-rc.1";
```

Tag as `vX.Y.Z-rc.1`. The release workflow handles `v*` tags so the
artifacts ship; consumers who pin to non-pre-release tags ignore them.

When the pre-release hardens into the stable cut, drop the suffix in
`version.h`, rebuild the changelog entry under the stable version
header, and tag `vX.Y.Z`.
