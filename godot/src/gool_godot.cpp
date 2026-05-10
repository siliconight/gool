// godot/src/gool_godot.cpp
//
// Godot 4 GDExtension binding for the gool audio engine.
//
// Exposes the runtime, music channel, and voice-chat pieces to
// GDScript. Prefab Nodes under godot/addons/gool/prefabs/ wrap
// these to provide drag-and-drop scene nodes (AudioEmitter3D,
// VoiceChatPlayer, MusicStateController, ReverbZone,
// FootstepSurfacePlayer).
//
// Threading: all engine calls happen on Godot's main thread. The
// audio engine's render thread runs inside the miniaudio backend
// and never touches Godot state.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/audio_file_format.h"
#include "audio_engine/bus_config_loader.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/gpak.h"
#include "audio_engine/miniaudio_backend.h"
#include "audio_engine/music_channel.h"
#include "audio_engine/sound_bank.h"
#include "audio_engine/version.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <atomic>
#include <memory>

using namespace godot;

namespace gool {

static inline audio::Vec3 V3(const Vector3& v) {
    return audio::Vec3{static_cast<float>(v.x),
                        static_cast<float>(v.y),
                        static_cast<float>(v.z)};
}

static audio::AudioSoundId HashName(const String& name) {
    const auto utf8 = name.utf8();
    return audio::HashSoundName(std::string_view(utf8.get_data(),
                                                    utf8.length()));
}

static audio::AudioParameterId HashParam(const String& name) {
    const auto utf8 = name.utf8();
    return audio::HashParameterName(std::string_view(utf8.get_data(),
                                                       utf8.length()));
}

// =====================================================================
// GoolAudioRuntime
// =====================================================================
//
// Singleton-style Node. Add as autoload at /root/Gool. Init once;
// every other prefab calls into it.

class GoolAudioRuntime : public Node {
    GDCLASS(GoolAudioRuntime, Node);

public:
    GoolAudioRuntime() = default;
    ~GoolAudioRuntime() override { shutdown(); }

    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("init", "sample_rate", "buffer_size"),
                              &GoolAudioRuntime::init, DEFVAL(48000), DEFVAL(512));
        // Richer init that takes a JSON bus-config document. Lets
        // GDScript projects ship multi-tier sidechain ducking and
        // per-bus effect chains without a binding-method per knob.
        // Schema documented in include/audio_engine/bus_config_loader.h.
        // Returns true on success, false on parse error or runtime
        // init failure (errors are pushed to the engine error log).
        ClassDB::bind_method(D_METHOD("init_with_config",
                                       "config_json",
                                       "sample_rate",
                                       "buffer_size"),
                              &GoolAudioRuntime::init_with_config,
                              DEFVAL(48000), DEFVAL(512));
        ClassDB::bind_method(D_METHOD("shutdown"), &GoolAudioRuntime::shutdown);
        ClassDB::bind_method(D_METHOD("update", "delta"),
                              &GoolAudioRuntime::update);

        // Version metadata. Returns a Dictionary with keys
        // "major", "minor", "patch", "full", "commit". Useful for
        // debug overlays and crash reports.
        ClassDB::bind_method(D_METHOD("get_version"),
                              &GoolAudioRuntime::get_version);

        ClassDB::bind_method(D_METHOD("set_listener_transform",
                                       "position", "forward", "velocity"),
                              &GoolAudioRuntime::set_listener_transform);

        // Sound registration.
        ClassDB::bind_method(D_METHOD("register_pcm_sound",
                                       "name", "samples",
                                       "sample_rate", "channels"),
                              &GoolAudioRuntime::register_pcm_sound,
                              DEFVAL(48000), DEFVAL(1));
        // Load a sound file (.wav / .ogg / .flac / .opus) from any
        // Godot-readable path (including res://) and register it as
        // a one-shot PCM asset. The binding reads the raw bytes via
        // FileAccess and routes through the engine's
        // RegisterSoundFromMemory path so it works in PCK-packaged
        // builds, not just editor mode. Returns the AudioSoundId on
        // success, 0 on failure (file missing, format unsupported,
        // decoder compiled out — see CMake AUDIO_ENGINE_DECODERS_*).
        ClassDB::bind_method(D_METHOD("register_sound_from_file",
                                       "name", "path"),
                              &GoolAudioRuntime::register_sound_from_file);
        // Same as register_sound_from_file but takes already-loaded
        // bytes (e.g. from a Resource pack the host manages itself).
        // format_hint matches AudioFileFormat: 0=Auto, 1=Wav,
        // 2=OggVorbis, 3=Flac, 4=Opus. Auto sniffs by magic bytes.
        ClassDB::bind_method(D_METHOD("register_sound_from_bytes",
                                       "name", "bytes", "format_hint"),
                              &GoolAudioRuntime::register_sound_from_bytes,
                              DEFVAL(0));
        ClassDB::bind_method(D_METHOD("register_sound_definition",
                                       "name", "spatialized", "looping",
                                       "min_distance", "max_distance",
                                       "loop_crossfade_ms",
                                       "category", "target_bus_name"),
                              &GoolAudioRuntime::register_sound_definition,
                              DEFVAL(true), DEFVAL(false),
                              DEFVAL(1.0), DEFVAL(50.0), DEFVAL(0.0),
                              DEFVAL(0), DEFVAL(String()));
        // Bus-name → BusId resolver. Returns -1 if no bus matches.
        // Useful for hosts that need to call other BusId-taking
        // bindings (set_bus_gain_db, set_effect_parameter) by name.
        ClassDB::bind_method(D_METHOD("find_bus_id_by_name", "name"),
                              &GoolAudioRuntime::find_bus_id_by_name);
        ClassDB::bind_method(D_METHOD("load_sound_bank_from_json",
                                       "json_string", "gpak_path"),
                              &GoolAudioRuntime::load_sound_bank_from_json,
                              DEFVAL(""));

        // Playback (one-shot + handle-based).
        ClassDB::bind_method(D_METHOD("play_sound_at_location",
                                       "name", "position"),
                              &GoolAudioRuntime::play_sound_at_location);
        ClassDB::bind_method(D_METHOD("create_emitter",
                                       "name", "position",
                                       "looping", "fade_in_ms"),
                              &GoolAudioRuntime::create_emitter,
                              DEFVAL(false), DEFVAL(0.0));
        ClassDB::bind_method(D_METHOD("destroy_emitter",
                                       "handle_packed", "fade_out_ms"),
                              &GoolAudioRuntime::destroy_emitter,
                              DEFVAL(0.0));
        ClassDB::bind_method(D_METHOD("set_emitter_transform",
                                       "handle_packed",
                                       "position", "forward", "velocity"),
                              &GoolAudioRuntime::set_emitter_transform);
        ClassDB::bind_method(D_METHOD("set_emitter_playback_speed",
                                       "handle_packed", "speed", "smoothing_ms"),
                              &GoolAudioRuntime::set_emitter_playback_speed,
                              DEFVAL(50.0));

        // ---- Replication / multiplayer ----
        // The host's networking layer uses these to forward events
        // across the wire. See docs/replication_patterns.md for the
        // three supported patterns (server-authoritative,
        // client-predicted, client-authoritative).
        ClassDB::bind_method(D_METHOD("on_tick_advanced",
                                       "simulation_tick", "server_time_ms"),
                              &GoolAudioRuntime::on_tick_advanced);
        ClassDB::bind_method(D_METHOD("submit_event_local",
                                       "sound_name", "position",
                                       "prediction_id", "priority", "timestamp_ms"),
                              &GoolAudioRuntime::submit_event_local,
                              DEFVAL(0), DEFVAL(128), DEFVAL(0));
        ClassDB::bind_method(D_METHOD("submit_replicated_event",
                                       "sound_name", "position",
                                       "simulation_tick", "server_time_ms",
                                       "priority"),
                              &GoolAudioRuntime::submit_replicated_event,
                              DEFVAL(0), DEFVAL(0), DEFVAL(128));
        ClassDB::bind_method(D_METHOD("cancel_predicted_event",
                                       "prediction_id", "fade_out_ms"),
                              &GoolAudioRuntime::cancel_predicted_event,
                              DEFVAL(50.0));
        ClassDB::bind_method(D_METHOD("update_replicated_transform",
                                       "handle_packed",
                                       "position", "forward", "velocity",
                                       "simulation_tick"),
                              &GoolAudioRuntime::update_replicated_transform);
        ClassDB::bind_method(D_METHOD("make_prediction_id"),
                              &GoolAudioRuntime::make_prediction_id);

        // Voice chat (multiplayer).
        ClassDB::bind_method(D_METHOD("register_voice_source", "player_id"),
                              &GoolAudioRuntime::register_voice_source);
        ClassDB::bind_method(D_METHOD("submit_voice_packet",
                                       "player_id", "bytes",
                                       "sequence_number",
                                       "send_timestamp_ms",
                                       "arrival_timestamp_ms"),
                              &GoolAudioRuntime::submit_voice_packet,
                              DEFVAL(-1));
        ClassDB::bind_method(D_METHOD("get_voice_jitter_ms", "player_id"),
                              &GoolAudioRuntime::get_voice_jitter_ms);
        ClassDB::bind_method(D_METHOD("get_voice_packet_loss_ratio", "player_id"),
                              &GoolAudioRuntime::get_voice_packet_loss_ratio);

        // Global (RTPC) parameter store. The GDScript autoload
        // wraps these as Gool.set_rtpc / Gool.get_rtpc.
        ClassDB::bind_method(D_METHOD("set_global_parameter", "name", "value"),
                              &GoolAudioRuntime::set_global_parameter);
        ClassDB::bind_method(D_METHOD("get_global_parameter", "name"),
                              &GoolAudioRuntime::get_global_parameter);
        ClassDB::bind_method(D_METHOD("has_global_parameter", "name"),
                              &GoolAudioRuntime::has_global_parameter);
        ClassDB::bind_method(D_METHOD("clear_global_parameter", "name"),
                              &GoolAudioRuntime::clear_global_parameter);
        ClassDB::bind_method(D_METHOD("global_parameter_count"),
                              &GoolAudioRuntime::global_parameter_count);

        // Sound-level RTPC bindings (multi-target, multi-curve).
        // GDScript autoload wraps these as Gool.bind_volume_rtpc /
        // bind_pitch_rtpc / bind_lowpass_rtpc / bind_rtpc (advanced).
        ClassDB::bind_method(D_METHOD("set_sound_rtpc",
                                       "sound_name", "param_name", "target", "curve",
                                       "min_value", "max_value",
                                       "min_output", "max_output",
                                       "curve_exponent", "smoothing_ms"),
                              &GoolAudioRuntime::set_sound_rtpc,
                              DEFVAL("linear"), DEFVAL(2.0), DEFVAL(50.0));
        ClassDB::bind_method(D_METHOD("clear_sound_rtpc", "sound_name", "target"),
                              &GoolAudioRuntime::clear_sound_rtpc);
        ClassDB::bind_method(D_METHOD("clear_all_sound_rtpc", "sound_name"),
                              &GoolAudioRuntime::clear_all_sound_rtpc);
        ClassDB::bind_method(D_METHOD("sound_rtpc_binding_count"),
                              &GoolAudioRuntime::sound_rtpc_binding_count);

        // Misc.
        ClassDB::bind_method(D_METHOD("hash_sound_name", "name"),
                              &GoolAudioRuntime::hash_sound_name);
        ClassDB::bind_method(D_METHOD("hash_parameter_name", "name"),
                              &GoolAudioRuntime::hash_parameter_name);
        ClassDB::bind_method(D_METHOD("is_initialized"),
                              &GoolAudioRuntime::is_initialized);

        // Signals.
        ADD_SIGNAL(MethodInfo("ready_to_play"));
        ADD_SIGNAL(MethodInfo("voice_quality_warning",
                                PropertyInfo(Variant::INT, "player_id"),
                                PropertyInfo(Variant::FLOAT, "jitter_ms"),
                                PropertyInfo(Variant::FLOAT, "loss_ratio")));
    }

    bool init(int sample_rate, int buffer_size) {
        if (initialized_) return true;
        runtime_ = std::make_unique<audio::AudioRuntime>();

        audio::AudioConfig cfg;
        cfg.sampleRate = static_cast<uint32_t>(sample_rate);
        cfg.bufferSize = static_cast<uint32_t>(buffer_size);
        cfg.outputMode = audio::AudioOutputMode::Stereo;

        audio::AudioRuntimeDependencies deps;
        deps.backend = std::make_unique<audio::MiniaudioBackend>();

        const auto rc = runtime_->Initialize(cfg, std::move(deps));
        if (rc != audio::AudioResult::Success) {
            UtilityFunctions::push_error("GoolAudioRuntime: Initialize failed");
            runtime_.reset();
            return false;
        }
        audio::AudioListener listener;
        runtime_->SetListener(listener);
        bank_ = std::make_unique<audio::SoundBank>();
        initialized_ = true;
        emit_signal("ready_to_play");
        return true;
    }

    // Same as init() but takes a JSON config document describing the
    // bus graph + category routing. Empty `config_json` → behaves
    // identically to init(sample_rate, buffer_size). On parse error
    // the engine push_error()s the line + message and returns false
    // without modifying state — caller can retry with a corrected
    // config.
    bool init_with_config(const String& config_json,
                            int sample_rate, int buffer_size) {
        if (initialized_) return true;

        audio::AudioConfig cfg;
        cfg.sampleRate = static_cast<uint32_t>(sample_rate);
        cfg.bufferSize = static_cast<uint32_t>(buffer_size);
        cfg.outputMode = audio::AudioOutputMode::Stereo;

        if (config_json.length() > 0) {
            const auto utf8 = config_json.utf8();
            const auto pr = audio::BusConfigLoader::ParseFromJson(
                std::string_view(utf8.get_data(), utf8.length()));
            if (!pr.ok) {
                UtilityFunctions::push_error(
                    String("GoolAudioRuntime: bus config parse failed at line ")
                    + String::num_int64(pr.errorLine)
                    + String(": ") + pr.error.c_str());
                return false;
            }
            cfg.busGraph = pr.busGraph;
        }

        runtime_ = std::make_unique<audio::AudioRuntime>();
        audio::AudioRuntimeDependencies deps;
        deps.backend = std::make_unique<audio::MiniaudioBackend>();

        const auto rc = runtime_->Initialize(cfg, std::move(deps));
        if (rc != audio::AudioResult::Success) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: Initialize failed (with bus config)");
            runtime_.reset();
            return false;
        }
        audio::AudioListener listener;
        runtime_->SetListener(listener);
        bank_ = std::make_unique<audio::SoundBank>();
        initialized_ = true;
        emit_signal("ready_to_play");
        return true;
    }

    void shutdown() {
        if (!runtime_) return;
        bank_.reset();
        pak_.reset();
        runtime_->Shutdown();
        runtime_.reset();
        initialized_ = false;
    }

    // Returns the engine's version as a Godot Dictionary:
    //   { "major": int, "minor": int, "patch": int,
    //     "full":  String, "commit": String }
    // Available before init() — the version is compile-time, no
    // runtime state needed.
    Dictionary get_version() const {
        const auto v = audio::GetVersion();
        Dictionary d;
        d["major"]  = v.major;
        d["minor"]  = v.minor;
        d["patch"]  = v.patch;
        d["full"]   = String(v.full);
        d["commit"] = String(v.commit);
        return d;
    }

    void update(double delta) {
        if (!runtime_) return;
        runtime_->Update(static_cast<float>(delta));
    }

    void set_listener_transform(const Vector3& position,
                                 const Vector3& forward,
                                 const Vector3& velocity) {
        if (!runtime_) return;
        audio::AudioListener listener;
        listener.position = V3(position);
        listener.forward  = V3(forward);
        listener.velocity = V3(velocity);
        runtime_->SetListener(listener);
    }

    int64_t register_pcm_sound(const String&             name,
                                 const PackedFloat32Array& samples,
                                 int                       sample_rate,
                                 int                       channels) {
        if (!runtime_) return 0;
        const audio::AudioSoundId id = HashName(name);
        runtime_->RegisterPcmSound(
            id,
            std::span<const float>(samples.ptr(), samples.size()),
            static_cast<uint32_t>(sample_rate),
            static_cast<uint32_t>(channels));
        return static_cast<int64_t>(id);
    }

    // Maps an AudioResult to a human-readable diagnostic string.
    // Used by the file-load bindings to push_error with helpful
    // context when registration fails.
    static const char* AudioResultText(audio::AudioResult rc) noexcept {
        switch (rc) {
            case audio::AudioResult::Success:           return "ok";
            case audio::AudioResult::InvalidArgument:   return "invalid argument";
            case audio::AudioResult::AssetMissing:      return "asset missing";
            case audio::AudioResult::Unsupported:       return "format decoder not compiled in (set AUDIO_ENGINE_DECODERS_* in CMake)";
            case audio::AudioResult::IoError:           return "I/O error reading file";
            case audio::AudioResult::NotInitialized:    return "runtime not initialized";
            case audio::AudioResult::AlreadyInitialized:return "already initialized";
            case audio::AudioResult::BudgetExceeded:    return "budget exceeded";
            case audio::AudioResult::QueueFull:         return "command queue full";
            case audio::AudioResult::InternalError:     return "internal error";
            case audio::AudioResult::InvalidHandle:     return "invalid handle";
            case audio::AudioResult::BackendUnavailable:return "backend unavailable";
        }
        return "unknown";
    }

    int64_t register_sound_from_file(const String& name, const String& path) {
        if (!runtime_) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: register_sound_from_file called before init");
            return 0;
        }
        // Read bytes via Godot's FileAccess so res:// works in
        // packaged builds. Real-fs paths work too (FileAccess
        // accepts both res:// and absolute paths).
        Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
        if (f.is_null()) {
            UtilityFunctions::push_error(
                String("GoolAudioRuntime: register_sound_from_file failed to open '")
                + path + String("' (FileAccess error ")
                + String::num_int64(static_cast<int64_t>(FileAccess::get_open_error()))
                + String(")"));
            return 0;
        }
        const int64_t len = static_cast<int64_t>(f->get_length());
        if (len <= 0) {
            UtilityFunctions::push_error(
                String("GoolAudioRuntime: register_sound_from_file '")
                + path + String("' is empty"));
            return 0;
        }
        PackedByteArray bytes = f->get_buffer(len);
        f->close();
        return register_sound_from_bytes(name, bytes,
                                           static_cast<int>(audio::AudioFileFormat::Auto));
    }

    int64_t register_sound_from_bytes(const String&            name,
                                        const PackedByteArray&   bytes,
                                        int                       format_hint) {
        if (!runtime_) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: register_sound_from_bytes called before init");
            return 0;
        }
        if (bytes.size() == 0) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: register_sound_from_bytes received empty buffer");
            return 0;
        }
        // Clamp format hint to enum range; out-of-range maps to Auto.
        audio::AudioFileFormat fmt = audio::AudioFileFormat::Auto;
        if (format_hint >= 0 &&
            format_hint <= static_cast<int>(audio::AudioFileFormat::Opus)) {
            fmt = static_cast<audio::AudioFileFormat>(format_hint);
        }
        const audio::AudioSoundId id = HashName(name);
        const auto rc = runtime_->RegisterSoundFromMemory(
            id,
            std::span<const uint8_t>(bytes.ptr(),
                                       static_cast<size_t>(bytes.size())),
            fmt);
        if (rc != audio::AudioResult::Success) {
            UtilityFunctions::push_error(
                String("GoolAudioRuntime: register_sound failed for '")
                + name + String("': ")
                + String(AudioResultText(rc)));
            return 0;
        }
        return static_cast<int64_t>(id);
    }

    void register_sound_definition(const String& name,
                                    bool spatialized, bool looping,
                                    double min_distance, double max_distance,
                                    double loop_crossfade_ms,
                                    int category,
                                    const String& target_bus_name) {
        if (!runtime_) return;
        audio::SoundDefinition def;
        def.soundId           = HashName(name);
        def.spatialized       = spatialized;
        def.looping           = looping;
        // Category determines which bus the runtime routes to when
        // targetBus stays at kInvalidBusId. Hosts that don't pass
        // category get SFX (the most common default for game-audio
        // events). Out-of-range values clamp to SFX.
        if (category >= 0 && category < static_cast<int>(audio::AudioCategory::Count)) {
            def.category = static_cast<audio::AudioCategory>(category);
        } else {
            def.category = audio::AudioCategory::SFX;
        }
        // target_bus_name overrides category routing when non-empty.
        // Empty (the default) → leave targetBus at kInvalidBusId so
        // the runtime falls through to the configured category map.
        // Unknown bus names are reported as a non-fatal warning;
        // registration still succeeds with category routing.
        if (target_bus_name.length() > 0) {
            const auto utf8 = target_bus_name.utf8();
            const audio::BusId busId = runtime_->FindBusIdByName(
                std::string_view(utf8.get_data(), utf8.length()));
            if (busId == audio::kInvalidBusId) {
                UtilityFunctions::push_warning(
                    String("GoolAudioRuntime: register_sound_definition(\"")
                    + name + String("\") target_bus_name '") + target_bus_name
                    + String("' not found; falling back to category routing"));
            } else {
                def.targetBus = busId;
            }
        }
        def.attenuation.minDistance = static_cast<float>(min_distance);
        def.attenuation.maxDistance = static_cast<float>(max_distance);
        def.loopCrossfadeMs   = static_cast<float>(loop_crossfade_ms);
        runtime_->RegisterSoundDefinition(def);
    }

    int find_bus_id_by_name(const String& name) const {
        if (!runtime_) return -1;
        const auto utf8 = name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(), utf8.length()));
        return (id == audio::kInvalidBusId)
            ? -1
            : static_cast<int>(id);
    }

    bool load_sound_bank_from_json(const String& json_string,
                                     const String& gpak_path) {
        if (!runtime_ || !bank_) return false;
        audio::SoundBankLoadOptions opts;
        if (gpak_path.length() > 0) {
            pak_ = std::make_unique<audio::PakReader>();
            const auto utf8 = gpak_path.utf8();
            if (!pak_->Open(std::string_view(utf8.get_data(), utf8.length()))) {
                UtilityFunctions::push_error(
                    String("gpak open failed: ") + pak_->errorMessage().c_str());
                pak_.reset();
                return false;
            }
            opts.fileLoader = pak_->MakeSoundBankLoader();
        }
        const auto utf8 = json_string.utf8();
        const auto r = bank_->LoadFromJsonString(
            *runtime_,
            std::string_view(utf8.get_data(), utf8.length()),
            opts);
        if (!r.success) {
            UtilityFunctions::push_error(
                String("sound bank load failed at line ") +
                String::num_int64(r.errorLine) +
                String(": ") + r.errorMessage.c_str());
            return false;
        }
        return true;
    }

    void play_sound_at_location(const String& name, const Vector3& position) {
        if (!runtime_) return;
        audio::AudioSoundId id = audio::kInvalidSoundId;
        if (bank_) {
            const auto utf8 = name.utf8();
            id = bank_->Find(std::string_view(utf8.get_data(), utf8.length()));
        }
        if (id == audio::kInvalidSoundId) id = HashName(name);
        runtime_->SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
            id, V3(position)));
    }

    // EmitterHandle is a (index, generation) slot-map pair. We pack
    // it into an int64_t for transport across the GDScript boundary;
    // GDScript treats it as an opaque token.
    static int64_t PackHandle(audio::EmitterHandle h) {
        return (static_cast<int64_t>(h.generation) << 32) |
                static_cast<int64_t>(h.index);
    }
    static audio::EmitterHandle UnpackHandle(int64_t v) {
        audio::EmitterHandle h;
        h.index      = static_cast<uint32_t>(v & 0xFFFFFFFFu);
        h.generation = static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu);
        return h;
    }

    int64_t create_emitter(const String& name,
                             const Vector3& position,
                             bool looping,
                             double fade_in_ms) {
        if (!runtime_) return 0;
        audio::EmitterDescriptor desc;
        desc.soundId       = HashName(name);
        desc.position      = V3(position);
        desc.isLooping     = looping;
        desc.isSpatialized = true;
        desc.fadeInMs      = static_cast<float>(fade_in_ms);
        auto h = runtime_->CreateEmitter(desc);
        if (!h) return 0;
        return PackHandle(h.value());
    }

    void destroy_emitter(int64_t handle_packed, double fade_out_ms) {
        if (!runtime_) return;
        runtime_->DestroyEmitter(UnpackHandle(handle_packed),
                                   static_cast<float>(fade_out_ms));
    }

    void set_emitter_transform(int64_t handle_packed,
                                const Vector3& position,
                                const Vector3& forward,
                                const Vector3& velocity) {
        if (!runtime_) return;
        runtime_->SetEmitterTransform(UnpackHandle(handle_packed),
                                        V3(position), V3(forward), V3(velocity));
    }

    void set_emitter_playback_speed(int64_t handle_packed,
                                      double speed, double smoothing_ms) {
        if (!runtime_) return;
        runtime_->SetEmitterPlaybackSpeed(UnpackHandle(handle_packed),
                                            static_cast<float>(speed),
                                            static_cast<float>(smoothing_ms));
    }

    // Replication API. These match the engine's network-thread
    // entry points (the binding is single-threaded so they're
    // called from Godot's main thread, which is treated as the
    // network thread for binding purposes).

    void on_tick_advanced(int64_t simulation_tick, int64_t server_time_ms) {
        if (!runtime_) return;
        runtime_->OnTickAdvanced(
            static_cast<audio::SimulationTick>(simulation_tick),
            static_cast<audio::TimestampMs>(server_time_ms));
    }

    void submit_event_local(const String& sound_name,
                              const Vector3& position,
                              int64_t prediction_id,
                              int priority,
                              int64_t timestamp_ms) {
        if (!runtime_) return;
        audio::AudioSoundId id = audio::kInvalidSoundId;
        if (bank_) {
            const auto utf8 = sound_name.utf8();
            id = bank_->Find(std::string_view(utf8.get_data(), utf8.length()));
        }
        if (id == audio::kInvalidSoundId) id = HashName(sound_name);
        auto ev = audio::AudioEvent::MakePlaySoundAtLocation(
            id, V3(position),
            audio::AudioReplicationPolicy::LocalOnly,
            static_cast<audio::AudioPriority>(priority),
            static_cast<audio::TimestampMs>(timestamp_ms));
        ev.predictionId = static_cast<uint64_t>(prediction_id);
        runtime_->SubmitEvent(ev);
    }

    void submit_replicated_event(const String& sound_name,
                                   const Vector3& position,
                                   int64_t simulation_tick,
                                   int64_t server_time_ms,
                                   int priority) {
        if (!runtime_) return;
        audio::AudioSoundId id = audio::kInvalidSoundId;
        if (bank_) {
            const auto utf8 = sound_name.utf8();
            id = bank_->Find(std::string_view(utf8.get_data(), utf8.length()));
        }
        if (id == audio::kInvalidSoundId) id = HashName(sound_name);
        auto ev = audio::AudioEvent::MakePlaySoundAtLocation(
            id, V3(position),
            audio::AudioReplicationPolicy::ServerAuthoritative,
            static_cast<audio::AudioPriority>(priority),
            static_cast<audio::TimestampMs>(server_time_ms));
        ev.simulationTick = static_cast<audio::SimulationTick>(simulation_tick);
        runtime_->SubmitReplicatedEvent(ev);
    }

    void cancel_predicted_event(int64_t prediction_id, double fade_out_ms) {
        if (!runtime_) return;
        runtime_->CancelPredictedEvent(static_cast<uint64_t>(prediction_id),
                                         static_cast<float>(fade_out_ms));
    }

    void update_replicated_transform(int64_t handle_packed,
                                       const Vector3& position,
                                       const Vector3& forward,
                                       const Vector3& velocity,
                                       int64_t simulation_tick) {
        if (!runtime_) return;
        runtime_->UpdateReplicatedTransform(
            UnpackHandle(handle_packed),
            V3(position), V3(forward), V3(velocity),
            static_cast<audio::SimulationTick>(simulation_tick));
    }

    int64_t make_prediction_id() {
        // Monotonic counter starting from a value unlikely to collide
        // with a host-provided id. Nanoseconds-since-init is a clean
        // generator; we just need uniqueness within a session.
        static std::atomic<uint64_t> next{1};
        return static_cast<int64_t>(next.fetch_add(1, std::memory_order_relaxed));
    }

    bool register_voice_source(int64_t player_id) {
        if (!runtime_) return false;
        return static_cast<bool>(runtime_->RegisterVoiceSource(
            static_cast<audio::AudioPlayerId>(player_id)));
    }

    bool submit_voice_packet(int64_t player_id,
                               const PackedByteArray& bytes,
                               int sequence_number,
                               int64_t send_timestamp_ms,
                               int64_t arrival_timestamp_ms) {
        if (!runtime_) return false;
        if (bytes.size() == 0) return false;
        const auto rc = (arrival_timestamp_ms >= 0)
            ? runtime_->OnVoicePacket(
                static_cast<audio::AudioPlayerId>(player_id),
                bytes.ptr(),
                static_cast<size_t>(bytes.size()),
                static_cast<uint16_t>(sequence_number),
                static_cast<audio::TimestampMs>(send_timestamp_ms),
                static_cast<audio::TimestampMs>(arrival_timestamp_ms))
            : runtime_->OnVoicePacket(
                static_cast<audio::AudioPlayerId>(player_id),
                bytes.ptr(),
                static_cast<size_t>(bytes.size()),
                static_cast<uint16_t>(sequence_number),
                static_cast<audio::TimestampMs>(send_timestamp_ms));
        return rc == audio::AudioResult::Success;
    }

    double get_voice_jitter_ms(int64_t player_id) {
        if (!runtime_) return 0.0;
        audio::AudioRuntime::VoiceNetworkStats stats{};
        if (!runtime_->GetVoiceNetworkStats(
                static_cast<audio::AudioPlayerId>(player_id), stats)) {
            return 0.0;
        }
        return static_cast<double>(stats.observedJitterMs);
    }

    double get_voice_packet_loss_ratio(int64_t player_id) {
        if (!runtime_) return 0.0;
        audio::AudioRuntime::VoiceNetworkStats stats{};
        if (!runtime_->GetVoiceNetworkStats(
                static_cast<audio::AudioPlayerId>(player_id), stats)) {
            return 0.0;
        }
        const uint64_t expected =
            stats.packetsAccepted + stats.packetsLost + stats.plcGenerated;
        if (expected == 0) return 0.0;
        return static_cast<double>(stats.packetsLost) /
                static_cast<double>(expected);
    }

    int64_t hash_sound_name(const String& name) const {
        return static_cast<int64_t>(HashName(name));
    }

    int64_t hash_parameter_name(const String& name) const {
        return static_cast<int64_t>(HashParam(name));
    }

    // ---- Global (RTPC) parameter store -------------------------------
    // Surfaced as Gool.set_rtpc / Gool.get_rtpc in the GDScript
    // autoload. Names are hashed with HashParameterName so any
    // ASCII string maps to a stable AudioParameterId.

    bool set_global_parameter(const String& name, double value) {
        if (!runtime_) return false;
        const auto rc = runtime_->SetGlobalParameter(
            HashParam(name), static_cast<float>(value));
        return rc == audio::AudioResult::Success;
    }

    // Returns the stored value, or 0.0 if the parameter has never
    // been set. Use has_global_parameter() to disambiguate.
    double get_global_parameter(const String& name) const {
        if (!runtime_) return 0.0;
        float v = 0.0f;
        runtime_->GetGlobalParameter(HashParam(name), v);
        return static_cast<double>(v);
    }

    bool has_global_parameter(const String& name) const {
        if (!runtime_) return false;
        float v = 0.0f;
        return runtime_->GetGlobalParameter(HashParam(name), v);
    }

    bool clear_global_parameter(const String& name) {
        if (!runtime_) return false;
        return runtime_->ClearGlobalParameter(HashParam(name));
    }

    int64_t global_parameter_count() const {
        if (!runtime_) return 0;
        return static_cast<int64_t>(runtime_->GetGlobalParameterCount());
    }

    // ---- Sound-level RTPC bindings (multi-target, multi-curve) -------
    // Surfaced via GDScript autoload as Gool.bind_volume_rtpc /
    // bind_pitch_rtpc / bind_lowpass_rtpc / bind_rtpc (advanced).
    // Names are hashed via HashSoundName / HashParameterName so any
    // ASCII names work; target / curve are string-keyed for
    // GDScript ergonomics.

    bool set_sound_rtpc(const String& sound_name,
                          const String& param_name,
                          const String& target_name,
                          const String& curve_name,
                          double min_value,
                          double max_value,
                          double min_output,
                          double max_output,
                          double curve_exponent,
                          double smoothing_ms) {
        if (!runtime_) return false;
        audio::SoundRtpcBinding b;
        b.paramId       = HashParam(param_name);

        // Target string → enum.
        const auto t = target_name.utf8();
        const std::string ts(t.get_data(), static_cast<size_t>(t.length()));
        if      (ts == "volume")          b.target = audio::RtpcTarget::Volume;
        else if (ts == "pitch")           b.target = audio::RtpcTarget::Pitch;
        else if (ts == "lowpass" || ts == "lowpass_cutoff" ||
                 ts == "low_pass_cutoff") b.target = audio::RtpcTarget::LowPassCutoff;
        else if (ts == "reverb" || ts == "reverb_send")
                                          b.target = audio::RtpcTarget::ReverbSend;
        else                               return false;

        // Curve string → enum.
        const auto c = curve_name.utf8();
        const std::string cs(c.get_data(), static_cast<size_t>(c.length()));
        if      (cs == "linear")          b.curve = audio::RtpcCurve::Linear;
        else if (cs == "exponential" || cs == "exp")
                                          b.curve = audio::RtpcCurve::Exponential;
        else if (cs == "inverse_exponential" || cs == "inv_exp" ||
                 cs == "inverse_exp")     b.curve = audio::RtpcCurve::InverseExponential;
        else if (cs == "scurve" || cs == "smoothstep")
                                          b.curve = audio::RtpcCurve::SCurve;
        else                               return false;

        b.curveExponent = static_cast<float>(curve_exponent);
        b.minValue      = static_cast<float>(min_value);
        b.maxValue      = static_cast<float>(max_value);
        b.minOutput     = static_cast<float>(min_output);
        b.maxOutput     = static_cast<float>(max_output);
        b.smoothingMs   = static_cast<float>(smoothing_ms);

        const auto rc = runtime_->SetSoundRtpc(HashName(sound_name), b);
        return rc == audio::AudioResult::Success;
    }

    bool clear_sound_rtpc(const String& sound_name, const String& target_name) {
        if (!runtime_) return false;
        const auto t = target_name.utf8();
        const std::string ts(t.get_data(), static_cast<size_t>(t.length()));
        audio::RtpcTarget tgt;
        if      (ts == "volume")          tgt = audio::RtpcTarget::Volume;
        else if (ts == "pitch")           tgt = audio::RtpcTarget::Pitch;
        else if (ts == "lowpass" || ts == "lowpass_cutoff" ||
                 ts == "low_pass_cutoff") tgt = audio::RtpcTarget::LowPassCutoff;
        else if (ts == "reverb" || ts == "reverb_send")
                                          tgt = audio::RtpcTarget::ReverbSend;
        else                               return false;
        return runtime_->ClearSoundRtpc(HashName(sound_name), tgt);
    }

    int64_t clear_all_sound_rtpc(const String& sound_name) {
        if (!runtime_) return 0;
        return static_cast<int64_t>(
            runtime_->ClearAllSoundRtpc(HashName(sound_name)));
    }

    int64_t sound_rtpc_binding_count() const {
        if (!runtime_) return 0;
        return static_cast<int64_t>(runtime_->GetSoundRtpcBindingCount());
    }

    bool is_initialized() const { return initialized_; }

    audio::AudioRuntime* internal_runtime() const { return runtime_.get(); }

private:
    std::unique_ptr<audio::AudioRuntime> runtime_;
    std::unique_ptr<audio::SoundBank>     bank_;
    std::unique_ptr<audio::PakReader>     pak_;
    bool initialized_ = false;
};

// =====================================================================
// GoolMusicChannel
// =====================================================================

class GoolMusicChannel : public Node {
    GDCLASS(GoolMusicChannel, Node);

public:
    GoolMusicChannel() = default;
    ~GoolMusicChannel() override { stop(0.0); }

    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("attach", "runtime_node"),
                              &GoolMusicChannel::attach);
        ClassDB::bind_method(D_METHOD("play", "name", "fade_ms"),
                              &GoolMusicChannel::play, DEFVAL(1500.0));
        ClassDB::bind_method(D_METHOD("stop", "fade_ms"),
                              &GoolMusicChannel::stop, DEFVAL(1500.0));
        ClassDB::bind_method(D_METHOD("is_playing"),
                              &GoolMusicChannel::is_playing);
    }

    void attach(GoolAudioRuntime* runtime_node) {
        runtime_node_ = runtime_node;
        channel_.reset();
    }

    void play(const String& name, double fade_ms) {
        if (!ensure_channel()) return;
        channel_->Play(HashName(name), static_cast<float>(fade_ms));
    }

    void stop(double fade_ms) {
        if (channel_) channel_->Stop(static_cast<float>(fade_ms));
    }

    bool is_playing() const {
        return channel_ && !channel_->Current().IsNull();
    }

private:
    bool ensure_channel() {
        if (channel_) return true;
        if (!runtime_node_) {
            UtilityFunctions::push_error(
                "GoolMusicChannel: attach() must be called first");
            return false;
        }
        auto* rt = runtime_node_->internal_runtime();
        if (!rt) {
            UtilityFunctions::push_error(
                "GoolMusicChannel: runtime not initialized");
            return false;
        }
        channel_ = std::make_unique<audio::MusicChannel>(*rt);
        return true;
    }

    GoolAudioRuntime*                    runtime_node_ = nullptr;
    std::unique_ptr<audio::MusicChannel> channel_;
};

} // namespace gool

// =====================================================================
// Module init
// =====================================================================

void initialize_gool_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
    ClassDB::register_class<gool::GoolAudioRuntime>();
    ClassDB::register_class<gool::GoolMusicChannel>();
}

void uninitialize_gool_module(ModuleInitializationLevel p_level) {
    (void)p_level;
}

extern "C" {
GDExtensionBool GDE_EXPORT gool_godot_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr   p_library,
        GDExtensionInitialization*         r_initialization) {
    GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_gool_module);
    init_obj.register_terminator(uninitialize_gool_module);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
