// tests/unit/json_escape_test.cpp
//
// v0.80.0 regression test for the bug class fixed in this release.
//
// Pre-v0.80.0, the bus-config and sound-bank loaders shipped with
// hand-rolled JSON string parsers that handled only a subset of the
// nine spec-mandated escape sequences. The bus-config loader was
// missing \u, \b, and \f. The sound-bank loader was missing \u
// (it had \b and \f). The shipping FPS template (config_fps.json)
// embedded \u2014 and \u2192 in its _comment field, which made
// first-run installs fail for users who picked the FPS preset.
//
// This test exercises every JSON spec-mandated string escape through
// the bus-config loader's public API. If a future change to either
// loader regresses escape handling, this test catches it.
//
// The CI guard for shipped artifacts (shipped_artifacts_test.cpp)
// is a separate, complementary test: it ensures we never ship a
// JSON file the loader can't read. This test ensures the loader
// supports the full JSON spec regardless of what we ship.

#include "audio_engine/bus_config_loader.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

// Build a minimal bus-config JSON that exercises an escape sequence
// inside a bus name. The loader requires a bus literally named
// "Master" (line 779: `r.error = "no bus named 'Master'"`), so the
// graph has two buses: Master (the required root) and a child bus
// whose name carries the escape sequence under test. The child's
// debugName field is a fixed-size char[16] (silent strncpy
// truncation), so prefixes/suffixes are kept short — "X" before
// and "Y" after — leaving room for even the multi-byte UTF-8
// surrogate-pair decoded forms.
std::string buildConfigWithEscape(const std::string& nameWithEscape) {
    return std::string("{\n")
         + "  \"sample_rate\": 48000,\n"
         + "  \"buffer_size\": 512,\n"
         + "  \"buses\": [\n"
         + "    { \"name\": \"Master\", \"gain_db\": 0.0 },\n"
         + "    { \"name\": \"" + nameWithEscape + "\", \"parent\": \"Master\", \"gain_db\": 0.0 }\n"
         + "  ]\n"
         + "}\n";
}

// Verify a single escape sequence parses successfully and the
// resulting bus name contains the expected decoded byte(s).
// Returns true on pass.
bool testEscape(const char* what,
                const std::string& escapeInJson,
                const std::string& expectedDecoded) {
    const std::string cfg = buildConfigWithEscape("X" + escapeInJson + "Y");
    auto result = audio::BusConfigLoader::ParseFromJson(cfg);
    if (!result.ok || !result.error.empty()) {
        std::printf("  FAIL: escape %s — loader rejected with: %s "
                    "(line %d)\n",
                    what, result.error.c_str(), result.errorLine);
        return false;
    }
    if (result.busGraph.busCount != 2) {
        std::printf("  FAIL: escape %s — expected 2 buses (Master + child), got %u\n",
                    what, result.busGraph.busCount);
        return false;
    }
    // Master is at index 0 (id kBusMaster). The escape-bearing bus
    // is the one whose debugName is NOT "Master".
    const std::string nameA(result.busGraph.buses[0].debugName);
    const std::string nameB(result.busGraph.buses[1].debugName);
    const std::string& actualName = (nameA == "Master") ? nameB : nameA;
    const std::string expectedName = "X" + expectedDecoded + "Y";
    if (actualName != expectedName) {
        std::printf("  FAIL: escape %s — name mismatch.\n"
                    "         expected: %s\n"
                    "         got:      %s\n",
                    what, expectedName.c_str(), actualName.c_str());
        return false;
    }
    std::printf("  ok: %s\n", what);
    return true;
}

} // anonymous namespace

int main() {
    std::printf("[json_escape_test] all 9 JSON spec escapes round-trip "
                "through the bus-config loader\n");

    bool allOk = true;

    // The six "simple" escapes that ALL JSON parsers should handle.
    // Pre-v0.80.0 the bus-config loader handled six of these but not
    // the next three.
    allOk &= testEscape("\\\"  (quote)",         "\\\"", "\"");
    allOk &= testEscape("\\\\  (backslash)",     "\\\\", "\\");
    allOk &= testEscape("\\/   (forward slash)", "\\/",  "/");
    allOk &= testEscape("\\n   (newline)",       "\\n",  "\n");
    allOk &= testEscape("\\t   (tab)",           "\\t",  "\t");
    allOk &= testEscape("\\r   (carriage return)","\\r", "\r");

    // The three that pre-v0.80.0 rejected. \b and \f are uncommon
    // but spec-mandated. \u is the bug the user actually hit — used
    // for non-ASCII characters via codepoint.
    allOk &= testEscape("\\b   (backspace)",     "\\b",  "\b");
    allOk &= testEscape("\\f   (form feed)",     "\\f",  "\f");

    // \u escapes — the regression that brought us here.
    //   \u00E9 = é            (Latin small letter e with acute)
    //   \u2014 = em-dash       (the one in the FPS template)
    //   \u2192 = right arrow   (also in the FPS template)
    //   \u00A0 = non-breaking space
    //   \u0041 = A (ASCII; should be byte-identical to literal A)
    allOk &= testEscape("\\u00E9 (e-acute, BMP)",
                        "\\u00E9", "\xC3\xA9");          // UTF-8: 0xC3 0xA9
    allOk &= testEscape("\\u2014 (em-dash, BMP)",
                        "\\u2014", "\xE2\x80\x94");      // UTF-8: 0xE2 0x80 0x94
    allOk &= testEscape("\\u2192 (right arrow, BMP)",
                        "\\u2192", "\xE2\x86\x92");      // UTF-8: 0xE2 0x86 0x92
    allOk &= testEscape("\\u00A0 (NBSP, BMP)",
                        "\\u00A0", "\xC2\xA0");          // UTF-8: 0xC2 0xA0
    allOk &= testEscape("\\u0041 (ASCII A, BMP low)",
                        "\\u0041", "A");

    // Surrogate-pair encoding of a non-BMP codepoint.
    //   \uD83D\uDD0A = U+1F50A SPEAKER WITH THREE SOUND WAVES (🔊)
    //   UTF-8: 0xF0 0x9F 0x94 0x8A
    allOk &= testEscape("\\uD83D\\uDD0A (speaker emoji, non-BMP surrogate pair)",
                        "\\uD83D\\uDD0A",
                        "\xF0\x9F\x94\x8A");

    // Mixed escapes in one string — defense in depth against any
    // weird interaction between consecutive escape decodes.
    allOk &= testEscape("mixed (\\t + \\u2014 + \\n)",
                        "\\tEM\\u2014DASH\\n",
                        "\tEM\xE2\x80\x94" "DASH\n");

    std::printf("[json_escape_test] %s\n", allOk ? "PASSED" : "FAILED");
    return allOk ? 0 : 1;
}
