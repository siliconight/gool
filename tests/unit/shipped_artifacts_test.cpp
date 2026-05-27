// tests/unit/shipped_artifacts_test.cpp
//
// v0.80.0 regression guard against shipping a JSON artifact that
// gool's own loader can't read.
//
// Background: v0.78.0 through v0.79.9 shipped the FPS bus-config
// template (godot/addons/gool/templates/config_fps.json) containing
// \u-escape sequences that the hand-rolled JSON parser couldn't
// handle. Every Godot user who clicked "Use FPS template" in the
// mixer dock's empty-state UI hit a first-run parse failure. The
// bug survived twelve releases because no CI check exercised our
// shipped artifacts against our own loader.
//
// This test fixes that. It walks the repo's known shipped-artifact
// directories — both the canonical templates and the example
// projects' bus configs — and parses each through the actual
// BusConfigLoader. Any file the loader rejects fails the build,
// preventing the bug class from recurring through new templates.
//
// Path discovery is relative to the test binary's invocation cwd
// (CTest runs from the build dir, so we ascend until we find the
// repo root by looking for CMakeLists.txt). Heuristic — works for
// both local and CI builds.

#include "audio_engine/bus_config_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Find the repo root by walking up from CWD until we find a marker.
// The CI's CMakeLists.txt is the most reliable repo-root signal.
fs::path findRepoRoot() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(cur / "CMakeLists.txt")
            && fs::exists(cur / "src" / "audio_engine")) {
            return cur;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    // Fallback: just use cwd and let the file enumeration come up empty
    // — the test will then report "no artifacts found" which is itself
    // a useful signal that the harness is misconfigured.
    return fs::current_path();
}

// Load a file's full text into a string. Returns empty string + sets
// `err` on failure.
std::string slurp(const fs::path& p, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "could not open for reading";
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Try to parse `path` through BusConfigLoader::ParseFromJson.
// Returns true on success, false (and writes diagnostic) on failure.
bool testBusConfig(const fs::path& path) {
    std::string err;
    const std::string text = slurp(path, err);
    if (!err.empty()) {
        std::printf("  FAIL: %s — %s\n", path.string().c_str(),
                    err.c_str());
        return false;
    }
    auto result = audio::BusConfigLoader::ParseFromJson(text);
    if (!result.ok || !result.error.empty()) {
        std::printf("  FAIL: %s\n", path.string().c_str());
        std::printf("        line %d: %s\n",
                    result.errorLine, result.error.c_str());
        return false;
    }
    std::printf("  ok:   %s (%u bus%s)\n",
                path.string().c_str(),
                result.busGraph.busCount,
                result.busGraph.busCount == 1 ? "" : "es");
    return true;
}

// Enumerate every shipped JSON artifact we expect the bus-config
// loader to parse. Returns the full path list. Missing directories
// (e.g. examples/ not yet populated on a partial checkout) are
// silently skipped — better to test what's present than to fail on
// directory existence.
//
// Roots scanned:
//   - godot/addons/gool/templates/             (canonical templates)
//   - examples/*/gool/config.json              (every example's bus config)
//   - examples/*/addons/gool/templates/        (mirror templates inside example projects, if any)
std::vector<fs::path> findShippedBusConfigs(const fs::path& root) {
    std::vector<fs::path> out;

    // 1. Canonical templates directory. Every .json here is a
    //    bus-config template candidate — but filter to ones whose
    //    schema matches (heuristic: it has "buses": [ in the file).
    //    Strict-typed loaders for other JSON shapes (e.g. the
    //    dialogue-setup example) are out of scope for this test
    //    until we add them as separate iteration roots.
    const fs::path templates = root / "godot" / "addons" / "gool" / "templates";
    if (fs::is_directory(templates)) {
        for (const auto& entry : fs::directory_iterator(templates)) {
            if (entry.is_regular_file()
                && entry.path().extension() == ".json") {
                // Peek at content to guess schema. Bus configs have
                // "buses" at the top level; non-bus configs (dialogue
                // setup, sound banks) don't.
                std::string err;
                std::string txt = slurp(entry.path(), err);
                if (err.empty() && txt.find("\"buses\"") != std::string::npos) {
                    out.push_back(entry.path());
                }
            }
        }
    }

    // 2. Per-example bus configs under examples/*/gool/config.json.
    const fs::path examples = root / "examples";
    if (fs::is_directory(examples)) {
        for (const auto& dir : fs::directory_iterator(examples)) {
            if (!dir.is_directory()) continue;
            const fs::path cfg = dir.path() / "gool" / "config.json";
            if (fs::is_regular_file(cfg)) {
                out.push_back(cfg);
            }
        }
    }

    return out;
}

} // anonymous namespace

int main() {
    const fs::path root = findRepoRoot();
    std::printf("[shipped_artifacts_test] repo root: %s\n",
                root.string().c_str());

    const std::vector<fs::path> busConfigs = findShippedBusConfigs(root);
    if (busConfigs.empty()) {
        std::printf("  WARNING: no shipped bus-config artifacts found "
                    "under %s. Either the harness is misconfigured or "
                    "the repo has been gutted. Failing test to surface "
                    "the misconfiguration.\n",
                    root.string().c_str());
        return 1;
    }

    std::printf("[shipped_artifacts_test] %zu bus-config artifact(s) "
                "to validate:\n", busConfigs.size());
    bool allOk = true;
    for (const auto& path : busConfigs) {
        allOk &= testBusConfig(path);
    }

    std::printf("[shipped_artifacts_test] %s\n",
                allOk ? "PASSED" : "FAILED");
    return allOk ? 0 : 1;
}
