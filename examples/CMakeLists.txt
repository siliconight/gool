add_executable(audio_engine_minimal minimal/main.cpp)
target_link_libraries(audio_engine_minimal PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_ducking ducking/main.cpp)
target_link_libraries(audio_engine_ducking PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_multi_tier_ducking multi_tier_ducking/main.cpp)
target_link_libraries(audio_engine_multi_tier_ducking PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_streaming streaming/main.cpp)
target_link_libraries(audio_engine_streaming PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_sound_bank sound_bank/main.cpp)
target_link_libraries(audio_engine_sound_bank PRIVATE audio_engine::audio_engine)

add_executable(audio_engine_music_crossfade music_crossfade/main.cpp)
target_link_libraries(audio_engine_music_crossfade PRIVATE audio_engine::audio_engine)

if(AUDIO_ENGINE_BACKEND_MINIAUDIO)
    add_executable(audio_engine_playback playback/main.cpp)
    target_link_libraries(audio_engine_playback PRIVATE audio_engine::audio_engine)
    add_executable(audio_engine_hello_audio hello_audio/main.cpp)
    target_link_libraries(audio_engine_hello_audio PRIVATE audio_engine::audio_engine)
endif()
