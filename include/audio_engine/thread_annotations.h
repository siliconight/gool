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

// audio_engine/thread_annotations.h
//
// Clang Thread Safety Analysis annotations for encoding the thread-role
// contracts of the public API. Under Clang with -Wthread-safety, these
// annotations are checked at compile time against the capabilities held by
// the calling code. Under MSVC/GCC they expand to nothing.
//
// The capability types (GameThread, NetworkThread, ControlThread, RenderThread)
// are empty tag types. Host integrations declare instances of these and pass
// them around (or hold them as members) to grant the analyzer permission to
// call methods annotated with AUDIO_REQUIRES(role).
//
// This is the documentary form of the boundary. The runtime carries no
// extra data; the cost is zero. Stronger enforcement (e.g. a Clang
// plugin that rejects allocation/throw on the render thread) is a
// future addition.

#ifndef AUDIO_ENGINE_THREAD_ANNOTATIONS_H
#define AUDIO_ENGINE_THREAD_ANNOTATIONS_H

#if defined(__clang__) && !defined(SWIG)
#define AUDIO_TSA_ATTR(x) __attribute__((x))
#else
#define AUDIO_TSA_ATTR(x)
#endif

#define AUDIO_CAPABILITY(name)              AUDIO_TSA_ATTR(capability(name))
#define AUDIO_REQUIRES(...)                 AUDIO_TSA_ATTR(requires_capability(__VA_ARGS__))
#define AUDIO_EXCLUDES(...)                 AUDIO_TSA_ATTR(locks_excluded(__VA_ARGS__))
#define AUDIO_NO_THREAD_SAFETY_ANALYSIS     AUDIO_TSA_ATTR(no_thread_safety_analysis)

namespace audio {

// Capability tag types — empty by design. The "Tag" suffix is a
// hint that you don't use the type itself; you use the same-named
// instance below in AUDIO_REQUIRES(...) attributes.
//
// Before v0.11.15 these were named without the suffix, but Clang's
// thread safety analysis requires requires_capability(EXPR) where
// EXPR is a *value* (typically a global variable), not a type.
// Apple Clang surfaced this; GCC and MSVC don't, because AUDIO_TSA_ATTR
// expands to nothing on those compilers — the GameThread token was
// never parsed.
struct AUDIO_CAPABILITY("game thread")          GameThreadTag {};
struct AUDIO_CAPABILITY("network thread")       NetworkThreadTag {};
struct AUDIO_CAPABILITY("audio control thread") ControlThreadTag {};
struct AUDIO_CAPABILITY("audio render thread")  RenderThreadTag {};

// Instances. inline so each TU including this header gets the same
// definition (C++17 inline variables — no ODR violations). These exist
// purely for Clang TSA to refer to "GameThread" etc. as a value in
// requires_capability() attributes. Zero runtime cost; the structs are
// empty and the instances optimize away.
inline GameThreadTag     GameThread;
inline NetworkThreadTag  NetworkThread;
inline ControlThreadTag  ControlThread;
inline RenderThreadTag   RenderThread;

} // namespace audio

// Markers for render-path constraints. Currently documentary; intended to be
// wired to a Clang plugin or runtime allocation tracker in a follow-up.
#define AUDIO_NO_ALLOC      // do not allocate
#define AUDIO_RENDER_PATH   // called on the render thread

#endif // AUDIO_ENGINE_THREAD_ANNOTATIONS_H
