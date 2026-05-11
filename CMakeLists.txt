cmake_minimum_required(VERSION 3.20)
project(audio_engine
    VERSION 0.12.1
    DESCRIPTION "Runtime 3D Spatial Multiplayer Audio Engine"
    LANGUAGES CXX)

# Capture the current git commit SHA (short form) so it can be
# compiled into the library and surfaced via audio::GetVersion().
# Falls back to "unknown" when not in a git checkout.
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE AUDIO_ENGINE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT AUDIO_ENGINE_GIT_COMMIT)
    set(AUDIO_ENGINE_GIT_COMMIT "unknown")
endif()
message(STATUS "audio_engine: version ${PROJECT_VERSION} (commit ${AUDIO_ENGINE_GIT_COMMIT})")

# Sanity check: the version constants in include/audio_engine/version.h
# must match the project() VERSION above. The header is the runtime
# source of truth (compile-time constants); the project() declaration
# drives release artifact filenames. RELEASING.md documents the
# dual-edit requirement.

# Detect whether we are the top-level project (controls examples/tests by default).
if(NOT DEFINED AUDIO_ENGINE_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(AUDIO_ENGINE_TOP_LEVEL ON)
    else()
        set(AUDIO_ENGINE_TOP_LEVEL OFF)
    endif()
endif()

option(AUDIO_ENGINE_SHARED   "Build audio_engine as a shared library"  OFF)
option(AUDIO_ENGINE_BUILD_EXAMPLES "Build examples" ${AUDIO_ENGINE_TOP_LEVEL})
option(AUDIO_ENGINE_BUILD_TESTS    "Build unit tests" ${AUDIO_ENGINE_TOP_LEVEL})
option(AUDIO_ENGINE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(AUDIO_ENGINE_BACKEND_MINIAUDIO "Build the miniaudio backend (cross-platform device I/O). Required for the audible examples (playback, hello_audio); the core library and tests build without it." ON)
option(AUDIO_ENGINE_FETCH_MINIAUDIO   "Fetch miniaudio via FetchContent if not vendored locally" ON)

# File-format decoders. Each is independent and off by default to keep the
# core library dependency-free. When ON, the corresponding decoder TU pulls
# in its single-header library (dr_wav / stb_vorbis / dr_flac) located either
# under third_party/ or fetched via FetchContent.
option(AUDIO_ENGINE_DECODERS_WAV   "Compile WAV decoder (uses dr_wav)"        ON)
option(AUDIO_ENGINE_DECODERS_OGG   "Compile Ogg Vorbis decoder (uses stb_vorbis)" ON)
option(AUDIO_ENGINE_DECODERS_FLAC  "Compile FLAC decoder (uses dr_flac)"      ON)
option(AUDIO_ENGINE_DECODERS_OPUS  "Compile Ogg Opus file decoder (links libopusfile)" OFF)
option(AUDIO_ENGINE_FETCH_DECODERS "Fetch dr_libs / stb via FetchContent if not vendored locally" ON)

# Voice codec: libopus. Off by default. When ON, the build resolves
# libopus via three strategies in order: a vendored CMake project under
# third_party/opus/, the system package (find_package(Opus) /
# pkg_check_modules), or FetchContent against xiph/opus. The wrapper at
# src/audio_engine/voice/opus_voice_codec.cpp gates its content on the
# AUDIO_ENGINE_VOICE_OPUS define and is always in the source list — when
# the option is OFF the TU is empty.
option(AUDIO_ENGINE_VOICE_OPUS  "Compile Opus voice codec (links libopus)" OFF)
option(AUDIO_ENGINE_FETCH_OPUS  "Fetch libopus via FetchContent if not vendored or system-installed" ON)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

find_package(Threads REQUIRED)

# -------------------------------------------------------------------------
# Library target
# -------------------------------------------------------------------------
set(AUDIO_ENGINE_SOURCES
    src/audio_engine/result.cpp
    src/audio_engine/events.cpp
    src/audio_engine/attenuation.cpp
    src/audio_engine/assets/audio_asset_registry.cpp
    src/audio_engine/assets/sound_bank.cpp
    src/audio_engine/assets/gpak.cpp
    src/audio_engine/emitters/emitter_manager.cpp
    src/audio_engine/listeners/listener_manager.cpp
    src/audio_engine/voice/voice_source_manager.cpp
    src/audio_engine/voice/stub_voice_codec.cpp
    src/audio_engine/voice/opus_voice_codec.cpp
    src/audio_engine/spatial/default_spatializer.cpp
    src/audio_engine/spatial/spherical_head_spatializer.cpp
    src/audio_engine/spatial/occlusion_system.cpp
    src/audio_engine/mixer/audio_mixer.cpp
    src/audio_engine/mixer/bus_graph.cpp
    src/audio_engine/dsp/gain_effect.cpp
    src/audio_engine/dsp/biquad_filter.cpp
    src/audio_engine/dsp/compressor.cpp
    src/audio_engine/dsp/reverb_effect.cpp
    src/audio_engine/dsp/saturation_effect.cpp
    src/audio_engine/decoders/decoder_factory.cpp
    src/audio_engine/decoders/resampling_decoder.cpp
    src/audio_engine/decoders/wav_decoder.cpp
    src/audio_engine/decoders/ogg_vorbis_decoder.cpp
    src/audio_engine/decoders/opus_file_decoder.cpp
    src/audio_engine/decoders/flac_decoder.cpp
    src/audio_engine/backend/null_audio_backend.cpp
    src/audio_engine/orchestrator/sequence_player.cpp
    src/audio_engine/runtime/audio_runtime.cpp
    src/audio_engine/runtime/bus_config_loader.cpp
    src/audio_engine/runtime/logging.cpp
    src/audio_engine/runtime/memory_budget.cpp
    src/audio_engine/runtime/memory_locking.cpp
    src/audio_engine/runtime/music_channel.cpp
    src/audio_engine/runtime/replication_rate_limiter.cpp
    src/audio_engine/runtime/telemetry.cpp
    src/audio_engine/default_bounds_validator.cpp
    src/audio_engine/version.cpp
)

if(AUDIO_ENGINE_SHARED)
    add_library(audio_engine SHARED ${AUDIO_ENGINE_SOURCES})
    target_compile_definitions(audio_engine
        PUBLIC  AUDIO_ENGINE_SHARED_LIB
        PRIVATE AUDIO_ENGINE_BUILDING)
else()
    add_library(audio_engine STATIC ${AUDIO_ENGINE_SOURCES})
endif()
add_library(audio_engine::audio_engine ALIAS audio_engine)

target_include_directories(audio_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(audio_engine PUBLIC Threads::Threads)

# Stamp the git SHA captured at configure time into the library so
# audio::GetVersion().commit reflects what was actually built.
target_compile_definitions(audio_engine PRIVATE
    AE_BUILD_COMMIT="${AUDIO_ENGINE_GIT_COMMIT}")

# -------------------------------------------------------------------------
# Optional: miniaudio backend
# -------------------------------------------------------------------------
# Adds src/audio_engine/backend/miniaudio_backend.cpp to the library and
# locates miniaudio.h either from a vendored copy under third_party/miniaudio/
# or via FetchContent. Off by default so the core library has zero
# third-party dependencies.
if(AUDIO_ENGINE_BACKEND_MINIAUDIO)
    set(_AE_MINIAUDIO_VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio/miniaudio.h")

    if(EXISTS "${_AE_MINIAUDIO_VENDORED}")
        set(_AE_MINIAUDIO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")
        message(STATUS "audio_engine: using vendored miniaudio at ${_AE_MINIAUDIO_VENDORED}")
    elseif(AUDIO_ENGINE_FETCH_MINIAUDIO)
        include(FetchContent)
        # Tracks master. Pin to a specific tag for reproducible builds, e.g.
        #   set(AE_MINIAUDIO_TAG 0.11.21)
        if(NOT DEFINED AE_MINIAUDIO_TAG)
            set(AE_MINIAUDIO_TAG master)
        endif()
        FetchContent_Declare(
            audio_engine_miniaudio
            GIT_REPOSITORY https://github.com/mackron/miniaudio.git
            GIT_TAG        ${AE_MINIAUDIO_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(audio_engine_miniaudio)
        set(_AE_MINIAUDIO_INCLUDE_DIR "${audio_engine_miniaudio_SOURCE_DIR}")
        message(STATUS "audio_engine: fetched miniaudio (${AE_MINIAUDIO_TAG}) into ${_AE_MINIAUDIO_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "AUDIO_ENGINE_BACKEND_MINIAUDIO=ON but miniaudio.h was not found at\n"
            "  ${_AE_MINIAUDIO_VENDORED}\n"
            "and AUDIO_ENGINE_FETCH_MINIAUDIO=OFF. Either vendor miniaudio.h or\n"
            "set AUDIO_ENGINE_FETCH_MINIAUDIO=ON.")
    endif()

    target_sources(audio_engine PRIVATE
        src/audio_engine/backend/miniaudio_backend.cpp)
    target_include_directories(audio_engine PRIVATE "${_AE_MINIAUDIO_INCLUDE_DIR}")
    target_compile_definitions(audio_engine PUBLIC AUDIO_ENGINE_HAS_MINIAUDIO=1)

    # Per-platform link libs that miniaudio's chosen native backend needs.
    if(WIN32)
        # WASAPI / DirectSound / WinMM are linked dynamically via LoadLibrary
        # in miniaudio's default config; no extra libs required.
    elseif(APPLE)
        target_link_libraries(audio_engine PRIVATE
            "-framework CoreFoundation"
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework AudioUnit")
    elseif(ANDROID)
        target_link_libraries(audio_engine PRIVATE log OpenSLES)
    elseif(UNIX)
        target_link_libraries(audio_engine PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
endif()

# -------------------------------------------------------------------------
# Optional: file-format decoders (dr_wav / stb_vorbis / dr_flac)
# -------------------------------------------------------------------------
# Each decoder TU under src/audio_engine/decoders/ is compiled
# unconditionally: when its AUDIO_ENGINE_DECODERS_* macro isn't defined the
# TU is essentially empty and its factory returns Unsupported. When the flag
# IS defined we pull in the corresponding single-header library — either
# from third_party/ if vendored or via FetchContent.
#
# dr_wav and dr_flac come from the dr_libs repo; stb_vorbis from the stb
# repo. Both are public-domain.

if(AUDIO_ENGINE_DECODERS_WAV OR AUDIO_ENGINE_DECODERS_FLAC)
    set(_AE_DRLIBS_VENDORED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs")

    if(EXISTS "${_AE_DRLIBS_VENDORED_DIR}/dr_wav.h"
       OR EXISTS "${_AE_DRLIBS_VENDORED_DIR}/dr_flac.h")
        set(_AE_DRLIBS_INCLUDE_DIR "${_AE_DRLIBS_VENDORED_DIR}")
        message(STATUS "audio_engine: using vendored dr_libs at ${_AE_DRLIBS_VENDORED_DIR}")
    elseif(AUDIO_ENGINE_FETCH_DECODERS)
        include(FetchContent)
        if(NOT DEFINED AE_DRLIBS_TAG)
            set(AE_DRLIBS_TAG master)
        endif()
        FetchContent_Declare(
            audio_engine_dr_libs
            GIT_REPOSITORY https://github.com/mackron/dr_libs.git
            GIT_TAG        ${AE_DRLIBS_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(audio_engine_dr_libs)
        set(_AE_DRLIBS_INCLUDE_DIR "${audio_engine_dr_libs_SOURCE_DIR}")
        message(STATUS "audio_engine: fetched dr_libs (${AE_DRLIBS_TAG}) into ${_AE_DRLIBS_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "AUDIO_ENGINE_DECODERS_WAV/FLAC=ON but dr_libs headers were not\n"
            "found at ${_AE_DRLIBS_VENDORED_DIR} and AUDIO_ENGINE_FETCH_DECODERS=OFF.\n"
            "Either vendor dr_wav.h / dr_flac.h or set AUDIO_ENGINE_FETCH_DECODERS=ON.")
    endif()

    target_include_directories(audio_engine PRIVATE "${_AE_DRLIBS_INCLUDE_DIR}")
endif()

if(AUDIO_ENGINE_DECODERS_OGG)
    set(_AE_STB_VENDORED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

    if(EXISTS "${_AE_STB_VENDORED_DIR}/stb_vorbis.c")
        set(_AE_STB_INCLUDE_DIR "${_AE_STB_VENDORED_DIR}")
        message(STATUS "audio_engine: using vendored stb at ${_AE_STB_VENDORED_DIR}")
    elseif(AUDIO_ENGINE_FETCH_DECODERS)
        include(FetchContent)
        if(NOT DEFINED AE_STB_TAG)
            set(AE_STB_TAG master)
        endif()
        FetchContent_Declare(
            audio_engine_stb
            GIT_REPOSITORY https://github.com/nothings/stb.git
            GIT_TAG        ${AE_STB_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(audio_engine_stb)
        set(_AE_STB_INCLUDE_DIR "${audio_engine_stb_SOURCE_DIR}")
        message(STATUS "audio_engine: fetched stb (${AE_STB_TAG}) into ${_AE_STB_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR
            "AUDIO_ENGINE_DECODERS_OGG=ON but stb_vorbis.c was not found at\n"
            "${_AE_STB_VENDORED_DIR}/stb_vorbis.c and\n"
            "AUDIO_ENGINE_FETCH_DECODERS=OFF. Either vendor stb_vorbis.c or set\n"
            "AUDIO_ENGINE_FETCH_DECODERS=ON.")
    endif()

    target_include_directories(audio_engine PRIVATE "${_AE_STB_INCLUDE_DIR}")
endif()

if(AUDIO_ENGINE_DECODERS_WAV)
    target_compile_definitions(audio_engine PRIVATE AUDIO_ENGINE_DECODERS_WAV=1)
endif()
if(AUDIO_ENGINE_DECODERS_OGG)
    target_compile_definitions(audio_engine PRIVATE AUDIO_ENGINE_DECODERS_OGG=1)
endif()
if(AUDIO_ENGINE_DECODERS_FLAC)
    target_compile_definitions(audio_engine PRIVATE AUDIO_ENGINE_DECODERS_FLAC=1)
endif()

# -------------------------------------------------------------------------
# Optional: Opus file decoder (libopusfile)
# -------------------------------------------------------------------------
# libopusfile is a small C library that wraps libopus + libogg to read
# .opus container files. Single-header vendoring isn't viable (it's a
# real .c project with several TUs and platform glue), so resolution
# is system-only:
#   1. find_package(OpusFile QUIET) — modern installs ship one.
#   2. pkg-config opusfile — most Linux/macOS distros register it.
# AUDIO_ENGINE_FETCH_DECODERS does NOT cover libopusfile because its
# build system is autotools; integrating it via FetchContent is more
# trouble than it's worth. Adopters who can't install libopusfile via
# their package manager can build it from xiph/opusfile manually and
# point CMAKE_PREFIX_PATH at the install root.
#
# The wrapper TU at src/audio_engine/decoders/opus_file_decoder.cpp is
# always in the source list. AUDIO_ENGINE_DECODERS_OPUS=1 flips it to
# gated-on; in stub mode it expands to "Create returns nullptr".

if(AUDIO_ENGINE_DECODERS_OPUS)
    set(_AE_OPUSFILE_RESOLVED FALSE)

    # 1. find_package (some distros ship a CMake config).
    find_package(OpusFile QUIET)
    if(OpusFile_FOUND)
        message(STATUS "audio_engine: using system libopusfile via find_package(OpusFile)")
        target_link_libraries(audio_engine PRIVATE OpusFile::opusfile)
        set(_AE_OPUSFILE_RESOLVED TRUE)
    endif()

    # 2. pkg-config (most common path on Linux / macOS Homebrew).
    if(NOT _AE_OPUSFILE_RESOLVED)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(OPUSFILE QUIET IMPORTED_TARGET opusfile)
            if(OPUSFILE_FOUND)
                message(STATUS "audio_engine: using libopusfile via pkg-config")
                target_link_libraries(audio_engine PRIVATE PkgConfig::OPUSFILE)
                set(_AE_OPUSFILE_RESOLVED TRUE)
            endif()
        endif()
    endif()

    if(NOT _AE_OPUSFILE_RESOLVED)
        message(FATAL_ERROR
            "AUDIO_ENGINE_DECODERS_OPUS=ON but libopusfile was not found.\n"
            "Install via your package manager (e.g. apt install libopusfile-dev,\n"
            "brew install opusfile, vcpkg install opusfile) and re-run CMake.")
    endif()

    # On macOS Homebrew, opusfile.pc's `Cflags: -I${includedir}/opus`
    # adds the `opus/` subdirectory itself, not its parent. Our code
    # does `#include <opus/opusfile.h>` (the Linux convention), so the
    # compiler needs the *parent* of the `opus/` directory on the
    # include path. On Linux apt this falls out for free because
    # /usr/include is implicit; on Apple Silicon Homebrew (/opt/homebrew)
    # and Intel Homebrew (/usr/local) it isn't. Add it explicitly when
    # the file is actually there.
    if(APPLE)
        foreach(_prefix /opt/homebrew /usr/local)
            if(EXISTS "${_prefix}/include/opus/opusfile.h")
                target_include_directories(audio_engine PRIVATE "${_prefix}/include")
            endif()
        endforeach()
    endif()

    target_compile_definitions(audio_engine PRIVATE AUDIO_ENGINE_DECODERS_OPUS=1)
endif()

# -------------------------------------------------------------------------
# Optional: Opus voice codec
# -------------------------------------------------------------------------
# libopus is a real CMake library, not a single header, so the resolution
# strategy differs from the file decoders:
#   1. Vendored: a `third_party/opus/` directory containing the libopus
#      source tree (with its own CMakeLists.txt) — added as a subdirectory.
#   2. System: find_package(Opus) (modern installs) or pkg-config — most
#      Linux/macOS users have this via apt/brew.
#   3. FetchContent: clone xiph/opus if AUDIO_ENGINE_FETCH_OPUS=ON.
#
# The wrapper TU at src/audio_engine/voice/opus_voice_codec.cpp is always
# in the source list. AUDIO_ENGINE_VOICE_OPUS=1 flips it to gated-on; in
# stub mode it expands to an empty TU.

if(AUDIO_ENGINE_VOICE_OPUS)
    set(_AE_OPUS_VENDORED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus")
    set(_AE_OPUS_RESOLVED FALSE)

    # 1. Vendored (added as subdirectory).
    if(EXISTS "${_AE_OPUS_VENDORED_DIR}/CMakeLists.txt")
        message(STATUS "audio_engine: using vendored libopus at ${_AE_OPUS_VENDORED_DIR}")
        # libopus's tests / extras off; we only want the library target.
        set(OPUS_BUILD_TESTING       OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_AE_OPUS_VENDORED_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/opus_build" EXCLUDE_FROM_ALL)
        target_link_libraries(audio_engine PRIVATE opus)
        set(_AE_OPUS_RESOLVED TRUE)
    endif()

    # 2. System (find_package or pkg-config).
    if(NOT _AE_OPUS_RESOLVED)
        find_package(Opus QUIET)
        if(Opus_FOUND)
            message(STATUS "audio_engine: using system libopus via find_package(Opus)")
            target_link_libraries(audio_engine PRIVATE Opus::opus)
            set(_AE_OPUS_RESOLVED TRUE)
        endif()
    endif()
    if(NOT _AE_OPUS_RESOLVED)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(OPUS_PKG QUIET opus)
            if(OPUS_PKG_FOUND)
                message(STATUS "audio_engine: using system libopus via pkg-config (opus)")
                target_include_directories(audio_engine PRIVATE ${OPUS_PKG_INCLUDE_DIRS})
                target_link_libraries(audio_engine PRIVATE ${OPUS_PKG_LIBRARIES})
                target_link_directories(audio_engine PRIVATE ${OPUS_PKG_LIBRARY_DIRS})
                set(_AE_OPUS_RESOLVED TRUE)
            endif()
        endif()
    endif()

    # 3. FetchContent fallback against xiph/opus.
    if(NOT _AE_OPUS_RESOLVED AND AUDIO_ENGINE_FETCH_OPUS)
        include(FetchContent)
        if(NOT DEFINED AE_OPUS_TAG)
            set(AE_OPUS_TAG v1.5.2)
        endif()
        set(OPUS_BUILD_TESTING       OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            audio_engine_opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        ${AE_OPUS_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(audio_engine_opus)
        message(STATUS "audio_engine: fetched libopus (${AE_OPUS_TAG})")
        target_link_libraries(audio_engine PRIVATE opus)
        set(_AE_OPUS_RESOLVED TRUE)
    endif()

    if(NOT _AE_OPUS_RESOLVED)
        message(FATAL_ERROR
            "AUDIO_ENGINE_VOICE_OPUS=ON but libopus was not found:\n"
            "  - vendored: ${_AE_OPUS_VENDORED_DIR}/CMakeLists.txt missing\n"
            "  - system:   find_package(Opus) / pkg-config(opus) failed\n"
            "  - fetched:  AUDIO_ENGINE_FETCH_OPUS=OFF\n"
            "Set AUDIO_ENGINE_FETCH_OPUS=ON, install libopus-dev, or vendor\n"
            "the libopus source tree under third_party/opus/.")
    endif()

    # Same Homebrew prefix quirk as the DECODERS_OPUS block above:
    # opus.pc's Cflags point at the `opus/` subdirectory, but our
    # code does `#include <opus.h>` (no prefix) — wait, actually for
    # libopus the include is `<opus.h>` and Homebrew puts it at
    # /opt/homebrew/include/opus/opus.h, so we DO need the `opus/`
    # subdir on the path. opus.pc gets this right; this fallback
    # is belt-and-suspenders for when find_package(Opus) succeeds
    # but doesn't propagate include dirs cleanly.
    if(APPLE)
        foreach(_prefix /opt/homebrew /usr/local)
            if(EXISTS "${_prefix}/include/opus/opus.h")
                target_include_directories(audio_engine PRIVATE "${_prefix}/include/opus")
            endif()
        endforeach()
    endif()

    target_compile_definitions(audio_engine PRIVATE AUDIO_ENGINE_VOICE_OPUS=1)
endif()

# -------------------------------------------------------------------------
# Apple Silicon Homebrew quirk: opusfile.pc ships
#     Cflags: -I${includedir}/opus
# which is the WRONG prefix for our `#include <opus/opusfile.h>` style.
# pkg-config hands CMake `/opt/homebrew/Cellar/opusfile/X.Y/include/opus`,
# then the compiler looks for `opus/opusfile.h` relative to that path —
# resolving to `.../include/opus/opus/opusfile.h`, which doesn't exist.
#
# On Linux apt this is invisible: /usr/include is on the implicit search
# path, so `<opus/opusfile.h>` resolves to /usr/include/opus/opusfile.h
# regardless of what opusfile.pc claims. On macOS /opt/homebrew/include
# is NOT implicit, so we add it explicitly when the relevant headers
# actually live under it.
#
# Idempotent: target_include_directories dedupes, so this is a no-op if
# the prefix was already on the path. The `if EXISTS` guards keep us
# from polluting the include path with directories that don't exist.
# -------------------------------------------------------------------------
if(APPLE AND (AUDIO_ENGINE_DECODERS_OPUS OR AUDIO_ENGINE_VOICE_OPUS))
    foreach(_brew_prefix /opt/homebrew /usr/local)
        foreach(_header opus/opusfile.h opus/opus.h)
            if(EXISTS "${_brew_prefix}/include/${_header}")
                target_include_directories(audio_engine PRIVATE "${_brew_prefix}/include")
            endif()
        endforeach()
    endforeach()
endif()

if(MSVC)
    target_compile_options(audio_engine PRIVATE /W4)
    if(AUDIO_ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(audio_engine PRIVATE /WX)
    endif()
else()
    target_compile_options(audio_engine PRIVATE -Wall -Wextra -Wpedantic)
    if(AUDIO_ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(audio_engine PRIVATE -Werror)
    endif()
endif()

set_target_properties(audio_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET     hidden
    VISIBILITY_INLINES_HIDDEN ON)

# -------------------------------------------------------------------------
# Subdirectories
# -------------------------------------------------------------------------
if(AUDIO_ENGINE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AUDIO_ENGINE_TOP_LEVEL)
    add_subdirectory(tools)
endif()

if(AUDIO_ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
