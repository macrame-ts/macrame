#pragma once

#include "ts/task.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

namespace ts
{

// A reusable phase gate for cross-frame work: tasks `co_await gate.next()` to park until the
// start of the next frame, and the frame loop calls `open()` once per frame to release them.
//
// The pattern it serves (docs/internals/coroutine-first.md §4.7): a task that suspends on something
// outside the schedule - an I/O completion, a GPU fence, a multi-frame job - resumes at an
// arbitrary moment, possibly in the middle of a frame, where the systems it wants to touch are
// mid-update. Awaiting the gate after the external wait realigns it to a frame boundary:
//
//   co_await io_done;              // external completion, arbitrary timing
//   co_await gate.next();          // ... realign: resume at the next frame start
//   co_await world.access(...);    // now a sane point to touch the schedule's data
//
// Why this is a type and not two lines of user code. The hand-rolled version - one `Signal`,
// `trigger()` it each frame, `reset()` it for the next - has a missed-wakeup window (a task
// that reads the signal just before the frame boundary can attach to a gate that is about to
// be re-armed) and a precondition that is easy to violate (`Signal::reset` requires every
// waiter to have been released already, and is fatal otherwise). This gate closes both by
// handing out the current frame's signal under a mutex and installing a fresh one at `open()`,
// so a waiter is always attached to a specific frame's gate and no re-arm race exists. The
// cost is one bare block per frame, which is noise at frame scale; `Signal::reset()` remains
// available for the zero-allocation case when you can guarantee the precondition yourself.
//
// `open()` releases waiters through the scheduler rather than on the calling thread. A
// completing task resumes its awaiting frames on the thread that settled it, so an inline
// trigger from the frame loop would run every parked frame - however many accumulated -
// before `open()` returned, stalling frame start by an unbounded amount. Paying one task per
// frame keeps the frame loop's own thread free and lets the resumptions spread across workers.
// `open()` therefore returns immediately and the release is asynchronous; that is the right
// semantics for a phase signal, but it does mean "open() returned" is not "waiters ran".
//
// Not thread-safe against its own destruction (destroy it when no frame is in flight), but
// `next()` and `open()` may be called concurrently from any thread.
class Frame_gate
{
public:
    Frame_gate() = default;

    Frame_gate(const Frame_gate&) = delete;
    Frame_gate& operator=(const Frame_gate&) = delete;

    // A handle on the current frame's gate: it settles when `open()` is next called. Await it
    // to resume at the next frame boundary. Capturing the handle and awaiting it later is
    // fine - it names one specific frame's gate, so a boundary crossed in between releases it
    // rather than being missed.
    [[nodiscard("the gate handle is the wait: co_await it, or nothing parks until the boundary")]]
    Task<void> next()
    {
        std::scoped_lock lock(mutex_);
        // From here until the next `open()`, work is parked on something only the frame
        // loop - a non-worker thread - can release. Register that with the deadlock net,
        // or a frame whose workers happen to run dry while a task waits for the boundary
        // reports as deadlocked (docs/internals/waiting-rule-policy.md §7). Armed on demand rather
        // than for the gate's lifetime, so a gate nobody is waiting on masks nothing.
        if (!pending_)
            pending_.emplace();
        return current_;
    }

    // Frame boundary: release everyone waiting on the current gate and install a fresh one for
    // the next frame. Returns immediately; the release runs on a worker (see above).
    void open()
    {
        Signal opening;
        {
            std::scoped_lock lock(mutex_);
            opening = std::exchange(current_, Signal{});
            pending_.reset();   // this frame's external wakeup has arrived
        }
        ts::launch([opening]() mutable { opening.trigger(); },
            { .priority = priority_.load(std::memory_order_relaxed) });
    }

    // Priority of the release task. `low` (the default) keeps resumed cross-frame work behind
    // the frame's own critical path - the documented default for work that waited whole
    // frames already and can wait a few more microseconds. Atomic so setting it does not race
    // an `open()` on another thread (the class advertises cross-thread `open()`).
    void set_release_priority(Priority priority) { priority_.store(priority, std::memory_order_relaxed); }

private:
    std::mutex mutex_;
    Signal current_;
    // Live while someone has taken this frame's gate and `open()` has not yet come.
    std::optional<External_wait> pending_;
    std::atomic<Priority> priority_{ Priority::low };
};

} // namespace ts
