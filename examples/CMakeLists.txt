add_executable(audio_engine_minimal cpp/minimal/main.cpp)
target_link_libraries(audio_engine_minimal PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_ducking cpp/ducking/main.cpp)
target_link_libraries(audio_engine_ducking PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_multi_tier_ducking cpp/multi_tier_ducking/main.cpp)
target_link_libraries(audio_engine_multi_tier_ducking PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_streaming cpp/streaming/main.cpp)
target_link_libraries(audio_engine_streaming PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_sound_bank cpp/sound_bank/main.cpp)
target_link_libraries(audio_engine_sound_bank PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_music_crossfade cpp/music_crossfade/main.cpp)
target_link_libraries(audio_engine_music_crossfade PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_telemetry cpp/telemetry/main.cpp)
target_link_libraries(audio_engine_telemetry PRIVATE audio_engine::audio_engine)

if(AUDIO_ENGINE_BACKEND_MINIAUDIO)
    add_executable(audio_engine_playback cpp/playback/main.cpp)
    target_link_libraries(audio_engine_playback PRIVATE audio_engine::audio_engine)
    add_executable(audio_engine_hello_audio cpp/hello_audio/main.cpp)
    target_link_libraries(audio_engine_hello_audio PRIVATE audio_engine::audio_engine)
endif()
