// tests/unit/spsc_ring_test.cpp
//
// Exercises SpscRing<T> in single-thread mode and across two threads.
// Single-binary minimal harness; no gtest dependency.

#include "audio_engine/util/spsc_ring.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using audio::util::SpscRing;

namespace {

int gFails = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  " #cond "\n", __FILE__, __LINE__); ++gFails; } \
} while (0)

void TestBasic() {
    SpscRing<int> r(4);     // capacity 4 -> 5 storage slots, max items 4
    EXPECT(r.Empty());
    EXPECT(r.Capacity() == 4);

    int v = 0;
    EXPECT(!r.Pop(v));

    EXPECT(r.Push(10));
    EXPECT(r.Push(20));
    EXPECT(r.Push(30));
    EXPECT(r.Push(40));
    EXPECT(!r.Push(50));   // full

    EXPECT(r.Pop(v) && v == 10);
    EXPECT(r.Pop(v) && v == 20);
    EXPECT(r.Push(60));    // wraps
    EXPECT(r.Pop(v) && v == 30);
    EXPECT(r.Pop(v) && v == 40);
    EXPECT(r.Pop(v) && v == 60);
    EXPECT(!r.Pop(v));
    EXPECT(r.Empty());
}

void TestWrapAround() {
    SpscRing<int> r(3);
    int v = 0;
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 3; ++i) EXPECT(r.Push(round * 10 + i));
        EXPECT(!r.Push(999));
        for (int i = 0; i < 3; ++i) {
            EXPECT(r.Pop(v));
            EXPECT(v == round * 10 + i);
        }
        EXPECT(r.Empty());
    }
}

void TestSpscThreaded() {
    constexpr int kCount = 100000;
    SpscRing<int> r(1024);

    std::atomic<bool> ready{false};
    std::vector<int>  consumed;
    consumed.reserve(kCount);

    std::thread producer([&]() {
        while (!ready.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kCount; ++i) {
            while (!r.Push(i)) {}
        }
    });
    std::thread consumer([&]() {
        while (!ready.load(std::memory_order_acquire)) {}
        int v = 0;
        for (int i = 0; i < kCount; ++i) {
            while (!r.Pop(v)) {}
            consumed.push_back(v);
        }
    });
    ready.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    EXPECT(static_cast<int>(consumed.size()) == kCount);
    bool ordered = true;
    for (int i = 0; i < kCount; ++i) {
        if (consumed[static_cast<size_t>(i)] != i) {
            ordered = false; break;
        }
    }
    EXPECT(ordered);
}

} // namespace

int main() {
    TestBasic();
    TestWrapAround();
    TestSpscThreaded();
    if (gFails) {
        std::fprintf(stderr, "spsc_ring_test: %d failure(s)\n", gFails);
        return 1;
    }
    std::printf("spsc_ring_test: ok\n");
    return 0;
}
