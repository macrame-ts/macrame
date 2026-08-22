#pragma once

// The producer-side handles of the deferred-write layer: `Recorder<T>` (one per producer)
// and `Parallel_recorder<T>` (one producer parallelized internally, a slot per worker). They
// are minted by `Deferred<T>::recorder()` / `Versioned<T>::recorder()` and their parallel
// counterparts, so `deferred.h` / `versioned.h` include this header; it stands alone for the
// consumer that only needs to name a handle type. The journal they stage into is internal
// (`detail/journal.h`), as is the ordering contract they inherit from it.

#include "ts/detail/journal.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace ts
{

// A producer identity: its own storage (contention-free staging) and a stable key
// in the apply order (recorder creation order). Mint one per producer (one per
// graph node / thread); move-only - sharing one recorder across threads is safe
// but contends its slot mutex and reintroduces nondeterministic intra-slot order
// (use `Parallel_recorder` or per-lane recorders instead). Must not outlive the
// `Deferred`/`Versioned` it came from.
template<typename T>
class Recorder
{
public:
    // Empty (unbound) state - for late binding (a member assigned from
    // `recorder()` in init) and what a moved-from handle becomes. Staging on an
    // empty recorder is fatal under TS_SAFETY_CHECKS.
    Recorder() = default;

    // Destruction releases the slot back to the journal's free-list (bounding
    // live slots by peak concurrent recorders); staged-but-uncommitted commands
    // survive in the slot and drain on the next cut. Must not outlive the
    // owning `Deferred`/`Versioned`.
    ~Recorder()
    {
        release();
    }

    Recorder(Recorder&& other) noexcept
        : journal_(std::exchange(other.journal_, nullptr))
        , slot_(std::exchange(other.slot_, nullptr))
    {}

    Recorder& operator=(Recorder&& other) noexcept
    {
        if (this != &other)
        {
            release();
            journal_ = std::exchange(other.journal_, nullptr);
            slot_ = std::exchange(other.slot_, nullptr);
        }
        return *this;
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Append a deferred write. No grant on the target is taken or needed; the
    // closure runs later, under the commit's write access. Capture by value only
    // (it outlives the staging scope). For `Versioned` targets the closure must be
    // deterministic - it is applied to both replicas (see `Resync::replay`).
    template<typename Fn>
        requires detail::Stage_command<Fn, T>
    void stage(Fn&& fn)
    {
#if TS_SAFETY_CHECKS
        if (!slot_)
            fatal("Recorder::stage on an empty (default-constructed or moved-from) recorder");
#endif
        std::lock_guard lock(slot_->mutex);
        slot_->commands.emplace_back(std::forward<Fn>(fn));
    }

private:
    template<typename> friend class Deferred;
    template<typename> friend class Versioned;

    Recorder(detail::Journal<T>& journal, typename detail::Journal<T>::Slot& slot) noexcept
        : journal_(&journal)
        , slot_(&slot)
    {}

    void release()
    {
        if (slot_)
            journal_->release_slot(*slot_);
        journal_ = nullptr;
        slot_ = nullptr;
    }

    detail::Journal<T>* journal_ = nullptr;
    typename detail::Journal<T>::Slot* slot_ = nullptr;
};

// A producer handle for one logical producer parallelized internally (a
// `parallel_for` staging from many workers): placement is per-worker - each
// scheduler worker owns a private slot, so staging is contention-free under any
// chunking (guided, work-stealing, long tail), with no lane math and no
// balance/lane-count coupling. Slot 0 is the overflow lane for non-worker
// threads: the `parallel_for` caller's own share, `set_inline` bodies on
// external threads, workers of a different scheduler.
//
// The trade (vs index-keyed `Recorder` lanes): placement follows scheduling, so
// the cross-thread apply order is nondeterministic run to run (intra-thread
// stays FIFO; the slot sequence itself is fixed). Use it when per-key commands
// are single or commute (disjoint targets), or when the commit sorts anyway;
// use plain `Recorder`s keyed by logical index when raw order determinism
// matters. A commit-time sort key (Unity ECB-style) is planned with the
// typed-POD tier.
template<typename T>
class Parallel_recorder
{
public:
    Parallel_recorder() = default;   // empty; bind via parallel_recorder(). Staging on empty is fatal.

    // Releases all its slots (see `Recorder::~Recorder`).
    ~Parallel_recorder()
    {
        release();
    }

    Parallel_recorder(Parallel_recorder&& other) noexcept
        : journal_(std::exchange(other.journal_, nullptr))
        , slots_(std::move(other.slots_))
    {
        other.slots_.clear();
    }

    Parallel_recorder& operator=(Parallel_recorder&& other) noexcept
    {
        if (this != &other)
        {
            release();
            journal_ = std::exchange(other.journal_, nullptr);
            slots_ = std::move(other.slots_);
            other.slots_.clear();
        }
        return *this;
    }

    Parallel_recorder(const Parallel_recorder&) = delete;
    Parallel_recorder& operator=(const Parallel_recorder&) = delete;

    // Same contract as `Recorder::stage` (no grant, capture by value,
    // deterministic closures for `Versioned` targets); safe to call from any
    // thread concurrently.
    template<typename Fn>
        requires detail::Stage_command<Fn, T>
    void stage(Fn&& fn)
    {
#if TS_SAFETY_CHECKS
        if (slots_.empty())
            fatal("Parallel_recorder::stage on an empty (default-constructed or moved-from) recorder");
#endif
        auto* slot = slots_[lane()];
        std::lock_guard lock(slot->mutex);
        slot->commands.emplace_back(std::forward<Fn>(fn));
    }

private:
    template<typename> friend class Deferred;
    template<typename> friend class Versioned;

    Parallel_recorder(detail::Journal<T>& journal, Scheduler& scheduler)
        : journal_(&journal)
    {
        int workers = scheduler.worker_count();
        slots_.reserve(static_cast<std::size_t>(workers) + 1);
        for (int i = 0; i < workers + 1; ++i)
            slots_.push_back(&journal.add_slot());
    }

    void release()
    {
        for (auto* slot : slots_)
            journal_->release_slot(*slot);
        slots_.clear();
        journal_ = nullptr;
    }

    // This thread's slot: its worker index + 1 when it is a worker of the one process-wide pool
    // the recorder is bound to (the only pool with workers), else the shared overflow lane (0).
    // One TLS read + two compares.
    std::size_t lane() const
    {
        int w = current_worker_index();
        if (w < 0 || static_cast<std::size_t>(w) + 1 >= slots_.size())
            return 0;
        return static_cast<std::size_t>(w) + 1;
    }

    detail::Journal<T>* journal_ = nullptr;
    std::vector<typename detail::Journal<T>::Slot*> slots_;
};

} // namespace ts
