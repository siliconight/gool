add_executable(gpak_create gpak_create/main.cpp)
target_link_libraries(gpak_create PRIVATE audio_engine::audio_engine)
