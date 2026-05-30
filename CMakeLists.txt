cmake_minimum_required(VERSION 3.20)
project(audio_engine
    VERSION 0.81.16
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

# Sanitizer builds. Audit follow-up (item 6 of the v0.12.x memory-management
# audit). Mutually exclusive — ASAN and TSAN don't compose. Recommended
# compile mode: Debug or RelWithDebInfo + -O1 for readable stacks.
#
# ASAN catches: heap/stack overflows, use-after-free, leaks (with LSan),
#               double-free, init-order issues. Combined with UBSan in this
#               option to catch signed overflow / null deref / misaligned
#               loads in one build.
# TSAN catches: data races, lock-order inversions, threadsafe-library misuse.
#               5-15x slowdown; expect long test runs.
#
# CI runs both as separate jobs (.github/workflows/ci.yml). Local use:
#   cmake -S . -B build-asan -DAUDIO_ENGINE_SANITIZE_ASAN=ON
#   cmake -S . -B build-tsan -DAUDIO_ENGINE_SANITIZE_TSAN=ON
option(AUDIO_ENGINE_SANITIZE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(AUDIO_ENGINE_SANITIZE_TSAN "Build with ThreadSanitizer"          OFF)
# v0.17.0 Tier-3 hardening: libFuzzer harnesses for the three external
# input surfaces (JSON config, audio decoders, Opus voice packets).
# Requires Clang (libFuzzer is a Clang-only sanitizer). Off by default
# so the normal build is unaffected; turned on by the nightly fuzz CI
# workflow and by anyone running the harnesses locally. See
# tests/fuzz/*.cpp for the per-harness build instructions.
option(AUDIO_ENGINE_FUZZ           "Build libFuzzer harnesses (clang only)" OFF)
if(AUDIO_ENGINE_SANITIZE_ASAN AND AUDIO_ENGINE_SANITIZE_TSAN)
    message(FATAL_ERROR
        "AUDIO_ENGINE_SANITIZE_ASAN and AUDIO_ENGINE_SANITIZE_TSAN are "
        "mutually exclusive — ASan and TSan are incompatible at link time. "
        "Enable one or the other.")
endif()
if(AUDIO_ENGINE_SANITIZE_ASAN)
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
            CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
            CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
        message(WARNING
            "AUDIO_ENGINE_SANITIZE_ASAN tested only on GCC/Clang. "
            "MSVC has /fsanitize=address support but UBSan is unavailable; "
            "this option may not work as expected.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(AUDIO_ENGINE_SANITIZE_TSAN)
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
            CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
        message(WARNING
            "AUDIO_ENGINE_SANITIZE_TSAN tested only on GCC/Clang on Linux. "
            "AppleClang's TSan support is flakier; MSVC has no TSan.")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

option(AUDIO_ENGINE_BACKEND_MINIAUDIO "Build the miniaudio backend (cross-platform device I/O). Required for the audible examples (playback, hello_audio); the core library and tests build without it." ON)
option(AUDIO_ENGINE_FETCH_MINIAUDIO   "Fetch miniaudio via FetchContent if not vendored locally" ON)

# nlohmann/json: required by the JSON-config loaders (bus_config_loader,
# sound_bank). v0.80.0 replaced two hand-rolled parsers — both missing
# `\u`/`\b`/`\f` escape handling and other non-spec corners — with this
# single-header library. No option to disable it: the core JSON config
# path uses it. Fetched on demand if not vendored, same pattern as
# miniaudio / dr_libs / stb_vorbis.
option(AUDIO_ENGINE_FETCH_NLOHMANN_JSON "Fetch nlohmann/json via FetchContent if not vendored locally" ON)

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
    src/audio_engine/dsp/master_control.cpp
    src/audio_engine/dsp/material_eq.cpp
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
    src/audio_engine/runtime/denormal_protection.cpp
    src/audio_engine/runtime/logging.cpp
    src/audio_engine/runtime/memory_budget.cpp
    src/audio_engine/runtime/memory_locking.cpp
    src/audio_engine/runtime/music_channel.cpp
    src/audio_engine/runtime/thread_priority.cpp
    src/audio_engine/runtime/replication_rate_limiter.cpp
    src/audio_engine/runtime/telemetry.cpp
    src/audio_engine/util/json_string_decoder.cpp
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
    target_include_directories(audio_engine SYSTEM PRIVATE "${_AE_MINIAUDIO_INCLUDE_DIR}")
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
# nlohmann/json (required, not optional)
# -------------------------------------------------------------------------
# Single-header JSON library used by bus_config_loader.cpp and
# sound_bank.cpp. v0.80.0 replaced two hand-rolled parsers — both
# rejecting valid JSON containing `\u`/`\b`/`\f` escapes, and shipping
# with various other non-spec corners — with this library. No option to
# disable: the JSON-config code paths use it unconditionally.
#
# Resolution order:
#   1. Vendored copy at third_party/nlohmann/json.hpp
#      (drop in via scripts/fetch_nlohmann_json.sh or .bat)
#   2. FetchContent against upstream (AUDIO_ENGINE_FETCH_NLOHMANN_JSON=ON,
#      the default — pinned to v3.11.3)
#
# The header is included as <nlohmann/json.hpp>, so we add the parent
# directory of the nlohmann/ folder to the include path.
set(_AE_NLOHMANN_VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")

if(EXISTS "${_AE_NLOHMANN_VENDORED}")
    set(_AE_NLOHMANN_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    message(STATUS "audio_engine: using vendored nlohmann/json at ${_AE_NLOHMANN_VENDORED}")
elseif(AUDIO_ENGINE_FETCH_NLOHMANN_JSON)
    include(FetchContent)
    if(NOT DEFINED AE_NLOHMANN_TAG)
        set(AE_NLOHMANN_TAG v3.11.3)
    endif()
    # Suppress nlohmann's own tests/examples/install targets — we want
    # headers only. These cache vars are read by nlohmann's CMakeLists
    # if FetchContent ends up invoking add_subdirectory on it.
    set(JSON_BuildTests      OFF CACHE INTERNAL "")
    set(JSON_Install         OFF CACHE INTERNAL "")
    FetchContent_Declare(
        audio_engine_nlohmann
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        ${AE_NLOHMANN_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(audio_engine_nlohmann)
    set(_AE_NLOHMANN_INCLUDE_DIR "${audio_engine_nlohmann_SOURCE_DIR}/single_include")
    message(STATUS "audio_engine: fetched nlohmann/json (${AE_NLOHMANN_TAG}) into ${_AE_NLOHMANN_INCLUDE_DIR}")
else()
    message(FATAL_ERROR
        "nlohmann/json.hpp was not found at\n"
        "  ${_AE_NLOHMANN_VENDORED}\n"
        "and AUDIO_ENGINE_FETCH_NLOHMANN_JSON=OFF. Either vendor it via\n"
        "scripts/fetch_nlohmann_json.sh (or .bat) or set\n"
        "AUDIO_ENGINE_FETCH_NLOHMANN_JSON=ON (the default).")
endif()

target_include_directories(audio_engine SYSTEM PRIVATE "${_AE_NLOHMANN_INCLUDE_DIR}")

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

    target_include_directories(audio_engine SYSTEM PRIVATE "${_AE_DRLIBS_INCLUDE_DIR}")
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

    target_include_directories(audio_engine SYSTEM PRIVATE "${_AE_STB_INCLUDE_DIR}")
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
    #
    # Parallel issue for the link side (v0.21.3): when audio_engine is
    # consumed by a downstream shared library (libgool_godot.dylib),
    # the transitive `-lopus` from opusfile.pc's Requires: needs a
    # matching `-L/opt/homebrew/lib` at the consumer's link step.
    # PkgConfig::OPUSFILE's INTERFACE_LINK_DIRECTORIES doesn't always
    # propagate cleanly through a PRIVATE-linked static lib, so we
    # add the Homebrew prefix's lib directory PUBLIC on audio_engine.
    # PUBLIC (not PRIVATE) is intentional: the search path must be
    # visible to whatever links audio_engine.a, not just audio_engine
    # itself. This complements but doesn't replace the v0.21.2
    # CMAKE_PREFIX_PATH hint in the CI workflows.
    if(APPLE)
        foreach(_prefix /opt/homebrew /usr/local)
            if(EXISTS "${_prefix}/include/opus/opusfile.h")
                target_include_directories(audio_engine SYSTEM PRIVATE "${_prefix}/include")
                target_link_directories(audio_engine PUBLIC "${_prefix}/lib")
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
                target_include_directories(audio_engine SYSTEM PRIVATE ${OPUS_PKG_INCLUDE_DIRS})
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
                target_include_directories(audio_engine SYSTEM PRIVATE "${_prefix}/include/opus")
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
                target_include_directories(audio_engine SYSTEM PRIVATE "${_brew_prefix}/include")
            endif()
        endforeach()
    endforeach()
endif()

if(MSVC)
    target_compile_options(audio_engine PRIVATE /W4)
    # v0.15.0: warnings-as-errors is unconditional for gool's own private
    # compilation. PRIVATE means the flag does not propagate to downstream
    # consumers (who keep their own warning policies). The old option
    # AUDIO_ENGINE_WARNINGS_AS_ERRORS is preserved as a no-op for backward
    # compatibility — any existing CI scripts that toggle it keep working.
    target_compile_options(audio_engine PRIVATE /WX)
else()
    target_compile_options(audio_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(audio_engine PRIVATE -Werror)
endif()

# -------------------------------------------------------------------------
# v0.79.1: Release-only debloat flags.
#
# Goal: shrink the shipped library by removing code the runtime doesn't
# actually need. Each flag is justified individually:
#
#   -fno-rtti / /GR-       REMOVED in v0.79.6. The v0.79.1 audit
#                          confirmed audio_engine itself uses zero
#                          dynamic_cast / typeid, which made -fno-rtti
#                          look free. But gool_godot.cpp (the consumer)
#                          uses dynamic_cast<MiniaudioBackend*> in 3
#                          places to expose backend-specific stats.
#                          With -fno-rtti on audio_engine, typeinfo for
#                          MiniaudioBackend wasn't emitted, and Apple
#                          ld64 refused to link gool_godot on macOS
#                          (Linux GNU ld silently weak-linked the
#                          missing symbol, masking the bug until macOS
#                          CI caught it). Future refactor: replace the
#                          dynamic_casts with a virtual method on
#                          AudioBackend so -fno-rtti can come back.
#                          For now we ship without it. Disabling
#                          is provably safe per source audit.
#
#   -ffunction-sections    Place each function/data item in its own ELF
#   -fdata-sections        section. Pairs with --gc-sections at link.
#                          MSVC equivalent is /Gy (function sections);
#                          /Gw (data sections) is on by default in
#                          modern MSVC and has no separate switch in
#                          the form expected here.
#
#   -Wl,--gc-sections      At link time, drop sections nothing
#   /OPT:REF (MSVC)        references. Combined with -f*-sections,
#                          this is what actually shrinks the binary;
#                          the compile-side flags only make sections
#                          granular enough for the linker to act on.
#
#   GOOL_LOG_MIN_LEVEL    Compile-time logging gate, available but
#                         NOT forced in Release. Hosts who want a
#                         tighter shipping binary can pass
#                         -DGOOL_LOG_MIN_LEVEL=3 explicitly. Default
#                         (1 = Debug) preserves the v0.78.5 runtime-
#                         diagnostic capability for player bug
#                         reports. See logging.h for the macro
#                         definition and ShouldLog_ in
#                         audio_runtime_impl.h for the gate.
#
# NOT included here (and why):
#   -fno-exceptions   AudioRuntime::Update wraps the entire body in
#                     try/catch (v0.15.0 hardening to shield the
#                     runtime from misbehaving third-party callbacks).
#                     Disabling exceptions would silently disable
#                     that hardening. Keep enabled.
#   -flto             Defer to a later release with explicit benchmarking.
#                     LTO is a real win but interacts with the static-
#                     analysis CI (clang-tidy's compilation database
#                     needs per-TU output) and wants careful testing
#                     before turning on.
# -------------------------------------------------------------------------
if(MSVC)
    target_compile_options(audio_engine PRIVATE
        $<$<CONFIG:Release>:/Gy>                        # function sections
    )
    target_link_options(audio_engine PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>                   # gc unused
        $<$<CONFIG:Release>:/OPT:ICF>                   # fold identical funcs
    )
else()
    target_compile_options(audio_engine PRIVATE
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    # --gc-sections is a linker flag (not compiler), and Apple's linker
    # uses -dead_strip instead of --gc-sections. Conditional on platform.
    if(APPLE)
        target_link_options(audio_engine PRIVATE
            $<$<CONFIG:Release>:-Wl,-dead_strip>
        )
    else()
        target_link_options(audio_engine PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
        )
    endif()
endif()

# v0.79.1: NOT setting GOOL_LOG_MIN_LEVEL in Release by default.
#
# Rationale: compiling out Debug-level log call sites in shipped
# builds saves ~5 KB of binary, but removes the ability to enable
# verbose logging in a player's hands for bug reports. For audio
# middleware specifically — where bugs are often stochastic and
# session-specific — that runtime diagnostic capability is worth
# more than the 5 KB savings.
#
# The GOOL_LOG_MIN_LEVEL macro is defined in logging.h and DOES
# gate ShouldLog_ at compile time. Hosts who want the tighter
# shipping binary can opt in by passing -DGOOL_LOG_MIN_LEVEL=3
# (or higher) on their own compile line. We just don't force it.

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

# ---------------------------------------------------------------------------
# v0.17.0 Tier-3 hardening: libFuzzer harnesses.
#
# Three harnesses, one per external input surface. Each is a small
# .cpp under tests/fuzz/ that defines LLVMFuzzerTestOneInput and links
# against audio_engine + libFuzzer + ASAN+UBSan. We build them only
# when AUDIO_ENGINE_FUZZ=ON and the compiler is Clang (libFuzzer is a
# Clang-only sanitizer; GCC has libFuzzer-compatible harnesses but the
# -fsanitize=fuzzer flag is Clang-specific).
#
# The harnesses are NOT registered with ctest. They're long-running
# fuzz binaries the nightly fuzz workflow invokes with a time budget;
# they would not terminate under ctest's default test-run semantics.
# ---------------------------------------------------------------------------
if(AUDIO_ENGINE_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(FATAL_ERROR
            "AUDIO_ENGINE_FUZZ requires Clang (libFuzzer is Clang-only). "
            "Re-run with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
    endif()

    # Compose libFuzzer with ASAN+UBSan. Findings then surface as
    # ASAN/UBSan reports with a reproducer testcase saved in the
    # current working directory, which the nightly workflow uploads
    # as an artifact.
    set(_fuzz_flags "-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1")

    function(audio_engine_add_fuzz_harness target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE audio_engine)
        # v0.26.6: fuzz harnesses sometimes need to reach private
        # implementation headers (e.g. fuzz_audio_decoders pokes at
        # AudioDecoder + DecoderFactory directly to exercise the
        # format-sniff path that public APIs hide). audio_engine
        # declares src/ as a PRIVATE include dir; linking PRIVATE
        # against the target inherits PUBLIC includes (include/)
        # but NOT PRIVATE ones (src/), so we add src/ explicitly
        # here. Mirrors the engine's own self-include configuration.
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined
            -fno-omit-frame-pointer
            -g -O1)
        target_link_options(${target} PRIVATE
            -fsanitize=fuzzer,address,undefined)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endfunction()

    audio_engine_add_fuzz_harness(fuzz_bus_config_json
                                    tests/fuzz/fuzz_bus_config_json.cpp)
    audio_engine_add_fuzz_harness(fuzz_audio_decoders
                                    tests/fuzz/fuzz_audio_decoders.cpp)

    # Opus harness only built when libopus is available (the codec
    # itself is opt-in). When AUDIO_ENGINE_VOICE_OPUS=OFF the harness
    # would link against an unsupported codec and report no findings.
    if(AUDIO_ENGINE_VOICE_OPUS)
        audio_engine_add_fuzz_harness(fuzz_opus_voice
                                        tests/fuzz/fuzz_opus_voice.cpp)
    endif()
endif()
