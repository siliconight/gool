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

The release workflow runs on every `v*` tag and produces **two
artifact families per platform**:

**1. C++ static library archive** — for users embedding the engine
in their own C++ build:

```
gool-X.Y.Z-linux-x86_64.tar.gz
gool-X.Y.Z-windows-x86_64.zip
```

Contents: `lib/libaudio_engine.{a,lib}`, `include/audio_engine/`,
LICENSE, README, CHANGELOG.

**2. Godot addon archive** — for users dropping gool into a Godot
project (the artifact 95% of adopters want):

```
gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz
gool-X.Y.Z-godot-addon-windows-x86_64.zip
```

Contents: an `addons/gool/` directory ready to copy into a Godot
project root, with the prebuilt GDExtension binary in
`addons/gool/bin/`. The adopter does not need a C++ compiler.

GitHub also auto-attaches its own `Source code (zip)` and `Source
code (tar.gz)` to every tag. Those are convenient but unbranded
beyond the project name (filename: `gool-X.Y.Z.tar.gz`, top-level
folder: `gool-X.Y.Z/`). They include the full repo state at the
tag — useful for archival but include `.git/` references and
unbuilt third-party fetches.

If you want a clean, deterministic source archive (filtering out
build artifacts, third-party fetches, and IDE noise), run
`scripts/make_source_archive.sh` (or `.ps1` on Windows) on a
fresh checkout. It produces `dist/gool-X.Y.Z.tar.gz` containing
`gool-X.Y.Z/` with only the deliberate ship-set:

```bash
./scripts/make_source_archive.sh
# → dist/gool-X.Y.Z.tar.gz (~500 KB)
```

The script reads the version from `include/audio_engine/version.h`
and uses that for the archive name and the top-level directory
inside, so the artifact is always self-labeling. This is the same
convention release.yml uses for compiled artifacts (`gool-X.Y.Z-PLATFORM`)
and the addon archive (`gool-X.Y.Z-godot-addon-PLATFORM`).

The workflow runs roughly:

1. Builds the `audio_engine` static library
2. Stages + packages the C++ archive
3. Sets up MSVC dev env (Windows), installs SCons, fetches the
   single-header dependencies via `scripts/fetch_*` scripts
4. Restores godot-cpp build from cache (key: platform + ref); on
   miss, clones godot-cpp at `GODOT_CPP_REF` (currently `4.2`)
   and builds it with SCons (5–20 min on cache miss)
5. Configures + builds the GDExtension via `cmake -S godot`
6. Stages `addons/gool/` + the binary into the addon archive
7. Uploads both archives to the GitHub Release

Open https://github.com/siliconight/gool/releases and confirm:

- The release page shows the new tag
- All four artifacts are attached (2 archive families × 2 platforms)
- The release notes auto-generated from the tag message look right
  (or paste the relevant `CHANGELOG.md` section if not)
- Smoke-test the addon archive: download
  `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`, extract `addons/gool/`
  into a Godot project, enable the plugin, verify `Gool` autoload
  initializes without error

If the godot-cpp branch needs to bump (Godot 4.2 → 4.3 etc), update
`GODOT_CPP_REF` in `.github/workflows/release.yml` AND
`.github/workflows/ci.yml` AND `.github/workflows/nightly.yml` AND
`compatibility_minimum` in `godot/gool.gdextension`. They must
match.

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

## Sustainability of the prebuilt-binary path

SETUP.md offers users two install tracks: prebuilt binaries (Track A,
the fast path) and build-from-source (Track B, the contributor path).
Track A is the one most adopters use. Keeping it actually working
is a maintenance commitment, not a one-time setup.

### Failure modes the maintainer should watch for

1. **Silent release.yml failures.** If the release workflow breaks
   on a platform that CI didn't catch (a Windows-only template
   instantiation that compiles on Linux, an MSVC-version-specific
   warning under `-Werror`, etc.), the tag still gets created, but
   the binary archive for that platform doesn't get attached to
   the GitHub Release. The quickinstall script then 404s on that
   platform. Users hit it before the maintainer does. Mitigation:
   after every `v*` tag push, manually verify the GitHub Releases
   page shows BOTH the engine archives AND the addon archives for
   ALL claimed platforms. If anything is missing, treat it as a
   release-blocker per "What to do when the release breaks" above.

2. **Drift between addon path and release.yml staging logic.**
   `release.yml` has its own copy of which addon files to bundle
   into the platform-specific addon archives. If a new prefab gets
   added to `godot/addons/gool/prefabs/` but `release.yml`'s file
   list isn't updated, the shipped addon will silently miss it.
   Users won't see the new prefab until they look. Mitigation: any
   change to the addon directory structure should grep for the
   addon paths in `release.yml` and check whether the staging logic
   needs an update. This is the kind of drift a CI guard could
   catch — see "Future improvements" below.

3. **Quickinstall script staleness.** `scripts/quickinstall.{ps1,sh}`
   downloads from "latest release" via the GitHub API. If the API
   endpoint or response shape ever changes, the scripts break. They
   also hard-code platform detection logic that could miss new
   platforms (e.g. Windows ARM64). Mitigation: the scripts are
   small; periodically run them on a clean Godot project on each
   supported platform to verify they still work end-to-end.

4. **Binary-Godot version compatibility.** GDExtension binaries
   are tied to the `godot-cpp` version they were built against,
   which is tied to a specific Godot minor version. A user running
   Godot 4.5 with a binary built against 4.4 godot-cpp may hit
   subtle ABI issues. SETUP.md currently claims "Godot 4.2 or
   newer"; if a future Godot version breaks compatibility, that
   text and the GODOT_CPP_REF env var in CI workflows need
   updating in lockstep.

### Future improvements (not yet implemented)

These would tighten sustainability but require their own engineering
work. Listed in rough priority order:

1. **CI smoke test for the published addon archive.** Add a job
   that runs on `v*` tag push, AFTER `release.yml` uploads the
   archives. It downloads the just-uploaded addon archive, extracts
   it into a fresh Godot project, runs `examples/coop_4p_minimal`
   in Godot headless mode, and verifies the validation banner
   reaches PASSED state. If the smoke test fails, the maintainer
   gets an email-alert from the failed workflow, well before any
   user hits the issue.

2. **Drift guard on `release.yml`'s addon staging logic.** A
   scanner (similar to `check_version_sync.sh`) that walks
   `godot/addons/gool/` and ensures every file is accounted for
   in `release.yml`'s archive-staging step. Catches "added a new
   prefab but forgot to update the release workflow" the same way
   the version-sync scanner catches "bumped version.h but forgot
   plugin.cfg".

3. **macOS x86_64 in the build matrix.** SETUP.md mentions macOS
   support, but only `macos-arm64` is in the CI matrix as of
   v0.81.6. Apple Silicon users covered; Intel macOS users have
   to fall back to Track B. Adding `macos-x86_64` to release.yml
   is straightforward but adds CI time and platform-specific
   maintenance.

4. **An "I just want to try gool" landing page.** A
   `docs/try_gool.md` (or section in the main README) targeted at
   developers who haven't decided whether to adopt gool, with the
   one-line install + 60-second validation walkthrough as the
   only content. Today this information is split across SETUP.md
   and `examples/coop_4p_minimal/README.md`, and a prospective
   adopter has to assemble it themselves.

5. **Godot Asset Library submission.** The most discoverable
   distribution channel for Godot addons. Requires (1) the smoke
   test above (asset library reviewers expect "open project, hit
   play, it works"), (2) versioning that survives the asset
   library's manual review cadence, and (3) some sustained
   willingness to update the submission alongside each release.
   Worth doing once gool stabilizes past 1.0; premature pre-1.0.
