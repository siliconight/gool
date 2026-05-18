# gool engineering lessons learned

This document captures patterns we've learned the hard way during
gool's development — failure modes that have surfaced more than
once, processes that prevent regressions, and discipline the
codebase has earned over many iterations.

If you're about to ship a change to gool, scan the **Pre-ship
checklist** first. The rest of this doc is "why each item is on
the checklist."

When a new class of bug bites once, capture it here. When it bites
twice, add automated checks. When it bites three times, the
process broke — root-cause that.

---

## Pre-ship checklist

Run the sections that apply to the files your release touches. If
your change spans multiple file types, run multiple sections.

### Every release

- [ ] Version triple bumped consistently across:
  - `include/audio_engine/version.h` (kVersionMajor / kVersionMinor /
    kVersionPatch / kVersionString / kVersionFull)
  - `CMakeLists.txt` (`project(... VERSION X.Y.Z)`)
  - `tests/unit/version_test.cpp` (pinned-value comment +
    assertion at bottom)
  - `README.md` ("Current version" line)
- [ ] CHANGELOG entry under the new version header with a clear
      title summarizing what changed
- [ ] CHANGELOG link references at the bottom of the file
      (Unreleased compare URL + new tag URL)
- [ ] `bash scripts/make_source_archive.sh` builds clean
- [ ] Resulting tarball md5 captured and presented to the user

### For `.gd` (GDScript) file changes

- [ ] **Tab discipline**: zero space-leading lines in any modified
      `.gd` file. The CI smoke catches `Mixed use of tabs and
      spaces` as a fatal pattern, and the build script greps for
      `^    ` (4-space) lines.
- [ ] **Const-expression discipline**: no `const X = Name(...)`
      where `Name` is a non-built-in constructor. Specifically
      banned on the right side of `const`:
  - `PackedFloat32Array(...)`, `PackedInt32Array(...)`,
    `PackedStringArray(...)`, `PackedByteArray(...)`
  - Custom-class `.new()` calls
  - Function calls in general (even pure ones)
  - References to variables (only other consts and literals
    are valid)

  Allowed: `Color(...)`, `Vector2/3/4(...)`, `Rect2(...)`,
  `Transform2D/3D(...)`, array/dict literals `[...]` / `{...}`,
  arithmetic on the above, references to other consts.
- [ ] **Inner-class scope discipline**: if a file declares
      `class_name Foo`, an inner `class Bar` inside it must NOT
      reference `Foo.SOMETHING` anywhere in `Bar`'s body. Duplicate
      the constants into `Bar`, or pass values in through Bar's API.
- [ ] **No class_name self-references in the same file**: grep
      the file for `\b<own class_name>\.` — any hit (outside
      comments) is a parse risk in headless mode.
- [ ] **Autoload method existence**: if calling a method from
      inside the autoload's own method body without a receiver
      (e.g. `set_bus_gain_db(...)` not `_runtime.set_bus_gain_db(...)`),
      grep for `^func <method_name>` in the same file. No match
      = the call will fail at runtime even though it parses.

      Heuristic: build the set of bare `name(...)` calls in the
      file, build the set of `func name(...)` declarations, take
      the difference. **Strip string literals before matching**
      so that log messages like `push_warning("set_foo failed")`
      don't false-positive. The lazy regex `r'"[^"\n]*"'` followed
      by `r"'[^'\n]*'"` is good enough for single-line strings.

### For `.cpp` / `.h` file changes

- [ ] **All 35 audio-engine source files compile clean** with
      `-Wall -Wextra -Wpedantic` (the build script covers this)
- [ ] **MSVC `/W4 /WX` compatibility**: zero use of
      `std::strncpy` / `std::strcpy` / `std::strcat` /
      `std::sprintf` — all flagged by MSVC C4996 (deprecation
      warning that becomes a build error with `-WX`). Use bounded-
      copy helpers (see `bus_graph.cpp`'s `CopyBoundedString`) or
      `memcpy` with explicit length.
- [ ] **cppcheck-friendly patterns**: when a buffer's length
      computation is used in the same expression as the buffer
      itself, separate into two phases. Single-statement combos
      sometimes trigger false-positive `arrayIndexOutOfBoundsCond`.
- [ ] **`strnlen` is NOT in `std::`** — use the unqualified
      `strnlen(...)`. libstdc++ doesn't put it in `std` despite
      cppreference suggesting otherwise.
- [ ] `version_test` passes after version bump

### For doc work

- [ ] **Doc files go into `docs/` in the repo tree**, not into a
      scratchpad path that won't be picked up by
      `make_source_archive.sh`
- [ ] If the CHANGELOG mentions a file as part of this release,
      verify with `tar -tzf <tarball> | grep <filename>` that the
      file is actually in the built tarball before presenting
      to the user
- [ ] Cross-references to other docs use repo-relative paths so
      they work on GitHub and in local clones

### Reading CI failures

- [ ] When CI fails with multiple distinct error patterns,
      **enumerate every error before fixing**. The loudest error
      isn't always the only one. The v0.26.0 CI log showed two
      independent failures; fixing one without addressing the
      other required a third patch release.
- [ ] Specifically, read the CI's `KNOWN_REAL_ERRORS` scan output
      at the bottom of the smoke job — it's the authoritative
      list of failures, deduped from the much noisier raw log.

---

## GDScript / Godot 4 pitfalls

### Tab vs space indentation
*(Surfaced v0.23.10 – v0.23.15. Five releases bisected this.)*

GDScript requires consistent indentation per file. Mixing tabs
and spaces produces:

```
Parse Error: Mixed use of tabs and spaces for indentation.
```

This is in the CI's KNOWN_REAL_ERRORS scan as a fatal pattern.
The build script greps for any `^    ` (4-space) lines in any
`.gd` file in `godot/addons/`. All gool addon scripts use tabs
exclusively.

Why this kept biting through five releases: copy-pasting code
between scratchpad/conversation and the editor sometimes converts
tabs to spaces silently. The v0.23.15 fix was both a code fix
(re-tab everything) and a process fix (the build script's
grep is now a hard pre-ship check).

### Const expressions
*(Surfaced v0.26.0 – v0.26.3.)*

GDScript 4 restricts what's a valid constant expression on the
right side of `const`. Allowed:

- Literals: `5`, `"hello"`, `[1, 2, 3]`, `{}`
- Built-in type constructors: `Color(1, 0, 0)`, `Vector2(0, 0)`,
  `Rect2(...)`, `Vector3(...)`
- References to other consts in scope
- Arithmetic on the above: `2 * PI`, `Vector2(0, 0) + Vector2.RIGHT`

NOT allowed:

- `Packed*Array(literal)` — `PackedFloat32Array([1.0])`,
  `PackedInt32Array([...])`, etc.
- Custom-class constructors: `MyClass.new(...)`
- Generic function calls (even pure ones): `min(2, 3)` is NOT a
  valid const expression
- Variable references

Common pitfall: wanting a typed array as a const. Solution: use
plain `Array` typing with an array literal:

```gdscript
# WRONG — fails parse with "isn't a constant expression"
const MARKS: PackedFloat32Array = PackedFloat32Array([1.0, 2.0, 3.0])

# RIGHT — parses cleanly, iteration semantics identical
const MARKS: Array = [1.0, 2.0, 3.0]
```

The downside of `Array` over `Packed*Array` is variant boxing per
element (slightly more memory). For typical const arrays of
small size this is negligible.

### Inner class scope
*(Surfaced v0.26.0 – v0.26.2.)*

GDScript inner classes do NOT inherit the outer class's scope. An
inner `class Inner extends Control:` declared inside an outer
`class_name Outer extends Control` cannot access `Outer`'s
constants without explicit reference (`Outer.SOMETHING`). And
those explicit references fail in headless mode (see next
section).

Solution: make inner classes self-contained. Duplicate constants
the inner class needs into the inner class itself. Cost: ~10-20
duplicated lines. Benefit: zero load-order issues.

```gdscript
class_name Outer extends Control

const SHARED_VALUE: float = 1.0  # Outer's copy

class Inner extends Control:
    const SHARED_VALUE: float = 1.0  # Inner's copy — intentional duplicate

    func use_it():
        var x = SHARED_VALUE  # bare reference, no Outer. prefix
```

Alternative for non-constant values: pass them into the inner
class via setter or constructor. But for constants, duplication
is simpler.

### Class_name in headless mode
*(Surfaced v0.26.0 – v0.26.2.)*

Godot's `--headless` flag (used by the CI smoke job) does NOT
populate the global `class_name` registry the way `--editor`
does. References to other class_names from inside a script
being `load()`'d may fail to resolve. This produces:

```
Parse Error: Could not resolve class X, because of a parser error.
```

These lines are BENIGN for parse correctness in headless mode and
appear in the smoke log for every cross-script class_name
reference. The CI's KNOWN_REAL_ERRORS scan deliberately does NOT
include "Could not resolve class" or "because of a parser error"
patterns because they false-positive in headless mode (three
release attempts in v0.23.6, v0.23.10, v0.23.11 tried to use
these as catch mechanisms; all caused CI false positives).

What this means: headless smoke can't catch every class_name
issue. Editor-time error reporting catches them within seconds
of opening a project, which empirically has been sufficient
(the v0.23.10 cyclic-dependency episode was caught by Brannen
on first F5 attempt before CI even completed). The smoke
focuses on what it CAN catch: syntactic errors, const-expression
errors, indentation errors.

### Cyclic dependencies
*(Surfaced v0.23.10.)*

If `a.gd` references `class_name B` and `b.gd` references
`class_name A`, Godot's parser cannot resolve either at load
time. This produces:

```
Parse Error: Cyclic dependency between A and B.
```

Defense: don't have classes reference each other by class_name.
Use composition, dependency injection, or interface-based loose
coupling. If both classes truly need to know about each other,
one of them references the other by node path / autoload lookup
rather than class_name.

In gool, the GoolLog ↔ GoolLogContext episode was the canonical
case. The fix was making GoolLogContext stand alone and have
GoolLog construct it without GoolLogContext knowing about GoolLog.

### Autoload method existence
*(Surfaced v0.26.0 – v0.26.2.)*

If an autoload `Gool` declares `func handle_thing():` and inside
that function calls `set_value(...)`, that resolves to
`self.set_value(...)`. If `Gool` doesn't actually have a
`set_value` method at the autoload level (only its internal
`_runtime` object does), you get a runtime "not found in base"
error the first time `handle_thing` is called.

CI's headless smoke catches a SUBSET of these via the
`not found in base` pattern when the type analyzer can see the
missing method statically. Many slip through and surface only
in real use.

Defense: when calling a method from inside an autoload's own
method body, EITHER:

- Verify there's a `func <method_name>` declaration at the
  autoload level (grep `^func <name>` in the same file), OR
- Use an explicit receiver: `_runtime.set_value(...)` not bare
  `set_value(...)`

gool's discipline: internal autoload methods always use the
explicit receiver form. The grep `^func set_X` should match
exactly when an autoload-level setter wrapper exists.

---

## Editor-game plugin architecture

### Editor `@tool` scripts can't see the running game
*(Surfaced v0.24.0 → v0.25.0. This was a full architectural redo.)*

Godot 4 runs the editor and the running game in **separate
processes**. An editor-side `@tool` script's `get_tree()` reaches
the EDITOR's SceneTree, not the game's. Attempting to poll the
gool runtime singleton from an editor dock via `get_tree().root`
returns the editor's root and finds no `Gool` autoload — the dock
shows "Gool audio runtime not initialized" forever, even during
F5.

This was the v0.24.0 mistake — shipping a read-only mixer dock
that couldn't actually see the running game's bus stats. The
v0.25.0 fix used Godot's EngineDebugger cross-process channel:

- **Game side**: `EngineDebugger.register_message_capture("gool",
  callback)` for inbound; `EngineDebugger.send_message("gool:<msg>",
  [data])` for outbound. Only active when a debugger is attached
  (F5), zero-cost in exported builds.
- **Editor side**: declare an `EditorDebuggerPlugin`, claim a
  message prefix via `_has_capture("gool")`, receive game messages
  via `_capture(message, data, session_id)`, send via
  `EditorDebuggerSession.send_message(...)` (obtained from
  `get_session(session_id)`).

This is the supported pattern for editor↔game data in Godot 4.

### Plugin state across sessions
*(Surfaced v0.25.0 → v0.25.1.)*

`EditorDebuggerPlugin` instances persist across F5/F8 cycles —
the plugin is created once when the editor opens and destroyed
when the editor closes. Sessions (F5 runs) come and go inside
that lifetime.

The v0.25.0 dock stored per-session state in a Dictionary keyed
by session_id, accumulating across runs. On the second F5, stale
state from the first session shadowed new state from the second:
the mixer dock went blank.

The v0.25.1 fix: **single-slot state**. One `_latest_stats: Array`
and one `_current_session_id: int`. When a new session starts,
replace; don't accumulate. Diagnostic print on session start /
stop / message-receive makes the state transitions visible during
development.

### Bidirectional command channel
*(Added v0.26.0.)*

The EngineDebugger channel works both directions. Game→editor is
the meter-data flow. Editor→game added in v0.26.0 for fader drag
events: `session.send_message("gool:set_bus_gain", [bus_name, db])`
on the editor side, routed by the game-side `_on_debugger_capture`
to `_runtime.set_bus_gain_db(...)`.

The bidirectional pattern in gool: ~100 lines of channel
scaffolding (capture handler + send helpers on both sides) carries
an arbitrary number of message types. New commands are router
additions, not new channels. **Bridges pay compound interest** —
3.3b-2 (S/M/B), 3.3c (effect-param editing), 3.3d (topology) will
all reuse this scaffolding with zero further channel work.

---

## C++ portability and warning discipline

### MSVC C4996 on standard string functions
*(Surfaced v0.24.0 → v0.24.1.)*

MSVC treats `std::strncpy`, `std::strcpy`, `std::strcat`,
`std::sprintf` (and friends) as deprecated under `/W4 /WX` —
warnings that become errors. The Microsoft "secure" alternatives
(`strncpy_s`, etc.) aren't portable to other compilers.

Solution: inline bounded-copy helpers using `memcpy` with explicit
length. The canonical gool pattern (`bus_graph.cpp`):

```cpp
namespace {
void CopyBoundedString(const char* src, char* dst, size_t cap) {
    if (cap == 0) return;
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}
}  // namespace
```

This compiles clean on g++, clang, and MSVC. Note `strnlen`
(see below) is unqualified, not `std::strnlen`.

### cppcheck pessimism
*(Surfaced v0.24.1 → v0.24.2.)*

cppcheck sometimes false-positives on `arrayIndexOutOfBoundsCond`
when a loop's bounds computation is combined with the loop body
in a single expression. Two-phase implementations (compute the
bound, then use it) usually clear the warning.

`variableScope` warnings also fire on `static constexpr` locals
inside small functions where the value could be inlined; inlining
the value at the use site clears them.

The CI's `cppcheck` step does NOT have `continue-on-error`,
unlike `clang-tidy` and `lizard`. cppcheck must be clean to ship.

### `strnlen` is not in `std::`
*(Surfaced v0.24.2.)*

Despite cppreference suggesting `strnlen` is in `std::` in C++17,
libstdc++'s implementation doesn't put it there. Use unqualified
`strnlen(...)` to avoid namespace lookup failures on Linux/g++
builds.

---

## API design

### Silent no-ops
*(Surfaced v0.25.2.)*

Functions that *look* like they do something but actually do
nothing are dangerous. The gool case: `register_sound_definition`
was declared, documented, and stored the SoundDefinition data
internally — but `create_emitter` never consulted that store.
Sounds played anyway (using defaults), so smoke tests passed; the
bug only surfaced when the mixer dock revealed that a drone
sound configured as `MUSIC` was actually routing to the `Sfx` bus.

Defense: for every "register" or "set" API, follow the data flow
to the "use" site. Verify the use site actually consults the
registered data. If you can't find the use site, the register
call is dead code or a latent bug.

The mixer dock that surfaced this is itself part of the defense:
visual telemetry makes "wrong bus" mistakes obvious where
audio-only would hide them. This is a pattern — instrument the
mixer first, surface real bugs in misrouted audio, then fix them.

### Wrappers vs direct member calls
*(Surfaced v0.26.0 → v0.26.2.)*

When an autoload delegates to an internal member (e.g.
`_runtime.set_bus_gain_db(...)`), you can either:

- Add a public wrapper method on the autoload: `Gool.set_bus_gain_db(...)`
  that calls `_runtime.set_bus_gain_db(...)`
- Call `_runtime.set_bus_gain_db(...)` directly from internal
  autoload methods

Inconsistency between these two approaches creates "method exists
in spirit but not in code" gaps where calls like
`set_bus_gain_db(...)` (intended to land on self) silently resolve
to nothing — or fail at runtime with `not found in base`.

gool's current discipline: **direct-call internally**. The
autoload's internal methods always use the explicit receiver form
(`_runtime.X(...)`), never bare `X(...)`. Any bare `X(...)` call
in an autoload method must correspond to a declared `func X(...)`
in the same file.

If a public wrapper is needed (e.g. for clamping or validation),
declare it explicitly. The grep `^func <method_name>` should
match.

---

## Documentation workflow

### Scratchpad vs repo tree
*(Surfaced v0.26.0 → v0.26.1.)*

Doc work saved to ephemeral scratchpad paths (outside the repo
tree) does NOT get picked up by `scripts/make_source_archive.sh`,
which only packages the repo. The v0.26.0 release CHANGELOG
promised a `docs/audio_design/sidechain_tuning.md` file that
existed only in scratchpad; v0.26.1 was a docs-only patch to
fix the gap.

Rule: doc-style work goes directly into `docs/` in the gool repo
tree. The scratchpad is only for tarballs and external one-off
files that aren't meant to ship with gool (like a sandbox-specific
config.json for testing).

### CHANGELOG honesty

When a release goes wrong, the CHANGELOG entry should reflect what
actually shipped, not what was intended to ship. Don't retcon an
entry to claim a file is present when it isn't in the tag.

Format guidance: for follow-up patches that fix mistakes in a
prior release, the new entry should explicitly own the mistake.
The v0.26.1, v0.26.2, and v0.26.3 entries all do this — they
state what went wrong, why it wasn't caught, and what process
change came out of it. Future-you reading the CHANGELOG should be
able to reconstruct the failure mode without confusion.

---

## CI signals

### Headless smoke is a parse check, not a behavior check

`tests/godot/smoke/main.gd` calls `load()` on every `.gd` file
under `addons/gool` and reports `SMOKE OK` / `SMOKE FAIL` based on
whether each returns non-null plus a source-text scan of critical
scripts. It does NOT instantiate the scripts, run their `_ready`,
or exercise the GDExtension binding.

What headless smoke catches:

- Syntax errors
- Const-expression errors
- Class_name resolution errors that prevent parse
- Tab/space indentation errors
- Some `not found in base` errors (when the type analyzer can see
  the missing method statically)

What headless smoke does NOT catch:

- Runtime errors that depend on values not visible at parse time
- GDExtension binding behavior (that's the `build-gdextension`
  job)
- Multi-script interactions
- Editor-only paths (mixer dock UI, debugger plugin)

Defense in depth: headless smoke for parse, `build-gdextension`
job for binding compile, empirical sandbox testing (Brannen's
machine) for runtime behavior. The three tiers catch different
classes of failure; none of them is sufficient alone.

### Enumerate every error in the log
*(Surfaced v0.26.2 → v0.26.3.)*

When CI fails with multiple errors, ALL must be addressed. The
v0.26.0 CI log showed:

1. `Function "set_bus_gain_db()" not found in base self.`
2. `Assigned value for constant "SCALE_MARKS_DB" isn't a constant expression.`

In v0.26.2 the first error plus the inner-class outer-reference
issue were fixed, but the second error was missed even though it
was in the same log. Result: v0.26.2 also failed smoke, requiring
v0.26.3.

Rule: read the entire CI log. The `KNOWN_REAL_ERRORS` scan at the
bottom of the smoke job is the authoritative deduped list — work
through every entry, verify the fix addresses every one, not just
the most prominent.

When in doubt, run the local sweeps (`grep` equivalents of the
CI patterns) against the source before shipping. The pre-ship
checklist above includes these.

---

## Process-level lessons

### Bridges pay compound interest

When the cost of adding new capabilities to a system goes from
"build channel + capability" to just "add command to existing
channel + capability," that's the payoff of investing in good
infrastructure up front.

The v0.25.0 EngineDebugger bridge (game→editor for meter data)
was ~150 lines. v0.26.0 extended it to editor→game (~30 lines on
the editor side, ~70 lines of routing on the game side). 3.3b-2,
3.3c, and 3.3d will each be ~10 lines of new commands per
feature, not ~150 lines per feature. The original investment
amortizes.

The pattern generalizes: spend a release scaffolding the
communication layer / data flow / extension point. Subsequent
features ride on top with no further infrastructure cost.

### Empirical verification catches what smoke tests miss

The v0.25.2 SoundDefinition routing bug (drone audio routed to
Sfx bus instead of Music) passed every automated test in CI —
the unit tests, the C++ compile, the headless smoke, the
build-gdextension job. It was the mixer dock that surfaced it,
and only when Brannen looked at the meters during a sandbox F5.

Lesson: automated tests are necessary but not sufficient. The
final tier of defense is a human running the software with eyes
on the output. Build the tools that make this tier productive —
the mixer dock isn't just a feature, it's diagnostic
infrastructure that surfaces a class of bugs invisible to text-
only testing.

### Three iterations on one feature is a process smell

v0.26.0 → v0.26.1 → v0.26.2 → v0.26.3 was four releases for one
feature (Phase 3.3b-1 mixer dock interactive faders). Each
intermediate patch fixed something the previous patch missed.

When this happens, the failure isn't usually the code — it's the
process around shipping the code. In gool's case:

- v0.26.1 (missed doc) → process change: docs go directly into
  repo tree, not scratchpad
- v0.26.2 (parse error in inner class) → process change:
  class_name self-reference sweep before ship
- v0.26.3 (const-expression error) → process change: enumerate
  every CI error, automate the KNOWN_REAL_ERRORS source-text
  equivalent

The lessons that came out of those three patches became this
document plus permanent pre-ship steps. The hope: future
features ship in one or two releases, not four.

---

## What's NOT in this document

- Per-release functional context (read the CHANGELOG)
- Architecture overview (read `README.md` and `docs/THREADING.md`)
- Specific compiler/Godot version notes that are unlikely to recur
- Speculative future patterns we haven't actually hit yet

This document is a living record of patterns gool has been
burned by. When a class of bug bites twice, that's the signal
to add a section. When it bites three times, that's the signal
to automate detection.

*Last meaningful update: v0.26.4 — initial extraction of
accumulated lessons from CHANGELOG entries through v0.26.3.*
