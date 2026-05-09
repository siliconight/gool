# gool Terminology

gool deliberately uses the vocabulary that audio designers
already know from FMOD and Wwise. If you've shipped a game with
either of those middlewares, this list should look familiar.

This doc serves two purposes:
- For new users: confirms what each term means in gool.
- For migrants: confirms the term means the same thing it does in
  the middleware you came from, so the muscle memory transfers.

## Core terms

| Term         | gool meaning                                                       | FMOD                | Wwise                |
|--------------|--------------------------------------------------------------------|---------------------|----------------------|
| **Event**    | A trigger that produces sound (e.g. play this at this position)    | Event               | Event                |
| **Sound**    | A registered audio asset, identified by a hashed name              | (encapsulated)      | Sound SFX            |
| **Emitter**  | A live, playing instance of a sound; created from a SoundDefinition | EventInstance       | (game object)        |
| **Listener** | The position/orientation from which spatial audio is heard         | Listener            | Listener             |
| **Bus**      | A mixer channel that combines, processes, and routes audio          | Bus / Mix Group     | Audio Bus            |
| **Parameter**| A real-time control value driving DSP or mix behavior              | Parameter           | RTPC                 |
| **State**    | A named application mode that drives group/bus changes             | (Snapshot trigger)  | State                |
| **Snapshot** | A captured set of bus parameter values that can be applied         | Snapshot            | State (mixer side)   |
| **Group**    | A collection of sounds with a selection policy (random, etc.)      | Multi-instrument    | Random/Switch Container |
| **Send**     | Auxiliary routing of an emitter's signal to another bus            | Send                | Auxiliary Send        |
| **Voice**    | A single mixed channel within the renderer (a playing emitter)     | Voice               | Voice                |

## What gool calls things differently — and why

A short list. Where we diverged, it's because the FMOD/Wwise term
is genuinely ambiguous in our context, not because we're chasing
novelty.

### "Sound" vs. "Event"

In FMOD, "Event" is the unit of triggering and "Sound" is buried
inside the event's instrument graph. In gool, **Sound** is the
registered asset (the WAV/Ogg/FLAC bytes plus metadata) and
**Event** is the *intent to play one* (a discriminated union of
operations like `PlaySoundAtLocation`, `Stop`, `SetParameter`).
This matches how runtime events flow through queue → mixer in our
architecture, where the asset is data and the event is a message.

### "Emitter" vs. "EventInstance"

FMOD's EventInstance bundles "the sound playing" with "the
parameters to control it." gool calls this an **Emitter** because
the term is shorter and matches Unreal's convention. An emitter
has a position, a listener-relative gain, and parameters; you
call methods on its handle to control it.

### Why we use "Bus" and not "Channel" or "Group"

OpenAL uses "source group," Wwise uses "audio bus," DAWs use
"mixer channel." We use **Bus** because it's the most-correct term
for what's happening (a routable mixing point), and it's the term
both FMOD and Wwise picked. No reason to invent a third.

### Why we use "Snapshot"

We don't ship a `Snapshot` C++ class yet, but the term is
reserved and documented as the planned name for a saved bus-
parameter preset. When we ship it, calling it `Snapshot` will let
designers move from FMOD without learning new vocabulary.

## Things to NOT use the FMOD/Wwise term for

- **"Channel."** In FMOD's runtime API, a Channel is the C-level
  voice handle. We don't expose that to hosts; emitters are the
  unit of control. Don't call buses channels.
- **"Bank file."** FMOD's `.bank` is a binary-only container with
  events compiled into it. Our `.gpak` is a thin uncompressed
  archive of source assets; the JSON index is separate. Saying
  "bank" generically is fine; "the bank file" should be reserved
  for our `.gpak`.
- **"Track."** Wwise uses "Music Track" for layered music
  segments. gool doesn't have first-class music track grouping
  yet — use a Group with sequential policy + manual layering. If
  you ship music-track support, it'll be called a Music Track.

## Vocabulary stability

These terms are part of the public API and won't change. New
features may add new vocabulary, but established terms stay. If we
ever need to break a term, we'll alias the old one and document
the migration in the changelog.
