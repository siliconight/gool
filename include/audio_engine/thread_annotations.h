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

// Capability tag types. Empty by design.
struct AUDIO_CAPABILITY("game thread")          GameThread {};
struct AUDIO_CAPABILITY("network thread")       NetworkThread {};
struct AUDIO_CAPABILITY("audio control thread") ControlThread {};
struct AUDIO_CAPABILITY("audio render thread")  RenderThread {};

} // namespace audio

// Markers for render-path constraints. Currently documentary; intended to be
// wired to a Clang plugin or runtime allocation tracker in a follow-up.
#define AUDIO_NO_ALLOC      // do not allocate
#define AUDIO_RENDER_PATH   // called on the render thread

#endif // AUDIO_ENGINE_THREAD_ANNOTATIONS_H
