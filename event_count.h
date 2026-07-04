#pragma once

#include <atomic>
#include <cstdint>

namespace detail
{

// A minimal event count (Vyukov / futex idiom) for parking idle workers. Replaces the
// counting semaphore: a worker that finds no work snapshots the epoch (`prepare_wait`),
// RE-CHECKS its predicate (is there work / are we shutting down?), and parks
// (`commit_wait`) only if the epoch is unchanged. A `notify` that races the park is never
// lost -- it bumped the epoch, so `commit_wait` returns at once. The re-check between
// `prepare_wait` and `commit_wait` is essential: it catches work that was enqueued (and
// notified) just before the snapshot, which would otherwise park against a notify that has
// already fired.
//
// Built on `std::atomic::wait`/`notify` (a direct futex / WaitOnAddress wrapper); `notify`
// is a no-op when nothing is parked, so it is cheap to call. Unlike the semaphore this has
// no per-task count to keep balanced -- so the class of shutdown permit-accounting bug is
// gone -- and it extends to "is there work anywhere?" once tasks live in per-worker deques
// (stage 3), which a count-based semaphore cannot express.
class Event_count
{
public:
    // Snapshot the epoch; hold it across the predicate re-check, then pass to commit_wait.
    std::uint32_t prepare_wait() const noexcept
    {
        return epoch_.load(std::memory_order_acquire);
    }

    // Park until the epoch moves off `key` (a notify). Spurious wakeups are absorbed by
    // std::atomic::wait (it re-checks the value).
    void commit_wait(std::uint32_t key) const noexcept
    {
        epoch_.wait(key, std::memory_order_acquire);
    }

    // Abandon a prepared wait (the predicate became true) -- nothing to undo.
    void cancel_wait() const noexcept {}

    void notify_one() noexcept
    {
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_one();
    }

    void notify_all() noexcept
    {
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_all();
    }

private:
    std::atomic<std::uint32_t> epoch_{ 0 };
};

} // namespace detail
