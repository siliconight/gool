// godot/src/gool_godot.cpp
//
// Godot 4 GDExtension binding for the audio engine. Two node
// classes are exposed:
//
//   GoolAudioRuntime: the runtime singleton. Hosts call init() once,
//   then play_sound_at_location() etc. from GDScript.
//
//   GoolMusicChannel: the music-crossfade helper. Hosts call
//   play("track_name", fade_ms) and the binding handles the
//   coordinated fade-in/fade-out.
//
// This file expects godot-cpp on the include path. Build via
// `cmake -S godot -B build-godot -DGODOT_CPP_PATH=/path/to/godot-cpp`.
//
// The binding is intentionally thin: hot paths stay in C++, GDScript
// just calls in. No Godot frame-pump is hooked into AudioRuntime;
// the host calls runtime.update(dt) from a Node._process() callback.

#include "audio_engine/audio_runtime.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/miniaudio_backend.h"
#include "audio_engine/music_channel.h"
#include "audio_engine/sound_bank.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

using namespace godot;

namespace gool {

// Helper: convert Godot Vector3 to audio::Vec3.
static inline audio::Vec3 V3(const Vector3& v) {
    return audio::Vec3{static_cast<float>(v.x),
                        static_cast<float>(v.y),
                        static_cast<float>(v.z)};
}

// FNV-1a hash of a Godot string — same hash the engine uses for
// names registered via SoundBank, so GDScript can pass string names
// directly.
static audio::AudioSoundId HashName(const String& name) {
    const auto utf8 = name.utf8();
    return audio::HashSoundName(std::string_view(utf8.get_data(),
                                                    utf8.length()));
}

// =====================================================================
// GoolAudioRuntime
// =====================================================================
//
// Single Node. Add to autoload as the project's audio singleton.
// Owns one AudioRuntime + miniaudio backend.

class GoolAudioRuntime : public Node {
    GDCLASS(GoolAudioRuntime, Node);

public:
    GoolAudioRuntime() = default;
    ~GoolAudioRuntime() override = default;

    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("init", "sample_rate", "buffer_size"),
                              &GoolAudioRuntime::init, DEFVAL(48000), DEFVAL(512));
        ClassDB::bind_method(D_METHOD("shutdown"),
                              &GoolAudioRuntime::shutdown);
        ClassDB::bind_method(D_METHOD("update", "delta"),
                              &GoolAudioRuntime::update);
        ClassDB::bind_method(D_METHOD("set_listener_transform",
                                       "position", "forward", "velocity"),
                              &GoolAudioRuntime::set_listener_transform);
        ClassDB::bind_method(D_METHOD("register_pcm_sound",
                                       "name", "samples",
                                       "sample_rate", "channels"),
                              &GoolAudioRuntime::register_pcm_sound,
                              DEFVAL(48000), DEFVAL(1));
        ClassDB::bind_method(D_METHOD("play_sound_at_location",
                                       "name", "position"),
                              &GoolAudioRuntime::play_sound_at_location);
        ClassDB::bind_method(D_METHOD("hash_sound_name", "name"),
                              &GoolAudioRuntime::hash_sound_name);
        ClassDB::bind_method(D_METHOD("is_initialized"),
                              &GoolAudioRuntime::is_initialized);
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
        // Set a default listener at origin so spatializers run.
        audio::AudioListener listener;
        runtime_->SetListener(listener);
        initialized_ = true;
        return true;
    }

    void shutdown() {
        if (!runtime_) return;
        runtime_->Shutdown();
        runtime_.reset();
        initialized_ = false;
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

    int64_t register_pcm_sound(const String&         name,
                                 const PackedFloat32Array& samples,
                                 int                   sample_rate,
                                 int                   channels) {
        if (!runtime_) return 0;
        const audio::AudioSoundId id = HashName(name);
        runtime_->RegisterPcmSound(
            id,
            std::span<const float>(samples.ptr(), samples.size()),
            static_cast<uint32_t>(sample_rate),
            static_cast<uint32_t>(channels));
        audio::SoundDefinition def;
        def.soundId     = id;
        def.spatialized = true;
        def.targetBus   = audio::kBusMaster;
        runtime_->RegisterSoundDefinition(def);
        return static_cast<int64_t>(id);
    }

    void play_sound_at_location(const String& name, const Vector3& position) {
        if (!runtime_) return;
        const audio::AudioSoundId id = HashName(name);
        runtime_->SubmitEvent(audio::AudioEvent::MakePlaySoundAtLocation(
            id, V3(position)));
    }

    int64_t hash_sound_name(const String& name) const {
        return static_cast<int64_t>(HashName(name));
    }

    bool is_initialized() const { return initialized_; }

    // Friend access for GoolMusicChannel: it needs the underlying
    // runtime pointer to construct an audio::MusicChannel.
    audio::AudioRuntime* internal_runtime() const { return runtime_.get(); }

private:
    std::unique_ptr<audio::AudioRuntime> runtime_;
    bool initialized_ = false;
};

// =====================================================================
// GoolMusicChannel
// =====================================================================
//
// Wraps audio::MusicChannel. Construct with a reference to a
// GoolAudioRuntime; call play("track_name", fade_ms) to crossfade.

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
        channel_.reset();   // recreate on next play()
    }

    void play(const String& name, double fade_ms) {
        if (!ensure_channel()) return;
        const audio::AudioSoundId id = HashName(name);
        channel_->Play(id, static_cast<float>(fade_ms));
    }

    void stop(double fade_ms) {
        if (!channel_) return;
        channel_->Stop(static_cast<float>(fade_ms));
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
