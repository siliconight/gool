add_executable(audio_engine_spsc_ring_test                unit/spsc_ring_test.cpp)
add_executable(audio_engine_slot_map_test                 unit/slot_map_test.cpp)
add_executable(audio_engine_bus_graph_test                unit/bus_graph_test.cpp)
add_executable(audio_engine_compressor_test               unit/compressor_test.cpp)
add_executable(audio_engine_compressor_profile_test       unit/compressor_profile_test.cpp)
add_executable(audio_engine_saturation_test               unit/saturation_test.cpp)
add_executable(audio_engine_saturation_profile_test       unit/saturation_profile_test.cpp)
add_executable(audio_engine_bus_config_loader_test        unit/bus_config_loader_test.cpp)
add_executable(audio_engine_decoder_test                  unit/decoder_test.cpp)
add_executable(audio_engine_priority_eviction_test        unit/priority_eviction_test.cpp)
add_executable(audio_engine_occlusion_lpf_test            unit/occlusion_lpf_test.cpp)
add_executable(audio_engine_doppler_smoothing_test        unit/doppler_smoothing_test.cpp)
add_executable(audio_engine_air_absorption_test           unit/air_absorption_test.cpp)
add_executable(audio_engine_reverb_send_test              unit/reverb_send_test.cpp)
add_executable(audio_engine_voice_codec_test              unit/voice_codec_test.cpp)
add_executable(audio_engine_binaural_spatializer_test     unit/binaural_spatializer_test.cpp)
add_executable(audio_engine_production_readiness_test    unit/production_readiness_test.cpp)
add_executable(audio_engine_multiplayer_readiness_test   unit/multiplayer_readiness_test.cpp)
add_executable(audio_engine_jitter_buffer_test           unit/jitter_buffer_test.cpp)
add_executable(audio_engine_voice_telemetry_test         unit/voice_telemetry_test.cpp)
add_executable(audio_engine_voice_mute_budget_test       unit/voice_mute_budget_test.cpp)
add_executable(audio_engine_sound_bank_test              unit/sound_bank_test.cpp)
add_executable(audio_engine_crossfade_test               unit/crossfade_test.cpp)
add_executable(audio_engine_loop_crossfade_test          unit/loop_crossfade_test.cpp)
add_executable(audio_engine_biquad_eq_test               unit/biquad_eq_test.cpp)
add_executable(audio_engine_gpak_test                    unit/gpak_test.cpp)
add_executable(audio_engine_determinism_test             unit/determinism_test.cpp)
add_executable(audio_engine_replicated_events_test       unit/replicated_events_test.cpp)
add_executable(audio_engine_replication_rate_limit_test  unit/replication_rate_limit_test.cpp)
add_executable(audio_engine_default_bounds_validator_test unit/default_bounds_validator_test.cpp)
add_executable(audio_engine_version_test                  unit/version_test.cpp)
add_executable(audio_engine_memory_budget_test            unit/memory_budget_test.cpp)
add_executable(audio_engine_aligned_float_buffer_test     unit/aligned_float_buffer_test.cpp)
add_executable(audio_engine_material_occlusion_test       unit/material_occlusion_test.cpp)
add_executable(audio_engine_material_eq_curve_test        unit/material_eq_curve_test.cpp)
# v0.59.3 (Phase 6.E.1 audition): offline DSP audition of a sample
# buffer through a material's EQ curve. Verifies the audition path
# uses the same BiquadFilterEffect class as the runtime impact /
# listener EQ paths so the audition can never lie about what the
# runtime sounds like.
add_executable(audio_engine_material_eq_audition_test     unit/material_eq_audition_test.cpp)
add_executable(audio_engine_master_control_test           unit/master_control_test.cpp)
add_executable(audio_engine_thread_priority_test          unit/thread_priority_test.cpp)
add_executable(audio_engine_global_parameter_test         unit/global_parameter_test.cpp)
add_executable(audio_engine_sound_rtpc_test               unit/sound_rtpc_test.cpp)
add_executable(audio_engine_telemetry_test                unit/telemetry_test.cpp)
add_executable(audio_engine_logging_test                  unit/logging_test.cpp)
add_executable(audio_engine_integration_kitchen_sink_test unit/integration_kitchen_sink_test.cpp)

# Tests need access to internal headers (src/) for util classes that aren't
# part of the public API.
foreach(_t spsc_ring slot_map bus_graph compressor compressor_profile saturation saturation_profile decoder priority_eviction occlusion_lpf doppler_smoothing air_absorption reverb_send voice_codec binaural_spatializer production_readiness multiplayer_readiness jitter_buffer voice_telemetry voice_mute_budget biquad_eq integration_kitchen_sink aligned_float_buffer material_occlusion material_eq_curve material_eq_audition)
    target_include_directories(audio_engine_${_t}_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src)
endforeach()

# Tests that exercise compiled implementations link against the library.
# Header-only tests (spsc_ring, slot_map) don't need to.
target_link_libraries(audio_engine_spsc_ring_test                 PRIVATE Threads::Threads)
target_link_libraries(audio_engine_bus_graph_test                 PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_compressor_test                PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_compressor_profile_test        PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_saturation_test                PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_saturation_profile_test        PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_bus_config_loader_test         PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_decoder_test                   PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_priority_eviction_test         PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_occlusion_lpf_test             PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_doppler_smoothing_test         PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_air_absorption_test            PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_reverb_send_test               PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_voice_codec_test               PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_binaural_spatializer_test      PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_production_readiness_test     PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_multiplayer_readiness_test    PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_jitter_buffer_test            PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_voice_telemetry_test          PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_voice_mute_budget_test        PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_sound_bank_test               PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_crossfade_test                PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_loop_crossfade_test           PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_biquad_eq_test                PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_gpak_test                     PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_determinism_test              PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_replicated_events_test        PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_replication_rate_limit_test   PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_default_bounds_validator_test  PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_version_test                   PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_memory_budget_test             PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_aligned_float_buffer_test      PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_material_occlusion_test        PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_material_eq_curve_test         PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_material_eq_audition_test      PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_master_control_test            PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_thread_priority_test           PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_global_parameter_test          PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_sound_rtpc_test                PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_telemetry_test                 PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_logging_test                   PRIVATE audio_engine::audio_engine)
target_link_libraries(audio_engine_integration_kitchen_sink_test  PRIVATE audio_engine::audio_engine)

add_test(NAME spsc_ring_test                COMMAND audio_engine_spsc_ring_test)
add_test(NAME slot_map_test                 COMMAND audio_engine_slot_map_test)
add_test(NAME bus_graph_test                COMMAND audio_engine_bus_graph_test)
add_test(NAME compressor_test               COMMAND audio_engine_compressor_test)
add_test(NAME compressor_profile_test       COMMAND audio_engine_compressor_profile_test)
add_test(NAME saturation_test               COMMAND audio_engine_saturation_test)
add_test(NAME saturation_profile_test       COMMAND audio_engine_saturation_profile_test)
add_test(NAME bus_config_loader_test        COMMAND audio_engine_bus_config_loader_test)
add_test(NAME decoder_test                  COMMAND audio_engine_decoder_test)
add_test(NAME priority_eviction_test        COMMAND audio_engine_priority_eviction_test)
add_test(NAME occlusion_lpf_test            COMMAND audio_engine_occlusion_lpf_test)
add_test(NAME doppler_smoothing_test        COMMAND audio_engine_doppler_smoothing_test)
add_test(NAME air_absorption_test           COMMAND audio_engine_air_absorption_test)
add_test(NAME reverb_send_test              COMMAND audio_engine_reverb_send_test)
add_test(NAME voice_codec_test              COMMAND audio_engine_voice_codec_test)
add_test(NAME binaural_spatializer_test     COMMAND audio_engine_binaural_spatializer_test)
add_test(NAME production_readiness_test     COMMAND audio_engine_production_readiness_test)
add_test(NAME multiplayer_readiness_test    COMMAND audio_engine_multiplayer_readiness_test)
add_test(NAME jitter_buffer_test            COMMAND audio_engine_jitter_buffer_test)
add_test(NAME voice_telemetry_test          COMMAND audio_engine_voice_telemetry_test)
add_test(NAME voice_mute_budget_test        COMMAND audio_engine_voice_mute_budget_test)
add_test(NAME sound_bank_test               COMMAND audio_engine_sound_bank_test)
add_test(NAME crossfade_test                COMMAND audio_engine_crossfade_test)
add_test(NAME loop_crossfade_test           COMMAND audio_engine_loop_crossfade_test)
add_test(NAME biquad_eq_test                COMMAND audio_engine_biquad_eq_test)
add_test(NAME gpak_test                     COMMAND audio_engine_gpak_test)
add_test(NAME determinism_test              COMMAND audio_engine_determinism_test)
add_test(NAME replicated_events_test        COMMAND audio_engine_replicated_events_test)
add_test(NAME replication_rate_limit_test   COMMAND audio_engine_replication_rate_limit_test)
add_test(NAME default_bounds_validator_test COMMAND audio_engine_default_bounds_validator_test)
add_test(NAME version_test                  COMMAND audio_engine_version_test)
add_test(NAME memory_budget_test            COMMAND audio_engine_memory_budget_test)
add_test(NAME aligned_float_buffer_test     COMMAND audio_engine_aligned_float_buffer_test)
add_test(NAME material_occlusion_test       COMMAND audio_engine_material_occlusion_test)
add_test(NAME material_eq_curve_test        COMMAND audio_engine_material_eq_curve_test)
add_test(NAME material_eq_audition_test     COMMAND audio_engine_material_eq_audition_test)
add_test(NAME master_control_test           COMMAND audio_engine_master_control_test)
add_test(NAME thread_priority_test          COMMAND audio_engine_thread_priority_test)
add_test(NAME global_parameter_test         COMMAND audio_engine_global_parameter_test)
add_test(NAME sound_rtpc_test               COMMAND audio_engine_sound_rtpc_test)
add_test(NAME telemetry_test                 COMMAND audio_engine_telemetry_test)
add_test(NAME logging_test                   COMMAND audio_engine_logging_test)
add_test(NAME integration_kitchen_sink_test COMMAND audio_engine_integration_kitchen_sink_test)

# -----------------------------------------------------------------------------
# Benchmarks (deliberately NOT registered with add_test).
#
# Per Rule 9 ("measure before optimizing") and Rule 25 ("benchmark
# critical systems") of the architecture rubric, these are the
# baseline measurement tools for the engine's hot paths. They are
# not pass/fail tests; running them produces a table of timings
# the developer reads. Build with the rest of the project, run
# manually:
#
#   ./build/tests/audio_engine_parameter_smoother_bench
#   ./build/tests/audio_engine_rtpc_eval_bench
#   ./build/tests/audio_engine_audio_mixer_bench
#
# Captured baseline numbers live in docs/perf.md. Future
# optimization passes should compare against those.
add_executable(audio_engine_parameter_smoother_bench bench/parameter_smoother_bench.cpp)
target_link_libraries(audio_engine_parameter_smoother_bench PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_rtpc_eval_bench bench/rtpc_eval_bench.cpp)
target_link_libraries(audio_engine_rtpc_eval_bench PRIVATE audio_engine::audio_engine)

# v0.20.1: mixer hot-path bench. Drives the public OnRender + PostCommand
# surface to measure DrainCommands and MixVoiceSound_ (both private)
# under realistic per-voice load. Establishes the v0.21 "before" baseline
# for the planned decomposition of those two functions.
add_executable(audio_engine_audio_mixer_bench bench/audio_mixer_bench.cpp)
target_link_libraries(audio_engine_audio_mixer_bench PRIVATE audio_engine::audio_engine)

# Benches reach into the same private headers as their unit-test
# counterparts (parameter_smoother is in src/audio_engine/orchestrator/,
# the RTPC eval bench drives the runtime's internal RTPC graph). This
# foreach must come AFTER the add_executable lines above — earlier in
# v0.11.15 it was placed near the test foreach and the if(TARGET ...)
# guard silently no-op'd because CMake hadn't seen the target yet.
foreach(_b parameter_smoother_bench rtpc_eval_bench audio_mixer_bench)
    target_include_directories(audio_engine_${_b} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src)
endforeach()
