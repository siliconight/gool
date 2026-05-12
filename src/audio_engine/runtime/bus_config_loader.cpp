// audio_engine/runtime/bus_config_loader.cpp
//
// JSON → BusGraphConfig parser. Self-contained minimal recursive-
// descent parser; no shared scanner refactor in this iteration. The
// schema is small enough (5 effect kinds, ~30 distinct keys) that a
// dedicated parser stays under 400 LOC and keeps blast radius
// minimal.

#include "audio_engine/bus_config_loader.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace audio::BusConfigLoader {

namespace {

// =============================================================================
// Scanner: tokenizer + lightweight error-tracking JSON walker
// =============================================================================
//
// The parser doesn't materialize a tree first; it walks the source
// and writes directly into BusGraphConfig as it goes. Saves an
// allocation pass and matches the way the schema is structured
// (one object → one bus).

class Scanner {
public:
    Scanner(const char* begin, const char* end) noexcept
        : cur_(begin), end_(end) {}

    int   line()  const noexcept { return line_; }
    bool  done()  const noexcept { return cur_ >= end_; }
    char  peek()  const noexcept { return done() ? '\0' : *cur_; }

    void  advance() noexcept {
        if (done()) return;
        if (*cur_ == '\n') ++line_;
        ++cur_;
    }

    void skipWs() noexcept {
        while (!done()) {
            const char c = *cur_;
            // Whitespace.
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
                continue;
            }
            // Line comment // — not standard JSON but tolerated for
            // configs hosts hand-edit. Stops at end-of-line.
            if (c == '/' && cur_ + 1 < end_ && cur_[1] == '/') {
                advance(); advance();
                while (!done() && peek() != '\n') advance();
                continue;
            }
            break;
        }
    }

    bool match(char c) noexcept {
        skipWs();
        if (peek() == c) { advance(); return true; }
        return false;
    }

    void expect(char c, const char* what, std::string& err, int& errLine) {
        skipWs();
        if (peek() != c) {
            err = std::string("expected '") + c + "' " + what +
                   ", got '" + (done() ? std::string("EOF") : std::string(1, peek())) + "'";
            errLine = line_;
            return;
        }
        advance();
    }

    // Parse a JSON string literal. Returns false on malformed input.
    bool parseString(std::string& out, std::string& err, int& errLine) {
        skipWs();
        if (peek() != '"') {
            err = "expected string"; errLine = line_; return false;
        }
        advance(); // consume opening quote
        out.clear();
        while (!done()) {
            char c = peek();
            if (c == '"') { advance(); return true; }
            if (c == '\\') {
                advance();
                char esc = peek();
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    default:
                        err = std::string("bad escape \\") + esc;
                        errLine = line_;
                        return false;
                }
                advance();
            } else {
                out.push_back(c);
                advance();
            }
        }
        err = "unterminated string"; errLine = line_;
        return false;
    }

    // Parse a numeric token into a double.
    bool parseNumber(double& out, std::string& err, int& errLine) {
        skipWs();
        const char* begin = cur_;
        if (peek() == '-' || peek() == '+') advance();
        while (!done() && (std::isdigit(static_cast<unsigned char>(peek())) ||
                            peek() == '.' || peek() == 'e' ||
                            peek() == 'E' || peek() == '-' || peek() == '+')) {
            advance();
        }
        if (cur_ == begin) {
            err = "expected number"; errLine = line_; return false;
        }
        std::string buf(begin, cur_);
        char* endPtr = nullptr;
        out = std::strtod(buf.c_str(), &endPtr);
        if (endPtr == buf.c_str()) {
            err = "malformed number '" + buf + "'"; errLine = line_; return false;
        }
        return true;
    }

    bool parseBool(bool& out, std::string& err, int& errLine) {
        skipWs();
        if (cur_ + 4 <= end_ && std::memcmp(cur_, "true", 4) == 0) {
            for (int i = 0; i < 4; ++i) advance();
            out = true;
            return true;
        }
        if (cur_ + 5 <= end_ && std::memcmp(cur_, "false", 5) == 0) {
            for (int i = 0; i < 5; ++i) advance();
            out = false;
            return true;
        }
        err = "expected bool"; errLine = line_; return false;
    }

    // Skip an arbitrary JSON value (used for unknown keys we want to
    // tolerate without crashing). Returns false on malformed input.
    bool skipValue(std::string& err, int& errLine) {
        skipWs();
        char c = peek();
        if (c == '"') { std::string ignored; return parseString(ignored, err, errLine); }
        if (c == 't' || c == 'f') { bool b; return parseBool(b, err, errLine); }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            double d; return parseNumber(d, err, errLine);
        }
        if (c == '[') {
            advance();
            skipWs();
            if (peek() == ']') { advance(); return true; }
            while (true) {
                if (!skipValue(err, errLine)) return false;
                skipWs();
                if (match(',')) continue;
                expect(']', "to close array", err, errLine);
                return err.empty();
            }
        }
        if (c == '{') {
            advance();
            skipWs();
            if (peek() == '}') { advance(); return true; }
            while (true) {
                std::string k;
                if (!parseString(k, err, errLine)) return false;
                expect(':', "after key", err, errLine);
                if (!err.empty()) return false;
                if (!skipValue(err, errLine)) return false;
                skipWs();
                if (match(',')) continue;
                expect('}', "to close object", err, errLine);
                return err.empty();
            }
        }
        err = "unexpected token"; errLine = line_; return false;
    }

private:
    const char* cur_;
    const char* end_;
    int         line_ = 1;
};

// =============================================================================
// Schema-aware parsing helpers
// =============================================================================

bool parseEffect(Scanner& s, EffectConfig& fx,
                  std::string& err, int& errLine,
                  std::string& sidechainName /* output: bus name to resolve */) {
    s.expect('{', "to begin effect", err, errLine);
    if (!err.empty()) return false;
    sidechainName.clear();

    bool kindSet = false;
    while (true) {
        s.skipWs();
        if (s.peek() == '}') { s.advance(); break; }
        std::string key;
        if (!s.parseString(key, err, errLine)) return false;
        s.expect(':', "after effect key", err, errLine);
        if (!err.empty()) return false;

        // Dispatch on key. Keys recognized differ per effect kind;
        // unknown keys are tolerated (skipped) so future schema
        // additions don't break old configs.
        if (key == "kind") {
            std::string kind;
            if (!s.parseString(kind, err, errLine)) return false;
            if      (kind == "gain")        fx.kind = EffectKind::Gain;
            else if (kind == "biquad")      fx.kind = EffectKind::BiquadFilter;
            else if (kind == "compressor")  fx.kind = EffectKind::Compressor;
            else if (kind == "reverb")      fx.kind = EffectKind::Reverb;
            else if (kind == "saturation")  fx.kind = EffectKind::Saturation;
            else {
                err = "unknown effect kind '" + kind + "'";
                errLine = s.line(); return false;
            }
            kindSet = true;
        } else {
            double n = 0.0;
            // Numeric keys — parse as number when not a known string field.
            if (key == "gain_db")         { if (!s.parseNumber(n, err, errLine)) return false; fx.gainDb = static_cast<float>(n); }
            else if (key == "biquad_type") {
                std::string bt;
                if (!s.parseString(bt, err, errLine)) return false;
                if      (bt == "lowpass" || bt == "lp")    fx.biquadType = BiquadType::LowPass;
                else if (bt == "highpass"|| bt == "hp")    fx.biquadType = BiquadType::HighPass;
                else if (bt == "bandpass"|| bt == "bp")    fx.biquadType = BiquadType::BandPass;
                else if (bt == "lowshelf")                  fx.biquadType = BiquadType::LowShelf;
                else if (bt == "highshelf")                 fx.biquadType = BiquadType::HighShelf;
                else if (bt == "peak")                      fx.biquadType = BiquadType::Peak;
                else { err = "unknown biquad_type '" + bt + "'"; errLine = s.line(); return false; }
            }
            else if (key == "cutoff_hz")  { if (!s.parseNumber(n, err, errLine)) return false; fx.biquadCutoffHz = static_cast<float>(n); }
            else if (key == "q")          { if (!s.parseNumber(n, err, errLine)) return false; fx.biquadQ = static_cast<float>(n); }
            else if (key == "biquad_gain_db") { if (!s.parseNumber(n, err, errLine)) return false; fx.biquadGainDb = static_cast<float>(n); }
            // Compressor.
            else if (key == "threshold_db")       { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorThresholdDb     = static_cast<float>(n); }
            else if (key == "ratio")              { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorRatio           = static_cast<float>(n); }
            else if (key == "attack_ms")          { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorAttackMs        = static_cast<float>(n); }
            else if (key == "release_ms")         { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorReleaseMs       = static_cast<float>(n); }
            else if (key == "makeup_db")          { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorMakeupDb        = static_cast<float>(n); }
            else if (key == "knee_width_db")      { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorKneeWidthDb     = static_cast<float>(n); }
            else if (key == "mix_ratio")          { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorMixRatio        = static_cast<float>(n); }
            else if (key == "max_reduction_db")   { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorMaxReductionDb  = static_cast<float>(n); }
            else if (key == "sidechain_hpf_hz")   { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorSidechainHpfHz  = static_cast<float>(n); }
            else if (key == "hold_ms")            { if (!s.parseNumber(n, err, errLine)) return false; fx.compressorHoldMs          = static_cast<float>(n); }
            else if (key == "detection_mode") {
                std::string dm;
                if (!s.parseString(dm, err, errLine)) return false;
                if      (dm == "peak") fx.compressorDetectionMode = EffectConfig::CompressorDetectionMode::Peak;
                else if (dm == "rms")  fx.compressorDetectionMode = EffectConfig::CompressorDetectionMode::Rms;
                else { err = "unknown detection_mode '" + dm + "'"; errLine = s.line(); return false; }
            }
            else if (key == "sidechain_bus")      { if (!s.parseString(sidechainName, err, errLine)) return false; }
            // Reverb.
            else if (key == "room_size")          { if (!s.parseNumber(n, err, errLine)) return false; fx.reverbRoomSize = static_cast<float>(n); }
            else if (key == "damping")            { if (!s.parseNumber(n, err, errLine)) return false; fx.reverbDamping  = static_cast<float>(n); }
            else if (key == "wet_gain_db")        { if (!s.parseNumber(n, err, errLine)) return false; fx.reverbWetGainDb = static_cast<float>(n); }
            // Saturation.
            else if (key == "drive")              { if (!s.parseNumber(n, err, errLine)) return false; fx.saturationDrive       = static_cast<float>(n); }
            else if (key == "mix")                { if (!s.parseNumber(n, err, errLine)) return false; fx.saturationMix         = static_cast<float>(n); }
            else if (key == "output_gain")        { if (!s.parseNumber(n, err, errLine)) return false; fx.saturationOutputGain  = static_cast<float>(n); }
            else if (key == "bias")               { if (!s.parseNumber(n, err, errLine)) return false; fx.saturationBias        = static_cast<float>(n); }
            // Tolerate unknown keys for forward-compat.
            else { if (!s.skipValue(err, errLine)) return false; }
        }

        s.skipWs();
        if (!s.match(',')) {
            s.expect('}', "to close effect", err, errLine);
            if (!err.empty()) return false;
            break;
        }
    }

    if (!kindSet) {
        err = "effect missing 'kind'"; errLine = s.line(); return false;
    }
    return true;
}

struct ParsedBus {
    std::string  name;
    std::string  parentName;
    float        gainDb        = 0.0f;
    bool         silent        = false;
    EffectConfig effects[kMaxEffectsPerBus]{};
    uint32_t     effectCount   = 0;
    // Per-effect sidechain bus names captured during parse, resolved
    // to BusIds in the second pass once all bus names are known.
    std::string  effectSidechainNames[kMaxEffectsPerBus];
};

bool parseBus(Scanner& s, ParsedBus& bus, std::string& err, int& errLine) {
    s.expect('{', "to begin bus object", err, errLine);
    if (!err.empty()) return false;

    while (true) {
        s.skipWs();
        if (s.peek() == '}') { s.advance(); break; }
        std::string key;
        if (!s.parseString(key, err, errLine)) return false;
        s.expect(':', "after bus key", err, errLine);
        if (!err.empty()) return false;

        if (key == "name") {
            if (!s.parseString(bus.name, err, errLine)) return false;
        } else if (key == "parent") {
            if (!s.parseString(bus.parentName, err, errLine)) return false;
        } else if (key == "gain_db") {
            double n = 0.0;
            if (!s.parseNumber(n, err, errLine)) return false;
            bus.gainDb = static_cast<float>(n);
        } else if (key == "silent") {
            if (!s.parseBool(bus.silent, err, errLine)) return false;
        } else if (key == "effects") {
            s.expect('[', "to begin effects array", err, errLine);
            if (!err.empty()) return false;
            s.skipWs();
            if (s.peek() == ']') { s.advance(); }
            else {
                while (true) {
                    if (bus.effectCount >= kMaxEffectsPerBus) {
                        err = "too many effects on bus (max " +
                                std::to_string(kMaxEffectsPerBus) + ")";
                        errLine = s.line();
                        return false;
                    }
                    if (!parseEffect(s, bus.effects[bus.effectCount],
                                       err, errLine,
                                       bus.effectSidechainNames[bus.effectCount])) {
                        return false;
                    }
                    ++bus.effectCount;
                    s.skipWs();
                    if (s.match(',')) continue;
                    s.expect(']', "to close effects array", err, errLine);
                    if (!err.empty()) return false;
                    break;
                }
            }
        } else {
            // Tolerate unknown keys.
            if (!s.skipValue(err, errLine)) return false;
        }

        s.skipWs();
        if (!s.match(',')) {
            s.expect('}', "to close bus object", err, errLine);
            if (!err.empty()) return false;
            break;
        }
    }
    if (bus.name.empty()) { err = "bus missing 'name'"; errLine = s.line(); return false; }
    return true;
}

bool parseCategoryRouting(Scanner& s, BusGraphConfig& g,
                            const std::unordered_map<std::string, BusId>& nameToId,
                            std::string& err, int& errLine) {
    s.expect('{', "to begin category_routing object", err, errLine);
    if (!err.empty()) return false;

    auto resolveAndSet = [&](const std::string& cat, const std::string& busName) -> bool {
        auto it = nameToId.find(busName);
        if (it == nameToId.end()) {
            err = "category_routing target '" + busName + "' not found"; return false;
        }
        if      (cat == "music")    g.categoryMap.music    = it->second;
        else if (cat == "voice")    g.categoryMap.voice    = it->second;
        else if (cat == "sfx")      g.categoryMap.sfx      = it->second;
        else if (cat == "ambience") g.categoryMap.ambience = it->second;
        else if (cat == "ui")       g.categoryMap.ui       = it->second;
        else if (cat == "dialogue") g.categoryMap.dialogue = it->second;
        else { err = "unknown category '" + cat + "'"; return false; }
        return true;
    };

    while (true) {
        s.skipWs();
        if (s.peek() == '}') { s.advance(); break; }
        std::string cat;
        if (!s.parseString(cat, err, errLine)) return false;
        s.expect(':', "after category", err, errLine);
        if (!err.empty()) return false;
        std::string target;
        if (!s.parseString(target, err, errLine)) return false;
        if (!resolveAndSet(cat, target)) { errLine = s.line(); return false; }
        s.skipWs();
        if (!s.match(',')) {
            s.expect('}', "to close category_routing", err, errLine);
            if (!err.empty()) return false;
            break;
        }
    }
    return true;
}

} // anonymous namespace

ParseResult ParseFromJson(std::string_view json) {
    ParseResult r;
    if (json.empty()) {
        r.error = "empty input"; r.errorLine = 1; return r;
    }
    Scanner s(json.data(), json.data() + json.size());

    s.expect('{', "at root", r.error, r.errorLine);
    if (!r.error.empty()) return r;

    // First pass: parse all buses into a temporary list. Bus and
    // sidechain references are by name; we resolve to BusIds in a
    // second pass once every bus has been seen.
    std::vector<ParsedBus> parsedBuses;
    bool hasCategoryRouting = false;
    std::string categoryRoutingDeferred;  // captured raw text to re-parse later
    int          categoryRoutingDeferredLine = 0;

    while (true) {
        s.skipWs();
        if (s.peek() == '}') { s.advance(); break; }
        std::string key;
        if (!s.parseString(key, r.error, r.errorLine)) return r;
        s.expect(':', "after root key", r.error, r.errorLine);
        if (!r.error.empty()) return r;

        if (key == "buses") {
            s.expect('[', "to begin buses array", r.error, r.errorLine);
            if (!r.error.empty()) return r;
            s.skipWs();
            if (s.peek() == ']') { s.advance(); }
            else {
                while (true) {
                    if (parsedBuses.size() >= kMaxBuses) {
                        r.error = "too many buses (max " +
                                    std::to_string(kMaxBuses) + ")";
                        r.errorLine = s.line(); return r;
                    }
                    parsedBuses.emplace_back();
                    if (!parseBus(s, parsedBuses.back(), r.error, r.errorLine)) return r;
                    s.skipWs();
                    if (s.match(',')) continue;
                    s.expect(']', "to close buses array", r.error, r.errorLine);
                    if (!r.error.empty()) return r;
                    break;
                }
            }
        } else if (key == "category_routing") {
            // Defer parsing — we need the bus-name → BusId map first.
            // Capture the start of this object's text so we can re-scan
            // after the buses array is fully resolved. Simpler than
            // restructuring the parser into two passes over the file.
            hasCategoryRouting = true;
            categoryRoutingDeferredLine = s.line();
            // Walk and capture the JSON object text into a string.
            s.skipWs();
            int depth = 0;
            std::string captured;
            // Skip to the '{'.
            while (!s.done() && s.peek() != '{') {
                captured.push_back(s.peek());
                s.advance();
            }
            if (s.done()) {
                r.error = "expected '{' for category_routing";
                r.errorLine = s.line(); return r;
            }
            // Capture balanced braces.
            captured.push_back(s.peek()); s.advance(); depth = 1;
            bool inStr = false;
            while (!s.done() && depth > 0) {
                char c = s.peek();
                captured.push_back(c);
                s.advance();
                if (inStr) {
                    if (c == '\\' && !s.done()) {
                        captured.push_back(s.peek());
                        s.advance();
                    } else if (c == '"') {
                        inStr = false;
                    }
                } else {
                    if      (c == '"') inStr = true;
                    else if (c == '{') ++depth;
                    else if (c == '}') --depth;
                }
            }
            if (depth != 0) {
                r.error = "unterminated category_routing object";
                r.errorLine = s.line(); return r;
            }
            categoryRoutingDeferred = std::move(captured);
        } else {
            // Tolerate unknown root-level keys for forward compat.
            if (!s.skipValue(r.error, r.errorLine)) return r;
        }

        s.skipWs();
        if (!s.match(',')) {
            s.expect('}', "to close root", r.error, r.errorLine);
            if (!r.error.empty()) return r;
            break;
        }
    }

    // Resolve buses. Empty configuration is OK — engine auto-builds
    // a single-master topology when busGraph.busCount == 0. This is
    // the path back-compat configs (old config.json with only
    // sample_rate/buffer_size and no "buses" key) take.
    if (parsedBuses.empty()) {
        r.busGraph.busCount = 0;
        r.busGraph.categoryMap = CategoryBusMap{};
        r.ok = true;
        return r;
    }

    // Assign BusIds in declaration order. Master must be at id 0.
    // If any bus is named "Master" (any case), it gets id kBusMaster;
    // otherwise the first bus is treated as master.
    std::unordered_map<std::string, BusId> nameToId;
    BusId nextId = 0;
    int masterIndex = -1;
    for (size_t i = 0; i < parsedBuses.size(); ++i) {
        const std::string& n = parsedBuses[i].name;
        if (n == "Master" || n == "master") {
            if (masterIndex >= 0) {
                r.error = "duplicate Master bus"; r.errorLine = 1; return r;
            }
            masterIndex = static_cast<int>(i);
        }
        if (nameToId.count(n) > 0) {
            r.error = "duplicate bus name '" + n + "'"; r.errorLine = 1; return r;
        }
    }
    // Master gets id 0 (kBusMaster).
    if (masterIndex < 0) {
        r.error = "no bus named 'Master'"; r.errorLine = 1; return r;
    }
    nameToId[parsedBuses[masterIndex].name] = kBusMaster;
    nextId = 1;
    for (size_t i = 0; i < parsedBuses.size(); ++i) {
        if (static_cast<int>(i) == masterIndex) continue;
        nameToId[parsedBuses[i].name] = nextId++;
    }

    // Build BusGraphConfig.
    r.busGraph.busCount = static_cast<uint32_t>(parsedBuses.size());
    for (size_t i = 0; i < parsedBuses.size(); ++i) {
        const ParsedBus& p = parsedBuses[i];
        BusConfig& out     = r.busGraph.buses[nameToId[p.name]];
        out.id             = nameToId[p.name];
        out.outputGainDb   = p.gainDb;
        out.silent         = p.silent;
        // Parent: empty or "Master" → master; any other name → resolved.
        if (p.parentName.empty() || p.parentName == "Master" || p.parentName == "master") {
            out.parent = kBusMaster;
        } else {
            auto it = nameToId.find(p.parentName);
            if (it == nameToId.end()) {
                r.error = "bus '" + p.name + "' parent '" + p.parentName + "' not found";
                r.errorLine = 1; return r;
            }
            out.parent = it->second;
        }
        // Debug name (truncated). MSVC C4996 flags strncpy as deprecated in
        // favor of strncpy_s; the bounded copy + explicit NUL-termination on
        // the next line is intentional and matches the rest of the codebase.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4996)
#endif
        std::strncpy(out.debugName, p.name.c_str(), sizeof(out.debugName) - 1);
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
        out.debugName[sizeof(out.debugName) - 1] = '\0';

        // Effects.
        out.effectCount = p.effectCount;
        for (uint32_t e = 0; e < p.effectCount; ++e) {
            out.effects[e] = p.effects[e];
            // Resolve sidechain bus name → BusId, if set.
            if (!p.effectSidechainNames[e].empty()) {
                auto it = nameToId.find(p.effectSidechainNames[e]);
                if (it == nameToId.end()) {
                    r.error = "effect on bus '" + p.name + "' references unknown sidechain bus '" +
                              p.effectSidechainNames[e] + "'";
                    r.errorLine = 1; return r;
                }
                out.effects[e].compressorSidechainBus = it->second;
            }
        }
    }

    // Master must be the entry whose id is kBusMaster.
    // (We already put it there above.)
    r.busGraph.buses[kBusMaster].parent = kBusMaster;  // canonical

    // Default category routing → master (the struct default; explicit
    // for clarity).
    r.busGraph.categoryMap = CategoryBusMap{};

    // Now do the deferred category_routing parse.
    if (hasCategoryRouting) {
        Scanner s2(categoryRoutingDeferred.data(),
                    categoryRoutingDeferred.data() + categoryRoutingDeferred.size());
        std::string subErr;
        int subLine = categoryRoutingDeferredLine;
        if (!parseCategoryRouting(s2, r.busGraph, nameToId, subErr, subLine)) {
            r.error = std::move(subErr); r.errorLine = subLine; return r;
        }
    }

    r.ok = true;
    return r;
}

} // namespace audio::BusConfigLoader
