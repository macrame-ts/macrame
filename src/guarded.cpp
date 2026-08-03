#include "ts/guarded.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace ts
{

namespace
{
// The one process-wide scheduler, reconfigurable via `configure_scheduler`. A `unique_ptr`
// in a namespace static so program exit destroys it (joins its workers) -- clean shutdown,
// as the old Meyers static gave. `g_fast` is a lock-free read cache for the hot path (every
// `global_scheduler()` from an external thread); the mutex guards lazy init + the
// teardown/recreate swap. Reconfigure racing concurrent use is undefined (documented) -- the
// `g_fast` reset before teardown is belt-and-suspenders, not a use-vs-reconfigure guarantee.
std::mutex g_sched_mutex;
std::unique_ptr<Scheduler> g_scheduler;
Scheduler_config g_config;
std::atomic<Scheduler*> g_fast{ nullptr };
}

Scheduler& global_scheduler()
{
    if (Scheduler* s = g_fast.load(std::memory_order_acquire))
        return *s;
    std::lock_guard lock(g_sched_mutex);
    if (!g_scheduler)
    {
        g_scheduler = std::make_unique<Scheduler>(g_config);
        g_fast.store(g_scheduler.get(), std::memory_order_release);
    }
    return *g_scheduler;
}

Scheduler_config current_scheduler_config()
{
    std::lock_guard lock(g_sched_mutex);
    return g_config;
}

void configure_scheduler(Scheduler_config config)
{
    std::lock_guard lock(g_sched_mutex);
    g_fast.store(nullptr, std::memory_order_release);
    g_scheduler.reset();   // dtor: quit + join workers, drain queued tasks
    g_config = config;
    g_scheduler = std::make_unique<Scheduler>(config);
    g_fast.store(g_scheduler.get(), std::memory_order_release);
}

namespace detail
{

void submit_closure(Scheduler& scheduler, std::move_only_function<void()> closure, Priority priority)
{
    auto* held = new std::move_only_function<void()>(std::move(closure));
    scheduler.submit(
        [](void* data)
        {
            auto* fn = static_cast<std::move_only_function<void()>*>(data);
            (*fn)();
            delete fn;
        },
        held, priority);
}

// Trampoline for a queued block dispatch: the block travels as the entry's `data_` (its ref
// adopted from the queue), the generation via the block's `dispatch_arg`. No heap
// closure -- the block IS the payload -- so a bare task dispatch allocates nothing beyond its
// own block (killing the per-dispatch `submit_closure` alloc that hit every queued task).
static void run_block_dispatch(void* data)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});   // adopt the queued ref
    std::uint64_t gen = block->dispatch_arg.load(std::memory_order_acquire);
    if (block->execute)
        block->execute(block, gen);              // claims `gen` internally
    else if (block->claim(gen))                  // bodyless: claim guards against machinery bugs
        block->complete();
}   // `block` decrements here -> releases the ref the queue held

// A block whose prerequisites are all met: schedule it to run (its body, or, if
// bodyless, just complete). Bridges task.h's lock-counter to the scheduler.
//
// `gen` was captured by the RELEASER at/before the `num_locks` decrement that hit zero (see
// `release`), so it names the run that actually became ready -- it is never re-read here.
// One dispatch per run and re-arm only after settle (see `run_state`) mean the slot holds
// at most one live value at a time, so a plain release store publishes it.
void submit_ready(Task_ptr block, std::uint64_t gen)
{
    Priority priority = block->flags.priority;
    block->dispatch_arg.store(gen, std::memory_order_release);
    // Hand the block's ref to the queue (release, no dec); the trampoline adopts it back.
    global_scheduler().submit(&run_block_dispatch, block.release(), priority);
}

// ===== the evolved mutex pipe (docs/pipe-rebase.md §0.2) ====================================
//
// One mutex per pipe guards the admission state and the intrusive queue of the tasks' own
// embedded `Pipe_link`s. An admitted entry's TURN fires `release()` on its owner -- the pipe
// is a prerequisite source for the block machinery (the `pipe_count` trigger in task.h), not
// a dispatcher -- and a multi-object task's turn also enters its next link (the sequential
// canonical cascade). Turn-firing happens OUTSIDE the mutex: `release()` reaching zero
// dispatches (a scheduler submit, possibly a wake syscall; worker-less mode executes at
// submit), so admission collects the granted entries under the lock and fires after unlock.
// The task settles -> `advance_pipe_links` releases each entered pipe, admitting successors.

namespace
{

// A held grant's queue entry (`pipe_acquire`, the coroutine guards): a link with a null
// `owner` plus the grant callback. The one allocating pipe path; freed when its admission
// fires the callback. An immediate (uncontended) acquire allocates nothing.
struct Hold_node
{
    Pipe_link link;
    std::move_only_function<void()> on_acquired;
    Task_control_block* grant_owner = nullptr;   // published as `writer_owner` for a write hold
};

// The entries one admission pass granted, chained through their `next` fields (they left
// the queue, so the field is free). Fired after the mutex is released.
struct Granted
{
    Pipe_link* head = nullptr;
    Pipe_link* tail = nullptr;

    void push(Pipe_link* l)
    {
        l->next = nullptr;
        (tail != nullptr ? tail->next : head) = l;
        tail = l;
    }
};

// Caller holds `pipe.mutex` for every function below that mutates admission state.

void queue_push(Pipe& pipe, Pipe_link* l)
{
    l->next = nullptr;
    (pipe.queue_tail != nullptr ? pipe.queue_tail->next : pipe.queue_head) = l;
    pipe.queue_tail = l;
}

bool admissible(const Pipe& pipe, Access mode)
{
    if (mode == Access::read_only)
        return !pipe.writer_active;
    return !pipe.writer_active && pipe.active_readers == 0;
}

// Apply admission state for `l`: reader count / writer flag + grant-holder identity + the
// harness's grant-window epoch.
void admit_locked(Pipe& pipe, Pipe_link* l)
{
    if (l->mode == Access::read_only)
    {
        ++pipe.active_readers;
        return;
    }
    pipe.writer_active = true;
    Task_control_block* owner = l->owner != nullptr
        ? l->owner
        : reinterpret_cast<Hold_node*>(l)->grant_owner;   // `link` is the node's first member
    pipe.writer_owner.store(owner, std::memory_order_release);
#if TS_SAFETY_CHECKS
    pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window opens
#endif
}

// Admit as many queued entries as the reader/writer rules allow (FIFO: a blocked front
// blocks everything behind it), collecting them for post-unlock firing.
void collect_admissions(Pipe& pipe, Granted& granted)
{
    while (pipe.queue_head != nullptr && admissible(pipe, pipe.queue_head->mode))
    {
        Pipe_link* l = pipe.queue_head;
        pipe.queue_head = l->next;
        if (pipe.queue_head == nullptr)
            pipe.queue_tail = nullptr;
        admit_locked(pipe, l);
        granted.push(l);
    }
}

void pipe_enter_link(Pipe_link& l, Task_ptr* record);

// An admitted task entry's turn: enter the owner's next link (the cascade), then fire the
// pipe-turn prerequisite. Everything needed is read into locals first -- once `release`
// drops the owner's last lock the task can run, settle, and be destroyed (links included)
// on another thread, and this frame's entry ref is the only thing pinning it until then.
void fire_task_turn(Pipe_link& l)
{
    Task_control_block* owner = l.owner;
    std::uint8_t index = l.index;
    Task_ptr keep(owner, Adopt_ref{});   // adopt the entry's ref (taken at enter)
    if (index + 1 < owner->pipe_count)
        pipe_enter_link(owner->pipe_links[index + 1], nullptr);
    Task_control_block::release(keep);
}   // `keep` drops the entry ref; the owner lives on via the queue/handle refs downstream

// Fire one admission pass's granted entries, in admission order. Runs WITHOUT the mutex.
void fire_granted(Granted& granted)
{
    Pipe_link* l = granted.head;
    while (l != nullptr)
    {
        Pipe_link* next = l->next;   // read first: firing may free the entry's owner
        if (l->owner != nullptr)
        {
            fire_task_turn(*l);
        }
        else
        {
            // A held grant: hand the pipe to the holder via its callback (scheduled, as a
            // closure -- the holder resumes on a worker), then retire the node.
            auto* node = reinterpret_cast<Hold_node*>(l);
            submit_closure(global_scheduler(), std::move(node->on_acquired), Priority::normal);
            delete node;
        }
        l = next;
    }
}

// Release the pipe in `mode` and admit + fire whatever that unblocks; notify a
// `wait_until_idle` waiter if the pipe drained. The tail shared by every way an access
// ends (a settled task's `advance_pipe_links`, an inline body's release, `pipe_release`).
void release_and_redispatch(Pipe& pipe, Access mode)
{
    Granted granted;
    {
        std::scoped_lock lock(pipe.mutex);
        if (mode == Access::read_only)
        {
            --pipe.active_readers;
        }
        else
        {
            pipe.writer_active = false;
            pipe.writer_owner.store(nullptr, std::memory_order_release);   // write grant released
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window closes
#endif
        }

        collect_admissions(pipe, granted);

        if (pipe.queue_head == nullptr && pipe.active_readers == 0 && !pipe.writer_active)
            pipe.idle.notify_all();
    }
    fire_granted(granted);
}

// Enter `l` into its pipe: admit immediately when the queue is empty and the mode rules
// allow (the turn fires after unlock), else queue FIFO. The entry holds one ref on the
// owner from here until its turn fires (`fire_task_turn` adopts it) -- the push-UAF
// bracket: a fire-and-forget caller may drop its handle before the turn.
void pipe_enter_link(Pipe_link& l, Task_ptr* record)
{
    Pipe& pipe = *l.pipe;
    intrusive_inc(l.owner);   // the entry's ref
    bool now = false;
    {
        std::scoped_lock lock(pipe.mutex);
        if (record != nullptr)
            *record = Task_ptr(l.owner);   // enqueue-and-record, atomic under the mutex
        l.owner->pipes_entered = static_cast<std::uint8_t>(l.index + 1);   // advance retires [0, entered)
        if (pipe.queue_head == nullptr && admissible(pipe, l.mode))
        {
            admit_locked(pipe, &l);
            now = true;
        }
        else
        {
            queue_push(pipe, &l);
        }
    }
    if (now)
        fire_task_turn(l);
}

} // namespace

void pipe_enter_first(Task_control_block* blk, Task_ptr* record)
{
    pipe_enter_link(blk->pipe_links[0], record);
}

void advance_pipe_links(Task_control_block* blk)
{
    for (std::uint8_t i = 0; i < blk->pipes_entered; ++i)
        release_and_redispatch(*blk->pipe_links[i].pipe, blk->pipe_links[i].mode);
}

void pipe_links_on_complete(Task_control_block* blk)
{
    advance_pipe_links(blk);
}

bool pipe_try_inline(Scheduler&, Pipe& pipe, Access mode, const Task_ptr& block)
{
    {
        std::scoped_lock lock(pipe.mutex);
        if (pipe.queue_head != nullptr || !admissible(pipe, mode))
            return false;   // queued work ahead (FIFO) or mode-blocked -- defer to the queue
        admit_locked(pipe, &block->pipe_links[0]);
        block->pipes_entered = 1;   // settle's advance releases this admission
    }

    // Admitted: run the body inline on THIS thread (it installs its own access scope). The
    // caller blocks for its duration; the settle's `advance_pipe_links` releases the pipe.
    block->execute(block, /*gen*/ 0);
    return true;
}

bool pipe_acquire(Scheduler&, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired,
                  Task_control_block* owner)
{
    std::scoped_lock lock(pipe.mutex);
    // Admit at the front only if nothing is queued (FIFO) and the mode rule holds.
    if (pipe.queue_head == nullptr && admissible(pipe, mode))
    {
        if (mode == Access::read_only)
        {
            ++pipe.active_readers;   // acquired now; hold as a concurrent reader
        }
        else
        {
            pipe.writer_active = true;   // acquired now; hold as an exclusive writer
            pipe.writer_owner.store(owner, std::memory_order_release);
#if TS_SAFETY_CHECKS
            pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);   // write window opens
#endif
        }
        return true;
    }
    // Deferred: queue behind the pending work; admitted (FIFO) when it drains, firing the
    // callback. No admission pass here -- the blocking condition still holds.
    auto* node = new Hold_node();
    node->link.pipe = &pipe;
    node->link.mode = mode;
    node->on_acquired = std::move(on_acquired);
    node->grant_owner = owner;
    queue_push(pipe, &node->link);
    return false;
}

void pipe_release(Scheduler&, Pipe& pipe, Access mode)
{
    release_and_redispatch(pipe, mode);
}

#if TS_SAFETY_CHECKS
// ===== waits-for cycle detector (docs/coroutine-first.md §2) ================================
//
// One process-wide registry of {held pipe -> awaited pipe} edges, recorded by the coroutine
// awaiters at a genuine suspension on a pipe and cleared at resume. A cycle among the edges
// is the suspended-ABBA deadlock; it is checked on every insert under the registry mutex,
// so whichever awaiter inserts the closing edge sees the rest and faults. Cold path only
// (a deferred acquire / a suspending await), so a mutex + vector + DFS is plenty.
namespace
{

struct Waits_edge
{
    const Pipe* held;
    const Pipe* awaited;
    const void* ticket;                  // the recording awaiter (identity for clear)
    const Task_control_block* waiter;    // the suspending task, for the diagnostic
};

std::mutex waits_mutex;
std::vector<Waits_edge> waits_edges;

// Recover the owning `Pipe` from an `Access_context` entry's epoch source (the entry
// stores `&pipe->write_epoch`). Diagnostic-only pointer arithmetic, confined to this TU
// where the layout is known; `offsetof` on `Pipe` is conditionally-supported (non-
// standard-layout) and accepted by every toolchain we build on.
const Pipe* pipe_from_epoch(const std::atomic<std::uint64_t>* epoch) noexcept
{
    return reinterpret_cast<const Pipe*>(
        reinterpret_cast<const char*>(epoch) - offsetof(Pipe, write_epoch));
}

const char* pipe_name(const Pipe* pipe) noexcept
{
    return pipe->debug_name != nullptr ? pipe->debug_name : "<unnamed>";
}

// Does a chain of edges lead from `from` back to `target`? (`held == from` edges step to
// their awaited pipe.) Depth-bounded for safety; the registry is tiny (live suspensions).
bool waits_reaches(const Pipe* from, const Pipe* target, int depth) noexcept
{
    if (from == target)
        return true;
    if (depth > 64)
        return false;
    for (const Waits_edge& e : waits_edges)
    {
        if (e.held == from && waits_reaches(e.awaited, target, depth + 1))
            return true;
    }
    return false;
}

} // namespace

bool waits_for_record(const Access_context* held, const void* ticket, const Task_control_block* waiter,
                      Pipe* const* awaited, int count)
{
    if (held == nullptr || count == 0)
        return false;
    std::scoped_lock lock(waits_mutex);
    bool recorded = false;
    held->for_each_epoch([&](const std::atomic<std::uint64_t>* epoch)
    {
        const Pipe* held_pipe = pipe_from_epoch(epoch);
        for (int i = 0; i < count; ++i)
        {
            const Pipe* awaited_pipe = awaited[i];
            if (awaited_pipe == nullptr)
                continue;
            if (waits_reaches(awaited_pipe, held_pipe, 0))
            {
                // The closing edge. Find the counterpart edge out of the awaited pipe to
                // name the other task; for a self-cycle (awaiting an object this task
                // holds) there is none.
                const Waits_edge* other = nullptr;
                for (const Waits_edge& e : waits_edges)
                {
                    if (e.held == awaited_pipe)
                    {
                        other = &e;
                        break;
                    }
                }
                char message[512];
                if (awaited_pipe == held_pipe)
                {
                    std::snprintf(message, sizeof message,
                        "waits-for cycle: task (block %p) holding '%s' awaits the same object -- the "
                        "access queues behind the very grant the awaiter holds and the frame never "
                        "resumes; access it under the held grant instead (reentrancy covers the "
                        "writer-owner case)",
                        static_cast<const void*>(waiter), pipe_name(held_pipe));
                }
                else
                {
                    std::snprintf(message, sizeof message,
                        "waits-for cycle: task (block %p) holding '%s' awaits '%s', while task "
                        "(block %p) holding '%s' awaits '%s' -- a suspended ABBA deadlock (no thread "
                        "parks; the frames simply never resume). Prefer the access hierarchy: declare "
                        "the object on the node, read a Versioned snapshot, or stage via Deferred "
                        "(docs/coroutine-first.md section 2)",
                        static_cast<const void*>(waiter), pipe_name(held_pipe), pipe_name(awaited_pipe),
                        static_cast<const void*>(other != nullptr ? other->waiter : nullptr),
                        pipe_name(awaited_pipe),
                        other != nullptr ? pipe_name(other->awaited) : "?");
                }
                ts::fatal(message);
            }
            waits_edges.push_back({ held_pipe, awaited_pipe, ticket, waiter });
            recorded = true;
        }
    });
    return recorded;
}

void waits_for_clear(const void* ticket) noexcept
{
    std::scoped_lock lock(waits_mutex);
    std::erase_if(waits_edges, [&](const Waits_edge& e) { return e.ticket == ticket; });
}

// The `sync_wait` diagnostic (declared in task.h, defined here for the `Pipe` layout):
// the caller established that the wait is about to park inside a task. Sharp message
// when the wait target is a pipe task queued on a pipe the current scope's grant covers
// -- that shape deadlocks (the entry sits behind the very grant the waiter holds). The
// links carry the pipe identities, so multi-object jobs get the sharp match too.
void blocking_sync_diagnose(const Task_control_block* blk) noexcept
{
    if (current_access != nullptr)
    {
        for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
        {
            const Pipe_link& l = blk->pipe_links[i];
            if (l.pipe != nullptr && current_access->holds_epoch(&l.pipe->write_epoch))
            {
                ts::fatal("sync() inside a task on an access to an object this task already "
                    "holds -- this deadlocks; co_await it, or commit()/access under the held "
                    "grant");
            }
        }
    }
    ts::fatal("blocking sync() inside a task -- parks a worker and risks pool-exhaustion "
        "deadlock; co_await the task instead (or ts::nested / parallel_for for fork-join)");
}
#endif

} // namespace detail
} // namespace ts

