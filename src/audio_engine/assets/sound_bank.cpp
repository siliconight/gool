// Sound bank implementation.
//
// Two pieces:
//   1) A small recursive-descent JSON parser that walks the schema
//      directly into typed structs. No DOM tree, no dependency. ~400
//      lines of strict, line-tracking JSON 1:1 to schema mapping.
//   2) The bank itself: hashes names → AudioSoundIds, validates, and
//      either calls RegisterSoundFromFile or RegisterSoundFromMemory
//      (via a host-supplied byte loader) for each entry.
//
// Find() and Contains() are read-only and lock-free after a load.
// All allocation happens during Load*; the steady-state Find() path
// is a hash-map lookup plus, for groups, an atomic-fetch-add for
// selection.

#include "audio_engine/sound_bank.h"

#include "audio_engine/audio_runtime.h"
#include "audio_engine/attenuation.h"
#include "audio_engine/audio_file_format.h"
#include "audio_engine/emitter.h"
#include "audio_engine/result.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace audio {

// ---------------------------------------------------------------------
// Default callbacks
// ---------------------------------------------------------------------

namespace {

BusId DefaultBusResolver(std::string_view name) noexcept {
    if (name == "master") return kBusMaster;
    return kInvalidBusId;
}

bool DefaultFileLoader(std::string_view             path,
                        std::vector<uint8_t>&        out,
                        const std::filesystem::path& baseDir) {
    std::filesystem::path full(path);
    if (full.is_relative() && !baseDir.empty()) {
        full = baseDir / full;
    }
    std::ifstream in(full, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(out.size()));
    }
    return static_cast<bool>(in);
}

// ---------------------------------------------------------------------
// JSON parser
// ---------------------------------------------------------------------
//
// Strict subset: no comments, no trailing commas, no \u escapes (we
// don't need Unicode in field names or sound names). Enough for
// designer-authored bank files; rejects anything weird with a clear
// line-numbered error.

struct ParseError {
    std::string message;
    int         line = 1;
};

class JsonScanner {
public:
    JsonScanner(const char* begin, const char* end) noexcept
        : cur_(begin), end_(end) {}

    int  Line() const noexcept { return line_; }
    bool AtEnd() const noexcept { return cur_ >= end_; }

    char Peek() const noexcept { return AtEnd() ? '\0' : *cur_; }

    void Advance() noexcept {
        if (AtEnd()) return;
        if (*cur_ == '\n') ++line_;
        ++cur_;
    }

    void SkipWhitespace() noexcept {
        while (!AtEnd()) {
            const char c = *cur_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                Advance();
            } else {
                break;
            }
        }
    }

    bool Match(char c) noexcept {
        SkipWhitespace();
        if (Peek() == c) { Advance(); return true; }
        return false;
    }

    bool MatchKeyword(const char* kw) noexcept {
        SkipWhitespace();
        const size_t n = std::strlen(kw);
        if (static_cast<size_t>(end_ - cur_) < n) return false;
        if (std::memcmp(cur_, kw, n) != 0) return false;
        // Don't consume an identifier-like trailing char.
        const char next = (cur_ + n < end_) ? cur_[n] : '\0';
        if ((next >= 'a' && next <= 'z') ||
            (next >= 'A' && next <= 'Z') ||
            (next >= '0' && next <= '9') ||
            next == '_') return false;
        for (size_t i = 0; i < n; ++i) Advance();
        return true;
    }

    void Expect(char c, const char* what, ParseError& err) {
        SkipWhitespace();
        if (Peek() != c) {
            err.line = line_;
            err.message = std::string("expected '") + c + "' " + what;
            return;
        }
        Advance();
    }

    // Parse a JSON string into `out`. Caller has already consumed
    // (or will check) the leading quote? No — this function expects
    // to find a leading quote.
    bool ParseString(std::string& out, ParseError& err) {
        SkipWhitespace();
        if (Peek() != '"') {
            err.line = line_;
            err.message = "expected string";
            return false;
        }
        Advance(); // consume "
        out.clear();
        while (!AtEnd()) {
            const char c = Peek();
            if (c == '"') { Advance(); return true; }
            if (c == '\\') {
                Advance();
                const char esc = Peek();
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    default:
                        err.line = line_;
                        err.message = "unsupported string escape";
                        return false;
                }
                Advance();
            } else if (c == '\n') {
                err.line = line_;
                err.message = "unterminated string (newline inside string)";
                return false;
            } else {
                out.push_back(c);
                Advance();
            }
        }
        err.line = line_;
        err.message = "unterminated string at end of file";
        return false;
    }

    // Parse a JSON number. Accepts ints and floats, including
    // exponent form. Stores into `outF`. If the number was an integer
    // (no '.' or 'e'), `outIsInt` is set true and `outI` is also
    // populated.
    bool ParseNumber(double& outF, long long& outI, bool& outIsInt,
                      ParseError& err) {
        SkipWhitespace();
        const char* start = cur_;
        if (Peek() == '-' || Peek() == '+') Advance();
        bool sawDigit = false;
        bool isFloat  = false;
        while (Peek() >= '0' && Peek() <= '9') { Advance(); sawDigit = true; }
        if (Peek() == '.') { isFloat = true; Advance(); while (Peek() >= '0' && Peek() <= '9') Advance(); }
        if (Peek() == 'e' || Peek() == 'E') {
            isFloat = true;
            Advance();
            if (Peek() == '+' || Peek() == '-') Advance();
            while (Peek() >= '0' && Peek() <= '9') Advance();
        }
        if (!sawDigit) {
            err.line = line_;
            err.message = "expected number";
            return false;
        }
        std::string buf(start, cur_);
        char* endp = nullptr;
        outF = std::strtod(buf.c_str(), &endp);
        outIsInt = !isFloat;
        if (!isFloat) {
            outI = std::strtoll(buf.c_str(), &endp, 10);
        }
        return true;
    }

    // Skip an arbitrary JSON value (used for unknown fields).
    bool SkipValue(ParseError& err) {
        SkipWhitespace();
        const char c = Peek();
        if (c == '"') {
            std::string dummy;
            return ParseString(dummy, err);
        } else if (c == '{') {
            Advance();
            SkipWhitespace();
            if (Match('}')) return true;
            while (!AtEnd()) {
                std::string key;
                if (!ParseString(key, err)) return false;
                Expect(':', "after object key", err);
                if (!err.message.empty()) return false;
                if (!SkipValue(err)) return false;
                if (Match(',')) continue;
                Expect('}', "to close object", err);
                return err.message.empty();
            }
            return false;
        } else if (c == '[') {
            Advance();
            SkipWhitespace();
            if (Match(']')) return true;
            while (!AtEnd()) {
                if (!SkipValue(err)) return false;
                if (Match(',')) continue;
                Expect(']', "to close array", err);
                return err.message.empty();
            }
            return false;
        } else if (c == 't' || c == 'f' || c == 'n') {
            if (MatchKeyword("true") || MatchKeyword("false") ||
                MatchKeyword("null")) return true;
            err.line = line_;
            err.message = "expected true/false/null";
            return false;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            double f; long long i; bool ii;
            return ParseNumber(f, i, ii, err);
        }
        err.line = line_;
        err.message = "unexpected character in JSON value";
        return false;
    }

private:
    const char* cur_;
    const char* end_;
    int         line_ = 1;
};

// ---------------------------------------------------------------------
// Schema mapping
// ---------------------------------------------------------------------

struct ParsedDefaults {
    AudioCategory          category          = AudioCategory::SFX;
    AudioPriority          priority          = AudioPriority::Normal;
    std::string            busName           = "master";
    bool                   spatialized       = true;
    bool                   looping           = false;
    bool                   occlusionEnabled  = true;
    AudioReplicationPolicy replication       = AudioReplicationPolicy::LocalOnly;
    AttenuationSettings    attenuation;
    float                  loopCrossfadeMs   = 0.0f;
    bool                   hasCategory       = false;
    bool                   hasPriority       = false;
    bool                   hasBus            = false;
    bool                   hasSpatialized    = false;
    bool                   hasLooping        = false;
    bool                   hasOcclusion      = false;
    bool                   hasReplication    = false;
    bool                   hasAttenuation    = false;
    bool                   hasLoopCrossfade  = false;
};

struct ParsedRtpcBinding {
    std::string             paramName;
    RtpcTarget              target        = RtpcTarget::Volume;
    RtpcCurve               curve         = RtpcCurve::Linear;
    float                   curveExponent = 2.0f;
    float                   minValue      = 0.0f;
    float                   maxValue      = 1.0f;
    float                   minOutput     = 0.0f;
    float                   maxOutput     = 1.0f;
    float                   smoothingMs   = 50.0f;
    int                     lineDeclared  = 0;
};

struct ParsedSound {
    std::string name;
    std::string file;            // empty if none — required for now
    int         lineDeclared = 0;

    // Per-sound overrides. Each `has*` flag means "this entry sets
    // this field"; otherwise the bank uses the defaults.
    AudioCategory          category;
    bool                   hasCategory      = false;
    AudioPriority          priority;
    bool                   hasPriority      = false;
    std::string            busName;
    bool                   hasBus           = false;
    bool                   spatialized      = true;
    bool                   hasSpatialized   = false;
    bool                   looping          = false;
    bool                   hasLooping       = false;
    bool                   occlusionEnabled = true;
    bool                   hasOcclusion     = false;
    AudioReplicationPolicy replication;
    bool                   hasReplication   = false;
    AttenuationSettings    attenuation;
    bool                   hasAttenuation   = false;
    float                  loopCrossfadeMs  = 0.0f;
    bool                   hasLoopCrossfade = false;

    // RTPC bindings authored alongside the sound. Translated by the
    // loader into runtime.SetSoundRtpc() calls after the sound's
    // SoundDefinition has been registered.
    std::vector<ParsedRtpcBinding> rtpc;
};

enum class GroupPolicy : uint8_t { Random, RandomNoRepeat, Sequential, ByMaterial };

struct ParsedGroup {
    std::string                                                              name;
    GroupPolicy                                                              policy = GroupPolicy::Random;
    std::vector<std::string>                                                 memberNames;
    // For by_material groups, members are keyed by AudioMaterial. Each
    // bucket holds the variant names to pick among at play time. Empty
    // buckets are valid — they simply mean the material has no variant
    // assigned, and Find(name, material) falls back to Default (or
    // returns kInvalidSoundId if Default is also empty).
    std::array<std::vector<std::string>, kAudioMaterialCount>                membersByMaterial;
    int                                                                      lineDeclared = 0;
};

struct ParsedBank {
    int                       version = 0;
    ParsedDefaults            defaults;
    std::vector<ParsedSound>  sounds;
    std::vector<ParsedGroup>  groups;
};

// ---------------------------------------------------------------------
// Enum string mappings
// ---------------------------------------------------------------------

bool ParseCategory(const std::string& s, AudioCategory& out) {
    if (s == "SFX")        { out = AudioCategory::SFX;       return true; }
    if (s == "Voice")      { out = AudioCategory::Voice;     return true; }
    if (s == "Music")      { out = AudioCategory::Music;     return true; }
    if (s == "Ambience")   { out = AudioCategory::Ambience;  return true; }
    if (s == "UI")         { out = AudioCategory::UI;        return true; }
    if (s == "Dialogue")   { out = AudioCategory::Dialogue;  return true; }
    return false;
}

bool ParsePriority(const std::string& s, AudioPriority& out) {
    if (s == "Lowest")   { out = AudioPriority::Lowest;   return true; }
    if (s == "Low")      { out = AudioPriority::Low;      return true; }
    if (s == "Normal")   { out = AudioPriority::Normal;   return true; }
    if (s == "High")     { out = AudioPriority::High;     return true; }
    if (s == "Critical") { out = AudioPriority::Critical; return true; }
    return false;
}

bool ParseFalloff(const std::string& s, FalloffModel& out) {
    if (s == "Linear")        { out = FalloffModel::Linear;        return true; }
    if (s == "Logarithmic")   { out = FalloffModel::Logarithmic;   return true; }
    if (s == "InverseSquare") { out = FalloffModel::InverseSquare; return true; }
    return false;
}

bool ParseReplication(const std::string& s, AudioReplicationPolicy& out) {
    if (s == "LocalOnly")            { out = AudioReplicationPolicy::LocalOnly;            return true; }
    if (s == "OwnerOnly")            { out = AudioReplicationPolicy::OwnerOnly;            return true; }
    if (s == "RemoteRelevant")       { out = AudioReplicationPolicy::RemoteRelevant;       return true; }
    if (s == "Global")               { out = AudioReplicationPolicy::Global;               return true; }
    if (s == "ServerAuthoritative")  { out = AudioReplicationPolicy::ServerAuthoritative;  return true; }
    if (s == "Predicted")            { out = AudioReplicationPolicy::Predicted;            return true; }
    return false;
}

bool ParsePolicy(const std::string& s, GroupPolicy& out) {
    if (s == "random")            { out = GroupPolicy::Random;         return true; }
    if (s == "random_no_repeat")  { out = GroupPolicy::RandomNoRepeat; return true; }
    if (s == "sequential")        { out = GroupPolicy::Sequential;     return true; }
    if (s == "by_material")       { out = GroupPolicy::ByMaterial;     return true; }
    return false;
}

// Parse an AudioMaterial name (the enum literal as a string) to its
// enum value. Used by the by_material group policy's members_by_material
// dict keys. Case-sensitive on purpose — keys mirror the C++ enum
// spelling exactly, no inventive aliasing.
bool ParseAudioMaterial(const std::string& s, AudioMaterial& out) {
    if (s == "Default")  { out = AudioMaterial::Default;  return true; }
    if (s == "Air")      { out = AudioMaterial::Air;      return true; }
    if (s == "Glass")    { out = AudioMaterial::Glass;    return true; }
    if (s == "Wood")     { out = AudioMaterial::Wood;     return true; }
    if (s == "Drywall")  { out = AudioMaterial::Drywall;  return true; }
    if (s == "Concrete") { out = AudioMaterial::Concrete; return true; }
    if (s == "Metal")    { out = AudioMaterial::Metal;    return true; }
    if (s == "Curtain")  { out = AudioMaterial::Curtain;  return true; }
    if (s == "Foliage")  { out = AudioMaterial::Foliage;  return true; }
    return false;
}

// ---------------------------------------------------------------------
// Schema-aware parse routines
// ---------------------------------------------------------------------

bool ParseAttenuation(JsonScanner& s, AttenuationSettings& out, ParseError& err) {
    s.Expect('{', "to open attenuation object", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match('}')) return true;
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after attenuation key", err);
        if (!err.message.empty()) return false;
        if (key == "min" || key == "max" || key == "floor") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            const float v = static_cast<float>(f);
            if      (key == "min")   out.minDistance  = v;
            else if (key == "max")   out.maxDistance  = v;
            else                     out.volumeFloor  = v;
        } else if (key == "falloff") {
            std::string v;
            if (!s.ParseString(v, err)) return false;
            if (!ParseFalloff(v, out.falloffModel)) {
                err.line = s.Line();
                err.message = "unknown falloff model '" + v + "'";
                return false;
            }
        } else {
            if (!s.SkipValue(err)) return false;
        }
        if (s.Match(',')) continue;
        s.Expect('}', "to close attenuation object", err);
        return err.message.empty();
    }
}

// ---------------------------------------------------------------------
// RTPC binding parsers (v0.5+)
// ---------------------------------------------------------------------

bool ParseRtpcTargetName(const std::string& s, RtpcTarget& out) {
    if      (s == "volume")          out = RtpcTarget::Volume;
    else if (s == "pitch")           out = RtpcTarget::Pitch;
    else if (s == "lowpass" ||
             s == "lowpass_cutoff" ||
             s == "low_pass_cutoff") out = RtpcTarget::LowPassCutoff;
    else if (s == "reverb" ||
             s == "reverb_send")     out = RtpcTarget::ReverbSend;
    else return false;
    return true;
}

bool ParseRtpcCurveName(const std::string& s, RtpcCurve& out) {
    if      (s == "linear")               out = RtpcCurve::Linear;
    else if (s == "exponential" ||
             s == "exp")                  out = RtpcCurve::Exponential;
    else if (s == "inverse_exponential" ||
             s == "inv_exp" ||
             s == "inverse_exp")          out = RtpcCurve::InverseExponential;
    else if (s == "scurve" ||
             s == "smoothstep")           out = RtpcCurve::SCurve;
    else return false;
    return true;
}

bool ParseRtpcBinding(JsonScanner& s, ParsedRtpcBinding& out, ParseError& err) {
    out.lineDeclared = s.Line();
    s.Expect('{', "to open rtpc binding object", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match('}')) {
        err.line = out.lineDeclared;
        err.message = "rtpc binding has no fields";
        return false;
    }
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after rtpc binding key", err);
        if (!err.message.empty()) return false;

        if (key == "parameter" || key == "param") {
            if (!s.ParseString(out.paramName, err)) return false;
        } else if (key == "target") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseRtpcTargetName(v, out.target)) {
                err.line = s.Line();
                err.message = "unknown rtpc target '" + v +
                              "' (expected: volume, pitch, lowpass, reverb)";
                return false;
            }
        } else if (key == "curve") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseRtpcCurveName(v, out.curve)) {
                err.line = s.Line();
                err.message = "unknown rtpc curve '" + v +
                              "' (expected: linear, exp, inv_exp, scurve)";
                return false;
            }
        } else if (key == "curve_exponent" || key == "exponent") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.curveExponent = static_cast<float>(f);
        } else if (key == "min_value") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.minValue = static_cast<float>(f);
        } else if (key == "max_value") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.maxValue = static_cast<float>(f);
        } else if (key == "min_output") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.minOutput = static_cast<float>(f);
        } else if (key == "max_output") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.maxOutput = static_cast<float>(f);
        } else if (key == "smoothing_ms" || key == "smoothing") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.smoothingMs = static_cast<float>(f);
        } else {
            if (!s.SkipValue(err)) return false;
        }

        if (s.Match(',')) continue;
        s.Expect('}', "to close rtpc binding object", err);
        if (!err.message.empty()) return false;

        // Validation: required fields must have been supplied.
        if (out.paramName.empty()) {
            err.line = out.lineDeclared;
            err.message = "rtpc binding missing required 'parameter' field";
            return false;
        }
        return true;
    }
}

bool ParseRtpcArray(JsonScanner& s, std::vector<ParsedRtpcBinding>& out, ParseError& err) {
    s.Expect('[', "to open rtpc array", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match(']')) return true;
    while (true) {
        out.emplace_back();
        if (!ParseRtpcBinding(s, out.back(), err)) return false;
        if (s.Match(',')) continue;
        s.Expect(']', "to close rtpc array", err);
        return err.message.empty();
    }
}

// Parse defaults block. `outHas*` flags also flip true for any field
// the JSON sets, so per-sound entries can fall back through.
bool ParseDefaults(JsonScanner& s, ParsedDefaults& out, ParseError& err) {
    s.Expect('{', "to open defaults object", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match('}')) return true;
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after defaults key", err);
        if (!err.message.empty()) return false;

        if (key == "category") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseCategory(v, out.category)) {
                err.line = s.Line();
                err.message = "unknown category '" + v + "'";
                return false;
            }
            out.hasCategory = true;
        } else if (key == "priority") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParsePriority(v, out.priority)) {
                err.line = s.Line();
                err.message = "unknown priority '" + v + "'";
                return false;
            }
            out.hasPriority = true;
        } else if (key == "bus") {
            std::string v; if (!s.ParseString(v, err)) return false;
            out.busName = v; out.hasBus = true;
        } else if (key == "spatialized") {
            if (s.MatchKeyword("true"))       { out.spatialized = true;  out.hasSpatialized = true; }
            else if (s.MatchKeyword("false")) { out.spatialized = false; out.hasSpatialized = true; }
            else { err.line = s.Line(); err.message = "expected boolean for spatialized"; return false; }
        } else if (key == "looping") {
            if (s.MatchKeyword("true"))       { out.looping = true;  out.hasLooping = true; }
            else if (s.MatchKeyword("false")) { out.looping = false; out.hasLooping = true; }
            else { err.line = s.Line(); err.message = "expected boolean for looping"; return false; }
        } else if (key == "occlusionEnabled") {
            if (s.MatchKeyword("true"))       { out.occlusionEnabled = true;  out.hasOcclusion = true; }
            else if (s.MatchKeyword("false")) { out.occlusionEnabled = false; out.hasOcclusion = true; }
            else { err.line = s.Line(); err.message = "expected boolean for occlusionEnabled"; return false; }
        } else if (key == "replication") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseReplication(v, out.replication)) {
                err.line = s.Line();
                err.message = "unknown replication policy '" + v + "'";
                return false;
            }
            out.hasReplication = true;
        } else if (key == "attenuation") {
            if (!ParseAttenuation(s, out.attenuation, err)) return false;
            out.hasAttenuation = true;
        } else if (key == "loopCrossfadeMs") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.loopCrossfadeMs = static_cast<float>(f);
            out.hasLoopCrossfade = true;
        } else {
            if (!s.SkipValue(err)) return false;
        }

        if (s.Match(',')) continue;
        s.Expect('}', "to close defaults object", err);
        return err.message.empty();
    }
}

bool ParseSoundEntry(JsonScanner& s, ParsedSound& out, ParseError& err) {
    out.lineDeclared = s.Line();
    s.Expect('{', "to open sound entry", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match('}')) {
        err.line = out.lineDeclared;
        err.message = "sound entry has no fields";
        return false;
    }
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after sound entry key", err);
        if (!err.message.empty()) return false;

        if (key == "name") {
            if (!s.ParseString(out.name, err)) return false;
        } else if (key == "file") {
            if (!s.ParseString(out.file, err)) return false;
        } else if (key == "category") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseCategory(v, out.category)) {
                err.line = s.Line();
                err.message = "unknown category '" + v + "'";
                return false;
            }
            out.hasCategory = true;
        } else if (key == "priority") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParsePriority(v, out.priority)) {
                err.line = s.Line();
                err.message = "unknown priority '" + v + "'";
                return false;
            }
            out.hasPriority = true;
        } else if (key == "bus") {
            std::string v; if (!s.ParseString(v, err)) return false;
            out.busName = v; out.hasBus = true;
        } else if (key == "spatialized") {
            if (s.MatchKeyword("true"))       { out.spatialized = true;  out.hasSpatialized = true; }
            else if (s.MatchKeyword("false")) { out.spatialized = false; out.hasSpatialized = true; }
            else { err.line = s.Line(); err.message = "expected boolean for spatialized"; return false; }
        } else if (key == "looping") {
            if (s.MatchKeyword("true"))       { out.looping = true;  out.hasLooping = true; }
            else if (s.MatchKeyword("false")) { out.looping = false; out.hasLooping = true; }
            else { err.line = s.Line(); err.message = "expected boolean for looping"; return false; }
        } else if (key == "occlusionEnabled") {
            if (s.MatchKeyword("true"))       { out.occlusionEnabled = true;  out.hasOcclusion = true; }
            else if (s.MatchKeyword("false")) { out.occlusionEnabled = false; out.hasOcclusion = true; }
            else { err.line = s.Line(); err.message = "expected boolean for occlusionEnabled"; return false; }
        } else if (key == "replication") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParseReplication(v, out.replication)) {
                err.line = s.Line();
                err.message = "unknown replication policy '" + v + "'";
                return false;
            }
            out.hasReplication = true;
        } else if (key == "attenuation") {
            if (!ParseAttenuation(s, out.attenuation, err)) return false;
            out.hasAttenuation = true;
        } else if (key == "loopCrossfadeMs") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.loopCrossfadeMs = static_cast<float>(f);
            out.hasLoopCrossfade = true;
        } else if (key == "rtpc") {
            if (!ParseRtpcArray(s, out.rtpc, err)) return false;
        } else {
            if (!s.SkipValue(err)) return false;
        }

        if (s.Match(',')) continue;
        s.Expect('}', "to close sound entry", err);
        return err.message.empty();
    }
}

bool ParseGroupEntry(JsonScanner& s, ParsedGroup& out, ParseError& err) {
    out.lineDeclared = s.Line();
    s.Expect('{', "to open group entry", err);
    if (!err.message.empty()) return false;
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after group entry key", err);
        if (!err.message.empty()) return false;

        if (key == "name") {
            if (!s.ParseString(out.name, err)) return false;
        } else if (key == "policy") {
            std::string v; if (!s.ParseString(v, err)) return false;
            if (!ParsePolicy(v, out.policy)) {
                err.line = s.Line();
                err.message = "unknown group policy '" + v +
                              "' (expected random, random_no_repeat, "
                              "sequential, or by_material)";
                return false;
            }
        } else if (key == "members") {
            s.Expect('[', "to open members array", err);
            if (!err.message.empty()) return false;
            s.SkipWhitespace();
            if (!s.Match(']')) {
                while (true) {
                    std::string m;
                    if (!s.ParseString(m, err)) return false;
                    out.memberNames.push_back(std::move(m));
                    if (s.Match(',')) continue;
                    s.Expect(']', "to close members array", err);
                    if (!err.message.empty()) return false;
                    break;
                }
            }
        } else if (key == "members_by_material") {
            // Dict of {AudioMaterial-name: [variant names]} for the
            // by_material policy. Empty buckets are tolerated; missing
            // Default is tolerated; unknown material keys are a parse
            // error so typos don't silently disappear into nothing.
            s.Expect('{', "to open members_by_material object", err);
            if (!err.message.empty()) return false;
            s.SkipWhitespace();
            if (!s.Match('}')) {
                while (true) {
                    std::string materialKey;
                    if (!s.ParseString(materialKey, err)) return false;
                    s.Expect(':', "after material key", err);
                    if (!err.message.empty()) return false;

                    AudioMaterial mat;
                    if (!ParseAudioMaterial(materialKey, mat)) {
                        err.line = s.Line();
                        err.message = "unknown material '" + materialKey +
                                      "' in members_by_material (expected "
                                      "Default, Air, Glass, Wood, Drywall, "
                                      "Concrete, Metal, Curtain, or Foliage)";
                        return false;
                    }

                    s.Expect('[', "to open variant array", err);
                    if (!err.message.empty()) return false;
                    s.SkipWhitespace();
                    auto& bucket = out.membersByMaterial[
                        static_cast<size_t>(mat)];
                    if (!s.Match(']')) {
                        while (true) {
                            std::string m;
                            if (!s.ParseString(m, err)) return false;
                            bucket.push_back(std::move(m));
                            if (s.Match(',')) continue;
                            s.Expect(']', "to close variant array", err);
                            if (!err.message.empty()) return false;
                            break;
                        }
                    }

                    if (s.Match(',')) continue;
                    s.Expect('}', "to close members_by_material object", err);
                    if (!err.message.empty()) return false;
                    break;
                }
            }
        } else {
            if (!s.SkipValue(err)) return false;
        }

        if (s.Match(',')) continue;
        s.Expect('}', "to close group entry", err);
        return err.message.empty();
    }
}

bool ParseBankRoot(JsonScanner& s, ParsedBank& out, ParseError& err) {
    s.Expect('{', "to open root object", err);
    if (!err.message.empty()) return false;
    s.SkipWhitespace();
    if (s.Match('}')) return true;
    while (true) {
        std::string key;
        if (!s.ParseString(key, err)) return false;
        s.Expect(':', "after root key", err);
        if (!err.message.empty()) return false;

        if (key == "version") {
            double f; long long i; bool ii;
            if (!s.ParseNumber(f, i, ii, err)) return false;
            out.version = static_cast<int>(ii ? i : static_cast<long long>(f));
        } else if (key == "defaults") {
            if (!ParseDefaults(s, out.defaults, err)) return false;
        } else if (key == "sounds") {
            s.Expect('[', "to open sounds array", err);
            if (!err.message.empty()) return false;
            s.SkipWhitespace();
            if (!s.Match(']')) {
                while (true) {
                    out.sounds.emplace_back();
                    if (!ParseSoundEntry(s, out.sounds.back(), err)) return false;
                    if (s.Match(',')) continue;
                    s.Expect(']', "to close sounds array", err);
                    if (!err.message.empty()) return false;
                    break;
                }
            }
        } else if (key == "groups") {
            s.Expect('[', "to open groups array", err);
            if (!err.message.empty()) return false;
            s.SkipWhitespace();
            if (!s.Match(']')) {
                while (true) {
                    out.groups.emplace_back();
                    if (!ParseGroupEntry(s, out.groups.back(), err)) return false;
                    if (s.Match(',')) continue;
                    s.Expect(']', "to close groups array", err);
                    if (!err.message.empty()) return false;
                    break;
                }
            }
        } else {
            if (!s.SkipValue(err)) return false;
        }

        if (s.Match(',')) continue;
        s.Expect('}', "to close root object", err);
        return err.message.empty();
    }
}

bool ParseBankString(std::string_view json, ParsedBank& out, ParseError& err) {
    if (json.empty()) {
        err.message = "empty JSON";
        err.line = 1;
        return false;
    }
    JsonScanner s(json.data(), json.data() + json.size());
    if (!ParseBankRoot(s, out, err)) return false;
    s.SkipWhitespace();
    if (!s.AtEnd()) {
        err.line = s.Line();
        err.message = "unexpected trailing data after root object";
        return false;
    }
    if (out.version != 0 && out.version != 1) {
        err.message = "unsupported bank version " + std::to_string(out.version);
        return false;
    }
    return true;
}

} // namespace (anonymous)

// ---------------------------------------------------------------------
// Resolved bank state
// ---------------------------------------------------------------------

struct GroupRuntime {
    // For random / random_no_repeat / sequential: the flat member list.
    // For by_material: this is left empty (or unused); the
    // material-keyed buckets below are what Find consults.
    std::vector<AudioSoundId>      memberIds;
    GroupPolicy                    policy = GroupPolicy::Random;
    mutable std::atomic<int>       lastPicked{-1};
    mutable std::atomic<uint32_t>  nextIndex{0};
    // For by_material groups only. Indexed by static_cast<size_t>(
    // AudioMaterial). Empty buckets are valid (returns kInvalidSoundId
    // when the material has no variants and Default is also empty).
    std::array<std::vector<AudioSoundId>, kAudioMaterialCount> memberIdsByMaterial;
};

struct SoundBankImpl {
    std::unordered_map<std::string, AudioSoundId>     soundIds;
    std::unordered_map<std::string, GroupRuntime>     groups;
    enum class Source { None, File, String };
    Source                      lastSource = Source::None;
    std::string                 lastPath;
    std::string                 lastJson;
    SoundBankLoadOptions        lastOpts;
    mutable std::atomic<uint32_t> rng{0xA5F3D27Cu};

    static uint32_t XorshiftAdvance(std::atomic<uint32_t>& s) noexcept {
        uint32_t x = s.load(std::memory_order_relaxed);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (x == 0) x = 0xA5F3D27Cu;
        s.store(x, std::memory_order_relaxed);
        return x;
    }
};

struct SoundBank::Impl : SoundBankImpl {};

// ---------------------------------------------------------------------
// Public class
// ---------------------------------------------------------------------

SoundBank::SoundBank() : impl_(std::make_unique<Impl>()) {}
SoundBank::~SoundBank() = default;
SoundBank::SoundBank(SoundBank&&) noexcept = default;
SoundBank& SoundBank::operator=(SoundBank&&) noexcept = default;

uint32_t SoundBank::SoundCount() const noexcept {
    return static_cast<uint32_t>(impl_->soundIds.size());
}
uint32_t SoundBank::GroupCount() const noexcept {
    return static_cast<uint32_t>(impl_->groups.size());
}

bool SoundBank::Contains(std::string_view name) const noexcept {
    const std::string key(name);
    return impl_->soundIds.find(key) != impl_->soundIds.end()
        || impl_->groups.find(key)   != impl_->groups.end();
}

// Internal: pick one variant from a non-empty flat list using the
// bank's xorshift RNG. Used both by the no-material Find (for
// random/random_no_repeat/sequential) and by Find(name, material)
// for within-bucket variant selection in by_material groups.
namespace {
inline AudioSoundId PickRandomFromList(
        const std::vector<AudioSoundId>& list,
        std::atomic<uint32_t>& rng) noexcept {
    const uint32_t n = static_cast<uint32_t>(list.size());
    if (n == 0) return kInvalidSoundId;
    if (n == 1) return list[0];
    const uint32_t r = SoundBankImpl::XorshiftAdvance(rng);
    return list[r % n];
}
} // namespace

AudioSoundId SoundBank::Find(std::string_view name) const noexcept {
    const std::string key(name);
    if (auto it = impl_->soundIds.find(key); it != impl_->soundIds.end()) {
        return it->second;
    }
    auto gi = impl_->groups.find(key);
    if (gi == impl_->groups.end()) return kInvalidSoundId;
    const GroupRuntime& g = gi->second;

    // by_material groups called without a material: fall through to
    // the Default bucket. If Default is also empty, this is
    // kInvalidSoundId — designers can either handle that or supply
    // a material via Find(name, material).
    if (g.policy == GroupPolicy::ByMaterial) {
        const auto& def = g.memberIdsByMaterial[
            static_cast<size_t>(AudioMaterial::Default)];
        return PickRandomFromList(def, impl_->rng);
    }

    const uint32_t n = static_cast<uint32_t>(g.memberIds.size());
    if (n == 0) return kInvalidSoundId;
    switch (g.policy) {
    case GroupPolicy::Sequential: {
        const uint32_t i = g.nextIndex.fetch_add(1, std::memory_order_relaxed) % n;
        return g.memberIds[i];
    }
    case GroupPolicy::Random: {
        const uint32_t r = Impl::XorshiftAdvance(impl_->rng);
        return g.memberIds[r % n];
    }
    case GroupPolicy::RandomNoRepeat: {
        if (n == 1) return g.memberIds[0];
        const uint32_t r = Impl::XorshiftAdvance(impl_->rng);
        int last = g.lastPicked.load(std::memory_order_relaxed);
        uint32_t pick = r % n;
        if (static_cast<int>(pick) == last) pick = (pick + 1) % n;
        g.lastPicked.store(static_cast<int>(pick), std::memory_order_relaxed);
        return g.memberIds[pick];
    }
    case GroupPolicy::ByMaterial:
        // Handled above.
        return kInvalidSoundId;
    }
    return kInvalidSoundId;
}

AudioSoundId SoundBank::Find(std::string_view name,
                              AudioMaterial    material) const noexcept {
    const std::string key(name);

    // Sound names: material is ignored — a direct sound has no
    // material-keyed variants.
    if (auto it = impl_->soundIds.find(key); it != impl_->soundIds.end()) {
        return it->second;
    }

    auto gi = impl_->groups.find(key);
    if (gi == impl_->groups.end()) return kInvalidSoundId;
    const GroupRuntime& g = gi->second;

    // Non-by_material groups: material is ignored, behavior matches
    // the no-material overload.
    if (g.policy != GroupPolicy::ByMaterial) {
        return Find(name);
    }

    // by_material: try the requested material's bucket first. If
    // empty, fall through to Default. If both are empty, return
    // kInvalidSoundId (the lenient rule — silent miss, log on the
    // caller side if it matters).
    const size_t mi = static_cast<size_t>(material);
    if (mi < kAudioMaterialCount) {
        const auto& bucket = g.memberIdsByMaterial[mi];
        if (!bucket.empty()) {
            return PickRandomFromList(bucket, impl_->rng);
        }
    }
    const auto& def = g.memberIdsByMaterial[
        static_cast<size_t>(AudioMaterial::Default)];
    return PickRandomFromList(def, impl_->rng);
}

void SoundBank::Clear() noexcept {
    impl_->soundIds.clear();
    impl_->groups.clear();
    impl_->lastSource = Impl::Source::None;
    impl_->lastPath.clear();
    impl_->lastJson.clear();
}

namespace {

// Resolve final SoundDefinition for an entry by overlaying defaults
// with per-sound overrides.
SoundDefinition Resolve(const ParsedSound&    s,
                          const ParsedDefaults& d,
                          AudioSoundId          id,
                          BusId                 resolvedBus) {
    SoundDefinition def;
    def.soundId      = id;
    def.category     = s.hasCategory     ? s.category     : d.category;
    def.priority     = s.hasPriority     ? s.priority     : d.priority;
    def.spatialized  = s.hasSpatialized  ? s.spatialized  : d.spatialized;
    def.looping      = s.hasLooping      ? s.looping      : d.looping;
    def.occlusionEnabled = s.hasOcclusion ? s.occlusionEnabled : d.occlusionEnabled;
    def.defaultReplicationPolicy = s.hasReplication ? s.replication : d.replication;
    def.attenuation  = s.hasAttenuation  ? s.attenuation  : d.attenuation;
    def.loopCrossfadeMs = s.hasLoopCrossfade ? s.loopCrossfadeMs : d.loopCrossfadeMs;
    def.targetBus    = resolvedBus;
    return def;
}

AudioFileFormat InferFormat(std::string_view path) {
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
    auto endsWith = [&](std::string_view suffix) {
        if (path.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); ++i) {
            if (lower(path[path.size() - suffix.size() + i]) != suffix[i]) return false;
        }
        return true;
    };
    if (endsWith(".wav"))  return AudioFileFormat::Wav;
    if (endsWith(".ogg"))  return AudioFileFormat::OggVorbis;
    if (endsWith(".flac")) return AudioFileFormat::Flac;
    return AudioFileFormat::Auto;
}

SoundBankLoadResult ResolveAndRegister(SoundBankImpl&            impl,
                                         AudioRuntime&               runtime,
                                         const ParsedBank&           parsed,
                                         const SoundBankLoadOptions& opts,
                                         const std::filesystem::path& baseDir) {
    SoundBankLoadResult r;
    r.success = false;

    auto busResolver = opts.busResolver
        ? opts.busResolver
        : std::function<BusId(std::string_view)>(DefaultBusResolver);

    // Pass 1: validate names + collisions.
    std::unordered_set<std::string> declaredNames;
    declaredNames.reserve(parsed.sounds.size() + parsed.groups.size());
    std::unordered_map<uint32_t, std::string> hashToName;
    hashToName.reserve(parsed.sounds.size());
    for (const auto& s : parsed.sounds) {
        if (s.name.empty()) {
            r.errorLine = s.lineDeclared;
            r.errorMessage = "sound entry has empty 'name'";
            return r;
        }
        if (!declaredNames.insert(s.name).second) {
            r.errorLine = s.lineDeclared;
            r.errorMessage = "duplicate sound name '" + s.name + "'";
            return r;
        }
        const AudioSoundId h = HashSoundName(s.name);
        if (auto it = hashToName.find(h); it != hashToName.end()) {
            r.errorLine = s.lineDeclared;
            r.errorMessage = "hash collision between '" + s.name +
                             "' and '" + it->second +
                             "'; rename one of them";
            return r;
        }
        hashToName.emplace(h, s.name);
    }
    for (const auto& g : parsed.groups) {
        if (g.name.empty()) {
            r.errorLine = g.lineDeclared;
            r.errorMessage = "group entry has empty 'name'";
            return r;
        }
        if (!declaredNames.insert(g.name).second) {
            r.errorLine = g.lineDeclared;
            r.errorMessage = "name '" + g.name +
                             "' is declared as both sound and group";
            return r;
        }
        if (g.policy == GroupPolicy::ByMaterial) {
            // By-material groups must have at least one non-empty
            // bucket somewhere. Individual empty buckets are tolerated;
            // missing Default is tolerated (Find returns kInvalidSoundId
            // when the material has no bucket and Default is also
            // empty — designers can handle that silently or fall back).
            // What we won't accept is a group with literally zero
            // variants in any material, which is just a typo.
            bool anyVariants = false;
            for (const auto& bucket : g.membersByMaterial) {
                if (!bucket.empty()) { anyVariants = true; break; }
            }
            if (!anyVariants) {
                r.errorLine = g.lineDeclared;
                r.errorMessage = "by_material group '" + g.name +
                                 "' has no variants in any material "
                                 "(every bucket in members_by_material "
                                 "is empty)";
                return r;
            }
            if (opts.validateReferences) {
                for (const auto& bucket : g.membersByMaterial) {
                    for (const auto& m : bucket) {
                        bool found = false;
                        for (const auto& s : parsed.sounds) {
                            if (s.name == m) { found = true; break; }
                        }
                        if (!found) {
                            r.errorLine = g.lineDeclared;
                            r.errorMessage = "by_material group '" + g.name +
                                             "' references unknown sound '" +
                                             m + "'";
                            return r;
                        }
                    }
                }
            }
        } else {
            if (g.memberNames.empty()) {
                r.errorLine = g.lineDeclared;
                r.errorMessage = "group '" + g.name + "' has no members";
                return r;
            }
            if (opts.validateReferences) {
                for (const auto& m : g.memberNames) {
                    bool found = false;
                    for (const auto& s : parsed.sounds) if (s.name == m) { found = true; break; }
                    if (!found) {
                        r.errorLine = g.lineDeclared;
                        r.errorMessage = "group '" + g.name +
                                         "' member '" + m +
                                         "' is not a declared sound";
                        return r;
                    }
                }
            }
        }
    }

    // Pass 2: load file bytes + register with runtime.
    auto fileLoader = opts.fileLoader;
    std::vector<uint8_t> fileBuf;
    fileBuf.reserve(64 * 1024);

    impl.soundIds.reserve(parsed.sounds.size());

    for (const auto& s : parsed.sounds) {
        const AudioSoundId id = HashSoundName(s.name);

        // Resolve bus name now so we can fail early with a useful
        // message rather than silently defaulting to master.
        const std::string& busName = s.hasBus ? s.busName : parsed.defaults.busName;
        BusId bus = busResolver(busName);
        if (bus == kInvalidBusId && opts.validateReferences) {
            // Allow "master" via default resolver fallback even when
            // the host didn't supply one. busResolver above already
            // routed master through DefaultBusResolver.
            r.errorLine = s.lineDeclared;
            r.errorMessage = "sound '" + s.name +
                             "' references unknown bus '" + busName + "'";
            return r;
        }

        // Load file bytes — but only if a file was specified. An
        // entry without a `file` field is assumed to have been
        // pre-registered with the runtime under the same hashed id
        // (programmatic sounds, debug tones, runtime-generated audio).
        // The bank only registers the SoundDefinition in that case.
        if (!s.file.empty()) {
            fileBuf.clear();
            bool ok = false;
            if (fileLoader) {
                ok = fileLoader(s.file, fileBuf);
            } else {
                ok = DefaultFileLoader(s.file, fileBuf, baseDir);
            }
            if (!ok) {
                r.errorLine = s.lineDeclared;
                r.errorMessage = "sound '" + s.name +
                                 "': failed to load file '" + s.file + "'";
                return r;
            }

            const AudioFileFormat fmt = InferFormat(s.file);
            const AudioResult rc = runtime.RegisterSoundFromMemory(
                id,
                std::span<const uint8_t>(fileBuf.data(), fileBuf.size()),
                fmt);
            if (rc != AudioResult::Success) {
                r.errorLine = s.lineDeclared;
                r.errorMessage = "sound '" + s.name +
                                 "': RegisterSoundFromMemory failed (decoder may be disabled at build time)";
                return r;
            }
        }

        const SoundDefinition def = Resolve(s, parsed.defaults, id, bus);
        const AudioResult rd = runtime.RegisterSoundDefinition(def);
        if (rd != AudioResult::Success) {
            r.errorLine = s.lineDeclared;
            r.errorMessage = "sound '" + s.name +
                             "': RegisterSoundDefinition failed";
            return r;
        }

        // Translate authored rtpc bindings to runtime SetSoundRtpc
        // calls. Each binding hashes its parameter name and dispatches.
        for (const auto& pb : s.rtpc) {
            SoundRtpcBinding rb;
            rb.paramId       = HashParameterName(pb.paramName);
            rb.target        = pb.target;
            rb.curve         = pb.curve;
            rb.curveExponent = pb.curveExponent;
            rb.minValue      = pb.minValue;
            rb.maxValue      = pb.maxValue;
            rb.minOutput     = pb.minOutput;
            rb.maxOutput     = pb.maxOutput;
            rb.smoothingMs   = pb.smoothingMs;
            const AudioResult rrt = runtime.SetSoundRtpc(id, rb);
            if (rrt != AudioResult::Success) {
                r.errorLine = pb.lineDeclared;
                r.errorMessage = "sound '" + s.name +
                                 "': rtpc binding for parameter '" +
                                 pb.paramName + "' rejected (" +
                                 (rrt == AudioResult::BudgetExceeded
                                      ? "budget exceeded — bump AudioConfig::maxSoundRtpcBindings"
                                      : "invalid argument — check min/max range, NaN, or bad enum") +
                                 ")";
                return r;
            }
        }

        impl.soundIds[s.name] = id;
        ++r.soundsLoaded;
    }

    // Pass 3: build groups now that sounds are registered.
    impl.groups.reserve(parsed.groups.size());
    for (const auto& g : parsed.groups) {
        GroupRuntime& rt = impl.groups[g.name];
        rt.policy = g.policy;
        if (g.policy == GroupPolicy::ByMaterial) {
            for (size_t i = 0; i < kAudioMaterialCount; ++i) {
                rt.memberIdsByMaterial[i].reserve(g.membersByMaterial[i].size());
                for (const auto& m : g.membersByMaterial[i]) {
                    auto it = impl.soundIds.find(m);
                    if (it == impl.soundIds.end()) {
                        // Already validated in pass 1 if the option
                        // was set; if it wasn't, skip silently.
                        continue;
                    }
                    rt.memberIdsByMaterial[i].push_back(it->second);
                }
            }
        } else {
            rt.memberIds.reserve(g.memberNames.size());
            for (const auto& m : g.memberNames) {
                auto it = impl.soundIds.find(m);
                if (it == impl.soundIds.end()) {
                    continue;
                }
                rt.memberIds.push_back(it->second);
            }
        }
        ++r.groupsLoaded;
    }

    r.success = true;
    return r;
}

} // namespace (anonymous)

SoundBankLoadResult SoundBank::LoadFromJsonFile(AudioRuntime&               runtime,
                                                  std::string_view            jsonPath,
                                                  const SoundBankLoadOptions& opts) {
    SoundBankLoadResult r;
    std::ifstream in((std::string(jsonPath)), std::ios::binary);
    if (!in) {
        r.errorMessage = "could not open JSON file: " + std::string(jsonPath);
        return r;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();

    impl_->lastSource = Impl::Source::File;
    impl_->lastPath   = std::string(jsonPath);
    impl_->lastJson.clear();
    impl_->lastOpts   = opts;
    impl_->soundIds.clear();
    impl_->groups.clear();

    ParsedBank parsed;
    ParseError err;
    if (!ParseBankString(content, parsed, err)) {
        r.errorLine    = err.line;
        r.errorMessage = err.message;
        return r;
    }
    const std::filesystem::path baseDir =
        std::filesystem::path(std::string(jsonPath)).parent_path();
    return ResolveAndRegister(*impl_, runtime, parsed, opts, baseDir);
}

SoundBankLoadResult SoundBank::LoadFromJsonString(AudioRuntime&               runtime,
                                                    std::string_view            json,
                                                    const SoundBankLoadOptions& opts) {
    impl_->lastSource = Impl::Source::String;
    impl_->lastPath.clear();
    impl_->lastJson   = std::string(json);
    impl_->lastOpts   = opts;
    impl_->soundIds.clear();
    impl_->groups.clear();

    ParsedBank parsed;
    ParseError err;
    SoundBankLoadResult r;
    if (!ParseBankString(json, parsed, err)) {
        r.errorLine    = err.line;
        r.errorMessage = err.message;
        return r;
    }
    return ResolveAndRegister(*impl_, runtime, parsed, opts, {});
}

SoundBankLoadResult SoundBank::Reload(AudioRuntime& runtime) {
    // Snapshot first: LoadFromJson{File,String} mutates impl_->lastJson
    // / lastPath, and the string_view we pass in must outlive that
    // mutation.
    if (impl_->lastSource == Impl::Source::File) {
        const std::string path = impl_->lastPath;
        const SoundBankLoadOptions opts = impl_->lastOpts;
        return LoadFromJsonFile(runtime, path, opts);
    }
    if (impl_->lastSource == Impl::Source::String) {
        const std::string snap = impl_->lastJson;
        const SoundBankLoadOptions opts = impl_->lastOpts;
        return LoadFromJsonString(runtime, snap, opts);
    }
    SoundBankLoadResult r;
    r.errorMessage = "Reload called before any successful Load*";
    return r;
}

} // namespace audio
