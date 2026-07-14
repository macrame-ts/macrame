#pragma once

#include "access.h"
#include "fatal.h"
#include "guarded.h"
#include "task.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ts
{

// Deferred mutation of a `Guarded<T>`: producers `stage()` closures into private
// per-recorder storage (no grant on the target, contention-free between recorders),
// and one `commit` applies everything under a single write access. Readers of the
// target between stage and commit see none of the staged writes; after it, all of
// them -- the stable-snapshot contract. The pipe stays the only arbitration
// mechanism: the commit IS an ordinary write (an `async` job or a graph node's
// declared write access); staging itself needs no synchronization with anyone.
//
// Apply order is deterministic: recorders drain in creation order, FIFO within a
// recorder -- independent of which threads staged when. If a particular recorder's
// commands must make a particular commit, order the producer before the commit
// (graph edge / `after`); commands staged after a commit's cut ride the next one.
//
// See docs/command-buffer-design.md. v1 notes: commands are type-erased closures
// (`std::move_only_function`), one small allocation per command when the capture
// outgrows the SBO -- the typed-POD/arena tier is the planned follow-up; per-slot
// mutexes are uncontended in the intended one-producer-per-recorder use.

namespace detail
{

// Shared journal machinery: per-recorder slots with stable addresses, an atomic
// per-slot cut. Used by `Deferred` (single live state) and `Versioned` (two states;
// the same batch is applied twice there, so commands must be re-invocable --
// `move_only_function` invocation does not consume).
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
    std::deque<Slot> slots_;   // deque: stable addresses for outstanding recorders
};

} // namespace detail

// A producer identity: its own storage (contention-free staging) and a stable key
// in the apply order (recorder creation order). Mint one per producer (one per
// graph node / thread); move-only -- sharing one recorder across threads is safe
// but reintroduces nondeterministic intra-slot order. Must not outlive the
// `Deferred`/`Versioned` it came from.
template<typename T>
class Recorder
{
public:
    Recorder() = default;
    Recorder(Recorder&&) = default;
    Recorder& operator=(Recorder&&) = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Append a deferred write. No grant on the target is taken or needed; the
    // closure runs later, under the commit's write access. Capture by value only
    // (it outlives the staging scope). For `Versioned` targets the closure must be
    // deterministic -- it is applied to both replicas (see `Resync::replay`).
    template<typename Fn>
    void stage(Fn&& fn)
    {
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

// The command buffer: binds to a `Guarded<T>` for its lifetime. `recorder()` mints
// producer handles; `commit_async()` applies everything as one pipe write job;
// `commit(T&)` applies under an access grant the caller already holds (a graph
// node's declared write, or inside a `target.async` write body) -- no second pipe
// round-trip. Must outlive any pending `commit_async` (the destructor waits out
// the target's pipe) and any outstanding `Recorder`.
template<typename T>
class Deferred
{
public:
    explicit Deferred(Guarded<T>& target)
        : target_(&target)
    {}

    // Staged-but-uncommitted commands at destruction are lost writes -- the same
    // severity as an undeclared access. Call `discard()` first if dropping them is
    // intended (e.g. teardown mid-frame).
    ~Deferred()
    {
        // Let any queued commit_async run before judging leftovers.
        detail::Guarded_access::pipe(*target_).wait_until_idle();
#if TS_SAFETY_CHECKS
        if (journal_.has_staged())
            fatal("Deferred destroyed with staged uncommitted commands (lost writes); commit or discard() first");
#endif
    }

    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;

    Recorder<T> recorder()
    {
        return Recorder<T>(journal_.add_slot());
    }

    // Apply everything staged so far to `target`, which the caller must already
    // hold write access to (checked). Cut happens now: commands staged after this
    // point ride the next commit.
    void commit(T& target)
    {
#if TS_SAFETY_CHECKS
        access_check(&target);   // non-const overload: requires a read_write grant
#endif
        auto batch = journal_.cut();
        for (auto& cmd : batch)
            cmd(target);
    }

    // Apply as an ordinary pipe write job: one acquisition amortized over the whole
    // batch. The cut happens when the job RUNS (pipe FIFO position), so it captures
    // everything staged before the write actually happens. Returns the completion.
    Task<void> commit_async(Task_options opts = {})
    {
        return target_->async([this](T& t) { commit(t); }, opts);
    }

    // Drop everything staged so far, explicitly. The escape hatch for teardown.
    void discard()
    {
        journal_.cut();
    }

private:
    Guarded<T>* target_;
    detail::Journal<T> journal_;
};

} // namespace ts
