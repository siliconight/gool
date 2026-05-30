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

// audio_engine/util/slot_map.h
//
// Generation-counted slot map. Storage is a single allocation sized at
// construction; never resized. Slots are tracked by an intrusive freelist
// (the slot's `nextFree` index). Stale handles to recycled slots fail closed
// (Get returns nullptr, Free returns false) rather than addressing whatever
// resource currently occupies the slot.
//
// Index 0 is reserved as the null slot, so default-constructed handles
// (index=0, generation=0) are always invalid. Generation 0 is reserved as
// "never allocated"; the first allocation of a slot bumps generation to 1.

#ifndef AUDIO_ENGINE_UTIL_SLOT_MAP_H
#define AUDIO_ENGINE_UTIL_SLOT_MAP_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace audio::util {

template <typename Handle, typename T>
class SlotMap {
public:
    explicit SlotMap(uint32_t capacity)
        : slots_(static_cast<size_t>(capacity) + 1) {
        // Slot 0 is the null slot, never allocatable.
        slots_[0].generation = 0;
        slots_[0].occupied   = false;
        slots_[0].nextFree   = 0;

        // Build the freelist over indices [1, capacity].
        for (uint32_t i = 1; i <= capacity; ++i) {
            slots_[i].generation = 0;
            slots_[i].occupied   = false;
            slots_[i].nextFree   = (i + 1 <= capacity) ? (i + 1) : 0u;
        }
        firstFree_ = (capacity > 0) ? 1u : 0u;
        capacity_  = capacity;
    }

    SlotMap(const SlotMap&)            = delete;
    SlotMap& operator=(const SlotMap&) = delete;

    std::optional<Handle> Allocate(T&& value) {
        if (firstFree_ == 0) return std::nullopt;
        const uint32_t idx = firstFree_;
        firstFree_         = slots_[idx].nextFree;

        slots_[idx].generation += 1;
        if (slots_[idx].generation == 0) slots_[idx].generation = 1;   // skip 0
        slots_[idx].occupied = true;
        slots_[idx].value    = std::move(value);

        ++count_;
        return Handle{idx, slots_[idx].generation};
    }

    std::optional<Handle> Allocate(const T& value) {
        T copy = value;
        return Allocate(std::move(copy));
    }

    std::optional<Handle> AllocateDefault() {
        return Allocate(T{});
    }

    bool Free(Handle h) {
        if (!IsValid(h)) return false;
        const uint32_t idx = h.index;
        slots_[idx].occupied = false;
        slots_[idx].value    = T{};                // reset / destroy
        slots_[idx].nextFree = firstFree_;
        firstFree_           = idx;
        --count_;
        return true;
    }

    T* Get(Handle h) noexcept {
        if (!IsValid(h)) return nullptr;
        return &slots_[h.index].value;
    }

    const T* Get(Handle h) const noexcept {
        if (!IsValid(h)) return nullptr;
        return &slots_[h.index].value;
    }

    bool IsValid(Handle h) const noexcept {
        if (h.index == 0 || h.index > capacity_) return false;
        const Slot& s = slots_[h.index];
        return s.occupied && s.generation == h.generation;
    }

    uint32_t Count()    const noexcept { return count_; }
    uint32_t Capacity() const noexcept { return capacity_; }

    // Iterate occupied slots. fn is called as fn(Handle, T&) (or const T&).
    // Iteration is in slot-index order; insertion order is not preserved.
    template <typename F>
    void ForEach(F&& fn) {
        for (uint32_t i = 1; i <= capacity_; ++i) {
            if (slots_[i].occupied) {
                fn(Handle{i, slots_[i].generation}, slots_[i].value);
            }
        }
    }

    template <typename F>
    void ForEach(F&& fn) const {
        for (uint32_t i = 1; i <= capacity_; ++i) {
            if (slots_[i].occupied) {
                fn(Handle{i, slots_[i].generation}, slots_[i].value);
            }
        }
    }

private:
    struct Slot {
        T        value{};
        uint32_t generation = 0;
        uint32_t nextFree   = 0;
        bool     occupied   = false;
    };

    std::vector<Slot> slots_;
    uint32_t          firstFree_ = 0;
    uint32_t          count_     = 0;
    uint32_t          capacity_  = 0;
};

} // namespace audio::util

#endif // AUDIO_ENGINE_UTIL_SLOT_MAP_H
