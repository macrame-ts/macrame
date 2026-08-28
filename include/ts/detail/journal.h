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
// deterministic drain order. See docs/internals/command-buffer-design.md.
//
// v1 notes: commands are type-erased closures (`std::move_only_function`), one
// small allocation per command when the capture outgrows the SBO - the
// typed-POD/arena tier is the planned follow-up; per-slot mutexes are
// uncontended in the intended one-producer-per-slot use (they exist for the
// dynamic stage-vs-cut race).

template<typename T> class Deferred;
template<typename T> class Versioned;

namespace detail
{

// Per-recorder slots with stable addresses, an atomic per-slot cut. The same
// batch may be applied twice (`Versioned`'s replay resync), so commands must be
// re-invocable - `move_only_function` invocation does not consume.
//
// The ordering contract. Two properties, deliberately different in strength:
//   - intra-recorder order is FIFO and semantic - build on it (a producer's
//     spawn-before-add is honored).
//   - cross-recorder order is arbitrary - never build semantics on it. If
//     correctness depends on which producer's same-key write wins, that is a
//     cross-producer conflict; the fixes are one writer per key, commutative
//     commands, or (typed tier) explicit sort keys - not ordering guarantees.
//     It is, however, deterministic given a deterministic mint/destroy
//     sequence (slots drain in a fixed order every run), so behavior is
//     reproducible - including the behavior of bugs. `Parallel_recorder` is
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
    // it, so live slot count is bounded by peak concurrent recorders, not total
    // ever - mint-and-destroy per frame does not grow the journal. The
    // threshold below then only trips on the remaining pathology: minting
    // recorders that are all kept alive.
    // Reuse footnote: a recycled slot keeps its position, so a new producer
    // inherits the released producer's place in the (arbitrary) cross-recorder
    // order, and any commands the released producer staged but never committed
    // drain ahead of the new owner's. Under the ordering contract above this is
    // observable only to programs already building semantics on cross-recorder
    // order. It does interact with reproducibility: slot assignment is
    // deterministic iff the mint/destroy sequence is - concurrent dynamic
    // mint/destroy makes it racy.
    static constexpr std::size_t max_slots = 4096;

#if TS_SAFETY_CHECKS
    // A `Recorder` outliving its `Deferred`/`Versioned` would call `release_slot` on a
    // destroyed journal (UAF). Catch it here: `slots_` never shrinks and every slot is
    // either free-listed or held by a live recorder, so an outstanding count
    // (`slots_.size() > free_.size()`) at journal destruction means a recorder is still
    // alive. The journal dies during its owner's member destruction, before the outliving
    // recorder's own destructor runs, so the fatal precedes the UAF. No live-recorder
    // state is added - the count is exact from the existing members.
    ~Journal()
    {
        std::lock_guard lock(register_mutex_);
        if (slots_.size() > free_.size())
        {
            fatal("Journal destroyed while a Recorder still holds a slot - a Recorder must not "
                  "outlive the Deferred/Versioned it was minted from");
        }
    }
#endif

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
            fatal("Journal: slot count exceeded max_slots - recorders are being minted "
                  "and kept alive per frame; mint once per producer and reuse");
        }
#endif
        return slots_.emplace_back();
    }

    // Called by recorder destructors. The slot is recycled, not destroyed:
    // commands staged but not yet cut stay in it and drain on the next cut.
    void release_slot(Slot& slot)
    {
        std::lock_guard lock(register_mutex_);
        free_.push_back(&slot);
    }

    // Take everything staged so far into `out`, reusing its buffer (cleared first),
    // flattened in slot-creation order (FIFO within a slot). Stages racing the cut land
    // wholly before or wholly after it - a straggler rides the next cut. A caller with a
    // stable batch slot (`Versioned`/`Deferred`) passes it here so steady-state cuts
    // allocate nothing.
    void cut(std::vector<Command>& out)
    {
        out.clear();
        std::lock_guard lock(register_mutex_);
        // `slots_` holds every slot ever created up to the peak; a released one is empty
        // (in `free_`) but still walked, so the cut cost scales with peak recorder count,
        // not the live count. Fine for the intended few-long-lived-recorders use.
        for (Slot& slot : slots_)
        {
            std::lock_guard slot_lock(slot.mutex);
            for (Command& cmd : slot.commands)
                out.push_back(std::move(cmd));
            slot.commands.clear();
        }
    }

    // Value-returning form: a fresh batch each call, for one-shot callers that keep no
    // slot to reuse.
    std::vector<Command> cut()
    {
        std::vector<Command> batch;
        cut(batch);
        return batch;
    }

    // Drop everything staged without building a batch - the allocation-free `discard()`.
    void clear_staged()
    {
        std::lock_guard lock(register_mutex_);
        for (Slot& slot : slots_)
        {
            std::lock_guard slot_lock(slot.mutex);
            slot.commands.clear();
        }
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
    std::deque<Slot> slots_;      // deque: stable addresses for outstanding recorders (and Slot holds a mutex - immovable)
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

} // namespace ts
