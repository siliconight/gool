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
- [ ] `version_test` passes after version bump. **Clean rebuild
      required.** The version constants live in
      `include/audio_engine/version.h` and are baked into
      `version.o` at compile time; running `version_test` against
      a stale `.o` file produces a confusing assertion failure
      ("v.minor == 27 failed" even though the header says 27).
      Always `rm -f /tmp/lib_*.o` (or equivalent) between version
      bumps and the test build. Surfaced v0.27.0.

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

### Reading state: in-memory model > debounced disk
*(Surfaced v0.28.10, shipped v0.29.1. Comment said one thing, code did another, debounced save hid the discrepancy.)*

When a UI is driven by signals from a model that owns in-memory state
and ALSO persists to disk on a debounced schedule, every signal handler
in the UI must read from the in-memory model, never from disk.

The failure mode is sneaky because the wrong read works *eventually*:

  1. Model state updates → emits signal
  2. Handler reads disk — disk hasn't flushed yet → stale read
  3. UI rebuilds with stale data → visual change is missing
  4. Some unrelated event triggers a second rebuild (in our case,
     the game starting and the runtime poll providing fresh stats)
  5. NOW the change appears, with no clear correlation to the action

For gool, the dock's `_load_static_layout_from_config()` was named
"from_config" but the `_config_model` had become the authoritative
source as of v0.28.4. The disk was just a serialization target. The
fix was a one-function rewrite to call `_config_model.get_buses()`.

**Meta-lesson**: when two parallel paths feed the same UI (here:
`_rebuild_strips_from_runtime` for game-running, `_rebuild_strips_from_config`
for at-rest), identify the authoritative state source in each mode and
read from THAT:

- **Runtime mode**: engine stats arriving via debugger plugin are
  authoritative. Reading them is correct.
- **At-rest mode**: the in-memory config model is authoritative.
  Reading it (not the disk file) is correct.
- **Disk** is only authoritative on a fresh editor open *before* the
  model is constructed. After that, it's a serialization target, not
  a source.

When you find yourself reading a file that some other code path is
writing on a debounced schedule, that's the smell. Read from the
in-memory state instead.

### Signature changes: grep the callee, not the operands
*(Surfaced v0.29.0, fixed in v0.29.2 (after a rollback in v0.29.1). Test file's direct ctor calls slipped through a field-name grep.)*

When changing the signature of a function or constructor, search the
tree for the **symbol name** (e.g. `ReverbEffect(`, `loadFromDisk(`),
not just for the names of fields or arguments that pass through it.
The two sets aren't the same — callers exist that reference the
symbol but not its operands by name. The classic miss is a test file
that constructs an object directly with positional literals:

```cpp
ReverbEffect rv(0.85f, 0.4f, 0.0f);   // no field names, won't show up
                                      // in a grep for "reverbRoomSize"
```

In v0.29.0 the `ReverbEffect` constructor went from 3 args to 6 args.
I updated every site that referenced `reverbRoomSize`/`reverbDamping`
(the EffectConfig fields the ctor reads from). But three direct ctor
calls in `tests/unit/reverb_send_test.cpp` passed positional floats with
no field-name comments — invisible to the field-name grep. Result:
the engine compiled fine on all three CI platforms but the test source
file took the entire build matrix down with `error: no matching
function for call to ReverbEffect::ReverbEffect(float, float, float)`.

**Standard practice when changing a signature**: do BOTH searches:

```bash
# 1. Field-name search (the "operand" side)
grep -rn "reverbRoomSize\|reverbDamping" --include="*.cpp" --include="*.h" .

# 2. Symbol-name search (the "callee" side) — easy to forget
grep -rn "ReverbEffect *(" --include="*.cpp" --include="*.h" .
```

The symbol-name search catches direct construction, function pointers,
template instantiations, and any other place where the callee shows up
without its argument names visible.

**Meta-lesson**: a thorough audit of "all call sites of API X" requires
searching for X itself, not just for the data that flows through X. The
field-name approach feels equivalent because in normal usage you'd touch
both — but tests, mocks, and ad-hoc construction sites routinely break
that equivalence. The fix is mechanical (always run both greps); the
discipline is remembering to.

### Behavior changes: test assertions are part of the surface area
*(Surfaced v0.29.0, missed in v0.29.2, fixed v0.29.3. Sibling to the
"grep the callee" lesson — that one catches API changes; this one
catches behavior changes.)*

When changing a function's *implementation* (not just its signature),
the test assertions referencing that function may encode behavioral
expectations specific to the **old** implementation, even if the
function name and signature haven't changed. The classic miss is
updating a constructor call site (mechanical, find-and-replace) without
re-reading the surrounding assertions that test what the function does.

Concrete case: the `reverb_send_test` impulse response test contained
this block from the Freeverb era:

```cpp
const float earlyRms = Rms(buf, 0, kSampleRate / 20);          // 0-50 ms
const float lateRms  = Rms(buf, kSampleRate*4/10, kSampleRate*6/10); // 400-600 ms
EXPECT(lateRms < earlyRms);    // monotonic-ish decay
```

The "grep the callee" lesson (v0.29.0 → v0.29.2) caught and fixed the
`ReverbEffect` constructor calls inside this test. But the *assertion*
`lateRms < earlyRms` survived intact, even though it encoded Freeverb's
specific topology — Schroeder comb filters, energy decays exponentially
from t=0. Under Dattorro's plate topology, the cross-coupled tank has a
100-300 ms buildup phase where the late field is *louder* than the
early field. The assertion failed in CI on all three platforms with
the same message — late RMS was ~58× the early RMS, on every job, with
identical numeric output. The reverb itself was working correctly; only
the test's expectations were stale.

**Standard practice when changing a function's behavior:**

1. Run the "grep the callee" search to find all call sites.
2. For each call site, re-read the surrounding logic — comments,
   variable names, comparisons — and ask whether any of it encoded
   behavioral assumptions specific to the old implementation.
3. Pay special attention to test files: tests are *designed* to encode
   behavioral expectations, so they're the most likely place to find
   stale assumptions after a behavior change.

**Meta-lesson**: when migrating tests across a topology change, the
constructor call is the visible change but the assertions are the
load-bearing change. Apply the same care to the assertion logic as to
the constructor signature; updating one without the other is the
default failure mode and produces a green build with a red test
suite — exactly what happened on the v0.29.2 push.


*(Surfaced v0.28.9 → v0.28.10. Copy-pasted a working idiom from `PopupMenu` to `AcceptDialog`; it compiled, then errored at runtime.)*

Godot 4's dialog and popup classes look like they should share a base
class, but they don't:

```
Window
├── Popup
│   └── PopupMenu          ← has popup_hide
└── AcceptDialog            ← does NOT have popup_hide
    └── ConfirmationDialog  ← also no popup_hide; has confirmed + canceled
```

`AcceptDialog` and `ConfirmationDialog` extend `Window` *directly*, not
through `Popup`. So:

| Signal | PopupMenu | AcceptDialog | ConfirmationDialog |
|---|---|---|---|
| `popup_hide` | ✓ | ✗ | ✗ |
| `close_requested` | ✓ (via Window) | ✓ (via Window) | ✓ (via Window) |
| `confirmed` | – | ✓ | ✓ |
| `canceled` | – | – | ✓ |

The runtime error when you get it wrong:

```
Invalid access to property or key 'popup_hide'
on a base object of type 'AcceptDialog'.
```

…which is unusually clear. The GDScript parser doesn't catch this at
parse time because signal access on `Object` is dynamic; the failure
surfaces only when the offending `.connect(...)` line actually executes.

**Cleanup idiom that works across both families:**

```gdscript
# Connect both signals; queue_free is idempotent so double-firing is fine.
dlg.confirmed.connect(dlg.queue_free)         # OK button (AcceptDialog and below)
dlg.close_requested.connect(dlg.queue_free)   # X button (all Window descendants)
# Plus, for ConfirmationDialog only:
dlg.canceled.connect(dlg.queue_free)          # Cancel button
```

For PopupMenu, `popup_hide` covers the case where the menu auto-hides after
an item is selected (which is the dominant dismissal path); `close_requested`
catches the rare "click outside the menu" case.

**Meta-lesson**: when porting an idiom from one class to a sibling, check
the inheritance chain on the target class explicitly. The parent classes
look the same on the page but the actual `extends ...` line is what
determines available signals. Same shape as the `Callable.bind` lesson
below: rely on docs, not on muscle-memory from a related class.


*(Surfaced v0.28.8 → v0.28.9. Got the rule right initially, "fixed" it wrong during an audit, user found the bug at runtime.)*

In Godot 4, when you `.connect()` a signal with `Callable.bind(...)`:

```gdscript
menu.id_pressed.connect(handler.bind(bus_name))   # id_pressed(id: int)
```

…the slot is called as `handler(id, bus_name)` — **signal's own
args first, bound args appended after**.

This is opposite to Python's `functools.partial(f, x)`, which
*prepends*. The Godot docs are clear about this (Callable.bind:
"the bound arguments are passed after the arguments supplied by
the caller"), but the muscle memory from Python or from Godot 3
patterns can mislead.

The runtime error when you get it wrong:

```
Error calling from signal 'id_pressed' to callable
'…::_on_strip_context_menu_id_pressed':
Cannot convert argument 1 from int to String.
```

…meaning the int signal arg got fed to your first String param,
which means the signal arg arrived first and you have the
declaration backwards.

**The painful version of this lesson**: the v0.28.8 release shipped
with the handler *originally* declared correctly. During a
pre-tarball "audit" I convinced myself the rule was the other way
and "fixed" the signature, regressing it. The bug surfaced as soon
as the user tried right-clicking a strip. Two rules-of-thumb fell
out of this:

1. **An audit that changes correct code is worse than no audit.**
   When asserting code is wrong without testing it, the burden of
   proof goes way up. Particularly for binding conventions that
   differ between languages — look up the docs cold, don't reason
   from memory.

2. **Mixed-args bind sites are the rare case.** The vast majority
   of `.connect(...bind(x, y))` sites in this codebase are paired
   with signals that have *no args of their own* (`pressed`,
   `confirmed`, `canceled`, `popup_hide`). For those, slot
   ordering is unambiguous — bound args fill the only slots there
   are. Only mixed-args bind sites can fail this way; audit them
   carefully but leave the trivial cases alone.



GDScript's `%` operator (e.g. `"%0.1f" % value`) implements its own
format specifiers — NOT printf-compatible. The supported set is:

| Specifier | Use |
|---|---|
| `%s` | string |
| `%d` | int (also accepts float, truncates) |
| `%f` | float (precision OK: `%0.1f`, `%5.3f`) |
| `%c` | character |
| `%o` | octal |
| `%x` / `%X` | hex (lower/upper) |
| `%v` | vector |

Notable omissions: **`%g`, `%e`, `%a`, `%i`, `%u`** — all standard in
printf and Python, all unsupported in GDScript. Hitting one produces:

```
ERROR: String formatting error: unsupported format character.
```

The expression returns garbage and execution continues. Where the
result flows into structured output (JSON, network protocol, file
format), this silently corrupts data.

The v0.28.4 config patcher used `"%g" % value` for non-integer
floats. Python's `%` operator supports `%g` (round-trip tests
passed); GDScript's doesn't (every save with a non-integer fader
value silently failed). The patcher's parse-verify-and-restore-from-
backup mechanism hid this from the user across v0.28.4–v0.28.6;
the diagnostic dump added in v0.28.6 finally surfaced the actual
corrupted JSON, and v0.28.7 fixed the format call.

**Replacements:**
- `%g` → `String.num(value)` (Godot's default float-to-string,
  trims trailing zeros)
- `%e` → also unsupported; use `String.num()` and add an `e`
  suffix manually if you need scientific notation
- `%i` → use `%d`
- `%u` → use `%d` (no unsigned format in GDScript; just be careful
  about negative values)

When formatting numbers for JSON, network protocols, or any
structured output, prefer `String.num()` over `%`-based formatting.

### Inner class scope*(Surfaced v0.26.0 – v0.26.2.)*

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

### CMake PRIVATE includes don't transit to consumers
*(Surfaced v0.26.6.)*

The `audio_engine` target declares `src/` as a PRIVATE include
directory and `include/` as PUBLIC:

```cmake
target_include_directories(audio_engine
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

Consumers that link `target_link_libraries(foo PRIVATE audio_engine)`
inherit the PUBLIC interface (including its public includes) but
NOT its PRIVATE includes. So `foo` can `#include
"audio_engine/audio_file_format.h"` (in `include/`) but NOT
`#include "audio_engine/decoders/audio_decoder.h"` (in `src/`).

This is the correct default for a library — private headers
shouldn't leak to all consumers. But for test/fuzz targets that
need to reach internal API surface, you need to explicitly add
`src/` to the consumer's include path:

```cmake
target_include_directories(${test_target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

The fuzz `audio_engine_add_fuzz_harness` function in `CMakeLists.txt`
does this. New fuzz harnesses or unit tests that need to test
private APIs (DecoderFactory, internal mixer functions, etc.)
should follow the same pattern.

Failure mode: the build error is a fatal-error missing header,
which is easy to recognize but easy to misdiagnose as "the header
got renamed/moved" when actually the header is fine and the build
config is wrong.

### godot-cpp's Variant doesn't construct from `int` or `const char*`
*(Surfaced v0.28.0 → believed-fixed in v0.28.1 → actually
documented in v0.28.2 alongside the real bugs.)*

**Update from v0.28.2**: this lesson's *content* is still correct
(`Variant` really doesn't have `int` or `const char*` constructors;
the existing `get_bus_stats` pattern of explicit `int64_t` and
`String(...)` is the right convention), but the *framing* in
v0.28.1 was wrong. v0.28.1's CHANGELOG claimed this was the bug
breaking CI. It wasn't — MSVC was hitting earlier compile errors
on lines 96 and 1119 and stopping there, never reaching the
Variant assignment code. The Variant fixes were defensively
correct but didn't address what CI was actually failing on. See
the next two lessons below for what was actually broken.

The Variant-API truth, restated:

In Godot 4 / godot-cpp, `Variant` has constructors for `int64_t`,
`double`, `bool`, `String`, etc., but **not** for plain `int` and
**not** for `const char*`. Patterns:

```cpp
// ✗ These may fail to compile, or compile but pick a surprising overload:
d["kind"]      = static_cast<int>(my_enum_value);
d["kind_name"] = my_function_returning_const_char_ptr();

// ✓ These match the existing get_bus_stats convention:
d["kind"]      = static_cast<int64_t>(my_enum_value);
d["kind_name"] = String(my_function_returning_const_char_ptr());
// or, if the source is UTF-8:
d["kind_name"] = String::utf8(my_function_returning_const_char_ptr());
```

Applies to `Dictionary[key] = value` assignments, `Array.push_back(value)`,
and the keys themselves: `d[static_cast<int64_t>(x)]`, not
`d[static_cast<int>(x)]`.

### `using X = Y;` aliases TYPES; `namespace X = Y;` aliases NAMESPACES
*(Surfaced v0.28.0 → actually fixed in v0.28.2.)*

C++ has two distinct alias syntaxes with very different meanings:

- `using X = Y;` (alias-declaration, C++11) is for **types**.
  `Y` must name a type.
- `namespace X = Y;` (namespace-alias-definition, C++98) is for
  **namespaces**. `Y` must name a namespace.

They are not interchangeable. The compiler error if you use the
wrong one varies:

- **MSVC** correctly rejects `using X = some_namespace;` with
  `error C2061: syntax error: identifier '<name>'`. This cascades
  into "X is not a class or namespace name" for every subsequent
  use of `X::member`, which is why a single bad alias produced
  ~30 errors in the v0.28.0 CI log.
- **GCC** has historically accepted `using X = some_namespace;`
  as an extension and silently treats it as if you'd written
  `namespace X = some_namespace;`. This is why the bug compiled
  cleanly with my local g++ engine-only test (when I eventually
  set up a full binding test) but failed on Windows.

In gool, `audio::EffectParameter` is defined in `bus.h` as:

```cpp
namespace audio::EffectParameter {
    constexpr uint16_t Gain_GainDb = 1;
    constexpr uint16_t Compressor_ThresholdDb = 4;
    // ...
}
```

— a namespace holding constexpr uint16_t constants, NOT a type.
To alias it for local convenience inside a function:

```cpp
// ✓ Correct:
namespace EP = audio::EffectParameter;
auto x = EP::Gain_GainDb;

// ✗ Wrong — GCC accepts as extension, MSVC rejects:
using EP = audio::EffectParameter;
auto x = EP::Gain_GainDb;  // "EP is not a class or namespace"
```

Namespace aliases ARE valid at function/block scope, namespace
scope, and translation-unit scope. Use them when a long
namespace path appears 5+ times in a small region.

### `std::unique_ptr` has no implicit conversion to its raw pointer
*(Surfaced v0.28.0 → actually fixed in v0.28.2.)*

`std::unique_ptr<T>` provides:
- `operator->` — for member access (`runtime_->Method()`)
- `operator*` — for dereferencing
- `.get()` — explicit access to the underlying `T*`
- `.reset(...)` — replace the managed pointer
- contextual conversion to `bool`

It does **not** provide an implicit conversion to `T*`. This is
deliberate — the unique_ptr's value proposition is that ownership
is explicit, and silently producing raw pointers would undermine
that. So this fails:

```cpp
std::unique_ptr<Thing> p = std::make_unique<Thing>();
void f(Thing*);
f(p);          // ✗ no conversion from unique_ptr<Thing> to Thing*
f(p.get());    // ✓ explicit .get() is required
```

MSVC's error: `C2664: cannot convert argument 1 from
'std::unique_ptr<T,...>' to 'T *'`.

In gool's binding, `runtime_` is a `unique_ptr`. Pre-existing
code uses `runtime_->X()` everywhere, which works via
`operator->`. v0.28.0 added one call site that passed `runtime_`
as a positional argument to a helper expecting `audio::AudioRuntime*`
— and forgot the `.get()`. The other places that access the
underlying pointer directly (e.g. `internal_runtime()` at line
1640) do use `.get()`, so the pattern was already there to copy.

### The sandbox engine compile is not a binding compile
*(Surfaced v0.28.0 → re-confirmed in painful detail through v0.28.1 → finally believed in v0.28.2.)*

The local sandbox runs roughly:

```bash
find src -name "*.cpp" | xargs g++ -std=c++20 -Wall -Wextra -Wpedantic -c ...
```

That glob hits `src/`, not `godot/src/`. The Godot binding
(`godot/src/gool_godot.cpp`) is **never compiled by the sandbox**.
Until v0.28.1, "engine compiles clean locally" was being treated
as evidence that a release was safe to ship, even when the
release was specifically about binding-side changes
(`set_effect_parameter`, `get_bus_effects` — entirely binding
features).

Consequences observed in v0.28.0 → v0.28.1 → v0.28.2:

- v0.28.0 shipped with two distinct binding compile errors
  (namespace alias + unique_ptr deref). Engine compiled clean.
  CI failed across all three platforms at the same step.
- v0.28.1 was a guessing-game "fix" based on reading the CI
  summary (which only showed "Build GDExtension failed" without
  the actual error text, since the full log requires GitHub auth).
  The "fix" addressed Variant API conventions that MSVC was
  never reaching. CI failed at the exact same lines as v0.28.0.
- v0.28.2 was the first version where I read the actual CI
  errors (user-pasted log) and addressed the actual errors. The
  v0.28.1 Variant changes were kept because they're conventionally
  correct, but were never themselves the blocker.

The fix is one of three things, in increasing order of investment:

1. **Compile-isolation test.** For any binding helper that's pure
   C++ logic (like `_gool_fill_params_for_kind`), write a small
   `.cpp` with stubbed `godot::Dictionary`, `godot::Variant`, etc.
   that lets g++ exercise the syntax. Won't catch real Variant
   overload resolution issues but DOES catch namespace aliases,
   unique_ptr derefs, basic type plumbing. Pattern lives in
   `/tmp/binding_helper_check.cpp` during v0.28.2 development;
   not shipped, but the pattern is reproducible: ~80 lines of
   stubs + the helper copied verbatim + a trivial `main()`.
2. **Local godot-cpp checkout.** Clone https://github.com/godotengine/godot-cpp
   at the version pinned by `GODOT_CPP_REF` in the nightly workflow
   (currently `4.4`), build it once, then run the full GDExtension
   build locally: `cmake -S godot -B build-godot -DGODOT_CPP_PATH=...`
   followed by `cmake --build build-godot --config Release`. This
   IS the CI build, just locally. Catches everything CI would
   catch. Slow (full godot-cpp is ~5min from cold), but with
   ccache + a built godot-cpp it's a few seconds per edit.
3. **Add a CI smoke job that fails fast on just the binding.**
   The current workflow compiles the full addon archive after
   compile. A pre-compile syntax-only job (`-fsyntax-only` for
   gcc/clang or `/Zs` for MSVC) on just `gool_godot.cpp` would
   give faster feedback for binding errors. Lower priority — the
   real CI cycle is acceptable for a one-developer project.

**Don't ship binding changes claiming "tested locally" if you
only ran the engine compile.** That's the meta-lesson v0.28.1
should have learned but didn't.

### Three iterations on one feature is a process smell *(extended for v0.28.0–0.28.2)*

The v0.28.x trilogy is the **second** time three iterations have
been needed in the past month. The first was the solo-button
work (v0.27.0 → v0.27.1) which only took two. The fader-edit
work earlier also slipped. The common thread is the same: a
change that *seems* well-contained gets shipped, CI fails on
something the local checks didn't cover, and the response is to
guess + reship rather than reproduce the actual failure first.

The fix is in the previous lesson: read the actual error, don't
guess. The CI log requires GitHub auth to fully read; ask the
user to paste it before reshipping if you can't access it
yourself. Reshipping based on the summary line ("Build
GDExtension failed") is just guessing dressed up as a fix.

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

### Python-port-as-test catches algorithm bugs, not language-idiom bugs
*(Surfaced v0.28.4 – v0.28.7.)*

For GDScript code with non-trivial logic — the v0.28.4 config
patcher being the example — porting the algorithm to Python and
running Python unit tests is a fast feedback loop that doesn't
require a running Godot instance. The patcher's round-trip tests
against real config files caught several real algorithm bugs
during v0.28.4 development.

What it does NOT catch: places where the Python idiom and the
GDScript idiom diverge despite looking identical in source.
Examples that have actually bitten or could:

- `%g` works in Python's `%` operator, not in GDScript's
  (the v0.28.4–0.28.7 bug; see the `%` format-specifiers entry above)
- `"%i" % x` / `"%u" % x` similarly unsupported in GDScript
- Integer division: `5 / 2 == 2.5` in Python 3; in GDScript 4 it
  depends on static typing (typed int operands → 2; otherwise 2.5).
  A direct port that worked in Python may behave differently in
  GDScript if the value types differ
- String indexing returns a `str` of length 1 in Python; in
  GDScript 4 it returns a `String` of length 1 — usually fine,
  but list comprehension idioms don't translate
- `null` (GDScript) vs `None` (Python) — obvious but the
  comparison semantics differ: in GDScript `0 == null` is
  `false`; in Python `0 == None` is also `false` but the
  reasoning is different and string formatting around them isn't
  the same

The takeaway is NOT to abandon Python-port testing — the
algorithm verification is genuinely valuable, and the round-trip
test against real configs has caught real bugs. The takeaway is
to know what it doesn't cover:

1. **Don't borrow printf-isms from Python.** When the GDScript
   needs to format a value, look up the specifier in Godot's
   actual `%` operator docs, not C/Python printf.
2. **Strongly typed GDScript helps.** `var x: float = ...` and
   typed function signatures surface type-confusion early.
3. **Defensive output validation pays off.** The patcher
   parse-verifies its own output before committing the write
   (and restores from `.gool-backup` if invalid). This kept the
   `%g` silent corruption from reaching user data across three
   releases — without that step, every save would have written
   invalid JSON and users would have lost work. The v0.28.6
   addition (dump the failed text to `config.json.failed` for
   inspection) is the *diagnostic* counterpart: when the safety
   net fires, leave a forensic artifact behind.

Point 3 generalizes beyond GDScript: any layer that produces
structured output should validate its own output before committing,
and dump artifacts on validation failure. For the patcher: re-parse
the JSON. For a network protocol layer: re-decode the encoded
frame. For a binary file writer: read it back and compare hash.
Cheap, catches a lot, and turns a silent corruption into a loud
recoverable error.

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

*Last meaningful update: v0.28.7 — added the GDScript `%` format
specifier entry and the Python-port-as-test entry following the
v0.28.4-0.28.7 patcher %g saga. Previous extraction was v0.26.4.*
