#pragma once

// The shared micro-benchmark harness: timed-window repetition with a median
// report, used by every benchmark TU. Header-inline so bench TUs share one
// definition with no bench library.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using Clock = std::chrono::steady_clock;

// ~1 s per benchmark (warmup + reps measured rounds), median reported - the
// numbers are stable enough to track for regression monitoring.
inline constexpr auto target = std::chrono::milliseconds(200);
inline constexpr int reps = 4;
inline constexpr int warmup = 1;

// run `work` (returns ops completed per call) repeatedly until `target` elapses,
// warmup + reps times; returns ops/sec per measured rep
template<typename Work>
std::vector<double> measure(Work&& work)
{
    std::vector<double> ops_per_sec;
    for (int r = 0; r < warmup + reps; ++r)
    {
        std::uint64_t ops = 0;
        auto t0 = Clock::now();
        do { ops += work(); } while (Clock::now() - t0 < target);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        if (r >= warmup)
            ops_per_sec.push_back(ops / sec);
    }
    return ops_per_sec;
}

inline double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

inline void report(const char* name, std::vector<double> ops)
{
    double mn = *std::min_element(ops.begin(), ops.end());
    double mx = *std::max_element(ops.begin(), ops.end());
    double md = median(ops);
    std::printf("  %-10s %9.2f M/s   [min %7.2f, max %7.2f]   %9.1f ns/op\n",
        name, md / 1e6, mn / 1e6, mx / 1e6, 1e9 / md);
}
