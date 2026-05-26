#!/usr/bin/env python3
"""
v0.79.8: addon-script autoload-safety scanner.

Scans every .gd file under godot/addons/gool/ (and the example
addons/gool/ copies under examples/) for the v0.78.7-class bug:

  Referencing the `Gool`, `DialogueDirector`, or `MultiplayerBridge`
  identifier at class-body scope inside an addon script. Addon
  scripts are parsed during the same compile pass that registers
  these autoloads, so a class-body reference fails to resolve on
  first project open (the cascade that v0.78.7 fixed by hand for
  the 3 observed cases).

Safe patterns this scanner does NOT flag:
  - References inside function bodies (autoload exists by then)
  - References inside comments or string literals (display text)
  - The autoload implementation files themselves (runtime_singleton.gd,
    dialogue_director.gd, multiplayer_bridge.gd, plugin.gd)
  - `Engine.has_singleton("Gool")` and `get_node_or_null("/root/Gool")`
    — these are the recommended safe lookups

Run from repo root:
    python3 scripts/check_addon_autoload_safety.py

Exits 0 if clean, 1 if any suspect references found.
"""

import os
import re
import sys
from pathlib import Path

# Identifiers that name autoloads. A bare reference to these at class-body
# scope is the bug. (Re-add identifiers here if more autoloads ship.)
AUTOLOAD_IDENTIFIERS = ("Gool", "DialogueDirector", "MultiplayerBridge")

# Addon script implementations of the autoloads themselves — these
# legitimately use their own class symbols.
AUTOLOAD_IMPLEMENTATIONS = {
    "runtime_singleton.gd",
    "dialogue_director.gd",
    "multiplayer_bridge.gd",
    "plugin.gd",  # plugin.gd registers them via add_autoload_singleton
}

# Directories to scan. Both the canonical addon dir and the example
# copies (which mirror the canonical and must stay in sync).
SCAN_ROOTS = [
    "godot/addons/gool",
    "examples/coop_shooter_template/addons/gool",
    "examples/voice_chat/addons/gool",
]

# A reference is "safe" if it appears within these contexts:
SAFE_CONTEXTS = re.compile(
    r"(?:"
    # Inside a string literal
    r'"[^"]*'
    r"|'[^']*"
    # Inside `get_node_or_null("/root/Gool")` lookup
    r"|get_node_or_null\([^)]*"
    # Inside `Engine.has_singleton(\"Gool\")` or `Engine.get_singleton(\"Gool\")`
    r"|Engine\.(?:has|get)_singleton\([^)]*"
    r")"
)


def strip_multiline_strings(source: str) -> str:
    # Remove the contents of every triple-quoted block (double or single
    # quotes), preserving newlines so line numbers stay aligned with the
    # original. Docstring uses # comments to dodge the meta-issue of
    # describing triple-quote handling inside a triple-quoted docstring.
    out = []
    i = 0
    while i < len(source):
        # Look for the start of a triple-quoted block.
        if source[i : i + 3] in ('"""', "'''"):
            triple = source[i : i + 3]
            # Keep the opening triple so the line still parses, then skip to close.
            out.append(triple)
            i += 3
            close_idx = source.find(triple, i)
            if close_idx == -1:
                # Unterminated — keep newlines from the rest of the file
                out.append("\n" * source.count("\n", i))
                i = len(source)
            else:
                # Preserve newlines inside the stripped block so line numbers stay right.
                out.append("\n" * source.count("\n", i, close_idx))
                out.append(triple)
                i = close_idx + 3
        else:
            out.append(source[i])
            i += 1
    return "".join(out)


def strip_comments_and_strings(line: str) -> str:
    """Approximate strip — good enough for the detection heuristic.
    Removes # comments and "..."/'...' string literal contents."""
    out = []
    i = 0
    in_str = None
    while i < len(line):
        c = line[i]
        if in_str:
            if c == in_str and (i == 0 or line[i - 1] != "\\"):
                in_str = None
                out.append('"')  # placeholder so the string boundary stays
            i += 1
            continue
        if c == "#":
            break  # rest of line is comment
        if c in ('"', "'"):
            in_str = c
            out.append('"')
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return a list of (line_no, line_text, reason) for each suspect reference."""
    if path.name in AUTOLOAD_IMPLEMENTATIONS:
        return []

    suspects: list[tuple[int, str, str]] = []
    indent_stack: list[int] = []  # track func body indentation
    in_func_body_indent: int | None = None

    with open(path, encoding="utf-8") as f:
        source = f.read()

    # Strip triple-quoted blocks first so line-by-line scanning can't trip on
    # multi-line BBCode docstring content (e.g. help_panel.gd's content constants).
    source = strip_multiline_strings(source)
    lines = source.splitlines(keepends=True)

    for i, raw_line in enumerate(lines, 1):
        # Compute current indentation (tabs or spaces, doesn't matter — we
        # just compare relatively to the most recent `func` declaration).
        stripped_for_indent = raw_line.rstrip("\n")
        line_indent = len(stripped_for_indent) - len(stripped_for_indent.lstrip("\t "))

        # If we were inside a func body and this line is dedented to or
        # below the func declaration's level, we've left the func body.
        if in_func_body_indent is not None and stripped_for_indent.strip() != "":
            if line_indent <= in_func_body_indent:
                in_func_body_indent = None

        # Strip comments and string literals before scanning for identifiers.
        code_only = strip_comments_and_strings(raw_line)

        # Is this a func declaration? If yes, remember its indent so we
        # can detect when we leave its body.
        if re.match(r"^\s*(static\s+)?func\s+\w+", code_only):
            in_func_body_indent = line_indent
            continue  # the func signature line itself isn't class-body code

        # Search for any AUTOLOAD identifier reference that isn't already
        # protected by a safe context (string, get_node_or_null, has_singleton).
        for ident in AUTOLOAD_IDENTIFIERS:
            # Match the identifier as a word boundary + a method/property access
            # or comparison or assignment — i.e. it's being USED, not just named.
            pat = rf"\b{ident}\b\s*(?:\.|\(|\[|==|!=|=[^=]|is\b|as\b)"
            for m in re.finditer(pat, code_only):
                # Skip if the match is inside a safe-context bracket pair.
                # The strip already removed pure string content, but
                # `Engine.has_singleton("Gool")` puts the identifier inside
                # a function call. We handle that by checking the surrounding
                # ~30 chars for the safe-context markers.
                window_start = max(0, m.start() - 40)
                window = code_only[window_start : m.end()]
                if re.search(r"(?:get_node_or_null|Engine\.(?:has|get)_singleton)\s*\(", window):
                    continue

                # If we're inside a func body, this is safe.
                if in_func_body_indent is not None:
                    continue

                # Otherwise: class-body reference. Flag it.
                suspects.append((
                    i,
                    raw_line.rstrip("\n"),
                    f"class-body reference to '{ident}' (autoload identifier; "
                    f"use get_node_or_null('/root/{ident}') or "
                    f"Engine.has_singleton('{ident}') guard instead)",
                ))
                break  # one finding per line is enough

    return suspects


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    os.chdir(repo_root)

    total_suspects = 0
    files_scanned = 0

    for root in SCAN_ROOTS:
        root_path = Path(root)
        if not root_path.exists():
            continue
        for gd_path in sorted(root_path.rglob("*.gd")):
            files_scanned += 1
            findings = scan_file(gd_path)
            if findings:
                print(f"\n[FAIL] {gd_path}")
                for line_no, line_text, reason in findings:
                    print(f"  L{line_no}: {line_text}")
                    print(f"         → {reason}")
                total_suspects += len(findings)

    print()
    print(f"  Scanned {files_scanned} .gd files across {len(SCAN_ROOTS)} addon roots.")
    if total_suspects == 0:
        print("  OK: no class-body autoload references found in addon scripts.")
        return 0
    else:
        print(f"  FAIL: {total_suspects} suspect references — fix before committing.")
        print()
        print("  Why this matters: addon scripts are parsed during the same")
        print("  compile pass that registers the autoload. A bare reference")
        print("  to Gool/DialogueDirector/MultiplayerBridge at class-body")
        print("  scope fails to resolve on first project open, cascading")
        print("  into a parse-error storm that hides the real install state")
        print("  from new users.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
