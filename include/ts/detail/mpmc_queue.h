#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace ts::detail
{

// Bounded lock-free MPMC queue (Vyukov's array + per-cell sequence scheme) with an unbounded
// mutex-guarded overflow, so `push` never fails and never blocks the caller. The ring is the
// lock-free fast path; the overflow is written only when the ring is full (rare - a
// scheduler drains fast) and read only when `overflow_count_ > 0`, so under load both push
// and pop touch no lock. `T` must be trivially/cheaply movable (we store `Task_entry`).
//
// Caveat: once items spill to the overflow they are FIFO among themselves but are
// only drained after the ring empties, so under *sustained* overflow they are delayed
// relative to newer ring items (not lost - every pop path reaches them once the ring
// drains). In the full scheduler this is rare: worker-to-worker submits go to per-worker
// deques, leaving this global queue for external submits, high/low priorities, and overflow.
template<typename T>
class Mpmc_queue
{
public:
    explicit Mpmc_queue(std::size_t capacity = 8192)
        : mask_(round_pow2(capacity) - 1)
        , cells_(mask_ + 1)
    {
        for (std::size_t i = 0; i <= mask_; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    Mpmc_queue(const Mpmc_queue&) = delete;
    Mpmc_queue& operator=(const Mpmc_queue&) = delete;

    // Never fails: ring if it has room, else the mutex overflow.
    void push(T item)
    {
        if (ring_enqueue(item))
            return;
        std::scoped_lock lock(overflow_mutex_);
        overflow_.push_back(std::move(item));
        overflow_count_.fetch_add(1, std::memory_order_release);
    }

    // True if an item was popped. Ring first (lock-free), then the overflow (only if it has
    // items - the count check avoids the lock on the common empty-overflow path).
    bool pop(T& out)
    {
        if (ring_dequeue(out))
            return true;
        if (overflow_count_.load(std::memory_order_acquire) == 0)
            return false;
        std::scoped_lock lock(overflow_mutex_);
        if (overflow_.empty())
            return false;
        out = std::move(overflow_.front());
        overflow_.pop_front();
        overflow_count_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    // Approximate emptiness (racy under concurrent push/pop) - for the shutdown-drain check.
    bool empty() const
    {
        if (overflow_count_.load(std::memory_order_acquire) != 0)
            return false;
        std::size_t deq = dequeue_pos_.load(std::memory_order_acquire);
        std::size_t enq = enqueue_pos_.load(std::memory_order_acquire);
        return deq == enq;
    }

private:
    struct alignas(64) Cell
    {
        std::atomic<std::size_t> seq;
        T data{};
    };

    static std::size_t round_pow2(std::size_t n)
    {
        std::size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    bool ring_enqueue(const T& item)
    {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;)
        {
            cell = &cells_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0)
            {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;   // full
            else
                pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
        cell->data = item;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool ring_dequeue(T& out)
    {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;)
        {
            cell = &cells_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0)
            {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;   // empty
            else
                pos = dequeue_pos_.load(std::memory_order_relaxed);
        }
        out = std::move(cell->data);
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    alignas(64) std::atomic<std::size_t> enqueue_pos_;
    alignas(64) std::atomic<std::size_t> dequeue_pos_;
    std::size_t mask_;
    std::vector<Cell> cells_;

    alignas(64) std::mutex overflow_mutex_;
    std::deque<T> overflow_;
    std::atomic<std::size_t> overflow_count_{ 0 };
};

} // namespace ts::detail
