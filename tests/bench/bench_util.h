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

// tests/bench/bench_util.h
//
// Minimal benchmarking helper. No Google Benchmark dependency — we want
// the engine library to ship without benchmark machinery in its build
// graph. Just wall-clock timing with `Run(name, iters, fn)` reporting
// total time, ns/op, and ops/sec.
//
// Usage:
//
//     int main() {
//         BenchSuite suite{"my_bench"};
//         suite.Run("scenario_a", 1000, [&]() { do_thing(); });
//         suite.Run("scenario_b", 100,  [&]() { do_other(); });
//         return suite.Summary();
//     }
//
// Each Run prints a line. Summary prints a closing rule.
//
// Anti-DCE: the scenario function may return a value via
// DoNotOptimize(...) which forces the compiler to materialize the
// result. Without this, the optimizer can collapse "compute and
// throw away" benchmarks to nothing.

#ifndef AUDIO_ENGINE_TESTS_BENCH_UTIL_H
#define AUDIO_ENGINE_TESTS_BENCH_UTIL_H

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>

namespace audio_bench {

// Force the compiler to keep `value` materialized — used inside hot
// loops to prevent dead-code elimination of the work being measured.
// Pinned at "asm volatile + memory clobber" because that's the
// portable-ish form across GCC and Clang.
template <typename T>
inline void DoNotOptimize(T const& value) {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    (void)value;  // best effort on MSVC; usually fine in /O2
#endif
}

class BenchSuite {
public:
    explicit BenchSuite(const char* name) : name_(name) {
        std::printf("\n=== %s ===\n", name_);
        std::printf("%-40s %12s %12s %14s\n",
                    "scenario", "iters", "total_ms", "ns_per_op");
    }

    template <typename F>
    void Run(const char* scenario, size_t iters, F&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iters; ++i) fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double totalMs =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double nsPerOp =
            (totalMs * 1.0e6) / static_cast<double>(iters);
        std::printf("%-40s %12zu %12.3f %14.2f\n",
                    scenario, iters, totalMs, nsPerOp);
    }

    int Summary() const {
        std::printf("=== end %s ===\n\n", name_);
        return 0;
    }

private:
    const char* name_;
};

} // namespace audio_bench

#endif // AUDIO_ENGINE_TESTS_BENCH_UTIL_H
