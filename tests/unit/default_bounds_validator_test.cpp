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

// tests/unit/default_bounds_validator_test.cpp
//
// Validates DefaultBoundsValidator and ChainReplicationValidator.
// Together they are the secure-by-default option a host installs to
// reject malformed numeric fields without writing anti-cheat code.

#include "audio_engine/default_bounds_validator.h"
#include "audio_engine/events.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

namespace {

audio::AudioEvent MakeWellFormed() {
    auto ev = audio::AudioEvent::MakePlaySoundAtLocation(
        /*sound*/ 0xABCDu, audio::Vec3{1.0f, 2.0f, 3.0f});
    ev.forward  = {0.0f, 0.0f, 1.0f};
    ev.velocity = {0.0f, 0.0f, 0.0f};
    ev.parameterValue       = 0.5f;
    ev.parameterSmoothingMs = 50.0f;
    return ev;
}

void TestAcceptsWellFormed() {
    std::cout << "  [well-formed event passes]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    assert(v.ShouldAccept(ev, /*pid*/ 1));
    auto s = v.GetStats();
    assert(s.rejectedNonFiniteVec3   == 0);
    assert(s.rejectedExtremePosition == 0);
    assert(s.rejectedExtremeVelocity == 0);
}

void TestRejectsNaNPosition() {
    std::cout << "  [NaN in position rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.position.x = std::numeric_limits<float>::quiet_NaN();
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedNonFiniteVec3 == 1);
}

void TestRejectsInfVelocity() {
    std::cout << "  [+Inf in velocity rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.velocity.y = std::numeric_limits<float>::infinity();
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedNonFiniteVec3 == 1);
}

void TestRejectsExtremePosition() {
    std::cout << "  [position beyond magnitude cap rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.position = {2.0e6f, 0.0f, 0.0f}; // 2,000 km — over 1e6 default
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedExtremePosition == 1);
}

void TestRejectsExtremeVelocity() {
    std::cout << "  [velocity beyond magnitude cap rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.velocity = {2.0e5f, 0.0f, 0.0f}; // 200 km/s — over 1e5 default
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedExtremeVelocity == 1);
}

void TestRejectsNaNParameter() {
    std::cout << "  [NaN parameterValue rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.parameterValue = std::numeric_limits<float>::quiet_NaN();
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedNonFiniteParam == 1);
}

void TestRejectsExtremeParameter() {
    std::cout << "  [extreme parameterValue rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.parameterValue = 2.0e6f;
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedExtremeParam == 1);
}

void TestRejectsNegativeSmoothing() {
    std::cout << "  [negative smoothingMs rejected]\n";
    audio::DefaultBoundsValidator v{};
    auto ev = MakeWellFormed();
    ev.parameterSmoothingMs = -1.0f;
    assert(!v.ShouldAccept(ev, 1));
    assert(v.GetStats().rejectedExtremeParam == 1);
}

void TestUnknownSoundCallback() {
    std::cout << "  [unknown soundId rejected via host callback]\n";
    audio::DefaultBoundsValidatorConfig cfg;
    cfg.soundIdIsKnown = [](audio::AudioSoundId id) {
        return id == 0xABCDu; // only this id is "known"
    };
    audio::DefaultBoundsValidator v{cfg};

    auto ev = MakeWellFormed();
    assert(v.ShouldAccept(ev, 1));        // 0xABCD is known
    ev.soundId = 0xBAD;
    assert(!v.ShouldAccept(ev, 1));       // 0xBAD is unknown
    assert(v.GetStats().rejectedUnknownSound == 1);
}

void TestChainShortCircuits() {
    std::cout << "  [ChainReplicationValidator short-circuits on first reject]\n";

    // Validator that always accepts but counts calls.
    struct CountingValidator final : audio::IReplicationValidator {
        int calls = 0;
        bool ShouldAccept(const audio::AudioEvent&, audio::AudioPlayerId) noexcept override {
            ++calls; return true;
        }
    };

    audio::DefaultBoundsValidator strict{}; // will reject NaN
    CountingValidator counter{};

    audio::ChainReplicationValidator chain;
    chain.Add(&strict);
    chain.Add(&counter);

    auto goodEv = MakeWellFormed();
    assert(chain.ShouldAccept(goodEv, 1));
    assert(counter.calls == 1);

    auto badEv = MakeWellFormed();
    badEv.position.x = std::numeric_limits<float>::quiet_NaN();
    assert(!chain.ShouldAccept(badEv, 1));
    // strict rejected first; counter NOT called for the bad event.
    assert(counter.calls == 1);
}

} // namespace

int main() {
    std::cout << "[default_bounds_validator_test]\n";
    TestAcceptsWellFormed();
    TestRejectsNaNPosition();
    TestRejectsInfVelocity();
    TestRejectsExtremePosition();
    TestRejectsExtremeVelocity();
    TestRejectsNaNParameter();
    TestRejectsExtremeParameter();
    TestRejectsNegativeSmoothing();
    TestUnknownSoundCallback();
    TestChainShortCircuits();
    std::cout << "[default_bounds_validator_test] PASSED\n";
    return 0;
}
