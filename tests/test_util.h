#pragma once

#include "access.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace tests
{

// A guarded thread-unsafe "system" used across the suite.
class Counter
{
public:
    void increment()
    {
        TS_CHECK_ACCESS();
        ++value_;
    }

    void add(int n)
    {
        TS_CHECK_ACCESS();
        value_ += n;
    }

    int value() const
    {
        TS_CHECK_ACCESS();
        return value_;
    }

private:
    int value_ = 0;
};

inline void record_max(std::atomic<int>& max, int value)
{
    int prev = max.load();
    while (value > prev && !max.compare_exchange_weak(prev, value)) {}
}

template<typename Pred>
void wait_until(Pred pred)
{
    while (!pred())
        std::this_thread::yield();
}

// Deterministic concurrency check: N jobs each call arrive(); each waits (up to a
// timeout) for the others. met() is true only if all N were in flight at once.
// Replaces flaky "peak == N" timing assertions; fails (not hangs) without enough
// concurrency.
class Parallel_gate
{
public:
    explicit Parallel_gate(int count) : n_(count) {}

    void arrive()
    {
        if (arrived_.fetch_add(1) + 1 == n_)
            all_met_.store(true);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (arrived_.load() < n_ && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
    }

    bool met() const { return all_met_.load(); }

private:
    std::atomic<int> arrived_{ 0 };
    std::atomic<bool> all_met_{ false };
    int n_;
};

} // namespace tests
