// tests/fuzz/fuzz_bus_config_json.cpp
//
// libFuzzer harness for audio::BusConfigLoader::ParseFromJson.
//
// Surface under test: the JSON config parser is one of three input
// surfaces in gool that consume attacker-influenceable bytes (the
// others are audio decoders and Opus voice packets, each with its
// own harness). A malicious or corrupted bus_config.json should
// never crash the engine. The parser owns:
//   - JSON tokenization (via nlohmann_json)
//   - Bus-graph topology validation (parent references, cycles)
//   - Effect-chain validation (effect kinds, parameter ranges)
//   - String length bounds (bus names, effect names)
//
// Build (manual, outside CI): see CMakeLists.txt AUDIO_ENGINE_FUZZ option.
//   cmake -S . -B build-fuzz -DAUDIO_ENGINE_FUZZ=ON \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz --target fuzz_bus_config_json
//   ./build-fuzz/fuzz_bus_config_json -max_total_time=60
//
// The nightly fuzz CI workflow runs each harness for a fixed budget
// (60 seconds per harness) on every nightly trigger; failures are
// uploaded as artifacts with the reproducer corpus entry.

#include "audio_engine/bus_config_loader.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Interpret the input bytes as a JSON document. ParseFromJson takes
    // string_view (not null-terminated) so we can feed the raw fuzz
    // input directly without copying.
    const std::string_view json(reinterpret_cast<const char*>(data), size);

    // ParseFromJson is documented to never throw on malformed input —
    // it returns a ParseResult with an error code instead. Bugs we
    // are looking for: signed/unsigned overflow in length parsing,
    // out-of-bounds reads on truncated input, infinite recursion on
    // deeply-nested objects, double-free on partial parse failure,
    // memory leaks (when run under -fsanitize=address). All caught
    // by ASAN+UBSAN composed with libFuzzer's coverage feedback.
    //
    // We deliberately do nothing with the result; libFuzzer measures
    // coverage via instrumentation, not return value.
    (void) audio::BusConfigLoader::ParseFromJson(json);

    return 0;
}
