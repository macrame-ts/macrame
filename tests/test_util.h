#pragma once

#include "ts/access.h"
#include "ts/guarded.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>

namespace tests
{

// An array of `Guarded` needs one `ts::Named` per element -- the type has no unnamed
// constructor and is neither copyable nor movable, so every element is built in place from
// its own tag. Elements are identified by this helper's site rather than the caller's,
// which is all an anonymous fixture array needs.
template<typename T, std::size_t N>
auto make_guarded_array()
{
    return []<std::size_t... I>(std::index_sequence<I...>)
    {
        return std::array<ts::Guarded<T>, N>{ (void(I), ts::Named{})... };
    }(std::make_index_sequence<N>{});
}

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

// A short, deterministic spin used to WIDEN a body's window so overlapping accesses
// actually overlap in wall-clock time (raises TSan's chance of catching a
// reader/writer violation). Seeded per participant by index -- the harness bans
// nondeterministic RNG, and a fixed seed keeps runs reproducible. xorshift, bounded.
inline void tiny_spin(std::uint32_t seed)
{
    std::uint32_t x = seed * 2654435761u + 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    unsigned n = x & 0x3fu;   // 0..63 iterations
    volatile unsigned sink = 0;
    for (unsigned i = 0; i < n; ++i)
        sink = sink + i;
}

// The dual race oracle driven by pipe access bodies. Wrapped as `Guarded<Rw_probe>`, so
// a read grant calls the `const` verb and a write grant the non-const verb; there is one
// instance per `Guarded`, so its atomics are shared across every concurrent access to
// that object. Two independent detectors:
//
//  1. Explicit invariant (fires on any build/scheduler): a reader brackets its body with
//     `active_readers` +/-1 and asserts no writer is active; a writer asserts no reader
//     and no other writer is active. Cheap, but its own atomics are synchronized, so a
//     violation may be raced past and TSan sees nothing from the bookkeeping alone.
//
//  2. Unsynchronized payload (the exact TSan detector): a plain, non-atomic `payload_`
//     array on cache-line-separated words. A writer mutates EVERY word; a reader reads
//     ONE word on a rotating index, so per-word reader traffic stays low. If the pipe
//     ever overlaps a writer with a reader/writer, TSan reports the race on `payload_`
//     directly. The words are separated and reader traffic kept low on purpose -- TSan
//     keeps only ~4 shadow cells per 8-byte word, so a hot word with many concurrent
//     readers would evict the writer's shadow and MISS the race.
class Rw_probe
{
public:
    // Slots: enough that a rotating reader index keeps per-word reader traffic low, so a
    // conflicting writer's shadow is not evicted.
    static constexpr int slots = 64;

    // Read verb. Returns the observed value of the rotating word (so the read is not
    // dead-code-eliminated). `const` -> invoked under a read grant.
    int observe_read(std::uint32_t seed) const
    {
        TS_CHECK_ACCESS();
        active_readers_.fetch_add(1, std::memory_order_relaxed);
        if (writer_active_.load(std::memory_order_relaxed))
            violated_.store(true, std::memory_order_relaxed);
        record_max(max_readers_, active_readers_.load(std::memory_order_relaxed));

        int idx = static_cast<int>(read_cursor_.fetch_add(1, std::memory_order_relaxed) % slots);
        tiny_spin(seed);
        int v = payload_[idx].v;   // unsynchronized read -> TSan detector

        active_readers_.fetch_sub(1, std::memory_order_relaxed);
        return v;
    }

    // Write verb. Stamps a monotonically increasing value into every word (so a later
    // read can assert pipe-ordered visibility) and counts the write. Non-const -> invoked
    // under a write grant.
    void observe_write(std::uint32_t seed)
    {
        TS_CHECK_ACCESS();
        if (writer_active_.exchange(true, std::memory_order_relaxed))
            violated_.store(true, std::memory_order_relaxed);   // a second concurrent writer
        if (active_readers_.load(std::memory_order_relaxed) != 0)
            violated_.store(true, std::memory_order_relaxed);   // a reader overlaps this writer

        int stamp = ++writes_;
        tiny_spin(seed);
        for (int i = 0; i < slots; ++i)
            payload_[i].v = stamp;   // unsynchronized writes -> TSan detector

        writer_active_.store(false, std::memory_order_relaxed);
    }

    bool violated() const { return violated_.load(std::memory_order_relaxed); }
    int max_readers() const { return max_readers_.load(std::memory_order_relaxed); }
    int writes() const { return writes_; }
    int last_stamp() const { return payload_[0].v; }

private:
    struct alignas(std::hardware_destructive_interference_size) Word { int v = 0; };

    Word payload_[slots];
    int writes_ = 0;   // write count; only a writer touches it (single-writer under the pipe)

    mutable std::atomic<int> active_readers_{ 0 };
    mutable std::atomic<bool> writer_active_{ false };
    mutable std::atomic<bool> violated_{ false };
    mutable std::atomic<int> max_readers_{ 0 };
    mutable std::atomic<std::uint32_t> read_cursor_{ 0 };
};

} // namespace tests
