# First-enable restart dialog — verification checklist

The v0.75.2 release added a one-time dialog that fires when a developer
enables the gool plugin for the first time in a project. It exists to
prevent a real, common, hard-to-diagnose failure: Godot's GDScript
parser caches identifier resolution before the plugin's autoloads
(Gool, DialogueDirector, MultiplayerBridge) are registered, then
emits a cascade of `Identifier 'Gool' not declared` errors that can
crash the editor on the next open. A single editor restart clears
the cache and everything parses cleanly.

This dialog is editor-time UI, not runtime behavior — it can't be
exercised by the stress test rig. The checklist below is the canonical
manual verification procedure. Run it whenever you change anything
in `plugin.gd::_maybe_show_first_enable_restart_prompt` or the
`_FIRST_ENABLE_KEY` plumbing.

## Setup

You need a project that has **never** had gool enabled in it before.
The flag is per-project and persistent, so a project that's been
through the dialog once won't trigger it again. Two reliable ways:

- **Recommended:** create a fresh empty Godot project just for this
  verification. Throw it away afterward.
- **Or:** in an existing project that previously had gool enabled,
  remove the `addons/gool/editor/first_enable_completed` setting from
  `project.godot` (search for the key, delete the line, save). Confirm
  it's gone before continuing.

## The five-step verification

### 1. Dialog appears on first enable

1. Place the gool addon at `addons/gool/` in your test project.
2. Open the project in Godot 4.4+.
3. Open **Project → Project Settings → Plugins**.
4. Toggle the gool plugin's status to **Enable**.
5. **Expected:** within a frame or two, a centered dialog appears
   titled "gool: first-time setup" with explanatory text about why
   a restart is needed, an **Restart Editor Now** button, and an
   **I'll Restart Manually** button.

❌ If the dialog does **not** appear: the project setting may already
exist (re-check `project.godot`), the plugin failed to load (check
Output panel for autoload errors), or `_maybe_show_first_enable_restart_prompt`
isn't being called from `_enter_tree`.

### 2. Restart Editor Now path works

1. From the dialog above, click **Restart Editor Now**.
2. **Expected:** Godot saves the project (visible briefly in the
   editor's status bar) and restarts the editor cleanly. The
   project reopens with the gool plugin enabled and no
   `Identifier 'Gool' not declared` errors in the Output panel.
3. **Expected:** `project.godot` now contains
   `addons/gool/editor/first_enable_completed=true`.

❌ If the editor doesn't restart: `EditorInterface.restart_editor(true)`
may have failed. Check the Output panel for warnings.

### 3. Dialog does not reappear on subsequent enables

1. With the project from step 2, disable the gool plugin from
   **Project Settings → Plugins**.
2. Re-enable it.
3. **Expected:** no dialog appears. The plugin enables silently.

❌ If the dialog reappears: the project setting wasn't persisted
(check `project.godot`), or the early-return check on
`_FIRST_ENABLE_KEY` is broken.

### 4. Dismiss path persists the flag

Restart this part from a fresh project (or remove the setting from
`project.godot` again).

1. Enable the gool plugin.
2. When the dialog appears, click **I'll Restart Manually** (or press
   Escape).
3. **Expected:** the dialog closes. No editor restart happens.
4. **Expected:** `project.godot` now contains
   `addons/gool/editor/first_enable_completed=true` even though the
   user dismissed.
5. Disable and re-enable the plugin to confirm the dialog doesn't
   reappear (same as step 3).

❌ If the dialog reappears after dismiss: the flag is being set
**after** the dialog rather than before. Confirm the
`ProjectSettings.save()` call runs before `dialog.popup_centered`.

### 5. Read-only project handling

This step is optional — you only need it if you're changing how the
plugin handles `ProjectSettings.save()` failures.

1. Make `project.godot` read-only at the OS level (`chmod 444` on
   Linux/macOS, **Properties → Read-only** on Windows).
2. From a fresh first-enable state, enable the plugin.
3. **Expected:** dialog appears as normal.
4. **Expected:** the Output panel shows a warning containing
   `could not persist first-enable flag (ProjectSettings.save
   returned <int>)`.
5. **Expected:** clicking either button proceeds normally (no crash);
   the dialog will reappear on the next plugin enable since the flag
   couldn't be saved. This is the documented degraded behavior.

Don't forget to restore write permissions to `project.godot`
afterward.

## When this matters

Run this checklist:

- Before any release that changes `plugin.gd` first-enable code.
- After Godot major-version bumps (4.4 → 4.5, etc.) that might affect
  `EditorInterface.restart_editor` or `ProjectSettings.save` behavior.
- If a user reports the `Identifier 'Gool' not declared` cascade —
  this dialog is what's supposed to prevent it.

## Why this can't be automated (right now)

Godot doesn't expose a clean way to programmatically toggle a plugin
on/off from within itself for testing. The `EditorInterface` API has
plugin enumeration but no enable/disable trigger that fires the
plugin's `_enter_tree` cycle the same way a user click does. Adding
an automated harness for this would need either:

- A separate test runner that boots Godot in headless mode with a
  scripted plugin toggle (complex, fragile across Godot versions)
- A GUT/GdUnit integration test using `EditorPlugin._enter_tree`
  invocation simulation (won't catch the real
  `EditorInterface.restart_editor` path)
- A CI fixture that runs Godot via subprocess with project state
  pre-seeded (possible but high-maintenance)

None of these are clearly worth the complexity for a once-per-project
dialog. The manual checklist is the right tool for the job.
