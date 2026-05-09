cmake_minimum_required(VERSION 3.20)
project(gool_godot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS         OFF)

# Build the engine itself as a static lib alongside this binding.
add_subdirectory(.. ${CMAKE_BINARY_DIR}/audio_engine)

# godot-cpp must be supplied via -DGODOT_CPP_PATH=... — we don't
# vendor it because it's >100MB and tracks Godot's own release
# cadence. See https://github.com/godotengine/godot-cpp.
if(NOT DEFINED GODOT_CPP_PATH)
    message(FATAL_ERROR
        "Set GODOT_CPP_PATH to a built godot-cpp checkout, e.g.\n"
        "  cmake -S godot -B build -DGODOT_CPP_PATH=/path/to/godot-cpp")
endif()

# godot-cpp builds its own static lib; we link to its target.
add_subdirectory(${GODOT_CPP_PATH} ${CMAKE_BINARY_DIR}/godot-cpp)

# The shared library Godot loads at runtime.
add_library(gool_godot SHARED src/gool_godot.cpp)

target_link_libraries(gool_godot
    PRIVATE
        audio_engine::audio_engine
        godot-cpp)

set_target_properties(gool_godot PROPERTIES
    POSITION_INDEPENDENT_CODE  ON
    CXX_VISIBILITY_PRESET      hidden
    VISIBILITY_INLINES_HIDDEN  ON
    PREFIX                     "lib"     # libgool_godot.so / .dylib
)

# On Windows, the prefix is empty and extension is .dll — the
# default Godot expects.
if(WIN32)
    set_target_properties(gool_godot PROPERTIES PREFIX "")
endif()
