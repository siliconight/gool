// Copyright 2026 Brannen Graves
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing permissions
// and limitations under the License.

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
