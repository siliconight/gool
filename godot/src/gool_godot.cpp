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
#include "audio_engine/bus.h"
#include "audio_engine/bus_config_loader.h"
#include "audio_engine/config.h"
#include "audio_engine/emitter.h"
#include "audio_engine/events.h"
#include "audio_engine/geometry_query.h"  // AudioMaterial (Phase 5.1)
#include "audio_engine/gpak.h"
#include "audio_engine/material_eq.h"     // v0.61.0 — audition surface
#include "audio_engine/backend/miniaudio_backend.h"
#include "audio_engine/music_channel.h"
#include "audio_engine/sound_bank.h"
#include "audio_engine/version.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

// v0.31.0 (Phase 5.2): occlusion bridge.
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/dictionary.hpp>

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

// v0.28.0 (Phase 3.3c-1): helpers for the get_bus_effects introspection
// API. Kept at file scope (not inside GoolAudioRuntime) so the class
// definition reads cleanly and these helpers are reusable if a future
// editor-side binding wants them.

static const char* _gool_effect_kind_name(audio::EffectKind k) {
    switch (k) {
        case audio::EffectKind::Gain:           return "Gain";
        case audio::EffectKind::BiquadFilter:   return "BiquadFilter";
        case audio::EffectKind::Compressor:     return "Compressor";
        case audio::EffectKind::Reverb:         return "Reverb";
        case audio::EffectKind::Saturation:     return "Saturation";
        case audio::EffectKind::MasterControl:  return "MasterControl";
        case audio::EffectKind::None:
        default:                                return "None";
    }
}

// v0.64.0: short uppercase pill label for the mixer dock's FX chain
// summary band (Phase 5 of the UI evolution plan). Distinct from
// _gool_effect_kind_name above: this returns a 3-6 char abbreviation
// suitable for cramming into a 130px-wide strip's FX_BAND.
//
// Audience reminder: gool's audience is Godot devs without a strong
// audio background. BiquadFilter → "EQ" is technically lossy (a
// biquad can be a generic filter, not just an EQ band) but matches
// what users will recognize from every DAW. _kind_name remains the
// accurate display string for the effects panel header.
//
// Keep in sync with KIND_INT_TO_ABBREV in config_model.gd — the
// editor-time fallback path uses that table when no live runtime
// is present (no F5 session running).
static const char* _gool_effect_kind_abbreviation(audio::EffectKind k) {
    switch (k) {
        case audio::EffectKind::Gain:           return "GAIN";
        case audio::EffectKind::BiquadFilter:   return "EQ";
        case audio::EffectKind::Compressor:     return "COMP";
        case audio::EffectKind::Reverb:         return "REVERB";
        case audio::EffectKind::Saturation:     return "SAT";
        case audio::EffectKind::MasterControl:  return "MSTR";
        case audio::EffectKind::None:
        default:                                return "FX";
    }
}

// For an effect of the given kind, read every parameter the kind
// recognizes and stuff them into `out` as paramId → value. The set
// of params per kind is defined here (single source of truth on the
// binding side); the engine's GetParameter just serves the values.
// Keep in sync with EffectParameter:: in bus.h.
static void _gool_fill_params_for_kind(audio::AudioRuntime* rt,
                                        uint32_t busIdx,
                                        uint32_t effectIdx,
                                        audio::EffectKind kind,
                                        Dictionary& out) {
    if (rt == nullptr) return;
    // v0.28.2: `namespace EP = ...`, NOT `using EP = ...`.
    // EffectParameter is a namespace (a collection of constexpr
    // uint16_t members), not a type. C++11's alias-declaration
    // (`using X = Y;`) is for types only; namespace aliases use the
    // older `namespace X = Y;` syntax. GCC accepts the wrong form as
    // an extension; MSVC correctly rejects it, which is why v0.28.0
    // and v0.28.1 both failed CI at this line on the windows-x86_64
    // (and linux/macos clang) matrix entries even though the local
    // g++ engine compile passed cleanly.
    namespace EP = audio::EffectParameter;
    auto put = [&](uint16_t paramId) {
        out[static_cast<int64_t>(paramId)] =
            rt->GetEffectParameter(busIdx, effectIdx, paramId);
    };
    switch (kind) {
        case audio::EffectKind::Gain:
            put(EP::Gain_GainDb);
            break;
        case audio::EffectKind::BiquadFilter:
            put(EP::Biquad_CutoffHz);
            put(EP::Biquad_Q);
            put(EP::Biquad_GainDb);
            break;
        case audio::EffectKind::Compressor:
            put(EP::Compressor_ThresholdDb);
            put(EP::Compressor_Ratio);
            put(EP::Compressor_AttackMs);
            put(EP::Compressor_ReleaseMs);
            put(EP::Compressor_MakeupDb);
            put(EP::Compressor_KneeWidthDb);
            put(EP::Compressor_MixRatio);
            put(EP::Compressor_MaxReductionDb);
            put(EP::Compressor_SidechainHpfHz);
            put(EP::Compressor_HoldMs);
            put(EP::Compressor_DetectionMode);
            break;
        case audio::EffectKind::Reverb:
            // v0.29.0: Dattorro plate exposes 6 IDs. Decay and HfDamping
            // are the renamed aliases at the original 9/10 IDs.
            // v0.29.5: added DryGainDb (26) for insert use.
            put(EP::Reverb_PredelayMs);
            put(EP::Reverb_Decay);
            put(EP::Reverb_LfDamping);
            put(EP::Reverb_HfDamping);
            put(EP::Reverb_Diffusion);
            put(EP::Reverb_DryGainDb);
            put(EP::Reverb_WetGainDb);
            break;
        case audio::EffectKind::Saturation:
            put(EP::Saturation_Drive);
            put(EP::Saturation_Mix);
            put(EP::Saturation_OutputGain);
            put(EP::Saturation_Bias);
            break;
        case audio::EffectKind::MasterControl:
            // v0.63.0: Phase 7 Master FX Lite. Fill both config IDs
            // (so the dock can show / edit the full state) and
            // telemetry IDs (so the dock can show live LUFS, peak,
            // and gain reduction readings). Telemetry IDs are
            // read-only on the engine side — writes are ignored
            // by MasterControlEffect::OnParameter.
            put(EP::MC_GlueEnabled);
            put(EP::MC_RiderEnabled);
            put(EP::MC_LimiterEnabled);
            put(EP::MC_GlueThresholdDb);
            put(EP::MC_GlueRatio);
            put(EP::MC_GlueAttackMs);
            put(EP::MC_GlueReleaseMs);
            put(EP::MC_GlueKneeDb);
            put(EP::MC_GlueMakeupDb);
            put(EP::MC_RiderTargetLufs);
            put(EP::MC_RiderTimeConstMs);
            put(EP::MC_RiderMaxGainDb);
            put(EP::MC_RiderMinGainDb);
            put(EP::MC_RiderFreezeBelowLufs);
            put(EP::MC_LimiterCeilingDbtp);
            put(EP::MC_LimiterReleaseMs);
            put(EP::MC_LimiterLookaheadMs);
            put(EP::MC_TelLufsShortTerm);
            put(EP::MC_TelLufsIntegrated);
            put(EP::MC_TelPeakDb);
            put(EP::MC_TelTruePeakDbtp);
            put(EP::MC_TelGainReductionDb);
            put(EP::MC_TelRiderGainDb);
            break;
        case audio::EffectKind::None:
        default:
            break;
    }
}

// =====================================================================
// GodotGeometryQuery (v0.31.0, Phase 5.2)
// =====================================================================
//
// Implements audio::IAudioGeometryQuery using Godot's PhysicsServer3D.
// The runtime owns this via AudioRuntimeDependencies::geometryQuery;
// GoolAudioRuntime keeps a non-owning observer pointer so a current
// GoolListener3D can push its World3D space RID in.
//
// Threading: RaycastAudioOcclusion is called from
// OcclusionSystem::Update inside AudioRuntime::Update, which the
// GoolAudioRuntime calls from _process — i.e. Godot's main thread.
// PhysicsServer3D direct-space-state queries are safe from this
// context provided Godot's 3D physics is not running on a separate
// thread (the default; see ProjectSettings → physics/3d/run_on_separate_thread).
//
// Material resolution mirrors GDScript's `material_from_collider`:
// the metadata-on-collider path. Accepts either an int (one of the
// MATERIAL_* constants) or a Resource with a `material` int property
// (the GoolAudioMaterial path). Group membership is not honored here
// — designers can still tag via metadata in the inspector and the
// raycast will see it.

class GodotGeometryQuery final : public audio::IAudioGeometryQuery {
public:
    GodotGeometryQuery() = default;
    ~GodotGeometryQuery() override = default;

    void SetSpaceRID(const RID& rid) noexcept {
        space_rid_ = rid;
    }

    bool RaycastAudioOcclusion(const audio::Vec3& from,
                                 const audio::Vec3& to,
                                 audio::AudioOcclusionHit& outHit) noexcept override {
        outHit = {};
        if (!space_rid_.is_valid()) {
            // No world bound yet (no GoolListener3D has gone current).
            // Engine treats false as "unblocked" — same outcome as
            // NullGeometryQuery would return.
            return false;
        }

        PhysicsServer3D* ps = PhysicsServer3D::get_singleton();
        if (ps == nullptr) return false;
        PhysicsDirectSpaceState3D* state = ps->space_get_direct_state(space_rid_);
        if (state == nullptr) return false;

        const Vector3 v_from(from.x, from.y, from.z);
        const Vector3 v_to  (to.x,   to.y,   to.z);

        Ref<PhysicsRayQueryParameters3D> q = PhysicsRayQueryParameters3D::create(v_from, v_to);
        // Default collision mask 0xFFFFFFFF — hit everything. Designers
        // who need to exclude triggers / debug colliders from the
        // audio raycast can re-author per-layer logic later (project
        // setting `gool/occlusion/collision_mask` is a likely future
        // hook).
        q->set_collide_with_areas(false);
        q->set_collide_with_bodies(true);

        const Dictionary result = state->intersect_ray(q);
        if (result.is_empty()) return false;

        // Resolve material from the hit collider. The intersect_ray
        // dictionary's "collider" entry is a Godot Object pointer (a
        // CollisionObject3D, typically StaticBody3D or Area3D).
        audio::AudioMaterial material = audio::AudioMaterial::Default;
        if (result.has("collider")) {
            const Variant collider_var = result["collider"];
            // Variant→Object extraction. The pattern in godot-cpp is
            // the Variant's operator Object*() conversion via static
            // cast, gated on the actual type to avoid undefined
            // behavior when the variant isn't an object. Object::cast_to
            // is for downcasting *between* Object subclasses, not for
            // pulling an Object* out of a Variant.
            Object* collider = nullptr;
            if (collider_var.get_type() == Variant::OBJECT) {
                collider = static_cast<Object*>(collider_var);
            }
            if (collider != nullptr && collider->has_meta("gool_audio_material")) {
                const Variant meta = collider->get_meta("gool_audio_material");
                int material_int = 0;
                if (meta.get_type() == Variant::INT) {
                    material_int = static_cast<int>(meta);
                } else if (meta.get_type() == Variant::OBJECT) {
                    // GoolAudioMaterial resource path — duck-type by
                    // reading the `material` property without hard
                    // typing the resource (avoids needing the resource
                    // header in the binding).
                    Object* res = static_cast<Object*>(meta);
                    if (res != nullptr) {
                        const Variant inner = res->get("material");
                        if (inner.get_type() == Variant::INT) {
                            material_int = static_cast<int>(inner);
                        }
                    }
                }
                if (material_int >= 0
                    && material_int < static_cast<int>(audio::kAudioMaterialCount)) {
                    material = static_cast<audio::AudioMaterial>(material_int);
                }
            }
        }

        // Fill the hit. ResolveOcclusion will map the material to
        // (absorption, damping); we don't need to set the explicit
        // absorption/damping fields when material != Default.
        outHit.hit       = true;
        outHit.material  = material;
        if (result.has("position")) {
            const Vector3 hp = result["position"];
            outHit.hitPoint = audio::Vec3{
                static_cast<float>(hp.x),
                static_cast<float>(hp.y),
                static_cast<float>(hp.z)};
            const Vector3 d = hp - v_from;
            outHit.distance = static_cast<float>(d.length());
        }
        return true;
    }

private:
    RID space_rid_;
};

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

        // v0.22.7: diagnostic accessors exposing the audio backend's
        // internal state. Used by the GDScript runtime singleton to
        // log device info and detect dead-air silence (callback
        // running but writing zeros).
        ClassDB::bind_method(D_METHOD("get_backend_description"),
                              &GoolAudioRuntime::get_backend_description);
        ClassDB::bind_method(D_METHOD("get_render_stats"),
                              &GoolAudioRuntime::get_render_stats);
        ClassDB::bind_method(D_METHOD("reset_render_peak"),
                              &GoolAudioRuntime::reset_render_peak);
        // v0.24.0: per-bus metering for the editor mixer dock. Returns
        // an Array of Dictionaries with shape:
        //   [ { "name": "Master", "parent": -1, "peak_linear": 0.13 },
        //     { "name": "Music",  "parent":  0, "peak_linear": 0.05 }, ... ]
        // Read-and-reset semantics: peaks reflect samples since the
        // last call (so polling at 30 Hz gives 33ms windows). Cheap;
        // mixer dock can call every editor tick.
        ClassDB::bind_method(D_METHOD("get_bus_stats"),
                              &GoolAudioRuntime::get_bus_stats);

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

        // ---- v0.14.0: native-Godot integration --------------------
        //
        // register_sound_from_stream accepts any Godot AudioStream
        // resource (AudioStreamWAV, AudioStreamOggVorbis,
        // AudioStreamMP3, etc.) imported via Godot's standard asset
        // pipeline. This lets users keep their existing
        // .import-based workflow instead of routing files through a
        // separate gool path. Internally delegates to
        // register_sound_from_file when the stream has a resource
        // path, or extracts raw PCM from in-memory AudioStreamWAV
        // when used programmatically.
        ClassDB::bind_method(D_METHOD("register_sound_from_stream",
                                       "name", "stream"),
                              &GoolAudioRuntime::register_sound_from_stream);

        // set_bus_gain_db / set_master_volume_db expose the engine's
        // internal bus graph to GDScript. The previous comment in
        // this file promised these bindings but they were never
        // wired up; v0.14.0 fixes that. The intended usage is to
        // mirror Godot's AudioServer bus volumes from your settings
        // menu — see the autoload's sync_volume_from_godot_bus()
        // helper for the one-line integration.
        ClassDB::bind_method(D_METHOD("set_bus_gain_db",
                                       "bus_name", "gain_db"),
                              &GoolAudioRuntime::set_bus_gain_db);
        ClassDB::bind_method(D_METHOD("set_master_volume_db", "db"),
                              &GoolAudioRuntime::set_master_volume_db);
        // v0.27.0: per-bus mute / solo / effect-bypass setters. Used
        // by the mixer dock's S/M/B buttons via the EngineDebugger
        // editor↔game channel. Same name-resolution path as
        // set_bus_gain_db (FindBusIdByName + per-state setter).
        ClassDB::bind_method(D_METHOD("set_bus_muted",
                                       "bus_name", "muted"),
                              &GoolAudioRuntime::set_bus_muted);
        ClassDB::bind_method(D_METHOD("set_bus_soloed",
                                       "bus_name", "soloed"),
                              &GoolAudioRuntime::set_bus_soloed);
        ClassDB::bind_method(D_METHOD("set_bus_effects_bypassed",
                                       "bus_name", "bypassed"),
                              &GoolAudioRuntime::set_bus_effects_bypassed);
        // v0.28.0 (Phase 3.3c-1): effect-chain live edit + introspection.
        // set_effect_parameter mirrors set_bus_gain_db's name-resolution
        // pattern; get_bus_effects returns the structured Array-of-Dicts
        // described in the method docstring.
        ClassDB::bind_method(D_METHOD("set_effect_parameter",
                                       "bus_name", "effect_index",
                                       "param_id", "value"),
                              &GoolAudioRuntime::set_effect_parameter);
        ClassDB::bind_method(D_METHOD("get_bus_effects", "bus_name"),
                              &GoolAudioRuntime::get_bus_effects);
        ClassDB::bind_method(D_METHOD("register_sound_definition",
                                       "name", "spatialized", "looping",
                                       "min_distance", "max_distance",
                                       "loop_crossfade_ms",
                                       "category", "target_bus_name",
                                       "occlusion_enabled"),
                              &GoolAudioRuntime::register_sound_definition,
                              DEFVAL(true), DEFVAL(false),
                              DEFVAL(1.0), DEFVAL(50.0), DEFVAL(0.0),
                              DEFVAL(0), DEFVAL(String()),
                              DEFVAL(true));
        // Bus-name → BusId resolver. Returns -1 if no bus matches.
        // Useful for hosts that need to call other BusId-taking
        // bindings (set_bus_gain_db, set_effect_parameter) by name.
        ClassDB::bind_method(D_METHOD("find_bus_id_by_name", "name"),
                              &GoolAudioRuntime::find_bus_id_by_name);

        // v0.32.0 (Phase 5.3): material-aware reverb presets.
        // Returns the engine's ReverbPresetByMaterial table value
        // for a given AudioMaterial as a Dictionary the GDScript
        // ReverbZone prefab applies via set_effect_parameter.
        ClassDB::bind_method(D_METHOD("get_reverb_preset_for_material",
                                       "material"),
                              &GoolAudioRuntime::get_reverb_preset_for_material);

        // v0.33.0 (Phase 6.A): per-material EQ curves. Returns the
        // engine's MaterialEqByMaterial table value for a given
        // AudioMaterial as a Dictionary with low/mid/high band
        // parameters. Designers apply this manually for now via
        // Biquad effects on a bus; v0.34.0+ will wire it directly
        // into the impact and listener-space playback paths.
        ClassDB::bind_method(D_METHOD("get_material_eq_for_material",
                                       "material"),
                              &GoolAudioRuntime::get_material_eq_for_material);

        // v0.34.0 (Phase 6.B): apply a material's EQ curve to a
        // named bus's 3-biquad chain. The convention from cookbook
        // section 14 — first biquad is LowShelf, second is Peak,
        // third is HighShelf — is what this method writes to.
        // Returns false if the bus or chain isn't shaped right
        // (caller can decide whether to warn).
        ClassDB::bind_method(D_METHOD("apply_material_eq_to_bus",
                                       "bus_name", "material"),
                              &GoolAudioRuntime::apply_material_eq_to_bus);

        // v0.59.3 (Phase 6.E.1 audition): offline DSP processing of a
        // user-provided sample buffer through a material's EQ curve.
        // Used by the editor inspector's audition button. STATIC so
        // the editor inspector can call it without needing the
        // /root/Gool autoload to be reachable. See
        // process_buffer_through_material_eq() for the surface.
        ClassDB::bind_static_method("GoolAudioRuntime",
                                     D_METHOD("process_buffer_through_material_eq",
                                               "buffer", "material",
                                               "intensity", "sample_rate"),
                                     &GoolAudioRuntime::process_buffer_through_material_eq);

        // v0.60.0 (Phase 6.E.1 Option B audition): raw-curve audition
        // for override-enabled GoolAudioMaterial resources. Same
        // biquad path as process_buffer_through_material_eq but
        // takes curve values directly so the inspector can preview
        // override values that aren't in the engine table.
        ClassDB::bind_static_method("GoolAudioRuntime",
                                     D_METHOD("process_buffer_through_curve",
                                               "buffer",
                                               "low_freq_hz", "low_gain_db",
                                               "mid_freq_hz", "mid_gain_db", "mid_q",
                                               "high_freq_hz", "high_gain_db",
                                               "intensity", "sample_rate"),
                                     &GoolAudioRuntime::process_buffer_through_curve);

        // v0.31.0 (Phase 5.2): live occlusion controls.
        ClassDB::bind_method(D_METHOD("set_occlusion_enabled", "enabled"),
                              &GoolAudioRuntime::set_occlusion_enabled);
        ClassDB::bind_method(D_METHOD("set_occlusion_intensity", "intensity"),
                              &GoolAudioRuntime::set_occlusion_intensity);
        ClassDB::bind_method(D_METHOD("set_audio_world_space_rid", "rid"),
                              &GoolAudioRuntime::set_audio_world_space_rid);
        ClassDB::bind_method(D_METHOD("load_sound_bank_from_json",
                                       "json_string", "gpak_path",
                                       "skip_validation"),
                              &GoolAudioRuntime::load_sound_bank_from_json,
                              DEFVAL(""), DEFVAL(false));

        // Playback (one-shot + handle-based).
        ClassDB::bind_method(D_METHOD("play_sound_at_location",
                                       "name", "position"),
                              &GoolAudioRuntime::play_sound_at_location);
        // Phase 5.1 (v0.30.0): material-aware playback. Looks up the
        // sound bank entry as a by_material group keyed by `material`,
        // then plays the selected variant at `position`. If `name` is
        // a plain sound or a non-by_material group, behavior matches
        // play_sound_at_location and material is ignored.
        ClassDB::bind_method(D_METHOD("play_sound_at_location_for_material",
                                       "name", "position", "material"),
                              &GoolAudioRuntime::play_sound_at_location_for_material);
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

        // 2.4 voice-source mute/volume.
        ClassDB::bind_method(D_METHOD("set_voice_source_muted",
                                       "player_id", "muted"),
                              &GoolAudioRuntime::set_voice_source_muted);
        ClassDB::bind_method(D_METHOD("set_voice_source_volume",
                                       "player_id", "volume"),
                              &GoolAudioRuntime::set_voice_source_volume);

        // 2.6 outbound bandwidth budget.
        ClassDB::bind_method(D_METHOD("set_voice_bandwidth_budget",
                                       "player_id", "bytes_per_sec"),
                              &GoolAudioRuntime::set_voice_bandwidth_budget);
        ClassDB::bind_method(D_METHOD("suggest_voice_bitrate",
                                       "player_id", "frame_duration_ms"),
                              &GoolAudioRuntime::suggest_voice_bitrate);
        ClassDB::bind_method(D_METHOD("report_voice_bytes_sent",
                                       "player_id", "bytes",
                                       "bitrate_used_bps"),
                              &GoolAudioRuntime::report_voice_bytes_sent);

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

    // v0.31.0 (Phase 5.2): occlusion config plumbing shared between
    // the two init paths. Reads/registers ProjectSettings, applies
    // them to cfg, and constructs the GodotGeometryQuery dependency.
    //
    // Project settings registered (defaults applied on first run,
    // editable in Project Settings → General → Gool → Occlusion):
    //
    //   gool/occlusion/enabled    bool    default true
    //   gool/occlusion/intensity  float   default 0.7
    //
    // The settings are read on every init (re-init picks up changes
    // made in the editor between runs). Live runtime changes from
    // GDScript go through set_occlusion_enabled / set_occlusion_intensity,
    // which write to the runtime directly without touching settings.
    void _apply_occlusion_config(audio::AudioConfig&              cfg,
                                   audio::AudioRuntimeDependencies& deps) {
        ProjectSettings* ps = ProjectSettings::get_singleton();
        if (ps != nullptr) {
            // Enable flag.
            const String enabled_key = "gool/occlusion/enabled";
            if (!ps->has_setting(enabled_key)) {
                ps->set_setting(enabled_key, Variant(true));
                ps->set_initial_value(enabled_key, Variant(true));
            }
            cfg.enableOcclusion = static_cast<bool>(ps->get_setting(enabled_key));

            // Intensity multiplier. Default 0.7 — "gentle but
            // present" sweet spot; designers can dial up for
            // cinematic levels or down for clarity-critical sounds.
            const String intensity_key = "gool/occlusion/intensity";
            if (!ps->has_setting(intensity_key)) {
                ps->set_setting(intensity_key, Variant(0.7f));
                ps->set_initial_value(intensity_key, Variant(0.7f));
            }
            cfg.occlusionIntensity =
                static_cast<float>(static_cast<double>(ps->get_setting(intensity_key)));
        }

        // Construct the geometry query. Engine takes ownership via
        // std::move into deps.geometryQuery; we keep a non-owning
        // observer pointer so set_audio_world_space_rid (called from
        // GoolListener3D.set_current) can later push the World3D
        // space RID in.
        auto query = std::make_unique<GodotGeometryQuery>();
        geometry_query_ = query.get();
        deps.geometryQuery = std::move(query);
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
        _apply_occlusion_config(cfg, deps);

        const auto rc = runtime_->Initialize(cfg, std::move(deps));
        if (rc != audio::AudioResult::Success) {
            UtilityFunctions::push_error("GoolAudioRuntime: Initialize failed");
            runtime_.reset();
            geometry_query_ = nullptr;
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
        _apply_occlusion_config(cfg, deps);

        const auto rc = runtime_->Initialize(cfg, std::move(deps));
        if (rc != audio::AudioResult::Success) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: Initialize failed (with bus config)");
            runtime_.reset();
            geometry_query_ = nullptr;
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

    // v0.22.7: human-readable name of the audio device the backend
    // opened (e.g. "WASAPI / Speakers (Realtek HD Audio)"). Empty
    // string before Initialize, or if the runtime is using the null
    // backend, or if the installed backend doesn't override the
    // MiniaudioBackend-specific accessor.
    //
    // Useful for "wait, gool is sending audio to my MONITOR not my
    // HEADPHONES" diagnosis — the silence symptom we hit in v0.22.6
    // session. If the device name doesn't match where the user
    // expects audio, that's the bug, no C++ debugging needed.
    String get_backend_description() const {
        if (!runtime_) return String();
        const auto* backend = runtime_->GetBackend();
        if (!backend) return String();
        // We only do the downcast/diagnostic-method dance for the
        // miniaudio backend. NullAudioBackend (used in headless/CI)
        // returns empty here.
        const auto* mb =
            dynamic_cast<const audio::MiniaudioBackend*>(backend);
        if (!mb) return String();
        const char* desc = mb->DeviceDescription();
        return String(desc ? desc : "");
    }

    // v0.22.7: render-thread health metrics. Returns a Dictionary:
    //   {
    //     "callback_invocations": int  (monotonic, audio thread
    //                                    drives this — zero means
    //                                    the audio thread isn't
    //                                    actually running),
    //     "frames_rendered":      int  (total audio frames written
    //                                    to the device since
    //                                    Initialize),
    //     "peak_amplitude":       float (running max of |sample|
    //                                     since last reset_render_peak;
    //                                     0.0 means dead silence is
    //                                     being written),
    //     "exception_count":      int  (caught exceptions from the
    //                                    engine's OnRender path —
    //                                    nonzero means audio frames
    //                                    have been dropped to silence
    //                                    by the catch-all barrier)
    //   }
    //
    // Empty Dictionary if the runtime isn't initialized or doesn't
    // have a miniaudio backend.
    //
    // Designed for periodic polling from the control thread (e.g.
    // GoolAudioRuntime singleton's _process tick on Godot's game
    // thread). Lock-free reads; cheap.
    Dictionary get_render_stats() const {
        Dictionary d;
        if (!runtime_) return d;
        const auto* backend = runtime_->GetBackend();
        if (!backend) return d;
        const auto* mb =
            dynamic_cast<const audio::MiniaudioBackend*>(backend);
        if (!mb) return d;
        d["callback_invocations"] =
            static_cast<int64_t>(mb->CallbackInvocations());
        d["frames_rendered"] =
            static_cast<int64_t>(mb->FramesRendered());
        d["peak_amplitude"] = mb->PeakSampleAbs();
        d["exception_count"] =
            static_cast<int64_t>(mb->RenderCallbackExceptions());
        // v0.22.8: mixer-level diagnostics. These let GDScript
        // distinguish "no emitter active in mixer" from "voices
        // playing but bus chain silenced" from "audio reaching the
        // device" failure modes — the key discrimination the v0.22.7
        // session's "peak=0" output didn't have.
        //
        // v0.22.8.1: accessed via AudioRuntime forwarders rather than
        // directly through the mixer, so this binding TU doesn't need
        // to include the internal audio_mixer.h header (which would
        // break the gdextension build, where only include/ is on the
        // include path).
        d["active_voices"] =
            static_cast<int64_t>(runtime_->GetActiveVoicesApprox());
        d["mixer_peak"]    = runtime_->GetMasterPreGainPeak();
        d["master_gain"]   = runtime_->GetMasterGainLinear();
        // v0.39.0: emitter pool count for the dead-air diagnostic.
        // Lets GDScript distinguish "idle game with no emitters
        // active" (normal silence — not a bug) from "emitters
        // exist but their voice slots never promoted out of
        // Inactive" (the real bug case). Without this, the warning
        // produced false positives every 2 seconds in any scene
        // with no SFX currently playing.
        d["active_emitters"] =
            static_cast<int64_t>(runtime_->GetActiveEmitterCount());
        return d;
    }

    // v0.24.0: enumerate buses with their current peak levels. Returns
    // an Array of Dictionaries, one per bus, in BusGraph internal order
    // (master is always present; topology order depends on the config).
    // The dock smooths the linear peak in GDScript for display.
    //
    // Read-and-reset semantics on the peak field: the atomic exchange
    // means each poll covers samples since the last poll, not since
    // Initialize. Callers should poll at a steady cadence (~30 Hz) to
    // get sensible meter behavior.
    Array get_bus_stats() {
        Array out;
        if (!runtime_) return out;
        const uint32_t n = runtime_->GetBusCount();
        // Sentinel for "no parent" matches BusGraph::kInvalidIndex
        // (UINT32_MAX). Hard-coded here so this binding TU stays
        // free of internal mixer headers.
        constexpr uint32_t kNoParent = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i) {
            Dictionary d;
            d["name"]        = String::utf8(runtime_->GetBusName(i));
            const uint32_t parent = runtime_->GetBusParentIndex(i);
            d["parent"]      = (parent == kNoParent)
                                ? int64_t{-1}
                                : static_cast<int64_t>(parent);
            d["peak_linear"] = runtime_->ReadAndResetBusPeakLinear(i);
            // v0.27.0: per-bus mute / solo / effect-bypass state. Read
            // each callback so the mixer dock's S/M/B button visuals
            // reflect the actual runtime state, even if state was
            // changed by something other than the dock (e.g. a host
            // script calling Gool.set_bus_muted directly).
            d["muted"]    = runtime_->IsBusMuted(i);
            d["soloed"]   = runtime_->IsBusSoloed(i);
            d["bypassed"] = runtime_->IsBusEffectsBypassed(i);
            out.push_back(d);
        }
        return out;
    }

    // v0.22.7: zero the running-peak atomic so the next read of
    // get_render_stats()["peak_amplitude"] reflects samples written
    // SINCE this call, not since Initialize. The intended polling
    // pattern is "read peak, log it, reset, wait N seconds, repeat"
    // — that way a "peak == 0" reading proves silence in the most
    // recent window rather than the whole runtime lifetime.
    //
    // v0.22.8: also resets the mixer's master pre-gain peak so the
    // two peaks stay synchronized to the same observation window.
    //
    // No-op if the runtime isn't initialized or the backend isn't
    // a MiniaudioBackend.
    void reset_render_peak() {
        if (!runtime_) return;
        const auto* backend = runtime_->GetBackend();
        if (backend) {
            const auto* cmb =
                dynamic_cast<const audio::MiniaudioBackend*>(backend);
            if (cmb) {
                auto* mb = const_cast<audio::MiniaudioBackend*>(cmb);
                mb->ResetPeakSampleAbs();
            }
        }
        // v0.22.8.1: reset mixer peak through AudioRuntime forwarder,
        // not through GetMixer/const_cast as in the v0.22.8 first
        // attempt. The runtime forwarder is non-const-by-design
        // (Reset mutates state); no const_cast needed here.
        runtime_->ResetMasterPreGainPeak();
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
            // v0.18.0 / v0.20.0 network-API additions:
            case audio::AudioResult::DecodeError:       return "codec rejected the data";
            case audio::AudioResult::RateLimited:       return "replicated event rate-limited (token bucket empty)";
            case audio::AudioResult::PolicyViolation:   return "replicated event rejected by validator / replication policy";
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

    // ---- v0.14.0: native-Godot integration ----------------------------

    // Register a Godot AudioStream resource as a gool sound.
    //
    // Strategy: if the stream has a resource path (i.e. it was loaded
    // from disk via Godot's import pipeline), delegate to
    // register_sound_from_file with that path — gool's existing
    // decoder reads the original WAV/Ogg/FLAC/Opus bytes directly.
    // This is the 95% case: a user does
    //   var s = load("res://sfx/blip.ogg")
    //   Gool.register_sound_from_stream("blip", s)
    // and it Just Works without re-decoding through Godot's runtime.
    //
    // For procedural streams (no resource path), AudioStreamWAV's
    // raw PCM data is read directly out of its `data` property.
    // Other procedural subtypes (AudioStreamRandomizer, Polyphonic,
    // Generator) can't be reduced to a single PCM asset and the
    // binding refuses with a diagnostic.
    //
    // Returns the AudioSoundId on success, 0 on failure.
    int64_t register_sound_from_stream(const String&        name,
                                         const Ref<AudioStream>& stream) {
        if (!runtime_) {
            UtilityFunctions::push_error(
                "GoolAudioRuntime: register_sound_from_stream called before init");
            return 0;
        }
        if (stream.is_null()) {
            UtilityFunctions::push_error(
                String("GoolAudioRuntime: register_sound_from_stream '")
                + name + String("' received a null AudioStream"));
            return 0;
        }

        // Fast path: stream came from a file Godot's pipeline imported.
        // We delegate to register_sound_from_file, which reads the
        // ORIGINAL file via FileAccess (works in PCK-packaged builds)
        // and routes it through gool's decoder.
        const String path = stream->get_path();
        if (!path.is_empty()) {
            return register_sound_from_file(name, path);
        }

        // Procedural path: stream was constructed in code, no file
        // backing. Only AudioStreamWAV is supported here — it carries
        // its raw PCM bytes in the `data` property, which we can
        // re-marshal as WAV bytes and hand to the decoder.
        Ref<AudioStreamWAV> wav = stream;
        if (wav.is_valid()) {
            return register_wav_resource_as_pcm(name, wav);
        }

        // Unsupported procedural subtype.
        UtilityFunctions::push_warning(
            String("GoolAudioRuntime: register_sound_from_stream '")
            + name + String("' got an in-memory AudioStream of an unsupported ")
            + String("subtype. Only AudioStreamWAV is supported for procedural ")
            + String("streams; for AudioStreamRandomizer/Polyphonic, register ")
            + String("the constituent sounds individually. For procedural ")
            + String("Ogg/MP3 data without a resource path, use ")
            + String("register_sound_from_bytes() directly."));
        return 0;
    }

    // Helper for the procedural AudioStreamWAV case. AudioStreamWAV
    // exposes 16-bit signed integer PCM via its `data` PackedByteArray,
    // with `mix_rate`, `stereo`, and `format` describing the layout.
    // We convert to float32 in [-1, 1] and feed register_pcm_sound.
    //
    // Limitations:
    //   - Only FORMAT_16_BITS is supported. IMA-ADPCM and QOA would
    //     need decoders gool doesn't currently include in the
    //     procedural path. FORMAT_8_BITS is rare enough to leave for
    //     a future release.
    //   - Stream-loop metadata (loop_mode, loop_begin, loop_end) is
    //     not propagated to gool — gool's looping is configured per
    //     SoundDefinition. This matches the file-load path's
    //     behavior.
    int64_t register_wav_resource_as_pcm(const String& name,
                                           const Ref<AudioStreamWAV>& wav) {
        if (wav.is_null()) return 0;
        const PackedByteArray bytes = wav->get_data();
        if (bytes.size() == 0) {
            UtilityFunctions::push_error(
                String("GoolAudioRuntime: AudioStreamWAV '")
                + name + String("' has empty data"));
            return 0;
        }
        const int mix_rate = wav->get_mix_rate();
        const bool stereo  = wav->is_stereo();
        const int format   = static_cast<int>(wav->get_format());

        // AudioStreamWAV::Format::FORMAT_16_BITS == 1. Other values
        // (0=8_BITS, 2=IMA_ADPCM, 3=QOA) need decoders we don't carry.
        constexpr int FORMAT_16_BITS = 1;
        if (format != FORMAT_16_BITS) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: AudioStreamWAV '") + name
                + String("' uses format ") + String::num_int64(format)
                + String(", which the procedural path doesn't decode. ")
                + String("Re-import the source WAV with Compression=PCM ")
                + String("(16-bit), or save it to res:// and use ")
                + String("register_sound_from_stream / _from_file with a ")
                + String("file-backed stream."));
            return 0;
        }

        // Convert int16 → float32 in [-1, 1]. Stereo data is
        // interleaved L,R,L,R,... in the byte array, which matches
        // RegisterPcmSound's expected layout.
        const int channels = stereo ? 2 : 1;
        const int sample_count =
            static_cast<int>(bytes.size() / sizeof(int16_t));
        PackedFloat32Array samples;
        samples.resize(sample_count);
        const int16_t* src =
            reinterpret_cast<const int16_t*>(bytes.ptr());
        constexpr float kInv32768 = 1.0f / 32768.0f;
        for (int i = 0; i < sample_count; ++i) {
            // godot-cpp 4.x removed the `.write[i]` proxy; use ptrw() for
            // raw mutable access. PackedFloat32Array guarantees contiguous
            // storage so this is equivalent to the old idiom.
            samples.ptrw()[i] = static_cast<float>(src[i]) * kInv32768;
        }

        const audio::AudioSoundId id = HashName(name);
        runtime_->RegisterPcmSound(
            id,
            std::span<const float>(samples.ptr(),
                                     static_cast<size_t>(samples.size())),
            static_cast<uint32_t>(mix_rate),
            static_cast<uint32_t>(channels));
        return static_cast<int64_t>(id);
    }

    // Set the gain of a gool bus by name. The name is hashed and
    // resolved via FindBusIdByName, which is O(N) over kMaxBuses —
    // fine for the once-per-volume-change call frequency this is
    // meant for. Returns true on success, false if the bus name
    // doesn't exist in the engine's bus graph.
    bool set_bus_gain_db(const String& bus_name, double gain_db) {
        if (!runtime_) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: set_bus_gain_db unknown bus '")
                + bus_name + String("'. Check bus name spelling, or ")
                + String("inspect the bus graph via res://gool/config.json. ")
                + String("Default bus names are 'Master', 'SFX', 'Music', ")
                + String("'Voice', 'Ambient' depending on your config."));
            return false;
        }
        const auto rc = runtime_->SetBusGainDb(
            id, static_cast<float>(gain_db));
        return rc == audio::AudioResult::Success;
    }

    // Convenience wrapper for the most common case — adjusting the
    // master output level. Equivalent to set_bus_gain_db("Master", db).
    bool set_master_volume_db(double db) {
        return set_bus_gain_db(String("Master"), db);
    }

    // v0.27.0: per-bus mute / solo / effect-bypass setters.
    //
    // Same name → BusId resolution pattern as set_bus_gain_db. The
    // underlying engine APIs take BusId; the binding does the lookup
    // once per call (O(N) over kMaxBuses, ~16 buses max — negligible).
    //
    // Used by the mixer dock's S/M/B buttons via the EngineDebugger
    // editor↔game channel. The dock sends "gool:set_bus_mute" / "...solo"
    // / "...bypass" messages; the game-side handler in
    // addons/gool/runtime_singleton.gd::_on_debugger_capture routes
    // them to these methods.
    //
    // Returns true on success, false if the bus name doesn't exist or
    // the runtime isn't initialized. Unknown-bus warnings include the
    // same diagnostic hint as set_bus_gain_db.

    bool set_bus_muted(const String& bus_name, bool muted) {
        if (!runtime_) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: set_bus_muted unknown bus '")
                + bus_name + String("'. Check bus name in res://gool/config.json."));
            return false;
        }
        return runtime_->SetBusMuted(id, muted) == audio::AudioResult::Success;
    }

    bool set_bus_soloed(const String& bus_name, bool soloed) {
        if (!runtime_) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: set_bus_soloed unknown bus '")
                + bus_name + String("'. Check bus name in res://gool/config.json."));
            return false;
        }
        return runtime_->SetBusSoloed(id, soloed) == audio::AudioResult::Success;
    }

    bool set_bus_effects_bypassed(const String& bus_name, bool bypassed) {
        if (!runtime_) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: set_bus_effects_bypassed unknown bus '")
                + bus_name + String("'. Check bus name in res://gool/config.json."));
            return false;
        }
        return runtime_->SetBusEffectsBypassed(id, bypassed) == audio::AudioResult::Success;
    }

    // v0.28.0 (Phase 3.3c-1): live effect parameter set / read.
    //
    // set_effect_parameter routes through the existing AudioRuntime
    // command queue (5 ms ramp for gain-style params, immediate for
    // others). Same name → BusId resolution as set_bus_gain_db.
    // Param IDs are in EffectParameter::* (bus.h); GDScript-side
    // hosts should mirror those constants or pass the raw integers.
    bool set_effect_parameter(const String& bus_name,
                               int effect_index,
                               int param_id,
                               double value) {
        if (!runtime_) return false;
        if (effect_index < 0 || param_id < 0) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: set_effect_parameter unknown bus '")
                + bus_name + String("'. Check bus name in res://gool/config.json."));
            return false;
        }
        const auto rc = runtime_->SetEffectParameter(
            id,
            static_cast<uint32_t>(effect_index),
            static_cast<uint16_t>(param_id),
            static_cast<float>(value));
        return rc == audio::AudioResult::Success;
    }

    // get_bus_effects returns the full effect chain for a bus as an
    // Array of Dictionaries. Each entry:
    //   {
    //     "kind":      int    // EffectKind enum value (1..5)
    //     "kind_name": String // human-readable: "Gain", "Compressor", etc.
    //     "params":    Dict   // {param_id (int): current_value (float)}
    //   }
    //
    // The params dictionary contains only the parameter IDs the effect
    // type recognizes (e.g. a Compressor entry lists ThresholdDb,
    // Ratio, AttackMs, etc. but not Reverb_RoomSize). This means a
    // host (the upcoming 3.3c-2 dock UI) can iterate the keys without
    // a separate "what params does this effect type expose" call.
    //
    // Unknown bus → empty array + push_warning. Empty effect chain →
    // empty array (no warning).
    Array get_bus_effects(const String& bus_name) {
        Array out;
        if (!runtime_) return out;
        const auto utf8 = bus_name.utf8();
        const audio::BusId id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(),
                              static_cast<size_t>(utf8.length())));
        if (id == audio::kInvalidBusId) {
            UtilityFunctions::push_warning(
                String("GoolAudioRuntime: get_bus_effects unknown bus '")
                + bus_name + String("'. Check bus name in res://gool/config.json."));
            return out;
        }
        // FindBusIdByName returns BusId; we need the runtime-side index
        // to call the index-taking introspection APIs. Walk the bus
        // list to find the matching name. O(N), N ~= 16, called rarely.
        const uint32_t n = runtime_->GetBusCount();
        uint32_t busIdx = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i) {
            if (bus_name == String::utf8(runtime_->GetBusName(i))) {
                busIdx = i;
                break;
            }
        }
        if (busIdx == 0xFFFFFFFFu) return out;
        const uint32_t effectCount = runtime_->GetEffectCount(busIdx);
        for (uint32_t e = 0; e < effectCount; ++e) {
            const audio::EffectKind kind = runtime_->GetEffectKind(busIdx, e);
            Dictionary d;
            // v0.28.1: explicit int64_t cast + String wrap. godot-cpp's
            // Variant doesn't construct from `int` or `const char*` (only
            // int64_t and String), so the implicit conversions used in
            // v0.28.0 failed at the GDExtension link step. Match the
            // pattern of `d["parent"]` and `d["name"]` in get_bus_stats
            // above.
            d["kind"]      = static_cast<int64_t>(kind);
            d["kind_name"] = String(_gool_effect_kind_name(kind));
            // v0.64.0: short uppercase pill label for the mixer dock's
            // FX_BAND chain summary. See _gool_effect_kind_abbreviation
            // for the rationale; mirrored in config_model.gd's
            // KIND_INT_TO_ABBREV for the editor-time fallback path.
            d["kind_abbrev"] = String(_gool_effect_kind_abbreviation(kind));
            Dictionary params;
            // v0.28.2: runtime_ is std::unique_ptr<AudioRuntime>; the
            // helper takes a raw AudioRuntime*. .get() is required —
            // unique_ptr deliberately doesn't have an implicit
            // pointer conversion (MSVC C2664), which is the second
            // CI compile error in the v0.28.0 / v0.28.1 attempts.
            _gool_fill_params_for_kind(runtime_.get(), busIdx, e, kind, params);
            d["params"] = params;
            out.push_back(d);
        }
        return out;
    }

    void register_sound_definition(const String& name,
                                    bool spatialized, bool looping,
                                    double min_distance, double max_distance,
                                    double loop_crossfade_ms,
                                    int category,
                                    const String& target_bus_name,
                                    bool occlusion_enabled) {
        if (!runtime_) return;
        audio::SoundDefinition def;
        def.soundId           = HashName(name);
        def.spatialized       = spatialized;
        def.looping           = looping;
        // v0.31.0: per-sound occlusion opt-out. Default true (the
        // sound participates in geometry queries). Set false for
        // UI sounds, dialogue, narration, the player's own weapon
        // foley — anything where physical occlusion would compromise
        // readability or clarity. Music sounds already auto-opt-out
        // via music_channel.cpp.
        def.occlusionEnabled  = occlusion_enabled;
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

    // v0.32.0 (Phase 5.3): material-aware reverb preset lookup.
    // Returns the engine's per-material reverb preset (decay,
    // lf_damping, hf_damping, diffusion) for use by ReverbZone or
    // any host that wants to apply a material's acoustic character
    // to a reverb bus. Out-of-range material values fall through
    // to the Default preset (a balanced "average room"). Returns
    // a Dictionary because that's the friendly form for GDScript
    // consumers and matches the JSON authoring conventions for
    // reverb parameters in the bus config.
    Dictionary get_reverb_preset_for_material(int material) const {
        audio::AudioMaterial m = audio::AudioMaterial::Default;
        if (material >= 0
            && material < static_cast<int>(audio::kAudioMaterialCount)) {
            m = static_cast<audio::AudioMaterial>(material);
        }
        const audio::ReverbMaterialPreset p = audio::ReverbPresetByMaterial(m);
        Dictionary out;
        out["decay"]      = static_cast<double>(p.decay);
        out["lf_damping"] = static_cast<double>(p.lfDamping);
        out["hf_damping"] = static_cast<double>(p.hfDamping);
        out["diffusion"] = static_cast<double>(p.diffusion);
        return out;
    }

    // v0.33.0 (Phase 6.A): material EQ curve lookup. Returns the
    // engine's per-material 3-band EQ curve as a Dictionary —
    // low shelf, mid peaking band, high shelf, each with the
    // parameters needed to plug into a Biquad effect on a bus.
    //
    // Keys:
    //   low_gain_db    shelf gain below low_freq_hz (cut or boost)
    //   low_freq_hz    low shelf knee
    //   mid_gain_db    peaking band gain around mid_freq_hz
    //   mid_freq_hz    peaking band center
    //   mid_q          peaking band Q (sharpness)
    //   high_gain_db   shelf gain above high_freq_hz
    //   high_freq_hz   high shelf knee
    //   is_neutral     true if all three band gains are ~0 dB
    //                  (Air, Default — consumers should skip the
    //                  EQ stage entirely rather than spend cycles
    //                  on a no-op)
    //
    // The is_neutral flag is computed engine-side to keep the
    // designer-facing check trivial in GDScript ("if not
    // curve.is_neutral: apply_it()") without re-implementing the
    // epsilon comparison.
    Dictionary get_material_eq_for_material(int material) const {
        audio::AudioMaterial m = audio::AudioMaterial::Default;
        if (material >= 0
            && material < static_cast<int>(audio::kAudioMaterialCount)) {
            m = static_cast<audio::AudioMaterial>(material);
        }
        const audio::MaterialEqCurve c = audio::MaterialEqByMaterial(m);
        Dictionary out;
        out["low_gain_db"]  = static_cast<double>(c.lowGainDb);
        out["low_freq_hz"]  = static_cast<double>(c.lowFreqHz);
        out["mid_gain_db"]  = static_cast<double>(c.midGainDb);
        out["mid_freq_hz"]  = static_cast<double>(c.midFreqHz);
        out["mid_q"]        = static_cast<double>(c.midQ);
        out["high_gain_db"] = static_cast<double>(c.highGainDb);
        out["high_freq_hz"] = static_cast<double>(c.highFreqHz);
        out["is_neutral"]   = audio::MaterialEqIsNeutral(c);
        return out;
    }

    // v0.34.0 (Phase 6.B): push a material's EQ curve to the
    // first three biquads on a named bus. By convention from
    // cookbook section 14 those biquads are LowShelf / Peak /
    // HighShelf in that order; the gool runtime can't currently
    // introspect a biquad's *subtype* (GetEffectKind reports
    // "BiquadFilter" for all three), so this method trusts the
    // authoring contract and writes to indices 0, 1, 2.
    //
    // Designers wanting non-standard arrangements should call
    // set_effect_parameter directly with the curve values from
    // get_material_eq_for_material.
    //
    // Returns false if:
    //   - the bus doesn't exist
    //   - the first 3 effects on the bus aren't all biquads
    //     (the chain isn't shaped right — caller should warn the
    //     designer their bus config doesn't match the EQ contract)
    //
    // Neutral materials (Air / Default) still get written through
    // — the caller is responsible for checking is_neutral and
    // skipping the call if it wants to optimize. From this
    // method's perspective, writing 0 dB to all three biquads is
    // a valid "reset to flat" operation.
    bool apply_material_eq_to_bus(const String& bus_name, int material) {
        if (!runtime_) return false;
        const auto utf8 = bus_name.utf8();
        const audio::BusId bus_id = runtime_->FindBusIdByName(
            std::string_view(utf8.get_data(), utf8.length()));
        if (bus_id == audio::kInvalidBusId) {
            return false;
        }
        // Find the bus's index in the engine's bus array (for the
        // effect-introspection API which uses indices, not BusId
        // tokens). We mirror the same lookup that get_bus_effects
        // does.
        const uint32_t n = runtime_->GetBusCount();
        uint32_t bus_idx = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i) {
            if (bus_name == String::utf8(runtime_->GetBusName(i))) {
                bus_idx = i;
                break;
            }
        }
        if (bus_idx == 0xFFFFFFFFu) return false;

        // Verify the first 3 effects are biquads. We don't know
        // their subtypes (LowShelf vs Peak vs HighShelf) but we
        // can at least catch the case where someone wrote
        // Compressor first by mistake.
        const uint32_t effect_count = runtime_->GetEffectCount(bus_idx);
        if (effect_count < 3) return false;
        for (uint32_t i = 0; i < 3; ++i) {
            if (runtime_->GetEffectKind(bus_idx, i)
                != audio::EffectKind::BiquadFilter) {
                return false;
            }
        }

        // Look up the curve and push.
        audio::AudioMaterial m = audio::AudioMaterial::Default;
        if (material >= 0
            && material < static_cast<int>(audio::kAudioMaterialCount)) {
            m = static_cast<audio::AudioMaterial>(material);
        }
        const audio::MaterialEqCurve c = audio::MaterialEqByMaterial(m);

        namespace EP = audio::EffectParameter;
        // LowShelf at index 0: cutoff_hz, gain_db. Q stays at its
        // authored value (transition steepness — designer's call).
        runtime_->SetEffectParameter(bus_id, 0, EP::Biquad_CutoffHz, c.lowFreqHz);
        runtime_->SetEffectParameter(bus_id, 0, EP::Biquad_GainDb,   c.lowGainDb);
        // Peak at index 1: cutoff_hz (center), Q (sharpness),
        // gain_db.
        runtime_->SetEffectParameter(bus_id, 1, EP::Biquad_CutoffHz, c.midFreqHz);
        runtime_->SetEffectParameter(bus_id, 1, EP::Biquad_Q,        c.midQ);
        runtime_->SetEffectParameter(bus_id, 1, EP::Biquad_GainDb,   c.midGainDb);
        // HighShelf at index 2: cutoff_hz, gain_db.
        runtime_->SetEffectParameter(bus_id, 2, EP::Biquad_CutoffHz, c.highFreqHz);
        runtime_->SetEffectParameter(bus_id, 2, EP::Biquad_GainDb,   c.highGainDb);
        return true;
    }

    // v0.59.3 (Phase 6.E.1 audition): take a mono float buffer in, run
    // it through a three-biquad chain configured for the given
    // material's EQ curve at the given intensity, return the processed
    // buffer. Used by the editor inspector's "Audition" button so
    // designers can hear what each material does without booting an
    // F5 game session.
    //
    // The output is bit-identical to what the runtime impact-EQ /
    // listener-EQ paths produce when fed the same input, because this
    // method uses the same BiquadFilterEffect class with the same RBJ
    // cookbook coefficients that the runtime uses. The only thing this
    // doesn't share with the runtime is the bus chain topology (no
    // mix-down from other sources, no sidechain ducking, etc.) — by
    // design, since the audition is a pure preview of the EQ stage in
    // isolation.
    //
    // STATIC by design: the audition is pure math over stack-local
    // BiquadFilterEffect instances and the engine's per-material EQ
    // table. It doesn't read or modify any GoolAudioRuntime instance
    // state, doesn't touch the audio device, doesn't need init() to
    // have been called. Making it static lets the editor inspector
    // call it as `GoolAudioRuntime.process_buffer_through_material_eq`
    // without needing the /root/Gool autoload to be reachable — which
    // it isn't, since runtime_singleton.gd is not @tool and the
    // autoload only spins up in F5 game sessions, not the editor's
    // own SceneTree.
    //
    // Inputs:
    //   buffer    : a PackedFloat32Array of mono input samples
    //               (typically pink noise generated client-side in
    //               GDScript). Length is the audition duration in
    //               samples. The method processes the buffer in-place
    //               conceptually but returns a new array to avoid
    //               mutating the caller's data.
    //   material  : the AudioMaterial int (same range / semantics as
    //               get_material_eq_for_material).
    //   intensity : the realism-intensity multiplier applied to all
    //               three band gains (matching the runtime path
    //               _apply_scaled_material_eq_to_bus). Defaults to 1.0.
    //   sample_rate : 48000 by default; the BiquadFilterEffect computes
    //               its coefficients against this. Should match
    //               whatever the inspector then plays the buffer back
    //               at to keep the audible result honest.
    //
    // Returns: a PackedFloat32Array of the same length as input.
    // Returns an empty array on invalid material or empty input.
    //
    // Performance: ~3 multiplies per sample per biquad × 3 biquads =
    // ~9 multiplies/sample, plus housekeeping. At a 1 s 48 kHz buffer
    // that's ~430k multiplies — sub-millisecond on any modern host.
    // Called once per Audition button press, never on a render-thread
    // hot path.
    //
    // v0.61.0: refactored to a thin marshaler over the new public
    // engine surface ProcessBufferThroughMaterialEq() in
    // audio_engine/material_eq.h. Previously the binding constructed
    // BiquadFilterEffect instances here, reaching into the engine's
    // private dsp/biquad_filter.h. That broke gdextension CI in
    // v0.60.0 because the binding target didn't have the engine's
    // src/ on its include path. v0.61.0 puts the math on the proper
    // side of the public API boundary; the binding stays a pure
    // consumer of public headers.
    static PackedFloat32Array process_buffer_through_material_eq(
            const PackedFloat32Array& buffer,
            int material,
            double intensity,
            int sample_rate) {
        PackedFloat32Array out;
        const int frames = buffer.size();
        if (frames <= 0) return out;
        if (sample_rate <= 0) sample_rate = 48000;

        // Resolve material to the engine's AudioMaterial enum. Out-of-
        // range ints get clamped to Default here at the binding edge;
        // the engine surface also handles out-of-range gracefully but
        // we want a single explicit boundary check.
        audio::AudioMaterial m = audio::AudioMaterial::Default;
        if (material >= 0
            && material < static_cast<int>(audio::kAudioMaterialCount)) {
            m = static_cast<audio::AudioMaterial>(material);
        }

        // Copy input into out (engine API is in-place), then run the
        // canonical audition function. This is the same code path the
        // runtime impact-EQ and listener-EQ paths use, so what the
        // designer hears matches what F5 will produce.
        out.resize(frames);
        float* dst = out.ptrw();
        const float* src = buffer.ptr();
        for (int i = 0; i < frames; ++i) dst[i] = src[i];

        audio::ProcessBufferThroughMaterialEq(
            dst,
            static_cast<std::size_t>(frames),
            m,
            static_cast<float>(intensity),
            static_cast<std::uint32_t>(sample_rate));
        return out;
    }

    // v0.60.0 (Phase 6.E.1 — Option B audition): same as
    // process_buffer_through_material_eq() above, but takes raw
    // curve parameters instead of looking up by material int.
    // Used by the inspector when auditioning a GoolAudioMaterial
    // with override_enabled=true — the override values are passed
    // directly without going through the engine table.
    //
    // Same biquad code path (LowShelf → Peak → HighShelf with RBJ
    // cookbook coefficients), same static-method design (no
    // GoolAudioRuntime instance needed, reachable from editor
    // context without the /root/Gool autoload).
    //
    // The intensity multiplier is applied to all three band gains
    // here too, for consistency with the by-material path. Caller
    // typically passes 1.0 (intensity is already baked into the
    // per-band gain values when the inspector is auditioning an
    // override curve), but the scaling is available for cases
    // where someone wants to layer the global intensity on top of
    // the override.
    //
    // All inputs are floats so the binding is straightforward;
    // shelf Q is fixed at 1.0 to match the runtime impact / listener
    // chains (which don't expose shelf Q in their authoring contract).
    static PackedFloat32Array process_buffer_through_curve(
            const PackedFloat32Array& buffer,
            double low_freq_hz, double low_gain_db,
            double mid_freq_hz, double mid_gain_db, double mid_q,
            double high_freq_hz, double high_gain_db,
            double intensity,
            int sample_rate) {
        PackedFloat32Array out;
        const int frames = buffer.size();
        if (frames <= 0) return out;
        if (sample_rate <= 0) sample_rate = 48000;

        // Pack the inspector's per-band fields into the engine's
        // MaterialEqCurve struct, then call the canonical audition
        // function. Shelf Q is not in the struct (the runtime impact
        // and listener chains fix it at 1.0); the engine surface
        // handles that internally.
        //
        // v0.61.0: refactored to a thin marshaler over
        // ProcessBufferThroughMaterialEqCurve() in
        // audio_engine/material_eq.h. See the by-material method
        // above for the rationale.
        audio::MaterialEqCurve curve;
        curve.lowFreqHz  = static_cast<float>(low_freq_hz);
        curve.lowGainDb  = static_cast<float>(low_gain_db);
        curve.midFreqHz  = static_cast<float>(mid_freq_hz);
        curve.midGainDb  = static_cast<float>(mid_gain_db);
        curve.midQ       = static_cast<float>(mid_q);
        curve.highFreqHz = static_cast<float>(high_freq_hz);
        curve.highGainDb = static_cast<float>(high_gain_db);

        out.resize(frames);
        float* dst = out.ptrw();
        const float* src = buffer.ptr();
        for (int i = 0; i < frames; ++i) dst[i] = src[i];

        audio::ProcessBufferThroughMaterialEqCurve(
            dst,
            static_cast<std::size_t>(frames),
            curve,
            static_cast<float>(intensity),
            static_cast<std::uint32_t>(sample_rate));
        return out;
    }

    // v0.31.0 (Phase 5.2): live occlusion controls. Both safe to call
    // at any time; the next OcclusionSystem::Update tick picks up the
    // new value with the standard ~150 ms smoother.
    void set_occlusion_enabled(bool enabled) {
        if (!runtime_) return;
        runtime_->SetOcclusionEnabled(enabled);
    }

    void set_occlusion_intensity(double intensity) {
        if (!runtime_) return;
        runtime_->SetOcclusionIntensity(static_cast<float>(intensity));
    }

    // Push the current World3D's space RID into the geometry query.
    // Called by GoolListener3D when set_current(true) fires. Without
    // a valid space RID the geometry query reports no hit (treating
    // every sound as unobstructed), which is the safe fallback for
    // scenes that don't have a GoolListener3D in the tree yet.
    void set_audio_world_space_rid(const RID& rid) {
        if (geometry_query_ == nullptr) return;
        geometry_query_->SetSpaceRID(rid);
    }

    bool load_sound_bank_from_json(const String& json_string,
                                     const String& gpak_path,
                                     bool           skip_validation) {
        if (!runtime_ || !bank_) return false;
        audio::SoundBankLoadOptions opts;
        // skip_validation=true is for the "group-only" authoring
        // pattern: a JSON bank that declares groups whose members
        // are sounds the runtime knows about from outside the bank
        // (programmatic registration via register_pcm_sound /
        // register_sound_from_stream). With validation off, the
        // bank's pass-3 resolver hashes unknown member names rather
        // than dropping them, so the groups resolve against the
        // runtime's existing registrations. See docs/asset_pipeline.md
        // "Group-only JSON banks" for the designer-facing pattern.
        opts.validateReferences = !skip_validation;
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

    // Material-aware variant for impact sounds and similar
    // surface-dependent triggers. Mirrors play_sound_at_location but
    // passes the AudioMaterial through to the bank's by_material
    // lookup. Out-of-range material values fall through to Default;
    // the bank itself enforces the lenient rule for missing buckets.
    void play_sound_at_location_for_material(const String& name,
                                              const Vector3& position,
                                              int            material) {
        if (!runtime_) return;
        audio::AudioMaterial mat = audio::AudioMaterial::Default;
        if (material >= 0 &&
            material < static_cast<int>(audio::kAudioMaterialCount)) {
            mat = static_cast<audio::AudioMaterial>(material);
        }
        audio::AudioSoundId id = audio::kInvalidSoundId;
        if (bank_) {
            const auto utf8 = name.utf8();
            id = bank_->Find(
                std::string_view(utf8.get_data(), utf8.length()),
                mat);
        }
        // Note: no HashName fallback here — material-aware playback
        // only makes sense against a bank-registered group. If the
        // bank doesn't recognize the name, return without playing
        // anything (the lenient rule applies up the stack).
        if (id == audio::kInvalidSoundId) return;
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

        // v0.25.2: look up the registered SoundDefinition for this
        // sound (if any) and inherit routing metadata from it:
        // - category    → drives which bus the emitter routes to
        //                 via AudioConfig::categoryBusMap (the
        //                 "category_routing" section of config.json)
        // - targetBus   → explicit bus override; non-Invalid wins
        //                 over category routing
        // - spatialized → whether the engine applies 3D
        //                 position-based gain/pan. False for global
        //                 sounds like music drones.
        // - attenuation → minDistance / maxDistance / falloff curve
        //
        // Hosts that don't call register_sound_definition fall through
        // to the EmitterDescriptor struct defaults (category=SFX,
        // spatialized=true). This matches the pre-v0.25.2 behavior
        // for those hosts. The change is: hosts that DO call
        // register_sound_definition now get their metadata applied,
        // instead of being silently ignored (the bug that misrouted
        // the sandbox drone to Sfx through v0.25.1).
        //
        // The `looping` and `fade_in_ms` call-site parameters still
        // win over any SoundDefinition values — explicit args from
        // the caller have priority. SoundDefinition only fills in
        // routing/spatial metadata that create_emitter doesn't expose
        // as parameters.
        const audio::SoundDefinition* def =
            runtime_->GetSoundDefinition(desc.soundId);
        if (def != nullptr) {
            desc.category      = def->category;
            desc.targetBus     = def->targetBus;
            desc.isSpatialized = def->spatialized;
            desc.attenuation   = def->attenuation;
        } else {
            // No SoundDefinition registered; preserve pre-v0.25.2
            // behavior. The struct defaults already give SFX
            // category, so we only need to set the things the
            // old code set explicitly.
            desc.isSpatialized = true;
        }

        desc.position      = V3(position);
        desc.isLooping     = looping;
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

    // ---- 2.4 voice-source mute / volume ------------------------------
    // Returns true on success. False if no source is registered for
    // that player_id or the runtime isn't initialized.

    bool set_voice_source_muted(int64_t player_id, bool muted) {
        if (!runtime_) return false;
        return runtime_->SetVoiceSourceMuted(
                   static_cast<audio::AudioPlayerId>(player_id), muted)
                == audio::AudioResult::Success;
    }

    bool set_voice_source_volume(int64_t player_id, double volume) {
        if (!runtime_) return false;
        return runtime_->SetVoiceSourceVolume(
                   static_cast<audio::AudioPlayerId>(player_id),
                   static_cast<float>(volume))
                == audio::AudioResult::Success;
    }

    // ---- 2.6 outbound bandwidth budget -------------------------------

    bool set_voice_bandwidth_budget(int64_t player_id, int64_t bytes_per_sec) {
        if (!runtime_) return false;
        if (bytes_per_sec < 0) bytes_per_sec = 0;
        return runtime_->SetVoiceBandwidthBudget(
                   static_cast<audio::AudioPlayerId>(player_id),
                   static_cast<uint32_t>(bytes_per_sec))
                == audio::AudioResult::Success;
    }

    // Returns suggested bitrate in bps (32000 / 24000 / 16000) or 0
    // for "drop this frame entirely." Returns 32000 if the runtime
    // isn't initialized (fail-open: don't accidentally silence the
    // mic because of a runtime hiccup).
    int64_t suggest_voice_bitrate(int64_t player_id, int64_t frame_duration_ms) {
        if (!runtime_) return 32000;
        if (frame_duration_ms <= 0) return 32000;
        return static_cast<int64_t>(runtime_->SuggestVoiceBitrate(
            static_cast<audio::AudioPlayerId>(player_id),
            static_cast<uint32_t>(frame_duration_ms)));
    }

    bool report_voice_bytes_sent(int64_t player_id, int64_t bytes,
                                   int64_t bitrate_used_bps) {
        if (!runtime_) return false;
        if (bytes < 0) bytes = 0;
        return runtime_->ReportVoiceBytesSent(
                   static_cast<audio::AudioPlayerId>(player_id),
                   static_cast<uint32_t>(bytes),
                   static_cast<int32_t>(bitrate_used_bps))
                == audio::AudioResult::Success;
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
    // v0.31.0: non-owning observer into the engine-owned occlusion
    // geometry query (the engine moves the unique_ptr into its
    // AudioRuntimeDependencies during Initialize). Used by
    // set_audio_world_space_rid to push the current World3D's space
    // RID in after a GoolListener3D becomes current.
    GodotGeometryQuery*                   geometry_query_ = nullptr;
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
