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

// SPDX-License-Identifier: Apache-2.0
//
// IReplicationValidator: host-supplied policy hook for replicated audio
// events. The runtime calls ShouldAccept() on the network thread before
// rate limiting and before the SPSC ring push. A `false` return causes
// the event to be silently dropped and counted under
// `Stats::replicationEventsRejectedByValidator`.
//
// This is the seam to install per-game-mode policy that can't be expressed
// generically: "only certain players can submit ServerAuthoritative events
// during this round," "this prediction id has already been resolved,"
// "this gunshot's predicted-vs-actual delta exceeds an anti-cheat
// threshold," and so on.
//
// Threading: ShouldAccept() is called on the network thread (the same
// thread that calls SubmitReplicatedEvent). It must be reentrant and
// allocation-free for production deployments. The runtime never invokes
// this method from any other thread.

#pragma once

#include "audio_engine/events.h"
#include "audio_engine/types.h"

namespace audio {

class IReplicationValidator {
public:
    virtual ~IReplicationValidator() = default;

    // Return true to accept the event (it proceeds to the rate-limiter
    // check), false to reject it (silently dropped, counted in
    // `Stats::replicationEventsRejectedByValidator`).
    //
    // The `playerId` argument is `event.playerId`; passed separately so
    // implementations don't have to repeat the field access.
    virtual bool ShouldAccept(const AudioEvent& event,
                              AudioPlayerId      playerId) noexcept = 0;
};

} // namespace audio
