#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ts::detail
{

// Chase-Lev bounded work-stealing deque, with the Lê-Pop-Cohen-Nardelli ("Correct and
// Efficient Work-Stealing for Weak Memory Models") C11 memory orderings. The single OWNER
// pushes/takes at the bottom -- LIFO, cache-hot: this is the producer fast path (a task
// launching more work) and touches no shared cache line in the common case. THIEVES steal
// from the top -- FIFO, so a thief grabs the oldest (largest) subtree. Bounded: `push`
// returns false when full so the caller can overflow to a global queue (avoids the
// growable-array / safe-reclamation complexity). `T` must be trivially copyable.
//
// The two seq_cst fences (one in `take`, one in `steal`) are the crux -- they enforce the
// StoreLoad ordering between the owner's bottom-store and the thief's top-load that x86 TSO
// would otherwise reorder. The array cells are atomics (relaxed) so the last-element
// owner/thief overlap is data-race-free (resolved by the top CAS), not UB.
template<typename T>
class Work_stealing_deque
{
public:
    explicit Work_stealing_deque(std::size_t capacity = 1024)
        : mask_(round_pow2(capacity) - 1)
        , buffer_(mask_ + 1)
    {
        top_.store(0, std::memory_order_relaxed);
        bottom_.store(0, std::memory_order_relaxed);
    }

    Work_stealing_deque(const Work_stealing_deque&) = delete;
    Work_stealing_deque& operator=(const Work_stealing_deque&) = delete;

    // OWNER only. Push at the bottom; false if full (caller overflows elsewhere).
    bool push(T item)
    {
        std::int64_t b = bottom_.load(std::memory_order_relaxed);
        std::int64_t t = top_.load(std::memory_order_acquire);
        if (b - t > static_cast<std::int64_t>(mask_))   // capacity == mask_ + 1
            return false;   // full
        buffer_[static_cast<std::size_t>(b) & mask_].store(item, std::memory_order_relaxed);
        // Release STORE (not a standalone release fence): it still orders the cell store above
        // before publishing the new `bottom`, and a thief's acquire load of `bottom` in steal
        // synchronizes with it -- so the item (and everything the producer wrote before it, e.g.
        // a heap closure) is visible to the thief. Variable-based so ThreadSanitizer tracks the
        // happens-before (TSan does not model `atomic_thread_fence`).
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // OWNER only. Take from the bottom (LIFO). false if empty.
    bool take(T& out)
    {
        std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_relaxed);

        if (t <= b)
        {
            out = buffer_[static_cast<std::size_t>(b) & mask_].load(std::memory_order_relaxed);
            if (t == b)
            {
                // Last element: race a thief for it via the top CAS.
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return false;   // lost to a thief
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return true;
        }

        bottom_.store(b + 1, std::memory_order_relaxed);   // was empty
        return false;
    }

    // THIEF (any other thread). Steal from the top (FIFO). false if empty / lost the race.
    bool steal(T& out)
    {
        std::int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t b = bottom_.load(std::memory_order_acquire);
        if (t < b)
        {
            out = buffer_[static_cast<std::size_t>(t) & mask_].load(std::memory_order_relaxed);
            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed))
                return false;   // lost the race with the owner / another thief
            return true;
        }
        return false;   // empty
    }

    // Approximate emptiness (racy; for the shutdown-drain check).
    bool empty() const
    {
        std::int64_t b = bottom_.load(std::memory_order_acquire);
        std::int64_t t = top_.load(std::memory_order_acquire);
        return b <= t;
    }

private:
    static std::size_t round_pow2(std::size_t n)
    {
        std::size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    alignas(64) std::atomic<std::int64_t> top_;
    alignas(64) std::atomic<std::int64_t> bottom_;
    std::size_t mask_;
    std::vector<std::atomic<T>> buffer_;
};

} // namespace ts::detail
