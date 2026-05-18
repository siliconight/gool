#!/usr/bin/env python3
"""
Port of the targeted-byte patcher from config_model.gd to Python,
for round-trip testing against actual config.json files. The
goal is to verify that:

1. Patching a value (e.g. Music.gain_db = -3.0 → -3.5) produces a
   file that still parses as valid JSON.
2. Comments, whitespace, key order, and unrelated values are
   bit-for-bit preserved.
3. The patched value is correctly retrievable from the new file.

This test is run from CI / sweep scripts; if it ever drifts from
the GDScript port, we have a real regression risk in the dock's
persistence layer.
"""
import json
import sys
import re
from pathlib import Path


def find_matching_brace_close(text, open_pos):
    """Given position of '{', return position of matching '}'."""
    depth = 1
    i = open_pos + 1
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == '\\':
                i += 2; continue
            if ch == '"':
                in_string = False
            i += 1; continue
        if ch == '"':
            in_string = True
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def find_enclosing_brace_open(text, pos):
    """Find '{' opening the smallest object containing pos."""
    open_stack = []
    in_string = False
    k = 0
    while k <= pos and k < len(text):
        ch = text[k]
        if in_string:
            if ch == '\\':
                k += 2; continue
            if ch == '"':
                in_string = False
            k += 1; continue
        if ch == '"':
            in_string = True
        elif ch == '{':
            open_stack.append(k)
        elif ch == '}':
            if open_stack:
                open_stack.pop()
        k += 1
    return open_stack[-1] if open_stack else -1


def find_bus_block_range(text, bus_name):
    """Locate the {} range of the bus with the given name.
    
    Returns (start, end) where end is exclusive (one past close brace).
    Returns (-1, -1) on miss. Skips strings when looking for "name".
    """
    i = 0
    in_string = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if ch == '\\':
                i += 2; continue
            if ch == '"':
                in_string = False
            i += 1; continue
        if ch == '"':
            # Could be the start of a "name" key. Check the next 6 chars.
            if text[i:i+6] == '"name"':
                # Skip to the value: colon, whitespace, opening quote.
                j = i + 6
                while j < len(text) and text[j] in ' \t\n\r':
                    j += 1
                if j < len(text) and text[j] == ':':
                    j += 1
                    while j < len(text) and text[j] in ' \t\n\r':
                        j += 1
                    if j < len(text) and text[j] == '"':
                        # Parse the bus name value string (handle escapes).
                        k = j + 1
                        name_chars = []
                        while k < len(text):
                            c = text[k]
                            if c == '\\':
                                if k + 1 < len(text):
                                    name_chars.append(text[k+1])
                                k += 2
                                continue
                            if c == '"':
                                break
                            name_chars.append(c); k += 1
                        parsed_name = ''.join(name_chars)
                        if k < len(text) and parsed_name == bus_name:
                            brace_open = find_enclosing_brace_open(text, i)
                            if brace_open < 0:
                                return (-1, -1)
                            brace_close = find_matching_brace_close(text, brace_open)
                            if brace_close < 0:
                                return (-1, -1)
                            return (brace_open, brace_close + 1)
                        # Mismatch: skip past the closing quote of the
                        # bus name value so the outer scanner doesn't
                        # mis-enter the value string.
                        i = (k + 1) if k < len(text) else len(text)
                        continue
            in_string = True
        i += 1
    return (-1, -1)


def find_key_in_block(text, block_range, key):
    """Find a top-level key in a JSON object block.
    
    Returns the position of the key's opening quote, or -1.
    """
    start, end = block_range
    slice_text = text[start:end]
    needle = '"' + key + '"'
    i = 0
    in_string = False
    brace_depth = 0
    bracket_depth = 0
    while i < len(slice_text):
        ch = slice_text[i]
        if in_string:
            if ch == '\\':
                i += 2; continue
            if ch == '"':
                in_string = False
            i += 1; continue
        if ch == '"':
            if brace_depth == 1 and bracket_depth == 0 \
                    and slice_text[i:i+len(needle)] == needle:
                after = i + len(needle)
                j = after
                while j < len(slice_text) and slice_text[j] in ' \t\n\r':
                    j += 1
                if j < len(slice_text) and slice_text[j] == ':':
                    return start + i
            in_string = True
            i += 1
            continue
        if ch == '{':
            brace_depth += 1
        elif ch == '}':
            brace_depth -= 1
        elif ch == '[':
            bracket_depth += 1
        elif ch == ']':
            bracket_depth -= 1
        i += 1
    return -1


def find_value_range_for_key(text, block_range, key):
    """Locate the value byte range for a key in a block.
    
    Returns (value_start, value_end) where value_end is exclusive.
    For numeric values, range covers only the number. For string
    values, range includes the surrounding quotes.
    """
    key_pos = find_key_in_block(text, block_range, key)
    if key_pos < 0:
        return (-1, -1)
    i = key_pos + len(key) + 2  # skip "key"
    while i < len(text) and text[i] in ' \t\n\r':
        i += 1
    if i >= len(text) or text[i] != ':':
        return (-1, -1)
    i += 1
    while i < len(text) and text[i] in ' \t\n\r':
        i += 1
    if i >= len(text):
        return (-1, -1)
    if text[i] == '"':
        j = i + 1
        while j < len(text):
            c = text[j]
            if c == '\\':
                j += 2; continue
            if c == '"':
                return (i, j + 1)
            j += 1
        return (-1, -1)
    j = i
    while j < len(text):
        c = text[j]
        if c in ',}] \t\n\r':
            break
        j += 1
    return (i, j)


def format_number(value):
    """Match the GDScript _format_number behavior."""
    if value == int(value) and abs(value) < 1e15:
        return f"{value:0.1f}"
    return f"{value:g}"


def patch_number(text, block_range, key, value):
    """Replace an existing numeric value, or return None if not found."""
    vr = find_value_range_for_key(text, block_range, key)
    if vr[0] < 0:
        return None
    return text[:vr[0]] + format_number(value) + text[vr[1]:]


def patch_string(text, block_range, key, value):
    """Replace an existing string value (including its quotes)."""
    vr = find_value_range_for_key(text, block_range, key)
    if vr[0] < 0:
        return None
    return text[:vr[0]] + '"' + value + '"' + text[vr[1]:]


# -------- Tests --------

def assert_eq(actual, expected, msg):
    if actual != expected:
        print(f"  FAIL: {msg}")
        print(f"    expected: {expected!r}")
        print(f"    actual:   {actual!r}")
        return False
    print(f"  PASS: {msg}")
    return True


def test_round_trip(config_path):
    """Load a real config, patch a value, verify result is sane."""
    print(f"\n=== {config_path} ===")
    src = Path(config_path).read_text()
    parsed = json.loads(src)
    all_passed = True
    
    # Test 1: find Master block
    r = find_bus_block_range(src, "Master")
    ok = r[0] > 0 and r[1] > r[0]
    if not assert_eq(ok, True, f"find Master bus block ({r})"):
        all_passed = False
    
    # Test 2: find Music block (which has effects)
    r = find_bus_block_range(src, "Music")
    ok = r[0] > 0 and r[1] > r[0]
    if not assert_eq(ok, True, f"find Music bus block ({r})"):
        all_passed = False
        return False
    music_range = r
    
    # Test 3: find gain_db key in Music
    p = find_key_in_block(src, music_range, "gain_db")
    if not assert_eq(p > 0, True, f"find gain_db key in Music ({p})"):
        all_passed = False
    
    # Test 4: read gain_db value range
    vr = find_value_range_for_key(src, music_range, "gain_db")
    val_text = src[vr[0]:vr[1]]
    if not assert_eq(val_text, "-3.0", f"Music.gain_db value range = '-3.0'"):
        all_passed = False
    
    # Test 5: patch Music.gain_db -3.0 → -6.5
    patched = patch_number(src, music_range, "gain_db", -6.5)
    if patched is None:
        print("  FAIL: patch returned None"); return False
    new_parsed = json.loads(patched)
    
    # Find Music in new_parsed
    new_music = None
    for b in new_parsed["buses"]:
        if b["name"] == "Music":
            new_music = b; break
    if not assert_eq(new_music["gain_db"], -6.5, f"patched Music.gain_db = -6.5"):
        all_passed = False
    
    # Test 6: byte-perfect — everything OUTSIDE the value range unchanged
    before_value = src[:vr[0]]
    after_value = src[vr[1]:]
    new_vr_end = vr[0] + len(format_number(-6.5))
    if not assert_eq(patched[:vr[0]], before_value, "bytes before value unchanged"):
        all_passed = False
    if not assert_eq(patched[new_vr_end:], after_value, "bytes after value unchanged"):
        all_passed = False
    
    # Test 7: patch an effect param. Music has a compressor (effect 0)
    # with threshold_db: -20.0. Patch it to -22.5.
    # First find the effects array, then the first effect's block.
    effects_at = src.find('"effects"', music_range[0])
    if effects_at < 0 or effects_at >= music_range[1]:
        print("  SKIP: Music has no effects array")
        return all_passed
    bracket_open = src.find('[', effects_at)
    # Find first {
    i = bracket_open + 1
    in_str = False; depth = 0
    first_brace = -1
    while i < music_range[1]:
        c = src[i]
        if in_str:
            if c == '\\': i += 2; continue
            if c == '"': in_str = False
            i += 1; continue
        if c == '"': in_str = True
        elif c == '{':
            first_brace = i; break
        i += 1
    if first_brace < 0:
        print("  SKIP: no effect found"); return all_passed
    effect_close = find_matching_brace_close(src, first_brace)
    effect_range = (first_brace, effect_close + 1)
    
    vr = find_value_range_for_key(src, effect_range, "threshold_db")
    val_text = src[vr[0]:vr[1]]
    expected_thresh = parsed["buses"][[b["name"] for b in parsed["buses"]].index("Music")]["effects"][0]["threshold_db"]
    if not assert_eq(float(val_text), expected_thresh, f"Music compressor threshold_db = '{expected_thresh}'"):
        all_passed = False
    
    # Patch
    patched2 = patch_number(src, effect_range, "threshold_db", -22.5)
    if patched2 is None:
        print("  FAIL: patch threshold returned None"); return False
    new_parsed2 = json.loads(patched2)
    for b in new_parsed2["buses"]:
        if b["name"] == "Music":
            t = b["effects"][0]["threshold_db"]
            if not assert_eq(t, -22.5, f"patched threshold_db = -22.5"):
                all_passed = False
            break
    
    # Test 8: combine two patches in sequence (the actual save path)
    p1 = patch_number(src, music_range, "gain_db", -6.5)
    # Recompute music_range in patched output (it may have shifted by
    # a few bytes since "-3.0" → "-6.5" are same length, but in general
    # not always)
    music_range_p1 = find_bus_block_range(p1, "Music")
    # Now find first effect in p1
    effects_at = p1.find('"effects"', music_range_p1[0])
    bracket_open = p1.find('[', effects_at)
    i = bracket_open + 1
    in_str = False
    first_brace = -1
    while i < music_range_p1[1]:
        c = p1[i]
        if in_str:
            if c == '\\': i += 2; continue
            if c == '"': in_str = False
            i += 1; continue
        if c == '"': in_str = True
        elif c == '{':
            first_brace = i; break
        i += 1
    effect_close = find_matching_brace_close(p1, first_brace)
    effect_range_p1 = (first_brace, effect_close + 1)
    p2 = patch_number(p1, effect_range_p1, "threshold_db", -22.5)
    new_parsed3 = json.loads(p2)
    for b in new_parsed3["buses"]:
        if b["name"] == "Music":
            if not assert_eq(b["gain_db"], -6.5, "combined patches: gain_db = -6.5"):
                all_passed = False
            if not assert_eq(b["effects"][0]["threshold_db"], -22.5, "combined patches: threshold_db = -22.5"):
                all_passed = False
            break
    
    return all_passed


def main():
    configs = [
        "examples/coop_shooter_template/gool/config.json",
        "examples/multiplayer_audio_sandbox/gool/config.json",
    ]
    all_ok = True
    for c in configs:
        if not Path(c).exists():
            print(f"missing: {c}")
            continue
        if not test_round_trip(c):
            all_ok = False
    print()
    print("=" * 50)
    print("ALL TESTS PASSED" if all_ok else "TESTS FAILED")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
