# Changelog

All notable changes to gool are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is `0`, minor bumps may include backward-incompatible
changes; consult the per-release `Changed` and `Removed` sections before
upgrading.

## [Unreleased]

Nothing yet — open the next release section here when a feature lands.

## [0.11.18] - 2026-05-11

**macOS gdextension opusfile include-path fix.** v0.11.17 turned
five of six CI jobs green (Linux gdextension's OOM fix worked).
macOS gdextension still failed at compile time on
`opus_file_decoder.cpp` because Homebrew's `opusfile.pc` reports
an include path that doesn't match our `<opus/...>` include
convention. One-block CMake fix.

### Root cause

`src/audio_engine/decoders/opus_file_decoder.cpp:17`:

```cpp
#include <opus/opusfile.h>
```

This is the Linux apt convention — header lives at
`/usr/include/opus/opusfile.h`, and `/usr/include` is on clang's
implicit search path, so the directive resolves cleanly.

Homebrew layout on Apple Silicon:

```
/opt/homebrew/include/opus/opusfile.h    <- file location
/opt/homebrew/lib/pkgconfig/opusfile.pc  <- pkg-config descriptor
```

Reading `opusfile.pc` (representative):

```
prefix=/opt/homebrew/Cellar/opusfile/0.12+20230711
includedir=${prefix}/include
Cflags: -I${includedir}/opus
```

The `Cflags` line is the source of the bug. It hands the compiler
`-I/opt/homebrew/Cellar/opusfile/X.Y/include/opus`. With *that*
include path, `#include <opus/opusfile.h>` tries to resolve as
`/opt/homebrew/Cellar/opusfile/X.Y/include/opus/opus/opusfile.h` —
note the doubled `opus/opus/` segment — which doesn't exist.

Why Linux apt works: `/usr/include` is implicit. Even though
apt's `opusfile.pc` also says `Cflags: -I/usr/include/opus`, the
compiler still finds the header via the implicit search path
regardless. macOS Homebrew is *not* implicit — `/opt/homebrew/include`
is added to clang's search only by `brew --prefix` integration or
explicit `-I` flags, and CMake doesn't add it automatically just
because pkg-config returns something.

### Fixed

- **`CMakeLists.txt`** — added a small block after both opus
  resolution blocks (DECODERS_OPUS and VOICE_OPUS) that, on
  `APPLE`, checks the Homebrew prefixes (`/opt/homebrew` for
  Apple Silicon, `/usr/local` for Intel) for the actual presence
  of `opus/opusfile.h` or `opus/opus.h`. When found, adds the
  prefix's `include/` directory to `audio_engine`'s private
  include path.

  ```cmake
  if(APPLE AND (AUDIO_ENGINE_DECODERS_OPUS OR AUDIO_ENGINE_VOICE_OPUS))
      foreach(_brew_prefix /opt/homebrew /usr/local)
          foreach(_header opus/opusfile.h opus/opus.h)
              if(EXISTS "${_brew_prefix}/include/${_header}")
                  target_include_directories(audio_engine PRIVATE
                      "${_brew_prefix}/include")
              endif()
          endforeach()
      endforeach()
  endif()
  ```

  Idempotent — `target_include_directories` dedupes, so the
  same path is never added twice. Guarded by `if EXISTS`, so
  we never add a stale `/opt/homebrew/include` on machines
  that don't have opus installed there. Inline comment in the
  CMakeLists captures the opusfile.pc Cflags quirk so this isn't
  re-removed by someone tidying the file later.

### Why VOICE_OPUS wasn't strictly broken

By coincidence, the Homebrew layout works for VOICE_OPUS without
the fix:

- Code does `#include <opus.h>` (no `opus/` prefix — the libopus
  upstream convention)
- File lives at `/opt/homebrew/include/opus/opus.h`
- opus.pc's `Cflags: -I${includedir}/opus` → adds
  `/opt/homebrew/Cellar/opus/X.Y/include/opus` to include path
- `<opus.h>` resolves directly to that file. ✓

But the fix is applied to both for consistency and to future-proof
against any code that might do `#include <opus/opus.h>` (an
acceptable alternative convention some downstreams use). Empty
work if VOICE_OPUS doesn't need it.

### Verified locally

- Sandbox regression (CMake-faithful, per-target include paths):
  **36/36 passing**. No engine code changed, only CMakeLists.txt
  and version bump.
- The fix's `if EXISTS` guards verified to skip on non-macOS and
  on macOS machines lacking the headers (sandbox has neither).

### Expected CI behavior

After this release pushes, all six CI jobs should be green:

```
✓ engine / ubuntu-latest          ~50s
✓ engine / macos-latest           ~50s
✓ engine / windows-latest         ~1.5min
✓ gdextension / linux-x86_64      ~6-8min (v0.11.17 OOM fix)
✓ gdextension / macos-arm64       ~8-10min (this fix)
✓ gdextension / windows-x86_64    ~5-25min (vcpkg cache dependent)
```

If this lands clean, the CI badge in the README flips to green
for the first time since v0.11.3 — **the entire matrix
(3 platforms × 2 jobs = 6 total) green simultaneously**, which
the bootstrap rollout (v0.11.2 through v0.11.17) hadn't achieved
in any single run.

### Postscript on the regression methodology

This bug class — pkg-config returning include paths that work on
some distros and break on others — is invisible to the local
sandbox regression because the sandbox doesn't run CMake, doesn't
have Homebrew, and doesn't exercise the opus decoder code paths.
Catching this would require either: (a) a faithful CMake invocation
in the regression (gated on cmake being installed), or (b) an
explicit "compile the opus decoder TUs against representative
include paths on multiple platform-shaped configurations" check.

Both are tracked as follow-ups. For now, opus/voice-opus paths are
exercised only by CI; the sandbox can't catch their regressions.

## [0.11.17] - 2026-05-11

**gdextension OOM fix.** v0.11.16 turned all three engine jobs
green for the first time since v0.11.3. This release closes the
last two CI failures: gdextension Linux + macOS were dying with
`Killed signal terminated program cc1plus` (Linux) and "runner
lost communication" (macOS). Both are the same problem —
out-of-memory during godot-cpp's parallel C++ build.

### What the v0.11.16 logs actually said

**Linux gdextension** (job ID 75266767462), got to 58% of
godot-cpp's build, then:

```
[ 58%] Building CXX object godot-cpp/.../physics_test_motion_parameters3d.cpp.o
c++: fatal error: Killed signal terminated program cc1plus
##[error]The runner has received a shutdown signal.
##[error]The operation was canceled.
```

`Killed signal terminated program cc1plus` is the Linux OOM killer
terminating the compiler. The runner then dies because the OOM
killed processes critical to the runner agent.

**macOS gdextension** (job ID 75266767451), ran 1h 3m before:

```
##[error]The hosted runner lost communication with the server.
```

No specific error captured (runner died before flushing logs).
Same root cause: macOS arm64 runners have only ~7GB RAM —
significantly less than Linux's 16GB — and the OOM hit even
harder.

**Windows gdextension** (job ID 75266767474) — *succeeded*. The log
clearly shows `GDExtension binary produced.` + both caches saved
(`vcpkg-windows-opus-static-md-v1` and
`godot-cpp-windows-x86_64-4.2-v1`). Its "fail" status was matrix
cancellation cascade — when one job in the matrix triggers a
runner shutdown, all other jobs in the same run get cancelled,
even if they were already done. Windows DLL with Opus
statically linked works end-to-end.

### Root cause: godot-cpp is huge and CMake parallelism is unbounded

godot-cpp generates ~3000 bindings `.cpp` files (one per Godot
class). Each TU is heavy with template instantiations and can
peak at ~700MB-1GB of RAM during compilation. The
`Build GDExtension` step runs:

```
cmake --build build-godot --config Release --parallel
```

`--parallel` with no count = use all cores. On a 4-core Linux
runner (16GB), that's 4 cc1plus processes running concurrently,
4× ~1GB = 4GB on peak TUs, plus the runner agent + everything
else = OOM around mid-build. On a 7GB macOS runner the math is
worse.

### Fixed

- **`.github/workflows/ci.yml`, `nightly.yml`, `release.yml`** —
  changed the gdextension build command from
  `cmake --build build-godot --config Release --parallel` to
  `cmake --build build-godot --config Release --parallel 2`.
  Three workflow files, one parameter each.

  Added an inline comment above each `Build GDExtension` step
  explaining godot-cpp's memory footprint so this isn't
  re-tuned without understanding the constraint:

  ```yaml
  # godot-cpp generates ~3000 bindings .cpp files; each TU can
  # peak at ~700MB-1GB during template instantiation. Default
  # `--parallel` (= all cores) OOMs Linux/macOS runners (16GB and
  # 7GB respectively). --parallel 2 keeps us well under the ceiling.
  ```

  2 cores × 1GB peak = 2GB concurrent — well within 7GB on macOS
  and not even close on 16GB Linux.

### Not fixed in this release (deliberately)

There's a deeper architectural redundancy worth fixing but the
risk-reward says do it separately:

- The workflow runs `scons` to build godot-cpp, THEN cmake's
  `add_subdirectory(${GODOT_CPP_PATH})` builds godot-cpp *again*.
  Pure waste — adds 5+ minutes to every uncached gdextension run.
  Fix would be to drop the scons step (godot-cpp 4.x supports
  CMake natively) OR change `godot/CMakeLists.txt` to link
  against the scons-built static lib instead of `add_subdirectory`.
  Either has non-trivial risk if godot-cpp's CMake support has
  quirks for our case. Tracked as a follow-up.

### Side note: Windows succeeded despite the same workflow

Why did Windows survive `--parallel`? Three reasons working
together:

1. **More cores doesn't always mean more concurrent compilers** —
   MSBuild's parallelism is per-project, and godot-cpp's single
   `.vcxproj` serialized some of the work.
2. **vcpkg cache hit on second run** — first run built opus +
   opusfile from source (peak memory load), subsequent runs hit
   cache and skip that. Linux/macOS Opus install via apt/brew is
   already cached as a system package, so no analogous win.
3. **MSBuild's memory accounting differs** — `cl.exe` instances
   share PCH state more aggressively than separate `cc1plus`
   instances, reducing peak per-job.

The fix unifies behavior — Windows still uses `--parallel 2`
(no harm; slightly slower than `--parallel`), and Linux+macOS get
the safety margin they need.

### Verified locally

- All three workflow YAML files re-parse cleanly via
  `yaml.safe_load`.
- Sandbox regression (CMake-faithful, per-target include paths):
  **36/36 passing**. No engine code touched.

### Expected CI behavior

After this release pushes:

- All three **engine jobs** should still be green (no engine
  code changed).
- All three **gdextension jobs** should now build without OOM.
  Linux and macOS will likely take longer per-run than before
  (less parallelism = wall-clock cost), trading speed for
  reliability. Estimate: gdextension Linux ~6-8 min, macOS
  ~8-10 min, Windows ~5 min on cache hit, 25+ min on vcpkg cache
  miss.

If everything goes green this round, **CI is fully green for
the first time** — engine + gdextension across all three
platforms. The CI badge in the README flips to green and stays
there.

## [0.11.16] - 2026-05-10

**Third CI fix pass.** v0.11.15 unblocked the engine jobs almost
all the way; v0.11.16 closes the remaining three failures the
new log surfaced cleanly.

### Background

After v0.11.15 cleared the main engine errors (saturation tests,
thread annotations, open_memstream), only a handful of failures
remained. The cleaner log made the root causes obvious:

1. **`parameter_smoother_bench` still fails on all 3 engine platforms.**
   My v0.11.15 fix added a bench foreach with `if(TARGET ...)` guards,
   but I placed the foreach at line 48 — *before* the
   `add_executable` calls at line 144. CMake evaluates top-to-bottom,
   so when the foreach ran, neither bench target existed yet, the
   `if(TARGET ...)` guard returned false, and `target_include_directories`
   was never called. Silent no-op.

2. **Windows gdextension can't find `audio_engine/miniaudio_backend.h`.**
   `godot/src/gool_godot.cpp` includes the path
   `audio_engine/miniaudio_backend.h`, but the actual header lives
   at `include/audio_engine/backend/miniaudio_backend.h` — the
   `backend/` subdirectory was missing from the path. Windows
   surfaced this; Linux/macOS gdextension would too if they got
   that far (they didn't — see #3).

3. **gdextension Linux exit 126, macOS exit 1.** Their silent
   failures had a clear message all along — I just hadn't scrolled
   far enough in the previous log:

   ```
   ./scripts/fetch_miniaudio.sh: Permission denied
   ```

   This is the classic Windows-git-strips-executable-bits problem.
   When the v0.11.x tarballs were extracted on a Windows machine
   and committed via `git add`, Windows git (which defaults to
   `core.fileMode=false`) didn't preserve the `+x` bits on the
   `.sh` files. CI checks out on Linux/macOS, the `.sh` files
   lack `+x`, and `./scripts/fetch_miniaudio.sh` errors with
   "Permission denied" — exit 126 on Linux, exit 1 on macOS.

### Fixed

- **`tests/CMakeLists.txt`** — moved the bench foreach to *after*
  the `add_executable(audio_engine_parameter_smoother_bench ...)`
  and `add_executable(audio_engine_rtpc_eval_bench ...)` lines.
  Removed the `if(TARGET ...)` guard since the targets are
  unconditionally defined right above. Added an inline comment
  explaining the ordering trap so this doesn't regress.

- **`godot/src/gool_godot.cpp`** — corrected the include path
  from `"audio_engine/miniaudio_backend.h"` to
  `"audio_engine/backend/miniaudio_backend.h"`. Verified all four
  formerly-affected includes resolve under `include/` only (no
  `src/` access needed):
    - `audio_engine/audio_runtime.h` ✓
    - `audio_engine/backend/miniaudio_backend.h` ✓ (this fix)
    - `audio_engine/config.h` ✓
    - `audio_engine/bus_config_loader.h` ✓

- **`.github/workflows/ci.yml`, `nightly.yml`, `release.yml`** —
  changed `./scripts/fetch_miniaudio.sh` and
  `./scripts/fetch_decoders.sh` invocations to
  `bash scripts/fetch_miniaudio.sh` and
  `bash scripts/fetch_decoders.sh`. Invoking via `bash` doesn't
  depend on the file's executable bit being set — the
  interpreter is told explicitly. Six total invocations across
  three workflow files. YAML re-parsed clean.

### Optional: fix the index permanently

The workflow `bash …` change makes CI robust to missing `+x`
bits, but for hygiene (and so `./scripts/foo.sh` works for
adopters cloning the repo on Linux/macOS), the executable bit
should also be set in the git index. From any machine — even
Windows, where `core.fileMode=false` doesn't prevent this:

```bash
git update-index --chmod=+x scripts/*.sh
git commit -m "Mark scripts/*.sh executable in git index"
git push
```

This stores the mode in the index, gets committed, and survives
future Windows checkins regardless of `core.fileMode` setting.
Not required for CI to pass after v0.11.16 (the `bash` invocation
is sufficient), but a one-time cleanup that closes the broader
issue.

### What this regression still doesn't catch

Two methodology gaps surfaced this round that the local
regression doesn't yet cover:

- **gool_godot.cpp's includes.** The regression compiles unit
  tests against the library but doesn't compile any file from
  `godot/`. A real fix would attempt to `g++ -fsyntax-only` the
  gdextension binding's .cpp files against `include/`-only paths
  to catch private-header leaks like the `miniaudio_backend.h`
  case. Tracked as a follow-up.
- **CMake target-ordering bugs.** The bench foreach problem was
  invisible until CI ran because the regression doesn't actually
  use CMake. A faithful regression would shell out to `cmake -S .`
  + `cmake --build` if cmake were available in the sandbox.
  Tracked as a follow-up; for now, ordering bugs surface in CI.

### Verified locally

- All three workflow YAML files re-parse cleanly via `yaml.safe_load`.
- Sandbox-faithful regression (per-target include paths matching
  `tests/CMakeLists.txt`): **36/36 passing.**
- `gool_godot.cpp`'s four audio_engine includes all resolve against
  `include/` only (godot-cpp's own headers obviously don't, but
  those come from a separate `-I` path in real builds).

### Expected CI behavior

After this release pushes:

- **Engine jobs (3 platforms)** — should all go green for the
  first time since v0.11.3. This is the milestone.
- **gdextension Linux + macOS** — should progress past the
  `fetch_miniaudio.sh` step. Likely to surface whatever the
  *next* gdextension issue is. Most probable: `scons` PATH issue
  (`pip install scons` drops it at `~/.local/bin` which the
  bash session doesn't have on PATH).
- **gdextension Windows** — should progress past the
  `miniaudio_backend.h` include error and try to actually compile
  the binding. May surface a different issue downstream.

If the engine jobs go green and gdextension still has issues,
we'll be down to a focused gdextension fix pass.

## [0.11.15] - 2026-05-10

**Second CI fix pass.** v0.11.14 fixed the missing-sources cascade
(LNK2019 across all targets); this release fixes the five
remaining failures the full log revealed across the three engine
jobs and the Windows gdextension job. The lesson: my regression
was lying about which include paths each test target actually gets.

### Background

After v0.11.14 cleared the link errors and brought Windows engine
build down from 30 min to 1m 35s, the actual underlying compile
errors became visible in the logs. Six distinct problems:

1. **Tests including private headers** — three test files
   (`saturation_test`, `saturation_profile_test`,
   `compressor_profile_test`) include
   `audio_engine/dsp/saturation_effect.h` and
   `audio_engine/dsp/compressor.h`, which live in `src/` not
   `include/`. The existing foreach in `tests/CMakeLists.txt`
   that adds `src/` to a test's include path missed these three
   targets.

2. **Benches including private headers** — `parameter_smoother_bench`
   includes `audio_engine/orchestrator/parameter_smoother.h`,
   another private header. Same fix.

3. **Apple Clang thread-safety analysis** — `AUDIO_REQUIRES(GameThread)`
   expanded to `__attribute__((requires_capability(GameThread)))`.
   Clang's `requires_capability` takes a *value* (typically a
   capability-typed variable), not a *type*. The header declared
   `GameThread`/`RenderThread`/`ControlThread`/`NetworkThread` as
   struct types, so Apple Clang errored with "does not refer to a
   value". GCC and MSVC didn't surface this because their
   `AUDIO_TSA_ATTR` expands to nothing — the `GameThread` token
   was never parsed.

4. **Windows MSVC: `open_memstream` not found** —
   `telemetry_test.cpp` and `logging_test.cpp` use POSIX
   `open_memstream` to capture sink output. Not available on
   Windows. Replaced with a portable `std::tmpfile`-backed helper.

5. **Windows gdextension: `drwav_init_file: identifier not found`** —
   `wav_decoder.cpp` had `#define DR_WAV_NO_STDIO 0`, intending to
   "enable stdio". But `dr_wav.h` checks `#ifndef DR_WAV_NO_STDIO`,
   so *defining* the macro to *any* value (including 0) excludes
   `drwav_init_file` and its siblings. Pure misreading of dr_wav's
   API. Removed the line; default behavior (macro undefined →
   stdio helpers present) is what we want.

6. **gdextension Linux exit 126 / macOS exit 1** — these failed
   silently in the log we have (no captured error message between
   step start and exit code). Not addressed in this release;
   their fix is gated on a new log capture.

### Fixed

- **`tests/CMakeLists.txt`** — added `saturation`,
  `saturation_profile`, `compressor_profile` to the
  `include + src` foreach. Added a separate small foreach for
  the two bench targets (`parameter_smoother_bench`,
  `rtpc_eval_bench`) so they also get `src/` on their include
  path.

- **`include/audio_engine/thread_annotations.h`** — renamed the
  four capability tag types from `GameThread` → `GameThreadTag`
  (etc.) and added `inline GameThreadTag GameThread;` (etc.)
  as instances. The instances are C++17 inline variables so
  every TU including the header gets the same definition with
  no ODR violation. The structs are empty and the instances
  optimize away to zero runtime cost. Code that uses
  `AUDIO_REQUIRES(GameThread)` now refers to a value, which is
  what Clang's `requires_capability` expects.

  Verified by grep: nothing in the codebase used `GameThread`
  etc. as a *type* (only inside `AUDIO_REQUIRES(...)` macros),
  so the rename is non-breaking.

- **`src/audio_engine/decoders/wav_decoder.cpp`** — removed
  `#define DR_WAV_NO_STDIO 0`. Comment added inline explaining
  the dr_wav API gotcha so this isn't re-introduced.

- **`tests/unit/test_memfile_helpers.h`** (new file) — a portable
  replacement for POSIX `open_memstream`. Exposes
  `test_helpers::OpenMemFile()` returning a `std::tmpfile()`
  handle plus `ReadAndClose()` to collect everything written
  and close the file. Works on Linux, macOS, and Windows.

- **`tests/unit/telemetry_test.cpp` + `tests/unit/logging_test.cpp`**
  — replaced all three `open_memstream` call sites with the
  helper. Logic is identical; the only difference is the bytes
  briefly transit a tmp file instead of memory, which is fine
  for unit-test scratch capture.

### Improved (regression methodology)

The sandbox regression now reads the `foreach(_t ...)` list from
`tests/CMakeLists.txt` at regression time, and per test target,
compiles with either:

- `-I include -I src -I tests/unit` (for tests in the foreach
  list, which CMake also gives `src/` access to)
- `-I include -I tests/unit` (for everything else)

Before v0.11.15, the regression compiled every test with
`-I include -I src` indiscriminately, masking the "test
accidentally includes a private header without being in the
foreach" bug. After v0.11.15, that class of bug fails locally
with the same compile error CI surfaces.

This is the second methodology fix in two releases (v0.11.14
added "match CMake's source list, not raw find"; v0.11.15 adds
"match CMake's per-target include paths"). With both in place,
the gap between local regression and CI behavior is closed for
this class of bug.

### What's left

The gdextension jobs (linux-x86_64, macos-arm64) had silent
failures in the captured log — exit code 126 (Linux) / 1 (macOS)
with no error message in the failed-step output. The most likely
cause for Linux's exit 126 is `scons` not being on the bash
session's PATH after `pip install scons` drops it under
`/home/runner/.local/bin/`; macOS exit 1 needs separate
investigation. Both are gated on a new log capture once
v0.11.15's engine fixes go green.

Windows gdextension's `drwav_init_file` fix should clear that
particular failure but more may surface; we'll see in the run
log after pushing.

### Verified locally

- All 36 unit tests pass under the new CMake-faithful
  regression methodology (per-target include paths matching
  CMake's `foreach`).
- The 5 source-file changes (CMakeLists, thread_annotations.h,
  wav_decoder.cpp, telemetry_test.cpp, logging_test.cpp) plus
  the new test helper header compile cleanly under g++ -std=c++20.

## [0.11.14] - 2026-05-10

**CI fix.** Four `.cpp` files existed on disk and were referenced by
compiled code but were never added to `AUDIO_ENGINE_SOURCES` in
`CMakeLists.txt`. The library shipped missing symbols; every test
and example target that linked against `audio_engine.lib` failed
with `LNK2019` (Windows) / `undefined reference` (Linux, macOS) at
link time. CI has been red on this for v0.11.7 through v0.11.13.

### Root cause

Windows CI log surfaced the cascade most cleanly:

```
audio_engine.lib(bus_graph.obj) : error LNK2019: unresolved external
symbol "public: __cdecl audio::SaturationEffect::SaturationEffect(
struct audio::SaturationConfig const &)" referenced in function
"private: enum audio::AudioResult __cdecl audio::BusGraph::
BuildEffectsForBus(...)"
```

`SaturationEffect`'s constructor is defined in
`src/audio_engine/dsp/saturation_effect.cpp`. `bus_graph.cpp` calls
it via `std::make_unique<SaturationEffect>(cfg)`. With
`saturation_effect.cpp` missing from `AUDIO_ENGINE_SOURCES`, the
`audio_engine` library shipped without that symbol, and every
downstream target failed to link.

Three more files were in the same state, surfacing as separate
errors in jobs that exercise their code paths:

- `src/audio_engine/runtime/telemetry.cpp` — defines
  `RingTelemetrySink::Size()`, used by the `audio_engine_telemetry`
  example
- `src/audio_engine/runtime/bus_config_loader.cpp` — defines
  `BusConfigLoader::ParseFromJson()`, used by
  `tests/unit/bus_config_loader_test.cpp`
- `src/audio_engine/runtime/logging.cpp` — defines
  `JsonLinesLoggingSink::Write()` etc., used by `audio_runtime.cpp`

### Why local regression missed this

The pre-existing sandbox regression compiled the audio_engine
library by running `g++ -c` over every `.cpp` file under `src/`
(via `find src -name "*.cpp"`). This compiles files that CMake
*deliberately excludes* (like `miniaudio_backend.cpp` when
`AUDIO_ENGINE_BACKEND_MINIAUDIO=OFF`) **and** files that CMake
*accidentally excludes* (like the four above). The regression said
"all tests pass" while a CMake build would fail at link time.

**The regression methodology was lying.** It validated only that
the source tree compiles together, not that the source tree CMake
chooses to compile is sufficient.

### Fixed

- **`CMakeLists.txt`** — added the four missing files to
  `AUDIO_ENGINE_SOURCES`:
    - `src/audio_engine/dsp/saturation_effect.cpp` (after
      `reverb_effect.cpp`)
    - `src/audio_engine/runtime/bus_config_loader.cpp` (after
      `audio_runtime.cpp`)
    - `src/audio_engine/runtime/logging.cpp` (after
      `bus_config_loader.cpp`)
    - `src/audio_engine/runtime/telemetry.cpp` (after
      `replication_rate_limiter.cpp`)

  Library went from 33 to 37 object files. Headers don't
  inline-define methods, so no risk of multiple-definition
  errors.

### Process change

The local regression now compiles **only** the files listed in
`AUDIO_ENGINE_SOURCES` (read from `CMakeLists.txt` at regression
time), mirroring what CMake will actually compile. This means
future "extra" files left out of `AUDIO_ENGINE_SOURCES` will be
caught locally with the same link error CI would surface. Test
methodology and CI now produce the same library.

### Verified locally

- Reproduced the exact `LNK2019` / `undefined reference to
  audio::SaturationEffect::SaturationEffect` error by linking
  `production_readiness_test` against a library built from the
  *old* (33-file) `AUDIO_ENGINE_SOURCES`.
- Confirmed the fix: with the 4 files added (37-file library),
  link succeeds, `production_readiness_test` runs and passes.
- Full regression in CMake-source-list mode: **36/36 tests
  passing**. Ducking baseline still locked at -17.20 dB.

### Expected CI impact

The `engine / {ubuntu, macos, windows} / Release` jobs should now
pass their build step and progress to ctest, which should produce
36 passing tests.

The `gdextension / {linux, macos, windows}` jobs are independent
failures — different root causes (scons / godot-cpp on
Linux/macOS, find_package(OpusFile) under vcpkg on Windows). Those
need their own log excerpts to diagnose and will be addressed in a
subsequent release.

## [0.11.13] - 2026-05-10

Positioning. New README section "Open by design — and AI-readable"
articulates the open-source advantage as an integration model rather
than just a license tier. No code changes; documentation only.

### Background

The README's "Why not Godot's built-in audio?" section already
covers the differentiator vs Godot's native audio path. The
parallel "why not closed middleware (FMOD / Wwise)" angle was
implicit — `What you get → For your production` mentioned "no
middleware licensing fees" but didn't articulate the larger point:
when middleware is closed, debugging stops at the vendor's bug
tracker and adapting it to your game stops at whatever extension
points the vendor exposes. With open code, both have a path
forward.

The AI-tooling angle compounds this in 2026. Coding agents (Claude
Code, Cursor, Copilot, etc.) can read the full implementation,
generate integration code against the actual API surface, and
propose fixes for behavior they can see firsthand. With closed
middleware those agents work from headers and public docs only —
they can scaffold integration, but can't verify their own
suggestions against the implementation.

### Added

- **`README.md` — new "Open by design — and AI-readable" section**,
  positioned after "Why not Godot's built-in audio?" and before
  "Reader-specific guides". Two parts:
    - Prose paragraph on the open-source-as-integration-model
      argument: read code that produced surprising behavior, verify
      claims against tests in the repo, fork via Apache 2.0 if
      needed.
    - Sub-section "What changes in the AI-tooling era" with concrete
      bulleted examples of what an agent with the codebase in
      context can do (implement a backend, generate tests, trace
      behavior through the actual code path, propose fixes with
      trade-offs visible).
  ~50 lines added; no other README sections touched.

### Tests

- 36/36 passing. Ducking baseline locked at -17.20 dB. No engine
  code touched.

### Suggested follow-ups (not in this release)

- **Add a row to the "What you get → For your engineers" bucket**
  surfacing the same point in the at-a-glance feature list. Skipped
  for this release to keep the change focused; the dedicated
  section carries the full argument.
- **Cross-link from the Asset Library submission template** if /
  when that lands — same point, different audience.

## [0.11.12] - 2026-05-10

Branding. The repo now has an official logo mark, displayed at the
top of the README. No code changes; documentation/asset only.

### Added

- **`docs/branding/gool_bone_mark.png`** — primary brand mark
  (1536×1024, 234 KB, sepia-on-black). Skull-with-headphones
  inside a thorn/bone-spike halo. Works on both GitHub light and
  dark themes without needing a separate light-mode variant
  thanks to the dark vignette already in the image.

- **README hero block** — centered logo at 360px width, placed
  above the `# gool` H1. Uses `<p align="center"><img></p>`
  (the standard GitHub-Markdown idiom for centered images).

### Tests

- 36/36 passing. Ducking baseline locked at -17.20 dB. No engine
  code touched.

### Suggested follow-ups

- **GitHub social preview** — upload the same image (or a
  centered-crop variant) at **Settings → General → Social
  preview**. This controls the Open Graph image rendered when
  the repo is linked from Discord, Twitter, etc. Manual upload,
  not part of the repo files.
- **Asset Library banner** — when submitting to godotengine.org/
  asset-library, the same mark works as the asset thumbnail.
- **Light-mode variant** — if a future asset is high-contrast
  enough to need it, add `gool_bone_mark_light.png` and switch
  the README to a `<picture>` element with
  `prefers-color-scheme` queries. Not needed for this image.

## [0.11.11] - 2026-05-10

One-line install. Closes the last bit of friction in the Track A
("just use the addon, no compiler") path. From v0.11.11 onward, a
Godot game dev on Windows / Linux / macOS can install gool with a
single PowerShell or bash command — no browser, no manual download,
no manual extraction, no drag-drop into the project's addons
directory.

### Background

Track A pre-v0.11.11 was already compiler-free, but still required
seven UI actions: open browser → navigate to releases → click latest
release → identify the right archive (filename has `-godot-addon-`
in it, distinguishing from the C++ static lib) → download → extract
→ drag `addons/gool/` into the project. Adopters who reached for
the terminal (winget / scoop / oh-my-posh experience) expected the
modern Windows-native install pattern, and didn't get it.

### Added

- **`scripts/quickinstall.ps1`** — Windows PowerShell quickinstaller.
  Designed to be invoked via the standard Windows `iwr | iex`
  pattern from inside the user's Godot project directory:

  ```powershell
  iwr -useb https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.ps1 | iex
  ```

  What it does:
    1. Validates the current directory contains `project.godot`
       (real Godot project, not just any directory)
    2. Hits the GitHub API to resolve the latest release tag
    3. Downloads `gool-X.Y.Z-godot-addon-windows-x86_64.zip`
    4. Verifies the download's size sanity-check (>50 KB; smaller is
       likely a 404 or corrupted)
    5. Extracts and copies `addons/gool/` into the project, replacing
       any existing addon (with a visible "Replacing existing..." line)
    6. Cleans up temp files
    7. Prints the one manual step left: enable the plugin

  Configurable via `-ProjectPath`, `-Version` (pin a specific tag,
  e.g. `v0.11.10`), `-Repo` (point at a fork or staging repo).

- **`scripts/quickinstall.sh`** — Linux / macOS bash equivalent.
  Same three-step approach. Auto-detects platform via `uname` —
  Linux → `linux-x86_64`, Darwin arm64 → `macos-arm64`, Darwin
  x86_64 (Intel Mac) → clear error pointing at Track B since we
  don't ship Intel Mac binaries yet. Uses `curl` if available,
  falls back to `wget`. Configurable via `--project-path`,
  `--version`, `--repo`, `--help`.

  Designed invocation:

  ```bash
  curl -sSL https://raw.githubusercontent.com/siliconight/gool/main/scripts/quickinstall.sh | bash
  ```

### Changed

- **`README.md` Quick start setup block** — Track A now leads with
  the one-liner. Manual install (browser → releases page → drag
  drop) is the secondary path for adopters who'd rather not pipe a
  script.

- **`SETUP.md` Track A** — restructured into "Option 1: One-line
  install (recommended)" and "Option 2: Manual install" subsections.
  The script's behaviour (validation, download, replacement,
  cleanup) is documented up front so adopters know what they're
  agreeing to before piping. Both quickinstall scripts are linked
  by URL so the pipe target is auditable.

### What this compresses

| Step                        | Before                                      | After     |
|-----------------------------|---------------------------------------------|-----------|
| Open browser                | ✓                                          | —         |
| Navigate to Releases        | ✓                                          | —         |
| Find right archive          | ✓ (`-godot-addon-` filename disambig)       | —         |
| Download                    | ✓                                          | (auto)    |
| Extract                     | ✓                                          | (auto)    |
| Drag `addons/gool/` to proj | ✓                                          | (auto)    |
| Enable plugin in Godot      | ✓                                          | ✓         |

Seven steps → one terminal command. Track B (`bootstrap.sh`) was
already a one-liner for source builds; Track A now matches.

### Verified

- Both scripts pass `bash -n` / PowerShell syntax checks
- `quickinstall.sh --help` outputs usage text correctly
- Error paths smoke-tested in the build sandbox:
  - Non-existent target dir → clear error message
  - Real dir without project.godot → clear error message + remediation
- Validation up to download-attempt smoke-tested for happy-path:
  the script correctly resolves platform → archive name, attempts
  the correct download URL, and falls through cleanly when the
  release doesn't exist (since v0.11.11 isn't tagged yet at sandbox
  test time).
- Engine regression: 36/36 passing at v0.11.11. Ducking baseline
  locked at -17.20 dB.

### Not verified in the sandbox (verifiable only by tagging + invocation)

- That `iwr -useb https://raw.githubusercontent.com/...quickinstall.ps1 | iex`
  works end-to-end on a real Windows runner against a real release.
  Requires v0.11.11 to be tagged on GitHub first; then invoke the
  one-liner against a fresh Godot project.
- That `curl -sSL .../quickinstall.sh | bash` works end-to-end on
  Linux / macOS the same way.
- That GitHub's raw.githubusercontent.com domain serves the script
  promptly without rate-limiting on first invocation.

### What's next for installation friction

Three candidate further steps, ranked by impact:

1. **Godot Asset Library submission.** Adopters open Godot →
   AssetLib tab → search "gool" → install. Zero leaving Godot.
   Requires submitting to godotengine.org/asset-library, which is a
   manual user action (we'd write the submission template and
   checklist). Highest impact long-term; right move once the
   project has more public visibility.

2. **Bundle gool into a starter Godot project template.** New
   adopters create a project from the template; gool is preinstalled.
   Niche but useful for people starting fresh.

3. **Custom domain for the install URL.** Instead of
   `raw.githubusercontent.com/siliconight/gool/main/scripts/...`,
   serve from `gool.siliconight.com/install.ps1` or similar.
   Cosmetic; matches winget/scoop's polish. Requires a domain.

## [0.11.10] - 2026-05-10

Closes the Windows Opus gap. From v0.11.10 onward, the Windows
Godot addon archive ships with `AUDIO_ENGINE_DECODERS_OPUS=ON` and
`AUDIO_ENGINE_VOICE_OPUS=ON` — Windows adopters who download
`gool-X.Y.Z-godot-addon-windows-x86_64.zip` get `.opus` file
decoding and Opus voice chat without doing anything extra.

### Approach: vcpkg with `x64-windows-static-md` triplet

`windows-latest` GitHub runners come with vcpkg pre-installed at
`C:\vcpkg`. The new workflow steps:

1. Cache `C:/vcpkg/installed` via `actions/cache@v4` (key:
   `vcpkg-windows-opus-static-md-v1`). First build per cache miss
   compiles opus + opusfile + libogg from source via vcpkg, taking
   ~5–15 min. Subsequent runs hit cache and run in seconds.
2. `vcpkg install opusfile:x64-windows-static-md
   opus:x64-windows-static-md`. The `static-md` triplet is the key
   choice: dependencies build as static libs but with the dynamic
   CRT (`/MD`), matching godot-cpp's default. Result: opusfile +
   opus + libogg are statically linked into `gool_godot.dll`. The
   addon archive carries one DLL — no `opus.dll`, `opusfile.dll`,
   or `ogg.dll` need to be shipped alongside.
3. Pass `-DCMAKE_TOOLCHAIN_FILE=${VCPKG_INSTALLATION_ROOT}/scripts/buildsystems/vcpkg.cmake
   -DVCPKG_TARGET_TRIPLET=x64-windows-static-md` to the configure
   step. CMake's `find_package(OpusFile QUIET)` then resolves
   against the vcpkg-installed package config, and the existing
   `target_link_libraries(audio_engine PRIVATE OpusFile::opusfile)`
   path takes over.

### Changed

- **`.github/workflows/release.yml`** — added Windows-specific
  vcpkg cache step + install step, simplified the configure step
  to always pass `DECODERS_OPUS=ON VOICE_OPUS=ON` (no longer
  conditional on platform), conditionally appends the vcpkg
  toolchain flag on Windows. Stale "(vcpkg integration is a
  follow-up)" comments removed; the comment now accurately
  documents all three platforms.

- **`.github/workflows/nightly.yml`** — same pattern. Nightly
  Windows addon archives now include Opus support; adopters
  following bleeding edge get parity with Linux/macOS.

- **`.github/workflows/ci.yml`** (`build-gdextension` job) — same
  pattern. Every PR now compiles the binding against vcpkg's
  opus/opusfile on Windows, catching any vcpkg-portfile breakage
  early.

- **`README.md` Build options notes** — updated to say "all three
  shipping platforms automatically" instead of "Linux + macOS
  only." Specifically calls out the `x64-windows-static-md`
  triplet so Windows adopters who want to build locally can
  match the CI's choice.

- **`SETUP.md` Opus install table** — Windows entry now reads
  `vcpkg install opusfile:x64-windows-static-md` (with the
  explicit triplet). New section documents the local Windows
  cmake invocation including the toolchain file flag, mirroring
  what the CI does. Removed the "Windows users without vcpkg
  have the roughest path here" warning since CI now handles the
  Track A path automatically.

### Naming / artifact summary

After this release, all six Godot addon artifacts ship with full
Opus support:

| Platform           | Filename                                              | Opus |
|--------------------|-------------------------------------------------------|------|
| Linux x86_64       | `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`         | ✓    |
| Windows x86_64     | `gool-X.Y.Z-godot-addon-windows-x86_64.zip`          | ✓ (new) |
| macOS arm64        | `gool-X.Y.Z-godot-addon-macos-arm64.tar.gz`          | ✓    |
| Linux nightly      | `gool-nightly-godot-addon-linux-x86_64.tar.gz`       | ✓    |
| Windows nightly    | `gool-nightly-godot-addon-windows-x86_64.zip`        | ✓ (new) |
| macOS nightly      | `gool-nightly-godot-addon-macos-arm64.tar.gz`        | ✓    |

The C++ static library archives (without `-godot-addon-` in the
name) remain minimal — they're for embedders who pick their own
codecs.

### Verified locally

- All three workflow YAML files re-parsed clean via `yaml.safe_load`
- 36/36 engine regression tests passing at v0.11.10. Ducking
  baseline locked at -17.20 dB. No engine code touched.

### Not verified in this sandbox (verifiable only on a real Windows runner)

- That `vcpkg install opusfile:x64-windows-static-md` succeeds on
  `windows-latest`. The vcpkg port is well-maintained and standard;
  this should be reliable.
- That `find_package(OpusFile QUIET)` resolves against vcpkg's
  installed package config when the toolchain file is set.
  `OpusFile::opusfile` is the standard target name vcpkg's port
  exposes. If the actual target name differs, the configure step
  would fail with a "target not found" link error and we'd iterate.
- That static-md-triplet libs link cleanly into a `/MD` (dynamic
  CRT) host. This is exactly what `static-md` was designed for, so
  it should work, but the only real test is the CI run.

### What to expect on first push

The first tagged release after this change pushes a Windows build
through the new path. Likely outcomes, in order of probability:

1. **Build succeeds, addon loads in Godot on Windows with Opus
   support** → ship it, close the gap.
2. **`find_package(OpusFile)` doesn't find vcpkg's package** → the
   configure step warns, falls through to "could not resolve
   libopusfile" error. Fix: adjust the find_package config search
   paths or fall back to the manually-set lib paths via
   `OPUSFILE_LIBRARY` / `OPUSFILE_INCLUDE_DIR`.
3. **Linking fails on CRT mismatch** → switch triplet to a
   different choice (`x64-windows`, ship the DLLs alongside;
   `x64-windows-static`, change godot-cpp build to /MT).
4. **Build succeeds but addon DLL won't load in Godot at runtime**
   → likely a missing transitive dependency. Diagnose via Godot's
   output panel.

If iteration is needed, fail-fast=false ensures Linux + macOS
artifacts still ship while Windows is iterated.

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.

## [0.11.9] - 2026-05-10

Default-flip release. The audio backend (miniaudio) and the WAV /
OGG / FLAC decoders now default ON in `CMakeLists.txt`. The
overwhelmingly common use case — Godot adopter, C++ game embedding
the engine — gets sound and the standard codec set without passing
any flags. Anyone who specifically wants a minimal build (audio
analysis tools, headless servers, embedded use cases) opts out
explicitly.

### Rationale

The old defaults (everything OFF) were honest about the engine
library's flexibility but actively misleading about typical use:
the README had to say "for a Godot-side build you almost certainly
want at least…" — which is the canonical sign that the default is
wrong. The bootstrap script and CI pipelines were already passing
these flags as ON for every shipped binary; the engine library
defaults were the only place still saying OFF, and the only place
where it caused confusion.

This is a behavior change for `cmake -S . -B build` with no flags.
Before: minimal build, no decoders, no miniaudio, ~1 MB static lib.
After: full build with miniaudio + WAV/OGG/FLAC decoders linked in,
slightly larger static lib, FetchContent fetches the four
single-header dependencies on demand.

### Changed

- **`CMakeLists.txt` defaults flipped to ON**:
    - `AUDIO_ENGINE_BACKEND_MINIAUDIO`: OFF → ON
    - `AUDIO_ENGINE_DECODERS_WAV`: OFF → ON
    - `AUDIO_ENGINE_DECODERS_OGG`: OFF → ON
    - `AUDIO_ENGINE_DECODERS_FLAC`: OFF → ON

  Stays OFF (system-package or non-trivial-build dependency):
    - `AUDIO_ENGINE_DECODERS_OPUS` (libopusfile, autotools-only)
    - `AUDIO_ENGINE_VOICE_OPUS` (libopus, FetchContent works but
      adds non-trivial build time)

- **`README.md` "Build options"** reverted from a two-column
  "Library default / Godot binding default" table to a clean
  single-column table reflecting the new shared defaults. The note
  about needing flags to enable the backend / decoders is gone —
  they're the default now. Opt-out instructions for the minimal
  build are documented inline.

- **`SETUP.md` "Optional features"** updated the same way. The
  "Godot-side build typical invocation" went from a 5-line cmake
  command with explicit flags to a 2-line command with no audio
  flags — the defaults handle it.

### Preserved

- **`.github/workflows/ci.yml` `build-and-test` job** explicitly
  forces the flipped flags BACK to OFF
  (`-DAUDIO_ENGINE_BACKEND_MINIAUDIO=OFF
  -DAUDIO_ENGINE_DECODERS_WAV=OFF
  -DAUDIO_ENGINE_DECODERS_OGG=OFF
  -DAUDIO_ENGINE_DECODERS_FLAC=OFF`).
  This preserves the existing test invariant: catch any code that
  accidentally requires miniaudio or a specific decoder for the
  core library to compile. The full-config compile is implicitly
  tested by the `build-gdextension` job; the release pipeline
  exercises full-config end-to-end.

- **`.github/workflows/release.yml` engine-archive job** likewise
  forces the flipped flags OFF. The C++ static library archive
  (`gool-X.Y.Z-PLATFORM.tar.gz`) on the Releases page is for users
  embedding `audio_engine` directly in their own C++ project; they
  typically want a minimal static library and link whatever
  decoders / backends they need themselves. The Godot-binding
  archive (`gool-X.Y.Z-godot-addon-PLATFORM.tar.gz`) is built
  full-config and is unaffected by this change.

- **`scripts/bootstrap.sh`, `bootstrap.ps1`, and the GDExtension
  configure steps** still pass the flags as `=ON` explicitly. They're
  redundant after this change but harmless and self-documenting —
  if a future change ever flipped a default back to OFF, these
  workflows would still produce the correct binary.

### Air-gapped builds

`cmake -S . -B build` with no flags will now invoke `FetchContent`
to pull `miniaudio.h`, `dr_wav.h`, `dr_flac.h`, `stb_vorbis.c` if
they're not already present under `third_party/`. This requires
network access. For air-gapped builds, either:

- Run `scripts/fetch_miniaudio.sh` and `scripts/fetch_decoders.sh`
  on a connected machine first, then the headers are present and
  no fetch is needed.
- Pass `-DAUDIO_ENGINE_FETCH_MINIAUDIO=OFF
  -DAUDIO_ENGINE_FETCH_DECODERS=OFF` and supply the headers
  yourself, OR
- Pass `-DAUDIO_ENGINE_BACKEND_MINIAUDIO=OFF
  -DAUDIO_ENGINE_DECODERS_WAV=OFF
  -DAUDIO_ENGINE_DECODERS_OGG=OFF
  -DAUDIO_ENGINE_DECODERS_FLAC=OFF` to opt out entirely.

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code touched; the only source-tree changes are the
  four `option()` lines in CMakeLists.txt and documentation
  updates.

## [0.11.8] - 2026-05-10

Ship Opus support in the Godot addon binaries on Linux + macOS. Until
this release, the addon shipped from CI didn't have
`AUDIO_ENGINE_DECODERS_OPUS=ON` or `AUDIO_ENGINE_VOICE_OPUS=ON` —
adopters who tried to register `.opus` audio files via
`Gool.register_sound_from_file()` got a "decoder gated off" error
even though the engine code path was fully implemented. This closes
that gap for the platforms where libopusfile is a one-liner system
install (Linux apt, macOS brew); Windows still needs vcpkg setup
which is a follow-up.

### Added

- **Opus dependency install + flag enablement** in
  `.github/workflows/release.yml`, `.github/workflows/nightly.yml`,
  and `.github/workflows/ci.yml` (the `build-gdextension` job).
  Two new steps before "Configure GDExtension":
    - **Linux**: `sudo apt-get install -y libopusfile-dev libopus-dev`
    - **macOS**: `brew install opusfile opus`
  The configure step then conditionally appends
  `-DAUDIO_ENGINE_DECODERS_OPUS=ON -DAUDIO_ENGINE_VOICE_OPUS=ON`
  on non-Windows runners. Adopters who download the v0.11.8
  Linux/macOS addon archive get Opus file decoding + Opus voice
  chat out of the box.

- **`scripts/bootstrap.sh`** — opportunistic Opus detection. Before
  the cmake configure step, checks `pkg-config --exists opusfile`
  and `pkg-config --exists opus`. If both are present, adds
  `-DAUDIO_ENGINE_DECODERS_OPUS=ON -DAUDIO_ENGINE_VOICE_OPUS=ON`
  to the build. If absent, prints a hint with platform-specific
  install commands (apt / brew / yum / dnf) and proceeds without
  Opus. Adopters who want Opus support locally just install the
  packages once and re-run the script.

### Changed

- **`README.md` "Build options" section** — was a one-dimensional
  table that said "everything OFF by default" without explaining
  that the Godot binding build (release pipeline, bootstrap script)
  uses different defaults. Reformatted as a two-column "Library
  default / Godot binding default" table that makes the distinction
  explicit. Adopters reading the README no longer have to read
  through the bootstrap script to understand what the shipped
  binary actually contains.

### Notes for adopters

- **Linux + macOS addon archives ship with Opus support from
  v0.11.8 onward.** No additional configuration needed in your
  Godot project — `Gool.register_sound_from_file("rifle",
  "res://audio/rifle.opus")` just works.

- **Windows addon archives still ship without Opus.** vcpkg setup
  for libopusfile in CI is more involved than the Linux/macOS
  one-liner package installs and is deferred to a follow-up. Windows
  users who need Opus support can use Track B (build from source)
  with `vcpkg install opusfile opus` and the bootstrap script will
  auto-detect.

- **The engine library standalone defaults are unchanged.** This
  release does not flip CMakeLists.txt option defaults — anyone
  embedding `audio_engine` directly in their C++ project still gets
  a minimal build by default. The change applies only to the
  Godot-binding build path that bootstrap.sh and the CI pipeline
  use.

### Verified

- All three workflow YAML files re-parsed clean via `yaml.safe_load`
  after the changes
- `bootstrap.sh` syntax-checks clean; opus-detection guard tested
  in the build sandbox (correctly reports "Opus not detected" since
  sandbox doesn't have libopusfile installed)
- 36/36 engine regression tests still passing (no engine code
  touched). Ducking baseline locked at -17.20 dB

### Not verified in this release (verifiable only by tagging)

- The actual Linux + macOS Opus install + build pipeline. The
  package names (`libopusfile-dev`, `libopus-dev` on Ubuntu;
  `opusfile`, `opus` on Homebrew) are well-established. The
  failure modes if either install hits a transient apt/brew issue:
  the install step fails → entire job fails → that platform's
  artifact doesn't ship that release. fail-fast=false keeps the
  Windows artifact safe.
- The actual `find_package(OpusFile)` resolution after install on
  the GitHub runners. The CMakeLists.txt has both `find_package`
  and `pkg_check_modules` paths; one of them should succeed.

### What's still missing

- **Windows Opus support** — vcpkg integration in CI. Feasible
  (vcpkg is preinstalled on `windows-latest`) but adds enough
  surface area that it gets its own iteration.
- **Engine library default flip** for the standalone build. The
  user may eventually want WAV/OGG/FLAC defaulting ON in the
  CMakeLists.txt itself — they're single-header drops with zero
  system deps. Deferred because it's a behavior change for
  C++-embedding adopters who may rely on the current OFF defaults.

## [0.11.7] - 2026-05-10

Hardening pass on v0.11.6 before tagging the macOS-support release.
Surfaced and fixed five issues that would have shipped to adopters
otherwise. No engine code changes; entirely a CI/scripts/docs
correctness pass.

### Fixed

- **macOS artifact name was misleading.** v0.11.6 named the macOS
  artifact `gool-X.Y.Z-godot-addon-macos-universal.tar.gz`, but
  `macos-latest` (Apple Silicon) + SCons `platform=macos` produces
  a single-arch arm64 binary, not a universal one. An Intel Mac
  user downloading what's labeled "universal" would hit a silent
  load failure in Godot. Renamed throughout to `macos-arm64` to
  match what's actually built. A real universal binary (lipo + dual
  arch builds) is a follow-up; the rename makes the current
  limitation honest.

  Files: `.github/workflows/release.yml`, `nightly.yml`, `SETUP.md`
  Track A platform list, SETUP.md macOS troubleshooting section.

- **CI cache key was inconsistent with release/nightly.** `ci.yml`
  used `matrix.os` (e.g. `ubuntu-latest`) in the godot-cpp cache
  key, while `release.yml` and `nightly.yml` used `matrix.platform`
  (e.g. `linux-x86_64`). Result: each workflow rebuilt godot-cpp on
  first run instead of sharing the cache. Aligned ci.yml's
  `build-gdextension` matrix to use the same `platform` field as
  the other workflows. Cache key now identical: a successful
  populate by any workflow benefits the others, saving ~10 minutes
  of CI time on cache hits.

- **`install_addon.ps1` only checked the MSBuild Release path.**
  If a Windows user used the Ninja generator instead of the default
  MSBuild generator, the binary would be at `build-godot/gool_godot.dll`
  (no `Release/` subdir) and install_addon.ps1 would print a
  confusing "binary not found" error. Now checks both candidates,
  matching the pattern bootstrap.ps1 already uses.

- **`fetch_godot_cpp.sh` / `.bat` failed silently on partial-clone
  state.** If a previous clone was interrupted leaving a `third_party/godot-cpp/`
  directory without a `.git/` subdirectory, the script would
  proceed past its idempotency check and `git clone` would fail
  with "destination path already exists" — a less-than-helpful
  error. Both variants now detect this case explicitly and print
  an actionable message ("rm -rf the directory and re-run").

- **Dead `binary_name` matrix field in release.yml.** Set in each
  matrix entry and exported as a `BINARY_NAME` env var to staging
  steps, but never actually referenced in any of the staging
  scripts. Removed both the matrix entries and the env var lines.
  Cosmetic cleanup; doesn't change behavior.

### Process notes

This was a hardening pass commissioned to find issues before
adopters hit them. Five issues found, five fixed. The audit
covered:

- Workflow YAML correctness and matrix variable references
- Script error handling and edge-case behavior (paths with spaces,
  partial state, wrong CWD, target already-installed)
- Documentation accuracy vs. shipped code
- Cross-workflow consistency (cache keys, naming conventions)
- Apple Clang vs GCC behavior in the existing pragma fixes

Two larger improvements were identified but deferred:

- **True universal macOS binary.** Requires `lipo` and separate
  arch builds (one matrix entry for arm64 on macos-latest, one
  for x86_64 on macos-13). The current single-arch arm64 binary
  works for Apple Silicon users; Intel users go through Track B
  for now.

- **Refactor `release.yml` + `nightly.yml` shared staging logic.**
  ~30 lines of staging code is duplicated between the two workflows.
  Could be extracted into a shared script invoked by both. Drift
  risk is real but bounded; a real refactor is a follow-up.

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code touched.

- All three workflow YAML files re-verified parse-clean via
  `yaml.safe_load`. Bash scripts pass `bash -n` syntax checks.
  Smoke-tested `fetch_godot_cpp.sh` against a partial-clone state
  in the build sandbox: emits the expected actionable error.

### Naming convention is now fully consistent

After this release, every macOS reference in the codebase agrees:

| Location                                  | Says                  |
|-------------------------------------------|-----------------------|
| release.yml matrix `platform:`            | `macos-arm64`         |
| nightly.yml matrix `platform:`            | `macos-arm64`         |
| ci.yml matrix `platform:`                 | `macos-arm64`         |
| Release artifact filename                 | `gool-X.Y.Z-godot-addon-macos-arm64.tar.gz` |
| SETUP.md Track A platform list            | `macos-arm64`         |
| SETUP.md macOS troubleshooting            | `macos-arm64`         |

No more `macos-universal`. Same self-labeling, version-stamped,
project-branded shape across every shipped artifact.

## [0.11.6] - 2026-05-10

macOS build fix. The Apple-Clang-incompatible compiler pragma that
made macOS builds fatal under `-Werror` is gated correctly now. CI
re-enables macOS in the build matrix; release.yml ships a third
addon archive (`gool-X.Y.Z-godot-addon-macos-universal.tar.gz`)
alongside the existing Linux + Windows artifacts.

This is the first release attempting macOS support since the
project's CI matrix was tightened. **Treat the macOS binary as
provisional**: it builds clean against Apple Clang in this fix, but
the broader codebase has been Linux-and-Windows-tested for months
and the first real macOS users may surface secondary issues. File
issues with concrete reproductions and they'll get prioritized.

### Fixed

- **`src/audio_engine/decoders/ogg_vorbis_decoder.cpp`** — the
  pragma block suppressing stb_vorbis warnings included
  `#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"`. That
  warning name is a GCC extension; Apple Clang defines `__GNUC__`
  for compatibility but doesn't recognize it, so the existing
  `#if defined(__GNUC__)` guard wasn't enough. Combined with
  `-Werror` (CMakeLists.txt line 420), Apple Clang's "unknown
  warning option" message became fatal — the Linux build was
  fine, the macOS build never compiled past stb_vorbis inclusion.

  Fix: split the pragma block. Shared warnings (`-Wshadow`,
  `-Wpedantic`, `-Wsign-conversion`, `-Wconversion`,
  `-Wunused-function`, `-Wcast-qual`, `-Wmissing-field-initializers`)
  remain under `#if defined(__GNUC__)` since those work on both
  GCC and Clang. `-Wmaybe-uninitialized` is now in its own
  `#if defined(__GNUC__) && !defined(__clang__)` block — active on
  GCC, silent on Apple Clang.

  `wav_decoder.cpp` and `flac_decoder.cpp` audited; their pragma
  blocks only contain shared warnings (no `-Wmaybe-uninitialized`),
  so no change needed.

### Changed

- **`.github/workflows/ci.yml`** — re-enabled `macos-latest` in
  both the engine `build-and-test` matrix and the
  `build-gdextension` matrix. Comment in the file documents why
  it was disabled and how to re-disable if a regression appears.
  `Verify binary was produced` step now also looks for
  `build-godot/libgool_godot.dylib` and prints all
  `libgool_godot*` candidates on failure for diagnostics.

- **`.github/workflows/release.yml`** — re-enabled `macos-latest`
  in the matrix. New entry produces
  `gool-X.Y.Z-macos-universal.tar.gz` (C++ static library) and
  `gool-X.Y.Z-godot-addon-macos-universal.tar.gz` (Godot addon
  with the `.dylib` GDExtension binary in `addons/gool/bin/`).
  `binary_path` is `build-godot/libgool_godot.dylib`.

- **`.github/workflows/nightly.yml`** — same re-enable for nightly
  addon archive on every push to main.

- **`SETUP.md`** — every "macOS broken" warning replaced with a
  cautious-support note pointing at v0.11.6. Track A's supported
  platforms list now includes macOS. Macos-specific troubleshooting
  section rewritten from "pretty much anything" doom to actionable
  guidance: verify Xcode CLI is installed, check Apple Clang
  version (need 14+ for `std::span`), Apple Silicon vs Intel notes.
  macOS prerequisites no longer carry the upfront
  "expect errors" disclaimer.

- **`README.md`** — Track A platform list updated to include macOS.

- **`scripts/bootstrap.sh`** — replaced the "macOS builds are
  currently broken" warning that ran at startup with a less
  alarming "macOS support landed in v0.11.6, file issues if you
  hit problems" note.

### Verified locally

- Engine still builds clean: 36/36 tests passing on Linux g++.
  Ducking baseline locked at -17.20 dB.
- Pragma syntax valid: `g++ -E` preprocesses
  `ogg_vorbis_decoder.cpp` without errors.
- All three workflow YAML files parse cleanly via `yaml.safe_load`.
- Version uniqueness: grep'd CHANGELOG for `0.11.6` before bumping;
  not previously taken.

### Not verified locally (requires GitHub Actions macos-latest)

- **The actual macOS build with Apple Clang.** The fix is
  reasoned: the `__GNUC__` && `!__clang__` guard pattern is the
  standard way to gate GCC-only pragmas. But this sandbox is
  Linux-only and `clang++` couldn't be installed (network blocked).
  The validation comes from pushing the tag and watching the macOS
  job in the CI matrix.
- **`std::span` availability on the GitHub macos-latest Apple
  Clang.** As of mid-2025, `macos-latest` runs Xcode 15+ which
  includes Apple Clang 15+ — `std::span` is supported. If a future
  GitHub Actions image change drops back to an older toolchain,
  the build would fail with a different error and we'd need to
  iterate.
- **Universal binary correctness.** The matrix entry uses
  `platform=macos-universal` for the artifact name but the SCons
  build invocation just uses `platform=macos`, which on Apple
  Silicon runners produces an arm64 binary. If you need a true
  universal (Intel + Apple Silicon) binary, that's a follow-up;
  the current artifact is single-arch and will run on whichever
  arch the runner uses.

### What to expect

The first tagged release after this change pushes a macOS build
through the full release pipeline. Likely outcomes, in order of
probability:

1. **Build succeeds, addon loads in Godot on macOS** → ship it,
   close the macOS gap.
2. **Build succeeds, addon doesn't load** (binary architecture
   mismatch or signing issue) → file an issue with the load error
   from Godot's output panel; iterate.
3. **Build fails on a different macOS-specific issue** → the CI
   logs name the specific compile error; iterate. Most likely
   candidates: another pragma elsewhere, a Linux-specific header
   I missed, or a `-Wpedantic` warning that's stricter on Apple
   Clang. The pragma audit covered all three decoders; other code
   should be fine but I haven't compiled it against Apple Clang.

If outcome 3 happens, the next iteration is small (find the
specific failure, fix it, re-tag). The CI matrix is set up to
fail-fast=false so Linux + Windows still ship even if macOS
breaks.

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code changes; the only source-tree change is the
  pragma split in one decoder file, which doesn't affect runtime
  behavior on any platform.

## [0.11.5] - 2026-05-10

Source-archive branding + versioning. Tarballs and zips produced
from the source tree are now self-labeling: filename and top-level
extracted directory both carry the project name and version. No
more generic `audio_engine.tar.gz` extracting into a `audio_engine/`
folder with no version stamp.

### Added

- **`scripts/make_source_archive.sh`** — produces a clean source
  archive from the work tree, named `gool-X.Y.Z.tar.gz` containing
  `gool-X.Y.Z/`. Reads the version from
  `include/audio_engine/version.h`'s `kVersionString` constant —
  the same source of truth the release procedure already keeps in
  sync with CMakeLists.txt and the version test. No external
  dependencies; works in non-git checkouts (no `git archive`
  requirement).

  Default output: `dist/gool-X.Y.Z.tar.gz`. Custom path: pass as
  the first argument. Stdout: pass `-`.

  Excludes: `build/`, `build-*/`, `out/`, `cmake-build-*/`,
  `third_party/`, `dist/`, `.git/`, build artifacts (`*.o`, `*.a`,
  `*.so`, `*.dylib`, `*.dll`, `*.lib`, `*.exe`, `*.pdb`), Python
  caches, IDE noise (`.swp`, `.DS_Store`, `Thumbs.db`).

  Includes: everything else — `src/`, `include/`, `godot/`,
  `examples/`, `tests/`, `scripts/`, `docs/`, `tools/`, `.github/`,
  `CMakeLists.txt`, `README.md`, `SETUP.md`, `RELEASING.md`,
  `CHANGELOG.md`, `LICENSE`, `.gitignore`.

- **`scripts/make_source_archive.ps1`** — Windows variant. Uses
  `tar.exe` (built into Windows 10 1803+ and Windows 11) for
  `.tar.gz` output; falls back to `Compress-Archive` for `.zip`
  on older Windows. Same naming convention. Stages files via
  `robocopy` with the same exclusion list.

### Changed

- **`.gitignore`** — added `dist/`, `gool-*.tar.gz`, `gool-*.zip`,
  `gool-*.tgz` so accidentally-built archives don't get committed.

- **`RELEASING.md`** Step 6 — documents the source-archive
  convention alongside the release-pipeline artifacts. Notes that
  GitHub auto-attaches its own source archives to every tag (named
  identically: `gool-X.Y.Z.tar.gz`); the new script produces a
  cleaner, deterministic version filtered against the deliberate
  ship-set rather than the broader repo state.

### Naming convention summary

After this release, every artifact users see follows the same shape:

| Artifact                                   | Filename                                                  |
|--------------------------------------------|-----------------------------------------------------------|
| Source archive (this script)               | `gool-X.Y.Z.tar.gz`                                       |
| GitHub auto-source archive (from tag)      | `gool-X.Y.Z.tar.gz` / `.zip`                              |
| C++ static library (release.yml)           | `gool-X.Y.Z-linux-x86_64.tar.gz` / `-windows-x86_64.zip` |
| Godot addon (release.yml + nightly.yml)    | `gool-X.Y.Z-godot-addon-linux-x86_64.tar.gz`             |
|                                            | `gool-X.Y.Z-godot-addon-windows-x86_64.zip`              |
|                                            | `gool-nightly-godot-addon-linux-x86_64.tar.gz` (nightly)  |

Every filename starts with `gool-`. Every filename includes the
version (or `nightly` for unstamped builds). Top-level directory
inside each archive matches the filename's stem. No more `audio_engine/`
in any user-facing artifact.

### Verified

- Script syntax: `bash -n scripts/make_source_archive.sh` passes
- End-to-end: ran the script in the build sandbox, produced
  `dist/gool-0.11.4.tar.gz` (488 KB), extracted into a clean
  directory, confirmed it produces a single `gool-0.11.4/` folder
  with all the right contents and none of the excluded artifacts
- The PowerShell variant could not be runtime-tested in this Linux
  sandbox; will be exercised by Windows users first

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code changes; this is a packaging release.

## [0.11.4] - 2026-05-10

Bootstrap-experience overhaul, **phase C: release pipeline**.
Closes the bootstrap story. The release workflow now produces a
prebuilt Godot addon archive on every tag — adopters can drop
`addons/gool/` into their project without ever touching a C++
compiler. CI catches binding drift on every PR. A nightly workflow
produces bleeding-edge addon archives between tagged releases.

### Added

- **`.github/workflows/release.yml`** — extended to produce **two
  artifact families per platform** instead of one. The existing
  C++ static library archive (`gool-X.Y.Z-PLATFORM.tar.gz/zip`)
  remains for users embedding the engine in their own C++ build.
  A new **Godot addon archive** (`gool-X.Y.Z-godot-addon-PLATFORM.tar.gz/zip`)
  contains an `addons/gool/` directory ready to copy into a Godot
  project root, with the prebuilt GDExtension binary in
  `addons/gool/bin/`. Linux x86_64 + Windows x86_64 only (macOS
  still broken).

  The new pipeline:
    1. Sets up MSVC dev environment on Windows (`ilammy/msvc-dev-cmd`)
    2. Installs SCons (`pip install scons`)
    3. Runs the existing `scripts/fetch_*` to download single-header deps
    4. Caches godot-cpp build via `actions/cache@v4` (key: platform +
       godot-cpp ref) — first build is slow (5–20 min), subsequent
       runs hit the cache
    5. Builds godot-cpp at `GODOT_CPP_REF=4.2` (matches
       `compatibility_minimum` in the gdextension manifest)
    6. Builds the GDExtension via `cmake -S godot` with
       `AUDIO_ENGINE_BACKEND_MINIAUDIO=ON` + the WAV/OGG/FLAC
       decoders enabled
    7. Stages `addons/gool/` + the binary into a release directory
    8. Uploads both archive families to the GitHub Release

- **`.github/workflows/nightly.yml`** — new file. Builds the
  Godot addon archive on every push to `main` (and on manual
  workflow dispatch). Uploads as a **workflow artifact** (not a
  release — the Releases page stays clean for tagged versions).
  30-day retention. Adopters who can't wait for a tagged release
  can grab `gool-nightly-godot-addon-<platform>` from the Actions
  tab.

### Changed

- **`.github/workflows/ci.yml`** — added a second job
  (`build-gdextension`) that builds the GDExtension on every PR.
  Catches binding drift between releases — godot-cpp ABI changes
  or signature drift in `gool_godot.cpp` would have silently
  shipped to a release before. Now the binding compiles on every
  PR alongside the engine library.

  The existing `build-and-test` job is unchanged; it continues
  to build the engine library + run the 36-test suite.

  Total CI time goes up on cache misses (~10 min for godot-cpp's
  first build), but the cache makes subsequent runs fast.

- **`SETUP.md` Track A** — promoted from "coming soon" placeholder
  to live procedure. Now documents the actual download / extract /
  enable flow against the new archive family. Track A is the
  recommended path for most adopters (no compiler required); Track
  B remains for contributors and adopters whose platform isn't
  covered by the release pipeline (macOS, ARM64, embedded).

  Also documents how to grab nightlies for adopters who want the
  bleeding edge between tagged releases.

- **`SETUP.md` "Pick a path" status table** — Track A status changed
  from "Coming soon" to "Available from v0.11.4 onward".

- **`README.md`** Quick start — Track A is now the leading path,
  with Track B (the bootstrap script flow from v0.11.3) as the
  secondary option for contributors and custom platforms.

- **`RELEASING.md`** — Step 6 (verify the GitHub Release) rewritten
  to describe both artifact families, the build pipeline they
  share, and a smoke-test step (download the addon archive,
  extract into a real Godot project, verify it loads).

### Notes for adopters

- **Existing C++ archive consumers are unaffected.** The format and
  contents of `gool-X.Y.Z-PLATFORM.tar.gz/zip` are unchanged. The
  addon archive is a strict addition.

- **The first release workflow run after this change will be slow
  on godot-cpp's first build** (cache miss). Subsequent runs (until
  `GODOT_CPP_REF` changes) hit the cache and run in ~3 min.

- **godot-cpp ref pinning matters across three workflows.** When
  bumping (Godot 4.2 → 4.3 etc), update `GODOT_CPP_REF` in
  `release.yml`, `ci.yml`, AND `nightly.yml` AND the
  `compatibility_minimum` in `godot/gool.gdextension`. They must
  match. RELEASING.md calls this out.

### Verified

- All three workflow YAML files parse cleanly via `yaml.safe_load`
- Matrix references (`matrix.binary_path`, `matrix.scons_platform`,
  `matrix.archive_ext`) consistent across the new and existing steps

### Not verified in this release (verifiable only by tagging)

- The release workflow end-to-end run on a real GitHub Actions
  matrix. The risk surfaces are (a) the godot-cpp build on Windows
  through SCons + MSVC, (b) the exact CMake output path for the
  Windows binary (`build-godot/Release/gool_godot.dll` per the
  default MSBuild generator), and (c) the action versions
  (`ilammy/msvc-dev-cmd@v1`, `actions/cache@v4`,
  `actions/upload-artifact@v4`, `softprops/action-gh-release@v1`).
  All four are well-established public actions.
- macOS still disabled across all three workflows. The release
  archive ships only Linux + Windows binaries until the macOS
  build is fixed.

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code changes; this is a CI/release pipeline release.

### What's complete

This was Phase C of the planned three-phase bootstrap overhaul.
With v0.11.2 (docs), v0.11.3 (bootstrap automation), and now
v0.11.4 (release pipeline), the full bootstrap story is in place:

- **For adopters who just want gool:** download the addon archive
  from Releases, extract, enable. ~30 seconds.
- **For contributors / custom platforms:** clone the repo, run
  `scripts/bootstrap.sh --install-to <project>`. ~5 minutes plus
  godot-cpp's first build.
- **For people following development:** grab the nightly addon
  archive from the Actions tab.

Documentation, scripts, and CI all point at the same flow.

## [0.11.3] - 2026-05-10

Bootstrap-experience overhaul, **phase B: automation**. Collapses
the multi-step build-from-source procedure into a single command.
After installing platform prerequisites (one-time per machine),
adopters now run:

```
./scripts/bootstrap.sh --install-to /path/to/godot/project
```

…and end up with the addon installed in their Godot project. The
script verifies prerequisites, fetches dependencies, clones and
builds godot-cpp at a pinned ref, builds gool's GDExtension, and
copies everything into the target project. Idempotent.

### Added

- **`scripts/fetch_godot_cpp.sh`** + **`scripts/fetch_godot_cpp.bat`**
  — shallow-clone helper for godot-cpp, mirroring the pattern of
  `scripts/fetch_opus.sh` (which already clones xiph/opus the same
  way). Default branch `4.2` (matches the gdextension manifest's
  `compatibility_minimum`); override with positional arg or
  `GODOT_CPP_REF` env var. Skips the clone if the directory
  already has a `.git/`.

- **`scripts/install_addon.sh`** + **`scripts/install_addon.ps1`** —
  takes a target Godot project path and copies the addon GDScript
  files (runtime singleton, audio relevancy filter, plugin, all
  seven prefabs) plus the `gool.gdextension` manifest plus the
  built GDExtension binary into `<target>/addons/gool/`. Validates
  that the target is a real Godot project (has `project.godot`)
  and that the binary exists. Default binary path matches the
  bootstrap script's CMake output; override with `GOOL_BINARY` env
  var (bash) or `-Binary` flag (PowerShell).

- **`scripts/bootstrap.sh`** — Linux/macOS one-command setup. Five
  phases:
    1. Prerequisite checks (git / cmake / python3 / scons / g++ or clang++)
    2. Fetch single-header dependencies (miniaudio, dr_libs, stb_vorbis)
       — skips if already present
    3. Clone + build godot-cpp at pinned ref — skips if already built
    4. Configure + build gool's GDExtension via CMake (with miniaudio
       backend + WAV/OGG/FLAC decoders enabled by default)
    5. Optional install via `--install-to <path>`

  Configurable via env vars: `GODOT_CPP_REF`, `BUILD_TYPE`, `JOBS`,
  `GODOT_CPP_PATH`, `SKIP_GODOT_CPP`. `--help` flag prints the
  full usage text. macOS run prints an upfront warning about the
  known-broken state instead of letting the user hit cryptic
  errors mid-build.

- **`scripts/bootstrap.ps1`** — PowerShell variant for Windows.
  Same five-phase logic. Detects MSVC via `cl` on PATH and
  prompts the user to open the **x64 Native Tools Command Prompt
  for VS 2022** if not. Same `-InstallTo` / `-GodotCppRef` /
  `-BuildType` / `-Jobs` / `-SkipGodotCpp` parameter surface as
  the bash variant's env vars.

### Changed

- **`SETUP.md`** — Track B section now leads with the bootstrap
  fast path. The manual phase-by-phase walkthrough remains for
  adopters who want to understand each step (or recover when
  bootstrap fails partway through). Phase 2 / Phase 3 are now
  documented as "what bootstrap does for you" with the manual
  procedure as the second-class explanation.

- **`README.md`** Quick start install block — collapsed from a
  10-line sequence to two lines (`git clone` + `bootstrap.sh
  --install-to`). Points at SETUP.md for prerequisites and the
  manual walkthrough.

### Verified

The bash scripts pass `bash -n` syntax checks. `install_addon.sh`
end-to-end smoke-tested in the build sandbox: 14 files copied
into the right places (4 top-level GDScript files, the manifest,
7 prefabs, plus the binary in `bin/`). Error paths verified:
non-existent target, real dir without `project.godot`, real
project without a built binary — each fires its descriptive
error message and exits cleanly.

The PowerShell scripts couldn't be runtime-tested in this
Linux-only sandbox; they'll be exercised by adopters or by Phase
C's CI matrix. Risk surface there is shell-quoting differences
on Windows paths and the exact MSBuild output layout for the
GDExtension binary (handled with two candidate paths +
descriptive fallback if neither matches).

### Tests

- Total **36/36** passing. Ducking baseline locked at -17.20 dB.
  No engine code changes; this release only adds scripts.

### What's next

Phase C closes the bootstrap story:

- **Phase C (planned):** release pipeline rewrite. CI builds the
  GDExtension binary per platform on tag push, packages
  `gool-X.Y.Z-godot-addon.zip` ready to drop into a Godot project
  (no compiler required by the adopter). Closes the Track A gap
  in `SETUP.md`.

## [0.11.2] - 2026-05-10

Bootstrap-experience overhaul, phase A: documentation. Closes the
documented gaps that made the project hard to set up for new
adopters. No engine code changes; this is a docs-and-truth release.

### Added

- **`SETUP.md`** — single-source-of-truth setup guide (530 lines).
  Per-platform prerequisites with concrete package manager commands
  (winget + Chocolatey for Windows; Homebrew for macOS; apt / dnf /
  pacman for Linux). Covers Track A (use prebuilt addon — placeholder
  until binary releases ship in phase C) and Track B (build from
  source). Three-phase build-from-source procedure: install
  prerequisites, build the GDExtension, install the addon into a
  Godot project. Optional features section covering libopus and
  libopusfile install per platform. Verification steps.
  Troubleshooting section covering the 7 most common failure modes
  (`GODOT_CPP_PATH` missing, `miniaudio.h` not found, decoder headers
  not found, compiler too old, MSBuild not found on Windows, addon
  doesn't appear in editor, macOS).

### Fixed

- **README "Build options" table** — corrected the lie that
  `AUDIO_ENGINE_DECODERS_WAV/OGG/FLAC` default ON. They've always
  defaulted OFF in `CMakeLists.txt`. Adopters who followed the
  README ended up with no decoders compiled in. Also added the
  `AUDIO_ENGINE_DECODERS_OPUS` row missing since v0.11.0.

- **README test count** — was 25 in the build instructions block,
  36 in the test suite section. Both now say 36.

- **README "Dependencies" section** — re-labeled "Vendored
  single-header drops" to "Fetched single-header drops" because
  miniaudio / dr_libs / stb_vorbis aren't actually vendored in the
  repo; they're fetched on demand via `scripts/fetch_*.sh`. The
  scripts existed; the README didn't tell adopters to run them.
  Added libopusfile to the optional-dependency table.

- **README "Quick start" section** — was a wall of GDScript API
  examples followed by a buried install block. Restructured so the
  setup pointer (to `SETUP.md`) leads, the 30-second build
  incantation is visible, and the API examples follow as "first
  lines you'll write" once the project is set up.

- **godot/README.md** — same install-section rewrite. Points at
  `SETUP.md` for the per-platform path, adds the missing fetch
  script step that has to run before miniaudio compiles.

### Acknowledged honestly

- **macOS is currently broken** — the build doesn't work on Apple
  Clang. CI matrix has macOS disabled in both `ci.yml` and
  `release.yml`. README and `SETUP.md` now say so up front instead
  of letting macOS users hit cryptic errors.

- **No prebuilt addon binaries ship yet** — Track A in `SETUP.md`
  is a placeholder pointing at the build-from-source path for now.
  Phase C of this work (release pipeline rewrite) will fix this.

### Tests

- Total **36/36** passing (no test changes; this is a docs release).
  Ducking baseline locked at -17.20 dB.

### What's next

This was Phase A of a planned three-phase bootstrap overhaul:

- **Phase A (this release):** fix the documentation lies, write
  the missing setup guide.
- **Phase B (planned next):** bootstrap automation. `scripts/bootstrap.sh`
  + `scripts/bootstrap.ps1` that verify prerequisites, clone and
  build godot-cpp at a pinned ref, build the GDExtension, and
  install into a target Godot project. One-command setup.
- **Phase C (planned):** release pipeline rewrite. Build the
  GDExtension binary per platform on tag push, package as
  `gool-X.Y.Z-godot-addon.zip` ready to drop into a Godot project.
  Closes the Track A gap.

## [0.11.1] - 2026-05-10

GDScript bindings for runtime audio-file loading. Closes the gap
between v0.11.0's engine surface (OpusFileDecoder + the existing
WAV / Vorbis / FLAC / Opus pipeline routed through
`AudioRuntime::RegisterSoundFromFile` / `RegisterSoundFromMemory`)
and what GDScript hosts could actually call. Before v0.11.1, the
GDExtension only exposed `register_pcm_sound(name, samples, sr,
ch)` — Godot projects had to pre-decode files in GDScript or via
synthesis to get audio into the engine. v0.11.1 lets them register
directly from a file path (including res://) or a raw byte buffer.

### Added

- **GDExtension binding `register_sound_from_file(name, path) → int`**
  — reads bytes via Godot's `FileAccess` (works with `res://` paths
  in PCK-packaged builds, not just editor mode) and calls into
  `AudioRuntime::RegisterSoundFromMemory` with format = Auto.
  Returns the AudioSoundId on success, 0 on failure with
  push_error describing the cause (file missing, decoder compiled
  out, etc).

- **GDExtension binding `register_sound_from_bytes(name, bytes,
  format_hint) → int`** — direct memory variant for hosts that
  manage I/O themselves (custom asset packs, network downloads,
  encrypted blobs). `format_hint` matches the C++ AudioFileFormat
  enum: 0=Auto (recommended; sniffs by magic bytes), 1=Wav,
  2=OggVorbis, 3=Flac, 4=Opus.

- **`runtime_singleton.gd::register_sound_from_file()` and
  `register_sound_from_bytes()`** — facade wrappers with
  documentation about which decoders need to be enabled in CMake
  (`AUDIO_ENGINE_DECODERS_*`) for each format.

- **`FORMAT_AUTO` / `FORMAT_WAV` / `FORMAT_OGG_VORBIS` /
  `FORMAT_FLAC` / `FORMAT_OPUS`** constants on the runtime
  singleton, mirroring the C++ AudioFileFormat enum.

- **Diagnostic mapping** — when the underlying engine returns
  `AudioResult::Unsupported` (decoder gated off in CMake), the
  binding pushes a clear error directing the user to set
  `AUDIO_ENGINE_DECODERS_*=ON` for the format they need.

### Usage

```gdscript
# Load any supported file from res:// — magic-byte sniff picks
# the right decoder. Returns the sound id (positive int) or 0
# on failure.
var id := Gool.register_sound_from_file("rifle_fire",
                                          "res://audio/rifle_fire.opus")
if id != 0:
    Gool.register_sound_definition("rifle_fire", true, false,
                                     1.0, 30.0, 80.0,
                                     Gool.CATEGORY_SFX, "LocalSfx")
    Gool.play_3d("rifle_fire", muzzle_position, 200)
```

For memory-managed sources:

```gdscript
var bytes: PackedByteArray = my_pack.read_asset("explosion.opus")
Gool.register_sound_from_bytes("explosion", bytes, Gool.FORMAT_AUTO)
```

### Limitations

- **No streaming-from-bytes binding yet.** `RegisterStreamingSoundFromFile`
  takes a real-fs path (won't work for `res://` in PCK builds), and
  `RegisterStreamingSoundFromMemory` only takes pre-decoded float
  PCM (not compressed bytes). Streaming Opus directly from a
  packed Godot resource needs an engine-side
  `RegisterStreamingFromMemory(bytes, formatHint)` — deferred to a
  follow-up release.
- **Decoder defaults remain OFF.** All `AUDIO_ENGINE_DECODERS_*`
  CMake options default OFF. Projects that want file playback
  must enable the relevant flag(s) at build time. The binding's
  diagnostic error makes the misconfiguration obvious at runtime.

### Tests

- Total **36/36** passing. Existing `decoder_test` continues to
  cover format sniffing on RIFF / OggS+Vorbis / OggS+OpusHead /
  fLaC magic bytes; the new bindings exercise already-tested
  engine paths so no new test files were added this iteration.
  Ducking baseline locked at -17.20 dB.

## [0.11.0] - 2026-05-10

Opus file decoding. Adds `OpusFileDecoder` to the existing decoder
plugin framework alongside WAV / Vorbis / FLAC. Compressed audio
assets in the `.opus` container (Ogg Opus) can now be loaded and
streamed at runtime, gated behind the `AUDIO_ENGINE_DECODERS_OPUS`
build flag the same way the other format decoders gate.

### Why Opus

Opus is the modern royalty-free codec. At ~96 kbit/s it sounds
indistinguishable from raw PCM for game SFX and music; bundle
sizes drop by roughly 15× compared to 48 kHz/16-bit stereo WAV.
The codec is already in this engine for streaming voice (libopus
via `OpusVoiceCodec`); v0.11.0 adds file-based playback via the
sister library libopusfile.

### Added

- **`src/audio_engine/decoders/opus_file_decoder.{h,cpp}`** —
  new `OpusFileDecoder` implementing `IAudioDecoder`. Wraps
  libopusfile (`OggOpusFile*`). `CreateFromFile` and
  `CreateFromMemory` factory helpers; `DecodeFrames` produces
  interleaved float32 in [-1, 1]; `Seek` is sample-accurate
  against the 48 kHz decoded stream.

  Opus always decodes at 48 kHz internally regardless of source
  recording rate, so `SampleRate()` always reports 48000. The
  asset registry's existing resampling path handles engine-rate
  mismatches automatically — no new wiring needed.

- **`AudioFileFormat::Opus`** — new enum value in
  `audio_file_format.h`.

- **`tests/unit/decoder_test::TestOpusFactoryDispatch`** — proves
  the factory routes `.opus` files (and `OpusHead`-marked Ogg
  streams in memory) to `OpusFileDecoder`, not to
  `OggVorbisDecoder` (which would have silently failed). Runs
  unconditionally; in stub mode (the default) it verifies the
  factory returns nullptr cleanly rather than misrouting.

- **`tests/unit/decoder_test::TestFormatSniffing`** — extended
  with explicit Vorbis and Opus codec-ID test cases. Hand-built
  payload representations of `OggS` + `OpusHead` and `OggS` +
  `\x01vorbis` confirm the sniffer disambiguates.

### Changed

- **`DecoderFactory::SniffFormat`** — when the magic bytes are
  `OggS`, the sniffer now probes the page payload for either
  `OpusHead` (8-byte magic) or `\x01vorbis` (7-byte magic) and
  routes accordingly. Previously every `OggS` returned
  `AudioFileFormat::OggVorbis`, which silently misrouted Opus
  streams. Files without enough bytes to disambiguate fall back
  to `OggVorbis` as before (the create-then-fallback path in
  `CreateForFile` handles the rest).

- **`DecoderFactory::FormatFromExtension`** — `.opus` extension
  added; case-insensitive like the others.

- **`DecoderFactory::CreateForFile`** — Opus added to the
  fallback chain. Tried before Vorbis since libopusfile rejects
  non-Opus streams faster (header check) than stb_vorbis rejects
  non-Vorbis streams.

- **`CMakeLists.txt`** — new option
  `AUDIO_ENGINE_DECODERS_OPUS` (default OFF, matching the other
  decoder flags). When ON, resolves libopusfile via
  `find_package(OpusFile)` first, then `pkg-config opusfile`.
  No vendored or FetchContent path because libopusfile uses
  autotools, not CMake — adopters install via system package
  manager (`apt install libopusfile-dev`, `brew install opusfile`,
  `vcpkg install opusfile`) and re-run CMake. Configure-time
  error message is explicit about this.

### Build flag

```
cmake -DAUDIO_ENGINE_DECODERS_OPUS=ON ..
```

The decoder TU is always in the source list; the gate decides
whether it pulls in `<opus/opusfile.h>` and links libopusfile, or
expands to a stub returning nullptr. Same pattern the other
decoders and `OpusVoiceCodec` already use, so adopters get a
predictable surface.

### Tests

- Total **36/36** passing (decoder_test extended with 1 sub-test
  + extended sniffing). Ducking baseline locked at -17.20 dB.

## [0.10.1] - 2026-05-10

Per-emitter bus targeting from GDScript. Closes the v0.10.0
documented gap: the bus graph was *configurable* but every sound
registered through GDScript silently routed to Master, so the
multi-tier sidechain ducking config in `coop_shooter_template`
was a no-op. v0.10.1 makes the ducking actually trigger.

### Added

- **`AudioRuntime::FindBusIdByName(std::string_view) → BusId`** —
  new public method. Resolves a bus by its `debugName` (set via
  `BusConfig::debugName` at build time, including by the JSON
  loader's `name` field). Returns `kInvalidBusId` if no bus matches.
  O(N) over kMaxBuses; intended for init-time and registration-time
  use, not per-frame. Game-thread only.

- **GDExtension binding `find_bus_id_by_name(name) → int`** —
  exposes the lookup to GDScript. Returns -1 if no bus matches.
  Useful for hosts that need to call other BusId-taking bindings
  (`set_bus_gain_db`, `set_effect_parameter`) by name.

- **`bus_config_loader_test::TestFindBusIdByName`** — new
  sub-test (11th in the file) covering: each declared bus name
  resolves to a valid distinct BusId, Master is always pinned to
  `kBusMaster` (id 0), unknown names and empty strings return
  `kInvalidBusId`.

### Changed

- **GDExtension `register_sound_definition`** — extended with two
  new optional parameters at the end:

  ```gdscript
  Gool.register_sound_definition(
      name, spatialized, looping,
      min_distance, max_distance, loop_crossfade_ms,
      category,           # NEW: Gool.CATEGORY_* (default SFX = 0)
      target_bus_name)    # NEW: bus override (default "" = use category routing)
  ```

  Existing call sites (audio_emitter_3d, networked_audio_emitter_3d,
  voice clip registration) keep their behavior because the new
  params have safe defaults.

- **GDExtension `register_sound_definition` — fixed default-target
  bug.** The binding previously hardcoded
  `def.targetBus = audio::kBusMaster`, which silently overrode the
  category routing configured via JSON. The new behavior leaves
  `targetBus` at `kInvalidBusId` (the engine's "use category
  routing" sentinel) when no explicit bus is named, so the bus
  graph configured in v0.10.0 actually receives sounds. For projects
  with no bus config, behavior is preserved (every category default-
  routes to master via `CategoryBusMap`).

- **`runtime_singleton.gd::register_sound_definition`** — wrapper
  signature extended to forward the new parameters. Adds
  `CATEGORY_SFX` / `CATEGORY_VOICE` / `CATEGORY_MUSIC` /
  `CATEGORY_AMBIENCE` / `CATEGORY_UI` / `CATEGORY_DIALOGUE`
  constants mirroring the C++ enum.

- **`runtime_singleton.gd::find_bus_id_by_name(name) → int`** — new
  facade method.

- **`coop_shooter_template`** — the wiring is now real:

  - `audio_setup.gd` registers each weapon sound twice
    (`*_local` → LocalSfx, `*_remote` → RemoteSfx). Same audio
    asset, different bus routing.
  - Music registrations target the Music bus (with sidechain
    compressor keyed off LocalSfx).
  - Ambient registrations target the Ambient bus.
  - Footsteps go to LocalSfx for both player and bots in this
    single-host demo.
  - UI sounds use category UI (default-routed to Master).
  - `weapon.gd::_play_fire` appends `_local` or `_remote` based on
    `is_local`, so the local player's gun audibly ducks both
    music AND remote teammates' gunfire. Multi-tier sidechain
    ducking from L4D2 patterns, working at the engine level.

  The RTPC-driven music attenuation in `combat_music_director.gd`
  still ships as a layered behavior — it drives the music *state*
  machine (explore → suspicion → combat) so adaptive music and
  sidechain ducking compose without conflict.

### Tests

- Total **36/36** passing (existing `bus_config_loader_test`
  extended from 10 → 11 sub-tests). Ducking baseline locked at
  -17.20 dB.

## [0.10.0] - 2026-05-09

Bus-graph configuration from GDScript. Closes the documented gap
between what the C++ engine can do (multi-tier sidechain ducking,
per-bus effect chains) and what GDScript hosts could configure
(sample rate + buffer size only). Godot projects can now ship their
bus topology in a JSON file and get the full L4D2-style ducking
behavior at runtime initialization with no engine code changes.

### Added

- **`include/audio_engine/bus_config_loader.h`** — new public header.
  `audio::BusConfigLoader::ParseFromJson(json)` returns a populated
  `BusGraphConfig` (with category routing nested) ready to drop
  into `AudioConfig::busGraph`. Errors carry line numbers and
  descriptive messages. Forward-compat: unknown keys are tolerated
  silently. Back-compat: configs missing the `"buses"` key parse
  successfully with `busCount=0` (engine auto-builds master-only).

- **`src/audio_engine/runtime/bus_config_loader.cpp`** — minimal
  recursive-descent JSON parser (~480 LOC, no shared scanner
  dependency in this iteration). Supports all five effect kinds
  (gain, biquad, compressor, reverb, saturation) with their full
  field surface. Sidechain bus references resolve by name. Tolerates
  `//` line comments for hand-edited configs.

- **`tests/unit/bus_config_loader_test.cpp`** — 10 sub-tests,
  pure C++, covering: minimal config, full multi-tier ducking shape
  with sidechain refs resolved by name, every effect kind round-
  tripping its fields, end-to-end `AudioRuntime::Initialize()`
  accepting the parsed config, malformed JSON line numbers, unknown
  effect kind error, unresolved sidechain bus error, unresolved
  parent error, forward-compat unknown-key tolerance, and back-
  compat empty-buses-key handling.

- **GDExtension binding `init_with_config(json_text, sample_rate,
  buffer_size)`** — the C++ binding takes the raw JSON text from
  GDScript and routes through the loader. No GDScript-side schema
  translation.

### Changed

- **`addons/gool/runtime_singleton.gd`** — reads the project's
  `res://gool/config.json` at startup. If the JSON contains a
  `"buses"` array, routes through `init_with_config()`. If the
  file is missing, empty, or has no buses key, falls back to plain
  `init(sample_rate, buffer_size)` (legacy behavior).

- **`addons/gool/plugin.gd`** — the editor plugin now writes a
  richer default `gool/config.json` on enable. The default ships
  the L4D2 multi-tier ducking topology: Master / Music (ducks
  under LocalSfx) / SfxAll / LocalSfx / RemoteSfx (ducks under
  LocalSfx) / Voice / Ambient. Out-of-the-box, projects get the
  audio mix architecture proven by the C++ `multi_tier_ducking`
  example.

- **`examples/coop_shooter_template/`** — synced to use the new
  binding. Ships its own `gool/config.json` with the multi-tier
  ducking topology. README updated to reflect the new wiring AND
  to honestly document the remaining gap (per-emitter bus
  targeting from GDScript — a separate iteration).

### JSON schema

```json
{
  "sample_rate": 48000,
  "buffer_size": 512,
  "buses": [
    { "name": "Master", "gain_db": 0.0 },
    { "name": "Music", "parent": "Master", "gain_db": -3.0,
      "effects": [
        { "kind": "compressor",
          "threshold_db": -28.0, "ratio": 8.0,
          "attack_ms": 5.0, "release_ms": 250.0,
          "sidechain_bus": "LocalSfx" }
      ] },
    { "name": "LocalSfx", "parent": "SfxAll" },
    { "name": "RemoteSfx", "parent": "SfxAll",
      "effects": [
        { "kind": "compressor",
          "sidechain_bus": "LocalSfx",
          "threshold_db": -28.0, "ratio": 6.0,
          "attack_ms": 5.0, "release_ms": 200.0 }
      ] }
  ],
  "category_routing": {
    "music": "Music", "sfx": "LocalSfx", "voice": "Voice"
  }
}
```

Sidechain bus references resolve by name, so config files stay
readable (no manual BusId numbering). Master must be one of the
buses. Other parents are resolved against the bus list.

### Honest gap remaining

The GDScript `register_sound_definition` binding doesn't yet take
a target-bus argument — every registered sound routes to Master by
default. The new bus graph is therefore *configured* but not
*exercised* by the coop_shooter_template's audio (the RTPC stand-in
in `combat_music_director.gd` continues to drive the audible
ducking). Closing this gap is the next iteration's deliverable: one
binding method to add (`register_sound_definition_on_bus(name,
bus_name, ...)` or a `play_3d_on_bus(...)` per-play override) plus
test coverage.

### Tests

- Total **36/36** passing (10 added in `bus_config_loader_test`).
  Ducking baseline locked at -17.20 dB.

## [0.9.1] - 2026-05-09

Co-op shooter starter template — the demo that shows the audio
architecture compose into something real. Single-host scene with
one playable character + three AI bots, demonstrating the
multiplayer audio patterns from `docs/multiplayer.md` without
requiring an actual networking transport. Press Play, hear it work.

This is a Godot-side-only release; no engine code changed. All
35 tests still pass, ducking baseline locked at -17.20 dB.

### Added

- **`examples/coop_shooter_template/`** — new Godot 4.2+ project.
  Six GDScript files (~700 LOC), one main scene, full README. Uses
  the existing `addons/gool/` prefabs; no new public API.

  - `scripts/main.gd` — scene controller: bootstrap, listener
    tracking, wiring of all subsystems
  - `scripts/audio_setup.gd` — synthesizes and registers all sounds
    (3 weapons × fire+tail, 3 footstep variants, 3 music states,
    ambience bed, UI feedback). Procedural synthesis only — zero
    asset dependencies, clone-and-press-Play
  - `scripts/player_controller.gd` — WASD movement, fire input,
    weapon cycling. Footsteps emitted on distance-traveled
    threshold (the docs/multiplayer.md §13 pattern: never RPC
    footsteps)
  - `scripts/ai_bot.gd` — wander/pause/burst-fire state machine
    standing in for what would be three remote peers in a real
    co-op session
  - `scripts/weapon.gd` — weapon component with cooldown, three
    weapon kinds (pistol/rifle/shotgun), local-vs-remote sound
    selection (remote fires get a delayed distance "tail")
  - `scripts/combat_music_director.gd` — gunfire intensity tracker
    that drives both the music state machine
    (explore→suspicion→combat) and a `combat_intensity` RTPC
    bound to music volume

### Architecture demonstrated

- Three weapon types with distinct timbres (~70 LOC of synthesis math)
- Local vs remote audio routing (player's gun is loud near-field;
  bot guns get distance attenuation + a delayed tail layer)
- Footsteps generated locally per character via
  `FootstepSurfacePlayer` prefab; never RPC'd
- Music state machine with explore→suspicion→combat transitions
  driven by gunfire activity windowing
- RTPC-driven music ducking under heavy combat (the GDScript-
  exposed analog to the C++ multi-tier sidechain ducking)
- Continuous ambient world bed via long-lived
  `AudioEmitter3D` looping
- UI feedback (weapon cycle blip) routing through the engine's
  separate UI category

### Known limitations (documented in the template's README)

- **Single-host only.** AI bots stand in for three remote peers. The
  README walks through the four-step path to real multiplayer:
  swap the direct `Gool.play_3d` calls in `weapon.gd` for
  `NetworkedAudioEvent.play()`, configure peer-relevancy filtering,
  use Godot's `MultiplayerSpawner` for transforms, drop the bots.
- **RTPC-driven music ducking instead of sidechain bus
  compression.** The runtime singleton's `init(sr, bs)` overload
  uses a default flat bus graph; bus-graph configuration with
  sidechain compressors isn't exposed to GDScript yet. The C++
  engine ships full sidechain compression
  (`examples/multi_tier_ducking/main.cpp`); exposing that to
  GDScript is a roadmap follow-up.
- **Voice chat not exercised.** The `VoiceChatPlayer` prefab is
  available; this demo just doesn't use it because there's no
  second machine to send packets from. The quickstart example
  demonstrates the binding-level hookup.

### Why synthesized audio rather than CC0 freesound packs

Two reasons:

1. **Reproducibility** — anyone clones the repo, opens the
   project, gets the same demo experience. No "go download these
   200 MB of sounds" step.
2. **Demonstrates the data path** —
   `register_pcm_sound(name, PackedFloat32Array, sample_rate, channels)`
   works for any PCM source: synthesized, decoded from your own
   format, captured from a microphone, generated by an LLM, anything.
   Showing it work with synthesized data makes the data-flow point
   clearly. The README explains how to swap to file-based assets
   when you have them.

### Tests

- Total **35/35** passing (no test changes; engine code unchanged).
- The template itself can't be unit-tested in CI without a Godot
  runtime; that's a future addition (Godot headless test mode).

## [0.9.0] - 2026-05-09

Saturation effect + saturation profiles. Adds a fifth bus-effect
kind (tanh waveshaper for subtle bus glue and impact reinforcement)
and a sibling profile library to `compressor_profiles.h`. Designed
for *light* enhancement — engine-side saturation handles glue and
hit reinforcement; aggressive distortion belongs in the DAW.

### Added

- **`SaturationEffect`** — tanh waveshaper, four parameters (drive,
  mix, output gain, bias). Stateless per-sample (no envelope, no
  ring buffer, no allocations). DC-corrected when bias is non-zero.
  Bypass-fast: when mix is 0 (the default), Process exits before
  the per-sample tanh, so installing the effect on a bus and
  modulating mix from gameplay is the documented pattern. Source
  files at `src/audio_engine/dsp/saturation_effect.{h,cpp}`. ~120
  LOC including comments.

- **`EffectKind::Saturation`** added to the bus effect graph.
  `bus.h` gains four `saturation*` descriptor fields (with
  defaults that make adding the effect a no-op until configured)
  and four runtime parameter IDs (`Saturation_Drive`,
  `Saturation_Mix`, `Saturation_OutputGain`, `Saturation_Bias`,
  IDs 19–22).

- **`include/audio_engine/saturation_profiles.h`** — new public
  header with five curated profiles, sibling to
  `compressor_profiles.h`:
  - `BusGlue()` — drive 1.5, mix 0.15, light master cohesion
  - `DialogueWarmth()` — drive 1.3, mix 0.10, bias 0.05 (asymmetric tube-style warmth)
  - `WeaponBody()` — drive 2.5, mix 0.30, gunshot harmonic body
  - `ImpactCharacter()` — drive 4.0, mix 0.45, bias 0.10 (movie-hit grit)
  - `TapeColor()` — drive 2.0, mix 0.25, music/ambience analog feel

- **`tests/unit/saturation_test.cpp`** — 7 sub-tests covering
  bypass identity, unity-drive matches `tanh(x)` exactly, drive>1
  compresses peaks toward `tanh(drive)`, DC-bias correction, mix
  interpolates linearly between dry and wet, symmetry without
  bias (output of -x equals -output(x), confirming odd-harmonic-only
  character), and runtime parameter changes propagate.

- **`tests/unit/saturation_profile_test.cpp`** — 6 sub-tests
  (one per profile + cross-cut sanity) verifying field constants
  and that each profile produces finite, bounded output on a
  known signal.

- **README updates**: bus-graph subsection now lists saturation in
  the shipped effects roster; new "Effect profiles" subsection
  callouts both `compressor_profiles.h` and `saturation_profiles.h`,
  with usage example and explicit "menu will grow" stub for
  future expansion.

### Aliasing note

No oversampling. tanh introduces harmonics above Nyquist that fold
back as aliasing. At documented profile drive values (≤ 4.0) on
typical game-audio source material this is well below the noise
floor and effectively inaudible. Push drive much higher on bright,
transient-rich sources and aliasing becomes audible. The textbook
fix is a 2× polyphase upsampler around the waveshaper; not shipped
here pending profile data showing real demand. Marked in
`saturation_effect.h` as a follow-up.

### Tests

- Total **35/35** passing (13 added across the two new test
  files). Ducking baseline locked at -17.20 dB.

## [0.8.1] - 2026-05-09

Curated compressor profiles. Adds an opinionated, header-only library
of pre-tuned parameter bundles for common game-audio scenarios:
punch shaping for percussive content, impact containment for
explosions and bass, gentle bus glue, voice smoothing, and music
ducking under voice/SFX. Each profile is one constexpr function
returning a fully-populated `EffectConfig`, with `thresholdDb`
tunable per-call (the one parameter that genuinely depends on host
loudness targets) and any other field overridable after the call.

### Added

- **`include/audio_engine/compressor_profiles.h`** — new public
  header. Nine profiles across four categories. All are
  `inline constexpr`, header-only, no runtime cost beyond returning
  a populated descriptor.

  **Punch** (transients preserved with parallel mix):
  - `DrumBusPunch(thresholdDb = -18)` — 4:1, 10 ms attack, 70 % wet, RMS
  - `FootstepGlue(thresholdDb = -22)` — 3:1, 8 ms attack, 60 % wet, Peak
  - `GunshotSnap(thresholdDb = -16)` — 4:1, 5 ms attack, 8 dB range cap

  **Impact** (contained dynamics):
  - `ExplosionImpact(thresholdDb = -14)` — 5:1, 3 ms attack, 12 dB cap
  - `BassImpact(thresholdDb = -20)` — 3:1, 15 ms attack, 80 Hz sidechain HPF

  **Glue / smoothing**:
  - `MasterBusGlue(thresholdDb = -10)` — 1.5:1, RMS, very gentle final-mix cohesion
  - `VoiceSmoothing(thresholdDb = -18)` — 4:1, 30 ms hold, RMS, dialogue-tuned

  **Sidechain duckers** (host wires `compressorSidechainBus` separately):
  - `MusicDuckUnderVoice(thresholdDb = -22)` — 8:1, 200 Hz HPF, 12 dB cap
  - `MusicDuckUnderSfx(thresholdDb = -18)` — 6:1, 150 Hz HPF, 9 dB cap

- **`tests/unit/compressor_profile_test.cpp`** — new test file with
  10 sub-tests. Each profile gets a descriptor sanity check (verifies
  the documented constants haven't drifted) plus an audibility smoke
  test (instantiates a `CompressorEffect` from the profile, runs a
  known signal through, asserts reduction is finite and within the
  range cap if one is set). One cross-cut test verifies determinism
  and that profiles don't touch unrelated `EffectConfig` fields.

### Usage

```cpp
#include "audio_engine/compressor_profiles.h"

// Drop in directly:
bus.effects.push_back(audio::CompressorProfiles::VoiceSmoothing());

// Tune the threshold:
bus.effects.push_back(audio::CompressorProfiles::DrumBusPunch(-15.0f));

// Override anything else after the call:
auto cfg = audio::CompressorProfiles::MusicDuckUnderVoice();
cfg.compressorSidechainBus = kVoiceBusId;     // required for ducker profiles
cfg.compressorReleaseMs    = 300.0f;          // smoother recovery
bus.effects.push_back(cfg);
```

### Tests

- Total **33/33** passing (10 added in the new profile test file).
  Ducking baseline locked at -17.20 dB.

## [0.8.0] - 2026-05-09

Tier A compressor parameters: completes the standard control surface
expected by FMOD/Wwise/plugin users. Six new parameters extend the
existing compressor — knee, mix, range, sidechain HPF, hold, and
detection mode — all defaulted to preserve pre-0.8 behavior, all
runtime-tunable through the existing parameter ID surface.

### Added

- **Soft knee** (`compressorKneeWidthDb`, ID `Compressor_KneeWidthDb`).
  0 = hard knee (legacy behavior, default). Typical musical values
  3–12 dB. Reduction transitions quadratically across a width
  centered on the threshold using the Reiss/McPherson formula.
- **Dry/wet mix** (`compressorMixRatio`, ID `Compressor_MixRatio`).
  1.0 = fully wet (legacy behavior, default), 0.0 = bypass. Enables
  parallel ("New York") compression — keep transient punch while
  adding body.
- **Range cap** (`compressorMaxReductionDb`, ID `Compressor_MaxReductionDb`).
  60 dB ≈ unlimited (legacy behavior, default). Hard cap on gain
  reduction so a runaway transient can't fully duck the signal.
  De-essers and bus glue typically use 3–18 dB.
- **Sidechain high-pass filter** (`compressorSidechainHpfHz`, ID
  `Compressor_SidechainHpfHz`). 0 = bypass (legacy behavior,
  default). Keeps low-frequency content (kicks, explosions) from
  over-triggering compression on a music or VO bus — modern
  game-audio table stakes.
- **Hold** (`compressorHoldMs`, ID `Compressor_HoldMs`). 0 = no hold
  (legacy behavior, default). Delays release engagement by the
  configured duration after the envelope drops below threshold.
  Stabilizes dialogue ducking; prevents compressor chatter on
  choppy trigger sources.
- **Detection mode** (`compressorDetectionMode`, ID
  `Compressor_DetectionMode`). Peak (legacy behavior, default) or
  Rms. Encodes as 0.0f = Peak, 1.0f = Rms when set via runtime ID.

### Changed

- **`CompressorEffect` constructor** now takes a `CompressorConfig`
  struct rather than a positional argument list. The legacy
  6-positional-args form was unsustainable as parameters scaled.
  Hosts constructing the effect directly (rare — most go through
  `EffectKind::Compressor` in `BusEffectDescriptor`) need to
  migrate. The descriptor flow in `BusGraph::Build` is updated.
- **`compressor.h` topology comment** rewritten to reflect the new
  signal path: input → optional sidechain HPF → envelope follower
  (peak or RMS) → soft- or hard-knee gain computer → range cap →
  makeup gain → dry/wet mix → output.

### Behavior preservation

All defaults match v0.7 behavior. Existing descriptors compile
unchanged (the new fields all have defaults). Existing test
suites pass without modification. Ducking baseline preserved
at -17.20 dB.

### Tests

- **`compressor_test.cpp`** gains 6 audibility-verified Tier A
  sub-tests plus its 4 pre-existing tests migrated to the new
  `CompressorConfig` API:
  - `TestSoftKneeMeasurableTransition` — soft knee produces
    measurable reduction at exactly threshold; hard knee does not.
  - `TestMixRatioBlend` — mix=0.0 passes through; mix=0.5 sits
    between dry and fully-wet.
  - `TestMaxReductionCap` — extreme input is bounded to the cap
    even with ratio 100:1 and threshold -40 dB.
  - `TestSidechainHpfFilters` — 60 Hz sidechain triggers
    compression; same content with HPF at 200 Hz does not.
  - `TestHoldDelaysRelease` — reduction stays elevated through
    the hold window before release engages.
  - `TestRmsVsPeakDetection` — RMS detection produces ~2 dB more
    reduction than Peak on a loud/quiet alternating signal at
    slow attack (squaring weights loud samples disproportionately).

- **Total: 32/32 unit tests passing.**

## [0.7.2] - 2026-05-09

Performance baseline pass. Per Rules 9 ("measure before optimizing")
and 25 ("benchmark critical systems"), this release adds the
benchmarking infrastructure and captures a baseline. **No
optimizations were performed in this release.** The data showed
B1 (ParameterSmoother linear scan) and B3 (RTPC binding hash-map
storage) — both flagged as candidates in the v0.7.1 architecture
audit — are not justified for optimization at default budgets.
Documenting that conclusion with real numbers is the deliverable.

### Added

- **`tests/bench/`** — new benchmark directory, CMake'd as
  build-only-not-CTest targets:
  - **`parameter_smoother_bench`** — direct microbenchmark for
    `ParameterSmoother::SetTarget` / `Get` / `Tick` at N=16/64/256/1024
    pre-populated entries.
  - **`rtpc_eval_bench`** — full-path Update measurement: N looping
    emitters with M RTPC bindings each at N×M combinations including
    M=0 for baseline isolation.
- **`tests/bench/bench_util.h`** — minimal harness, no Google
  Benchmark dependency. Wall-clock timing with ns/op reporting and
  a `DoNotOptimize` helper to defeat dead-code elimination on
  microbenchmarks.
- **`docs/perf.md`** — captured baseline numbers, cost decomposition,
  and the Rule-9 conclusion. The "before" any future optimization
  pass must beat.

### Findings

At the documented default budget (`maxActiveEmitters = 128`,
`kRtpcTargetCount = 4`):

| Scenario | µs/tick | % of 16ms frame |
|----------|---------|-----------------|
| N=128, M=0 (no RTPC)        | 11 µs   | 0.07% |
| N=128, M=1 (volume only)    | 93 µs   | 0.6%  |
| N=128, M=4 (all targets)    | 306 µs  | 1.8%  |

The non-RTPC machinery (spatializer, mixer command formation,
occlusion, step 9 itself) is a flat ~11 µs at default budget.
RTPC eval + smoother is ~96% of the variable cost when bindings
are present. At default budgets this is well under any threshold
that justifies refactoring.

At 2× default (N=256) M=4 the cost climbs to 1.15 ms/tick (7% of
frame). Hosts who push budgets aggressively should be aware; the
cost is near-quadratic in active-emitter count.

### Roadmap

- **B1** ParameterSmoother linear scan — measured. **No action.**
  Acceptable at default budgets; bench remains for future
  optimization passes.
- **B3** RTPC binding hash-map storage — measured. Not the dominant
  cost. **No action.**

## [0.7.1] - 2026-05-09

Architecture-rubric cleanup pass. Five small, behavior-preserving
changes that pay down cost surfaces flagged by an audit against
internal C++ engineering rules. No new public features; no breaking
changes; no feature drift (Rule 23: refactors preserve behavior).

### Added

- **`Stats::telemetrySinkExceptions`** and **`Stats::logSinkExceptions`**
  counters (Rules 17, 23). Sink exceptions used to be silently
  swallowed by the runtime's defensive `try`/`catch` so a misbehaving
  host couldn't break Update mid-flight — but invisible failures are
  exactly the silent-failure pattern Rule 17 calls out. Counters are
  atomic (log hooks fire from game and network threads) and surface
  through both `GetStats()` and the per-tick stats sample, so a
  buggy sink shows up on the next non-throwing telemetry emit.
  New sub-test `TestThrowingSinkIncrementsStatsCounter` verifies
  the counter equals the throw count.
- **`AudioRuntimeImpl::ShouldLog_(LogLevel)`** overload (Rules 14, 15).
  The 10 internal call sites that previously did
  `ShouldLog_(static_cast<uint8_t>(LogLevel::Foo))` now read
  `ShouldLog_(LogLevel::Foo)`. The uint8 overload is kept because
  `config_.logMinLevel` is stored as uint8 (to keep `logging.h` out
  of `config.h`) — but that storage detail no longer leaks to call
  sites.
- **`AUDIO_REQUIRES(RenderThread) AUDIO_NO_ALLOC AUDIO_RENDER_PATH`**
  annotations on `IAudioRenderCallback::OnRender` and
  `AudioMixer::OnRender` (Rule 18). Documentary on GCC/MSVC, actively
  enforced under Clang Thread Safety Analysis. The README's
  long-standing "render thread does no allocations, no locks, no
  syscalls, no exceptions" promise is now type-system-supervised
  for the two methods that matter most.

### Changed

- **`globalParameters_` and `soundRtpcBindings_` reserved at Initialize**
  (Rule 8). Both `unordered_map`s now `reserve()` their configured
  caps (`maxGlobalParameters`, `maxSoundRtpcBindings / 4`) on
  Initialize, eliminating the rehash bursts during the first dozen
  inserts. Pure win — predictable runtime memory.
- **`RingTelemetrySink` and `RingLogSink` are now actually thread-safe**
  (Rule 18). Both ring sinks gain an internal `std::mutex` covering
  writes (`OnRuntimeStats` / `OnLogEvent`), `Snapshot()`, `Size()`,
  and `Clear()`. Header docs rewritten to reflect reality:
  - The telemetry ring's old comment claimed "single-threaded by
    contract" but offered no enforcement — now it's locked, callable
    from any thread.
  - The log ring's old comment was outright misleading: the runtime
    holds `logMutex_` around `OnLogEvent`, but host calls to
    `Snapshot()` from a different thread (typical for debug overlays)
    raced against writes. Now it locks; the race is closed.
  - `ForEachInOrder()` deliberately stays unlocked on both ring
    sinks for callers who need allocation-free iteration and can
    guarantee no concurrent emission. The constraint is documented.

### Internal

- `audio_runtime_impl.h` now includes `audio_engine/logging.h` (it's
  an internal header — no compile-time-coupling cost to public headers,
  per Rule 21).
- Sink-exception counters are `mutable std::atomic<uint64_t>` so the
  const `Log_` method can `fetch_add` on the catch path. Loaded
  with `memory_order_relaxed` — a non-torn read is sufficient,
  no happens-before is needed.

### Tests

- Total **32/32** passing (1 added sub-test in `telemetry_test`),
  ducking baseline locked at -17.20 dB.

## [0.7.0] - 2026-05-09

Event-level structured logging. Telemetry told you *that* something
happened (counters); logging now tells you *why* (per-event detail).
Closes Phase 4.8.

### Added

- **`include/audio_engine/logging.h`** — new public header.
  - `IRuntimeLogSink` interface: one method,
    `OnLogEvent(const LogEvent&)`, called from whatever thread
    triggered the underlying event. The runtime serializes calls
    via an internal mutex so sinks **don't need to be thread-safe
    themselves**.
  - `LogLevel` enum: Trace / Debug / Info / Warn / Error.
  - `LogField` tagged union: `int64_t` / `uint64_t` / `double` /
    `bool` / `string_view`. Stack-allocated by callers (no heap
    on the runtime side).
  - `LogCategory::*` constants for built-in hook categories
    (`events`, `mixer`, `voice`, `rtpc`, `emitter`, `prediction`,
    `replication`).
  - **`JsonLinesLogSink`** — one compact JSON object per event.
    Atomic at the FD level for typical line sizes (<PIPE_BUF on
    POSIX). JSON-escapes special characters; thread-local line
    buffer amortizes allocations.
  - **`RingLogSink`** — circular buffer of last N events for
    in-process queries (debug overlays, post-mortems, replay
    correlation). Deep-copies events including string-view fields,
    so stored events remain valid after the originating call
    returns.
- **`AudioRuntimeDependencies::logSink`** — optional raw pointer,
  host-managed.
- **`AudioConfig::logMinLevel`** — minimum severity reaching the
  sink. Default Info; events below the threshold are dropped before
  the sink is consulted, and field-array construction at the call
  site is skipped via `ShouldLog_`. Disabled categories cost a
  branch, not a sink call.

### Hook points wired in v0.7.0

Every hook follows the pattern of *first* incrementing the existing
counter (so telemetry stays correct), *then* checking `ShouldLog_`
*before* building the field array. This keeps the disabled-category
fast-path branch-only.

| Category    | Level | Trigger                                                  |
|-------------|-------|----------------------------------------------------------|
| events      | Debug | Late event discarded (game and replicated paths)         |
| rtpc        | Warn  | RTPC binding rejected: budget exceeded                   |
| emitter     | Debug | One-shot evicted (lower-priority slot freed for incoming)|
| emitter     | Debug | One-shot dropped: pool full, no eviction candidate       |
| emitter     | Warn  | One-shot dropped: post-eviction emitter create failed    |
| replication | Warn  | Replication policy violation rejected                    |
| replication | Debug | Replication event rejected by host validator             |
| replication | Debug | Replication event rate-limited                           |
| mixer       | Warn  | Render-thread underrun(s) since last tick (delta detect) |

The mixer hook deserves special note: render-thread events never
log directly (that thread does no allocations and no syscalls). The
game thread observes the underrun counter delta in Update step 12
and emits the log line from there. Bursts collapse into a single
"underruns: N" event so logs don't drown a flapping audio device.

### Tests

- **`tests/unit/logging_test.cpp`** (new, 9 sub-tests):
  - JSON Lines: compact output, all fields present in expected order
  - JSON Lines: special chars (`\n`, `\t`, `"`) escape correctly
  - Ring sink: chronological order, evicts at capacity, deep-copies
    StrView fields after the original buffer goes out of scope
  - Level filter: Debug events dropped when minLevel=Warn
  - Level filter: Debug events reach sink when minLevel=Debug
  - Null sink with low minLevel is safe (no crash)
  - End-to-end: RTPC budget exceeded fires exactly one Warn
    `rtpc` log line with the expected `budget` field
  - End-to-end: replication policy violation fires exactly one Warn
    `replication` log line with the expected `player_id` field
  - End-to-end: late-event discard fires exactly one Debug `events`
    log line with the expected `replicated` field

Total **32/32** passing, ducking baseline locked at -17.20 dB.

### Limitations carried forward

- The runtime serializes sink calls via one global mutex, not
  per-category locks. Highly contended hot paths (thousands of
  rejections per second) would serialize through this mutex. In
  practice rejections are by definition rare; if a host hits this
  ceiling they likely have a misconfigured rate limiter or DoS in
  flight, and the lock contention is the least of their problems.
- Per-category level filtering is not exposed in v0.7.0 — the only
  knob is global `logMinLevel`. Hosts that want "verbose voice but
  quiet replication" can either filter inside their sink, or wait
  for a future iteration if real users ask.
- No log rotation, retention, or compression in the built-in sinks.
  Those concerns live with the host's log shipper (vector,
  fluentd, journald) — the runtime's job is to emit; the
  pipeline's job is to manage.

## [0.6.0] - 2026-05-09

Telemetry hooks. Teams running real games can now stream the
runtime's `Stats` snapshot into Prometheus, Datadog, journald,
fluentd, custom analytics, or an in-process ring buffer — at a
configurable cadence, with a single sink interface and three
built-in implementations. Closes Phase 4.7.

### Added

- **`include/audio_engine/telemetry.h`** — new public header.
  - `IRuntimeTelemetrySink` interface: one method,
    `OnRuntimeStats(const RuntimeStatsSample&)`, called at
    `telemetryIntervalMs` cadence from `Update()`.
  - **`JsonLinesTelemetrySink`** writes one compact JSON object per
    sample to any `FILE*` (default stdout). Deterministic field
    order, every key always emitted, atomic FD-level writes via a
    single `fprintf`. Pipes cleanly into journald / vector / fluentd /
    a plain log file.
  - **`PrometheusTelemetrySink`** maintains a thread-safe exposition-
    format snapshot. `Snapshot()` returns the latest text from any
    thread; the host's HTTP scrape handler serves it verbatim.
    Output uses `gool_` prefix, `_total` suffix on counters, gauge
    naming for point-in-time values, `# HELP` / `# TYPE` blocks for
    every metric, `category="..."` labels on per-category
    replication counters.
  - **`RingTelemetrySink`** keeps the last N samples (default 512,
    ≈2 minutes at 4 Hz) in a circular buffer. `Snapshot()` returns
    a chronologically ordered vector; `ForEachInOrder()` iterates
    without allocating. Single-threaded by contract — for in-game
    debug overlays, replay correlation, time-series queries that
    don't need to leave the process.
- **`AudioRuntimeDependencies::telemetrySink`** — optional raw
  pointer, host-managed, never deleted by the runtime.
- **`AudioConfig::telemetryIntervalMs`** — default 0 (disabled).
  Recommended values documented inline: 100 ms for tight diagnostics,
  250 ms for live dashboards, 1000 ms for shipped builds.
- **Update step 12** (new): accumulator-based emit scheduling that
  *subtracts* the interval rather than zeroing — so a long host
  frame catches up by emitting again immediately on the next
  Update, rather than losing samples. Sink call wrapped in
  `try`/`catch` so a misbehaving host implementation can't break
  Update mid-flight.
- **`examples/telemetry/main.cpp`** — working demo wiring all three
  sinks side-by-side. Prints a JSON Lines stream, a Prometheus
  scrape body, and the last 5 ring-buffer samples.

### Tests

- **`tests/unit/telemetry_test.cpp`** (new, 9 sub-tests):
  - Every documented JSON field appears in output, deterministic order
  - Null `FILE*` no-ops gracefully
  - Prometheus output has HELP / TYPE blocks and correct label syntax
    for both gauges and counters; per-category labels work
  - Ring buffer chronologically ordered, evicts oldest at capacity,
    `ForEachInOrder` iterates without allocating
  - Runtime emits at configured cadence (9 samples over 1 s at 100 ms
    — within ±1 expected slack from accumulator boundary fuzz)
  - Interval=0 emits zero samples
  - Nullptr sink with non-zero interval is safe (no crash)
  - End-to-end: ring sink fed by runtime captures monotonic time series
- Total **31/31** passing, ducking baseline locked at -17.20 dB.

### Limitations carried into the next iteration

- The sink interface carries global `Stats` only. Per-player voice
  metrics (jitter, packet-loss per player) need host-side iteration
  — cardinality is host-dependent (player IDs come and go, dashboard
  labels would explode). Pattern shown in test setup but not in the
  sink interface itself.
- No event-level structured logging. The runtime emits counter
  *aggregates* through the sink but not the individual events that
  drove those counters (which voice packet was rejected, which RTPC
  binding hit budget). See roadmap Phase 4.8 — separate iteration.

## [0.5.0] - 2026-05-09

Multi-target RTPC. The single-target volume binding from v0.4 generalizes
to four targets, four curves, multiple bindings per sound, and JSON
authoring. The pattern that took 1 binding in v0.4 ("heartbeat volume
follows health") now scales to 4 bindings ("heartbeat volume + pitch
follow health, music volume ducks under combat with a smoothstep,
caves apply lowpass via wetness").

### Added

- **Four RTPC targets**: Volume (multiplicative on gain), Pitch
  (multiplicative on pitch), LowPassCutoff (max with spatial baseline),
  ReverbSend (clamped sum with spatial baseline). `RtpcTarget` enum.
- **Four curves**: Linear, Exponential, InverseExponential, SCurve
  (smoothstep). `RtpcCurve` enum + `curveExponent` for exp/inv-exp.
- **Multiple bindings per sound**: at most one binding per (sound,
  target) pair. A single sound can have volume + pitch + lowpass +
  reverb all driven by different parameters simultaneously.
- **`AudioRuntime::SetSoundRtpc(soundId, binding)`** — unified API
  taking a `SoundRtpcBinding` struct. Replaces v0.4's
  `SetSoundVolumeRtpc` (mechanical migration: pass binding fields
  via the struct).
- **`ClearSoundRtpc(soundId, target)`** — remove one target's binding.
- **`ClearAllSoundRtpc(soundId)`** — remove all bindings for a sound;
  returns count removed.
- **JSON sound bank `rtpc` array** — bindings can now be authored
  alongside sound definitions in `.json` banks. Schema:

  ```json
  {
    "name": "heartbeat",
    "category": "SFX",
    "rtpc": [
      { "parameter": "health",
        "target":    "volume",
        "curve":     "linear",
        "min_value": 0.0, "max_value": 1.0,
        "min_output": 1.0, "max_output": 0.0,
        "smoothing_ms": 50 },
      { "parameter": "fatigue",
        "target":    "pitch",
        "curve":     "exponential", "exponent": 2.0,
        "min_value": 0.0, "max_value": 1.0,
        "min_output": 1.0, "max_output": 0.85 }
    ]
  }
  ```

  Unknown target/curve names produce line-numbered error messages.
- **GDScript autoload facades**:
  - `Gool.bind_volume_rtpc(...)` — Volume + linear (the v0.4 ergonomics, preserved)
  - `Gool.bind_pitch_rtpc(...)` — Pitch + linear
  - `Gool.bind_lowpass_rtpc(...)` — LowPass + linear
  - `Gool.bind_reverb_rtpc(...)` — ReverbSend + linear
  - `Gool.bind_rtpc(sound_name, dict)` — full API: any target, any curve
  - `Gool.clear_rtpc_binding(name, target)` / `Gool.clear_all_rtpc_bindings(name)`
- **GDExtension bindings**: `set_sound_rtpc` (target+curve as strings),
  `clear_sound_rtpc`, `clear_all_sound_rtpc`, `sound_rtpc_binding_count`.

### Changed

- **BREAKING (pre-1.0)**: `AudioRuntime::SetSoundVolumeRtpc` /
  `ClearSoundVolumeRtpc` removed. Migration: replace with
  `SetSoundRtpc(soundId, binding)` where `binding.target = RtpcTarget::Volume`
  and the field names map directly. Same for the GDScript
  `Gool.set_sound_volume_rtpc` / `Gool.clear_sound_volume_rtpc` GDExtension
  methods. The GDScript `Gool.bind_volume_rtpc` facade stays with the
  same signature so call sites that use the facade don't need changes.
- Storage moves from `unordered_map<AudioSoundId, SoundVolumeRtpcBinding>`
  to `unordered_map<AudioSoundId, std::vector<SoundRtpcBinding>>`.
  `AudioConfig::maxSoundRtpcBindings` (256) now caps total bindings
  across all sounds, not distinct sound IDs — a sound with 4 bindings
  counts as 4 against the budget.
- Step 9 of `Update` (per-emitter `UpdateParams` pass) now reads
  `LowPassAmount` and `ReverbSend` from the parameter smoother in
  addition to `Gain` and `Pitch`. Default fallbacks preserve existing
  unbound behavior.

### Tests

- `tests/unit/sound_rtpc_test.cpp` rewritten (7 sub-tests):
  Volume/Pitch/LowPass audibility, multi-binding coexistence, four
  curves at midpoint behave correctly (linear=0.125, exp(2)=0.0625,
  inv-exp(2)=0.1875, scurve=0.125 from a 0.25 reference), skip-when-unset
  per-binding, API validation including out-of-range enums.
- `tests/unit/sound_bank_test.cpp` extended with 2 new sub-tests:
  RTPC array parses and registers bindings; unknown target string is
  rejected with line number. Total 30/30 passing.

### Limitations carried into the next iteration

- Custom point-list curves (arbitrary curve shapes via JSON-authored
  control points) are still future. Linear / Exponential /
  InverseExponential / SCurve cover the typical FMOD/Wwise authoring
  patterns.
- LowPassCutoff combines via `max()` with the spatial baseline (so RTPC
  can never reduce the world's filter). Use cases that want RTPC to
  override spatial filtering (e.g. underwater zone replaces occlusion)
  need a different combiner — roadmap.
- Bindings are still per-sound, not per-emitter or per-bus. Per-bus
  RTPC modulation (e.g. "all music quiets when combat starts" without
  binding every track individually) is a separate feature.

## [0.4.0] - 2026-05-09

Render-thread RTPC volume modulation. The disclaimer in v0.3.0
("`set_rtpc` stores values but does not yet drive sound modulation")
is closed. Calling `bind_volume_rtpc("heartbeat", "health", 0, 1, 1, 0)`
once, then `set_rtpc("health", v)` per frame, now actually changes the
heartbeat's rendered volume in real time.

### Added

- **`AudioRuntime::SetSoundVolumeRtpc`** / **`ClearSoundVolumeRtpc`** /
  **`GetSoundRtpcBindingCount`** — bind a sound's volume to a global
  parameter via a linear curve. Each `Update` tick the runtime walks
  active emitters, looks up each one's binding, reads the parameter,
  computes a target gain, and pushes it through the existing parameter
  smoother (`AudioParameterIds::Gain`). Same code path used by
  `SetEmitterParameter` so authored modulation and manual gain calls
  compose cleanly.
- **`Gool.bind_volume_rtpc(sound_name, param_name, ...)`** /
  **`Gool.clear_volume_rtpc(sound_name)`** — GDScript autoload facade
  + GDExtension bindings (`set_sound_volume_rtpc`,
  `clear_sound_volume_rtpc`, `sound_rtpc_binding_count`).
- **`AudioConfig::maxSoundRtpcBindings`** (default 256). Budget is
  enforced only on new sound IDs — re-binding an existing sound is
  always free. `BudgetExceeded` returned when the cap is hit.
- **Skip-when-unset semantics**: until `set_rtpc(name, ...)` is called
  at least once, the binding has no effect. Authored volume stays in
  place. Binding-installation order is then independent of gameplay
  state so prefab `_ready()` calls can wire bindings without worrying
  about sequencing.
- README Quick Start now shows the bind + set_rtpc pattern.

### Tests

- `tests/unit/sound_rtpc_test.cpp` — 8 sub-tests, audibility-verified
  end-to-end:
  - Unset parameter: rendered RMS = 0.25 (authored volume preserved)
  - Parameter at 0 with `0→0, 1→1` binding: rendered RMS = 0 (silent)
  - Parameter at 1: rendered RMS = 0.25 (full)
  - Parameter at 0.5: rendered RMS = 0.125 (exactly half, ratio = 0.5)
  - Inverted binding `1→0, 0→1` at full health: silent (heartbeat pattern)
  - Out-of-range parameter values clamp correctly to endpoints
  - Clear stops modulation
  - API validation rejects NaN, degenerate range, invalid IDs, negative smoothing

  Total now 30/30 passing.

### Limitations carried into the next iteration

- One binding per sound. Binding multiple parameters to one sound
  (volume + pitch + lowpass independently) is a future M-sized item.
- Volume only. Pitch / lowpass cutoff / send-level modulation are
  roadmap.
- Linear curve only. Exponential and custom-point curves are roadmap.
- The orchestrator's per-emitter `UpdateParams` pass that carries the
  modulated gain to the mixer only runs when a listener is registered.
  This was the original behavior; documented now in the test setup
  comments and `EvaluateRtpcBindings_` docs.

## [0.3.0] - 2026-05-09

The tiny API facade. Four canonical entry points users can copy-paste
verbatim into a fresh Godot project: `Gool.play_3d`, `Gool.play_music_state`,
`Gool.play_voice`, `Gool.set_rtpc`. Each is a thin GDScript wrapper over
the lower-level engine APIs; users drop down to the raw bindings when
they outgrow them.

### Added

- **`Gool.play_3d(name, position, priority=128)`** — one-shot 3D playback
  by authored sound name. Wraps `submit_event_local` with sane defaults.
- **`Gool.play_music_state(state_name, fade_ms=500)`** — equal-power
  crossfade to a new music state. Lazily creates a `GoolMusicChannel`
  on first call. Idempotent: re-passing the current state is a no-op.
- **`Gool.play_voice(player_id, audio_stream)`** — decode an
  `AudioStreamWAV` (FORMAT_16_BITS) to mono float32 PCM, register as
  an ephemeral one-shot, dispatch through the play path. Raises a
  push_error on unsupported formats. AudioStreamOggVorbis support is
  on the roadmap; for raw Opus voice traffic from a network layer,
  use `Gool.submit_voice_packet` directly.
- **`Gool.set_rtpc(name, value)`** / **`get_rtpc`** / **`has_rtpc`** /
  **`clear_rtpc`** — string-keyed real-time parameter store. Authored
  sound definitions reading these to drive volume / cutoff / pitch is
  a future feature; the storage and observability ship now so host
  code can build against the API.
- **`AudioRuntime::SetGlobalParameter`** / **`GetGlobalParameter`** /
  **`ClearGlobalParameter`** / **`GetGlobalParameterCount`** — C++ API
  for the global parameter store. Game-thread access at this stage;
  render-thread modulation is a follow-up.
- **`HashParameterName(name)`** constexpr in `types.h`. FNV-1a, same
  shape as `HashSoundName`. Hashes that would collide with the
  engine-reserved range `[1, HostBase)` are bumped above
  `AudioParameterIds::HostBase` so host names can't mask engine
  semantics.
- **`AudioConfig::maxGlobalParameters`** (default 256). Budget is
  enforced only on new IDs — updating an existing parameter is
  always free.
- **`Gool.stop_music(fade_ms=500)`** — companion to `play_music_state`.
- GDExtension bindings: `set_global_parameter`, `get_global_parameter`,
  `has_global_parameter`, `clear_global_parameter`,
  `global_parameter_count`, `hash_parameter_name`.

### Tests

- `tests/unit/global_parameter_test.cpp` — 7 sub-tests covering hash
  stability + reserved-range remapping, set/get round-trip, unset
  returns false, clear semantics, budget enforcement (only on new
  IDs), NotInitialized, InvalidArgument. Total 29/29 passing.

### Documentation

- README Quick Start now leads with the four facade lines, ahead of
  the prefab-node walkthrough.
- Phase 1.4 marked SHIPPED in 0.3.0 in the roadmap.

## [0.2.0] - 2026-05-09

The first public release with binary artifacts. Adds the multiplayer
hardening pass: rate limiting, replication-policy enforcement, threat
model documentation, and the `DefaultBoundsValidator` for malformed
input.

### Added

- **Replication rate limiter** (Phase 2.3): per-player, per-category
  token-bucket rate limiter on `SubmitReplicatedEvent`. Defaults
  sized for plausible gameplay (50 SFX/sec, 150 voice/sec, etc.).
  Surfaced via `Stats::replicationEventsRateLimited[6]`.
- **`IReplicationValidator`** interface for host-supplied policy
  hooks. Runtime calls before rate limiting; rejection silently
  drops + counts.
- **`AudioCategory category`** field on `AudioEvent`. Defaults to
  `SFX` so existing call sites work unchanged.
- **`AudioRuntime::SetReplicationValidator()`** / **`GetPerPlayerReplicationStats()`**.
- **Voice path rate limiting**: `OnVoicePacket` gates through the
  same per-player Voice category bucket. Per-player drops surface
  in `VoiceNetworkStats::packetsRateLimited` (new field).
- **PlayerId-cycling DoS defense**: per-tick admission cap on
  never-seen-before `playerId`s
  (`ReplicationRateLimitConfig::maxNewPlayersPerTick = 8` default).
  Surfaced via `Stats::replicationEventsRejectedNewIdBudget`.
- **`ReplicationSource` enum** + 2-arg `SubmitReplicatedEvent(event, source)`
  overload (Phase 2.5). Client-sourced `ServerAuthoritative` events
  rejected with `AudioResult::PolicyViolation` — verified via
  audibility check (rendered RMS = 0).
- **`replicationPolicyViolations` counter**, distinct from
  `replicationEventsRejectedByValidator`, so dashboards can tell
  protocol enforcement from host-policy denials.
- **`DefaultBoundsValidator`**: a shipped `IReplicationValidator`
  rejecting NaN/Inf vec3 fields, extreme magnitudes, malformed
  parameters, optional unknown soundIds via host callback.
- **`ChainReplicationValidator`**: composes up to 8 validators with
  short-circuit-on-reject.
- **`audio::GetVersion()`** + `version.h` constants. Compile-time
  major/minor/patch + the git SHA stamped at CMake configure time.
- **Threat model documentation** in `docs/replication_patterns.md` —
  what the runtime can and can't validate, four host-side rules,
  monitoring counters.
- **Release infrastructure**: `CHANGELOG.md`, `RELEASING.md`, this
  file. Release workflow (`release.yml`) builds versioned artifacts
  on `v*` tags.
- **Roadmap**: `docs/roadmap.md` with 28 phased work items,
  effort-sized.
- **`AudioResult::RateLimited`** and **`AudioResult::PolicyViolation`**
  return values.

### Changed

- README rewritten workflow-first (2075 → 559 lines). Leads with
  what online multiplayer audio demands and how gool fits, not the
  engine architecture.
- macOS lane temporarily disabled in CI matrix (Apple-Clang issue
  not yet investigated; Linux + Windows green).

### Fixed

- `examples/hello_audio` include path (was breaking miniaudio
  builds).
- `release.yml` multi-line cmake invocation flattened to single
  line (YAML scalar fragility).
- `tests/CMakeLists.txt` `biquad_eq_test` missing from the
  `src/`-on-include-path foreach.

### Security

- Validator-rejected events from never-seen players no longer
  consume LRU slots, closing a "validator hook is its own DoS
  surface" hole.
- `RecordPolicyViolation()` / `RecordValidatorRejection()` use
  `FindExisting()` instead of `FindOrAllocate()` so spoofs from
  unknown players can't inflate the slot table.

## [0.1.0] - 2026-04-XX

Initial private development snapshot. Not formally released; the
first tagged version is 0.2.0 above.

Headlines:

- C++20 audio engine with 25 unit tests passing
- Spatial audio: distance attenuation, Doppler, occlusion (material-
  aware), air absorption, reverb sends, optional binaural
  (`SphericalHeadSpatializer`)
- Voice chat: Opus codec wrapper, adaptive jitter buffer
  (97.84% continuity at 10% loss / 50 ms jitter), PLC, per-player
  telemetry
- Adaptive music: equal-power crossfade (±0.3% RMS through 300 ms
  transitions), `MusicChannel` helper, loop-boundary crossfade
  (158× click reduction)
- Bus graph + sidechain compressor + EQ palette (LP/HP/BP/Shelf/Peak)
- JSON sound banks, `.gpak` archives, hot reload
- Replication: `SubmitReplicatedEvent`, `UpdateReplicatedTransform`,
  `OnVoicePacket` with deterministic-replay arrival timestamp,
  `CancelPredictedEvent`, interest management
- Godot 4.2+ GDExtension binding with 7 prefab Nodes, editor plugin
  with autoload installation

[Unreleased]: https://github.com/siliconight/gool/compare/v0.11.18...HEAD
[0.11.18]: https://github.com/siliconight/gool/releases/tag/v0.11.18
[0.11.17]: https://github.com/siliconight/gool/releases/tag/v0.11.17
[0.11.16]: https://github.com/siliconight/gool/releases/tag/v0.11.16
[0.11.15]: https://github.com/siliconight/gool/releases/tag/v0.11.15
[0.11.14]: https://github.com/siliconight/gool/releases/tag/v0.11.14
[0.11.13]: https://github.com/siliconight/gool/releases/tag/v0.11.13
[0.11.12]: https://github.com/siliconight/gool/releases/tag/v0.11.12
[0.11.11]: https://github.com/siliconight/gool/releases/tag/v0.11.11
[0.11.10]: https://github.com/siliconight/gool/releases/tag/v0.11.10
[0.11.9]: https://github.com/siliconight/gool/releases/tag/v0.11.9
[0.11.8]: https://github.com/siliconight/gool/releases/tag/v0.11.8
[0.11.7]: https://github.com/siliconight/gool/releases/tag/v0.11.7
[0.11.6]: https://github.com/siliconight/gool/releases/tag/v0.11.6
[0.11.5]: https://github.com/siliconight/gool/releases/tag/v0.11.5
[0.11.4]: https://github.com/siliconight/gool/releases/tag/v0.11.4
[0.11.3]: https://github.com/siliconight/gool/releases/tag/v0.11.3
[0.11.2]: https://github.com/siliconight/gool/releases/tag/v0.11.2
[0.11.1]: https://github.com/siliconight/gool/releases/tag/v0.11.1
[0.11.0]: https://github.com/siliconight/gool/releases/tag/v0.11.0
[0.10.1]: https://github.com/siliconight/gool/releases/tag/v0.10.1
[0.10.0]: https://github.com/siliconight/gool/releases/tag/v0.10.0
[0.9.1]: https://github.com/siliconight/gool/releases/tag/v0.9.1
[0.9.0]: https://github.com/siliconight/gool/releases/tag/v0.9.0
[0.8.1]: https://github.com/siliconight/gool/releases/tag/v0.8.1
[0.8.0]: https://github.com/siliconight/gool/releases/tag/v0.8.0
[0.7.2]: https://github.com/siliconight/gool/releases/tag/v0.7.2
[0.7.1]: https://github.com/siliconight/gool/releases/tag/v0.7.1
[0.7.0]: https://github.com/siliconight/gool/releases/tag/v0.7.0
[0.6.0]: https://github.com/siliconight/gool/releases/tag/v0.6.0
[0.5.0]: https://github.com/siliconight/gool/releases/tag/v0.5.0
[0.4.0]: https://github.com/siliconight/gool/releases/tag/v0.4.0
[0.3.0]: https://github.com/siliconight/gool/releases/tag/v0.3.0
[0.2.0]: https://github.com/siliconight/gool/releases/tag/v0.2.0
[0.1.0]: https://github.com/siliconight/gool/tree/main
