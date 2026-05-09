// audio_engine/export.h
//
// Symbol visibility macro. Hand-written to keep the library buildable without
// CMake's generate_export_header. The CMake build defines
// AUDIO_ENGINE_BUILDING when compiling the library and AUDIO_ENGINE_SHARED
// when building/consuming the shared variant.

#ifndef AUDIO_ENGINE_EXPORT_H
#define AUDIO_ENGINE_EXPORT_H

#if defined(AUDIO_ENGINE_STATIC) || !defined(AUDIO_ENGINE_SHARED)
#  define AUDIO_ENGINE_EXPORT
#else
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(AUDIO_ENGINE_BUILDING)
#      define AUDIO_ENGINE_EXPORT __declspec(dllexport)
#    else
#      define AUDIO_ENGINE_EXPORT __declspec(dllimport)
#    endif
#  else
#    if defined(AUDIO_ENGINE_BUILDING)
#      define AUDIO_ENGINE_EXPORT __attribute__((visibility("default")))
#    else
#      define AUDIO_ENGINE_EXPORT
#    endif
#  endif
#endif

#endif // AUDIO_ENGINE_EXPORT_H
