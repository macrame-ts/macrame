#pragma once

#include "ts/fatal.h"
#include "ts/scheduler.h"

#include <concepts>
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
//
// THE ORDERING CONTRACT. Two properties, deliberately different in strength:
//   - INTRA-recorder order is FIFO and semantic -- build on it (a producer's
//     spawn-before-add is honored).
//   - CROSS-recorder order is ARBITRARY -- never build semantics on it. If
//     correctness depends on which producer's same-key write wins, that is a
//     cross-producer conflict; the fixes are one writer per key, commutative
//     commands, or (typed tier) explicit sort keys -- not ordering guarantees.
//     It is, however, DETERMINISTIC given a deterministic mint/destroy
//     sequence (slots drain in a fixed order every run), so behavior is
//     reproducible -- including the behavior of bugs. `Parallel_recorder` is
//     the explicit, localized surrender of that reproducibility across
//     threads.
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

    // A destroyed recorder's slot goes to the free-list and the next mint reuses
    // it, so live slot count is bounded by PEAK CONCURRENT recorders, not total
    // ever -- mint-and-destroy per frame does not grow the journal. The
    // threshold below then only trips on the remaining pathology: minting
    // recorders that are all kept alive.
    // Reuse footnote: a recycled slot keeps its POSITION, so a new producer
    // inherits the released producer's place in the (arbitrary) cross-recorder
    // order, and any commands the released producer staged but never committed
    // drain ahead of the new owner's. Under the ordering contract above this is
    // observable only to programs already building semantics on cross-recorder
    // order. It does interact with reproducibility: slot assignment is
    // deterministic iff the mint/destroy sequence is -- concurrent dynamic
    // mint/destroy makes it racy.
    static constexpr std::size_t max_slots = 4096;

    Slot& add_slot()
    {
        std::lock_guard lock(register_mutex_);
        if (!free_.empty())
        {
            Slot* slot = free_.back();
            free_.pop_back();
            return *slot;
        }
#if TS_SAFETY_CHECKS
        if (slots_.size() >= max_slots)
        {
            fatal("Journal: slot count exceeded max_slots -- recorders are being minted "
                  "and kept alive per frame; mint once per producer and reuse");
        }
#endif
        return slots_.emplace_back();
    }

    // Called by recorder destructors. The slot is recycled, NOT destroyed:
    // commands staged but not yet cut stay in it and drain on the next cut.
    void release_slot(Slot& slot)
    {
        std::lock_guard lock(register_mutex_);
        free_.push_back(&slot);
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
    std::deque<Slot> slots_;      // deque: stable addresses for outstanding recorders (and Slot holds a mutex -- immovable)
    std::vector<Slot*> free_;     // released by recorder dtors, reused by add_slot (see the reuse note above)
};

// A staged command is type-erased into `Journal<T>::Command` and applied later under
// the commit's write grant, so it must be callable as `void(T&)` (a non-void return
// is discarded). Gating at the `stage` boundary names this contract at the call
// site; without it a shape mismatch surfaces inside `move_only_function`'s
// constructor machinery.
template<typename Fn, typename T>
concept Stage_command = std::invocable<Fn&, T&>;

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
    // deterministic -- it is applied to both replicas (see `Resync::replay`).
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

    // Releases all its slots (see `Recorder::~Recorder`).
    ~Parallel_recorder()
    {
        release();
    }

    Parallel_recorder(Parallel_recorder&& other) noexcept
        : journal_(std::exchange(other.journal_, nullptr))
        , scheduler_(std::exchange(other.scheduler_, nullptr))
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
            scheduler_ = std::exchange(other.scheduler_, nullptr);
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
        , scheduler_(&scheduler)
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
        scheduler_ = nullptr;
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

    detail::Journal<T>* journal_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    std::vector<typename detail::Journal<T>::Slot*> slots_;
};

} // namespace ts
