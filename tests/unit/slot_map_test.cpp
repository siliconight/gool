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

// tests/unit/slot_map_test.cpp
//
// Verifies SlotMap<Handle, T>: handle generations, slot 0 reserved as
// null, generation 0 never returned for live slot, freelist reuse,
// stale-handle rejection.

#include "audio_engine/handles.h"
#include "audio_engine/util/slot_map.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace audio;
using audio::util::SlotMap;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

void TestBasicAllocateGet() {
    SlotMap<EmitterHandle, int> m(4);

    auto h0 = m.Allocate(100);
    EXPECT(h0.has_value());
    EXPECT(h0->index != 0);            // slot 0 reserved as null
    EXPECT(h0->generation != 0);       // generation 0 reserved
    EXPECT(*m.Get(*h0) == 100);
    EXPECT(m.IsValid(*h0));

    auto h1 = m.Allocate(200);
    EXPECT(h1.has_value());
    EXPECT(h1->index != h0->index);
    EXPECT(*m.Get(*h1) == 200);
}

void TestNullHandleRejected() {
    SlotMap<EmitterHandle, int> m(4);
    EmitterHandle null{};
    EXPECT(null.IsNull());
    EXPECT(!m.IsValid(null));
    EXPECT(m.Get(null) == nullptr);
    EXPECT(!m.Free(null));
}

void TestFreeAndStaleHandle() {
    SlotMap<EmitterHandle, int> m(4);
    auto h0 = m.Allocate(11);
    EXPECT(h0.has_value());
    const auto stale = *h0;

    EXPECT(m.Free(*h0));
    EXPECT(!m.IsValid(stale));
    EXPECT(m.Get(stale) == nullptr);
    EXPECT(!m.Free(stale));     // double-free rejected
}

void TestGenerationIncrementsAndFreelistReuse() {
    SlotMap<EmitterHandle, int> m(2);

    auto h0 = m.Allocate(1);
    auto h1 = m.Allocate(2);
    EXPECT(h0 && h1);
    EXPECT(!m.Allocate(3).has_value());      // capacity exhausted

    const auto h0Snapshot = *h0;
    EXPECT(m.Free(*h0));

    auto h2 = m.Allocate(33);
    EXPECT(h2.has_value());
    EXPECT(h2->index == h0Snapshot.index);          // freelist reused the slot
    EXPECT(h2->generation != h0Snapshot.generation); // but generation changed
    EXPECT(!m.IsValid(h0Snapshot));                  // old handle now stale
    EXPECT(*m.Get(*h2) == 33);
}

void TestCapacityExhaustion() {
    SlotMap<EmitterHandle, int> m(3);
    std::vector<EmitterHandle> handles;
    for (int i = 0; i < 3; ++i) {
        auto h = m.Allocate(i);
        EXPECT(h.has_value());
        handles.push_back(*h);
    }
    EXPECT(!m.Allocate(99).has_value());
    EXPECT(m.Count() == 3);
    EXPECT(m.Capacity() == 3);

    for (auto h : handles) EXPECT(m.Free(h));
    EXPECT(m.Count() == 0);

    // Fully refilled.
    for (int i = 0; i < 3; ++i) {
        EXPECT(m.Allocate(i + 100).has_value());
    }
}

void TestForEachVisitsAllLiveSlots() {
    SlotMap<EmitterHandle, int> m(4);
    auto h0 = m.Allocate(7);
    auto h1 = m.Allocate(8);
    auto h2 = m.Allocate(9);
    EXPECT(h0 && h1 && h2);
    EXPECT(m.Free(*h1));

    int sum = 0;
    int count = 0;
    m.ForEach([&](EmitterHandle, int& v) { sum += v; ++count; });
    EXPECT(count == 2);
    EXPECT(sum == 7 + 9);
}

} // namespace

int main() {
    TestBasicAllocateGet();
    TestNullHandleRejected();
    TestFreeAndStaleHandle();
    TestGenerationIncrementsAndFreelistReuse();
    TestCapacityExhaustion();
    TestForEachVisitsAllLiveSlots();
    if (gFails) {
        std::fprintf(stderr, "slot_map_test: %d failure(s)\n", gFails);
        return 1;
    }
    std::printf("slot_map_test: ok\n");
    return 0;
}
