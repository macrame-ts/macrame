#pragma once

#include "fatal.h"
#include "scheduler.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ts
{

// The shared staging machinery under `Deferred<T>` (journal + single live state,
// deferred.h) and `Versioned<T>` (journal + two replicas, versioned.h): per-
// producer slots with contention-free staging, an atomic cut, and a
// deterministic drain order. See docs/command-buffer-design.md.
//
// v1 notes: commands are type-erased closures (`std::move_only_function`), one
// small allocation per command when the capture outgrows the SBO -- the
// typed-POD/arena tier is the planned follow-up; per-slot mutexes are
// uncontended in the intended one-producer-per-slot use (they exist for the
// dynamic stage-vs-cut race).

template<typename T> class Deferred;
template<typename T> class Versioned;

namespace detail
{

// Per-recorder slots with stable addresses, an atomic per-slot cut. The same
// batch may be applied twice (`Versioned`'s replay resync), so commands must be
// re-invocable -- `move_only_function` invocation does not consume.
template<typename T>
class Journal
{
public:
    using Command = std::move_only_function<void(T&)>;

    struct Slot
    {
        std::mutex mutex;
        std::vector<Command> commands;
    };

    Slot& add_slot()
    {
        std::lock_guard lock(register_mutex_);
        return slots_.emplace_back();
    }

    // Take everything staged so far, flattened in slot-creation order (FIFO within
    // a slot). Stages racing the cut land wholly before or wholly after it -- a
    // straggler simply rides the next cut.
    std::vector<Command> cut()
    {
        std::vector<Command> batch;
        std::lock_guard lock(register_mutex_);
        for (Slot& slot : slots_)
        {
            std::lock_guard slot_lock(slot.mutex);
            for (Command& cmd : slot.commands)
                batch.push_back(std::move(cmd));
            slot.commands.clear();
        }
        return batch;
    }

    bool has_staged()
    {
        std::lock_guard lock(register_mutex_);
        for (Slot& slot : slots_)
        {
            std::lock_guard slot_lock(slot.mutex);
            if (!slot.commands.empty())
                return true;
        }
        return false;
    }

private:
    std::mutex register_mutex_;
    std::deque<Slot> slots_;   // deque: stable addresses for outstanding recorders (and Slot holds a mutex -- immovable)
};

} // namespace detail

// A producer identity: its own storage (contention-free staging) and a stable key
// in the apply order (recorder creation order). Mint one per producer (one per
// graph node / thread); move-only -- sharing one recorder across threads is safe
// but contends its slot mutex and reintroduces nondeterministic intra-slot order
// (use `Parallel_recorder` or per-lane recorders instead). Must not outlive the
// `Deferred`/`Versioned` it came from.
template<typename T>
class Recorder
{
public:
    // Empty (unbound) state -- for late binding (a member assigned from
    // `recorder()` in init) and what a moved-from handle becomes. Staging on an
    // empty recorder is fatal under TS_SAFETY_CHECKS.
    Recorder() = default;

    Recorder(Recorder&& other) noexcept
        : slot_(std::exchange(other.slot_, nullptr))
    {}

    Recorder& operator=(Recorder&& other) noexcept
    {
        slot_ = std::exchange(other.slot_, nullptr);
        return *this;
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Append a deferred write. No grant on the target is taken or needed; the
    // closure runs later, under the commit's write access. Capture by value only
    // (it outlives the staging scope). For `Versioned` targets the closure must be
    // deterministic -- it is applied to both replicas (see `Resync::replay`).
    template<typename Fn>
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

    explicit Recorder(typename detail::Journal<T>::Slot& slot) noexcept
        : slot_(&slot)
    {}

    typename detail::Journal<T>::Slot* slot_ = nullptr;
};

// A producer handle for ONE logical producer parallelized internally (a
// `parallel_for` staging from many workers): placement is per-WORKER -- each
// scheduler worker owns a private slot, so staging is contention-free under any
// chunking (guided, work-stealing, long tail), with no lane math and no
// balance/lane-count coupling. Slot 0 is the overflow lane for non-worker
// threads: the `parallel_for` caller's own share, retraction-inline bodies,
// `set_inline` bodies on external threads, workers of a different scheduler.
//
// The trade (vs index-keyed `Recorder` lanes): placement follows scheduling, so
// the CROSS-thread apply order is nondeterministic run to run (intra-thread
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
    Parallel_recorder(Parallel_recorder&&) = default;              // moved-from vector is empty -> inert
    Parallel_recorder& operator=(Parallel_recorder&&) = default;
    Parallel_recorder(const Parallel_recorder&) = delete;
    Parallel_recorder& operator=(const Parallel_recorder&) = delete;

    // Same contract as `Recorder::stage` (no grant, capture by value,
    // deterministic closures for `Versioned` targets); safe to call from any
    // thread concurrently.
    template<typename Fn>
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
        : scheduler_(&scheduler)
    {
        int workers = scheduler.worker_count();
        slots_.reserve(static_cast<std::size_t>(workers) + 1);
        for (int i = 0; i < workers + 1; ++i)
            slots_.push_back(&journal.add_slot());
    }

    // This thread's slot: its worker index + 1 when it is a worker of the bound
    // scheduler, else the shared overflow lane (0). One TLS read + two compares.
    std::size_t lane() const
    {
        int w = current_worker_index;
        if (current_scheduler != scheduler_ || w < 0 || static_cast<std::size_t>(w) + 1 >= slots_.size())
            return 0;
        return static_cast<std::size_t>(w) + 1;
    }

    std::vector<typename detail::Journal<T>::Slot*> slots_;
    Scheduler* scheduler_ = nullptr;
};

} // namespace ts
