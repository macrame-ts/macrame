#include "ts/guarded.h"
#include "ts/detail/suspension_registry.h"

#include <atomic>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace ts
{

namespace
{
// The one process-wide scheduler, brought up explicitly by `create_scheduler`. A `unique_ptr`
// in a namespace static so program exit destroys it (joins its workers) even if the user
// never calls `destroy_scheduler` - the clean-shutdown safety net. `g_fast` is a lock-free
// read cache for the hot path (every `global_scheduler()` from an external thread); the mutex
// guards create/destroy. Create/destroy racing concurrent use is undefined (documented), but
// `g_fast` stays published until the workers are joined, so a live worker can always resolve it.
std::mutex g_sched_mutex;
std::unique_ptr<Scheduler> g_scheduler;
Scheduler_config g_config;
std::atomic<Scheduler*> g_fast{ nullptr };
}

Scheduler& global_scheduler()
{
    if (Scheduler* s = g_fast.load(std::memory_order_acquire))
        return *s;
    ts::fatal("no Scheduler is running - call ts::create_scheduler() at startup before any "
              "scheduled work (a scheduler is never brought up lazily)");
}

void create_scheduler(Scheduler_config config)
{
    std::lock_guard lock(g_sched_mutex);
    if (g_scheduler)
    {
        ts::fatal("a Scheduler is already running - exactly one exists per process; call "
                  "destroy_scheduler() before creating another");
    }
    g_config = config;
    g_scheduler = detail::make_scheduler(config);
    g_fast.store(g_scheduler.get(), std::memory_order_release);
}

void destroy_scheduler()
{
    std::lock_guard lock(g_sched_mutex);
    if (!g_scheduler)
        ts::fatal("destroy_scheduler(): no Scheduler is running");
    // Join first, publish "none running" second. `~Scheduler` joins the workers in its own
    // body, before any member is torn down, so the object stays fully valid for exactly as
    // long as a worker can still be executing. Clearing `g_fast` first opened a window where
    // a live worker resolved `global_scheduler()` and hit the no-scheduler fatal - a late
    // `parallel_for` helper draining its trace scopes on the way out is the path that caught it.
    g_scheduler.reset();   // dtor: quit + join workers, drain queued tasks
    g_fast.store(nullptr, std::memory_order_release);
}

bool scheduler_running() noexcept
{
    return g_fast.load(std::memory_order_acquire) != nullptr;
}

Scheduler_config current_scheduler_config()
{
    std::lock_guard lock(g_sched_mutex);
    if (!g_scheduler)
        ts::fatal("current_scheduler_config(): no Scheduler is running");
    return g_config;
}

Scheduler_scope::Scheduler_scope(Scheduler_config config)
{
    if (scheduler_running())
    {
        prev_ = current_scheduler_config();   // reconfigure: restore this on exit
        destroy_scheduler();
    }
    create_scheduler(config);
}

Scheduler_scope::~Scheduler_scope()
{
    destroy_scheduler();
    if (prev_)
        create_scheduler(*prev_);
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
// adopted from the queue). No heap closure - the block is the payload - so a bare task
// dispatch allocates nothing beyond its own block (killing the per-dispatch `submit_closure`
// alloc that hit every queued task).
static void run_block_dispatch(void* data)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});   // adopt the queued ref
    if (block->execute)
        block->execute(block);              // claims internally
    else if (block->claim())                // bodyless: claim guards against machinery bugs
        block->complete();
}   // `block` decrements here -> releases the ref the queue held

// Trampoline for a borrowed block dispatch: the block travels as the entry's `data_` as a
// raw pointer with no ref transferred (the queue borrows it). The block's owner - a graph
// `Run_state`, which holds the node block for the whole run - provably outlives the
// dispatch, so the queue need not keep it alive. Saves the dispatch-hop inc/dec that
// `run_block_dispatch` pays. `Adopt_ref` wraps the pointer without an inc; `release()`
// detaches without the matching dec (this frame never owned a ref). The `Current_task`
// swap inside the body still takes its own (necessary) ref, balanced by its own lifetime.
static void run_borrowed_dispatch(void* data)
{
    auto* blk = static_cast<Task_control_block*>(data);
    Task_ptr borrowed(blk, Adopt_ref{});   // wrap without inc - we own no ref
    if (blk->execute)
        blk->execute(borrowed);            // claims internally
    else if (blk->claim())                 // bodyless: claim guards against machinery bugs
        blk->complete();
    borrowed.release();                    // detach without dec
}

// A borrowed block whose prerequisites are all met: schedule it on `scheduler`, carrying a
// raw pointer (no refcount). Only for `flags.borrowed` blocks (owner outlives the dispatch).
void submit_borrowed_on(Scheduler& scheduler, Task_control_block* blk)
{
    scheduler.submit(&run_borrowed_dispatch, blk, blk->flags.priority);
}

void submit_borrowed(Task_control_block* blk)
{
    submit_borrowed_on(global_scheduler(), blk);
}

// A block whose prerequisites are all met: schedule it to run on `scheduler` (its body,
// or, if bodyless, just complete). The explicit-scheduler form; `submit_ready` resolves
// the global and delegates here.
void submit_ready_on(Scheduler& scheduler, Task_ptr block)
{
    Priority priority = block->flags.priority;
    // Hand the block's ref to the queue (release, no dec); the trampoline adopts it back.
    scheduler.submit(&run_block_dispatch, block.release(), priority);
}

// A block whose prerequisites are all met: schedule it to run (its body, or, if
// bodyless, just complete). Bridges task.h's lock-counter to the scheduler.
void submit_ready(Task_ptr block)
{
    submit_ready_on(global_scheduler(), std::move(block));
}

// ===== the evolved mutex pipe (docs/pipe-rebase.md §0.2) ====================================
//
// One mutex per pipe guards the admission state and the intrusive queue of the tasks' own
// embedded `Pipe_link`s. An admitted entry's turn fires `release()` on its owner - the pipe
// is a prerequisite source for the block machinery (the `pipe_count` trigger in task.h), not
// a dispatcher - and a multi-object task's turn also enters its next link (the sequential
// canonical cascade). Turn-firing happens outside the mutex: `release()` reaching zero
// dispatches (a scheduler submit, possibly a wake syscall; worker-less mode executes at
// submit), so admission collects the granted entries under the lock and fires after unlock.
// The task settles -> `advance_pipe_links` releases each entered pipe, admitting successors.

namespace
{

// A held grant's queue entry (`pipe_acquire`, the coroutine guards): a link with a null
// `owner` (the base's initializer - the queue's hold-vs-task discriminator) plus the grant
// callback. Derives from `Pipe_link` so recovery from a queued link is a standard derived
// cast (the same contract as the block wrappers' `Block_backed`). The one allocating pipe
// path; freed when its admission fires the callback. An immediate (uncontended) acquire
// allocates nothing.
struct Hold_node : Pipe_link
{
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
        : static_cast<Hold_node*>(l)->grant_owner;   // a null-`owner` link is a `Hold_node`
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
// pipe-turn prerequisite. Everything needed is read into locals first - once `release`
// drops the owner's last lock the task can run, settle, and be destroyed (links included)
// on another thread, and this frame's entry ref is the only thing pinning it until then.
void fire_task_turn(Pipe_link& l)
{
    Task_control_block* owner = l.owner;
    std::uint8_t index = l.index;
    // A caller-owned block's entry carries no ref (see `pipe_enter_link`), so the wrapper is
    // defused below instead of dropping one. The flag is read into a local FIRST: `release`
    // can dispatch, run, settle and hand the block back to a destructing owner before it
    // returns, so nothing on this frame may touch the block afterwards.
    const bool caller_owned = owner->flags.caller_owned;
    Task_ptr keep(owner, Adopt_ref{});   // adopt the entry's ref (taken at enter)
    if (index + 1 < owner->pipe_count)
        pipe_enter_link(owner->pipe_links[index + 1], nullptr);
    Task_control_block::release(keep);
    if (caller_owned)
        keep.release();   // detach without a dec - no entry ref existed
}   // `keep` drops the entry ref; the owner lives on via the queue/handle refs downstream

// Fire one admission pass's granted entries, in admission order. Runs without the mutex.
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
            // closure - the holder resumes on a worker), then retire the node.
            auto* node = static_cast<Hold_node*>(l);
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

        // Drain notify under the mutex - the standard-blessed condition_variable teardown
        // pattern ([thread.condition.condvar]: only the notify need happen-before the
        // destruction). A `wait_until_idle` waiter cannot return from `idle.wait` until it
        // re-acquires `mutex`, which the scope's unlock below hands off; by then
        // `notify_all` has already returned, so the destroying waiter never races the
        // notify. UE's `FPipe` needed a refcounted, keep-alive drain event
        // (`EmptyEventRef` + a local copy taken before the last decrement - their
        // `// use-after-free territory!`) precisely because its `FEventCount::Notify` is
        // lock-free/unlocked; notify-under-lock makes us structurally immune to that race.
        if (pipe.queue_head == nullptr && pipe.active_readers == 0 && !pipe.writer_active)
            pipe.idle.notify_all();
    }
    // Post-unlock, and deliberately pipe-free: `fire_granted` walks the granted links (each
    // pinned by its owner's entry ref) and never binds `Pipe&`, so a waiter that the notify
    // above released may destroy the pipe concurrently with no lifetime question at all.
    // (A notify also implies nothing was granted - any admission sets
    // `writer_active`/`active_readers` - so the two cases do not even overlap.)
    fire_granted(granted);
}

// Enter `l` into its pipe: admit immediately when the queue is empty and the mode rules
// allow (the turn fires after unlock), else queue FIFO. The entry holds one ref on the
// owner from here until its turn fires (`fire_task_turn` adopts it) - the push-UAF
// bracket: a fire-and-forget caller may drop its handle before the turn.
void pipe_enter_link(Pipe_link& l, Task_ptr* record)
{
    Pipe& pipe = *l.pipe;
    // The entry's ref - the push-UAF bracket for detached owners. A caller-owned block
    // (`Access_op`) takes none: its storage is pinned by the op, whose destructor waits out
    // an unsettled access, and a ref here would be dropped by machinery that can outlive
    // the op's own resume-and-destroy (see `Flags::caller_owned`).
    if (!l.owner->flags.caller_owned)
        intrusive_inc(l.owner);
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

namespace
{
// The probe's brief critical section: every one of the block's bound pipes locked at once,
// released in reverse on scope exit. The links are bound in canonical (ascending pipe-address)
// order, so this acquires in that order - which is what keeps two probes over overlapping
// object sets from deadlocking each other, and nothing else in the library ever holds two pipe
// mutexes at the same time. `std::scoped_lock` is not usable here: the set of pipes a fire must
// actually take is a runtime count (lending removes the ones the caller already holds).
class Locked_pipes
{
public:
    Locked_pipes(const Pipe_link* links, std::uint8_t count)
        : links_(links)
        , count_(count)
    {
        for (std::uint8_t i = 0; i < count_; ++i)
            links_[i].pipe->mutex.lock();
    }

    ~Locked_pipes()
    {
        for (std::uint8_t i = count_; i > 0; --i)
            links_[i - 1].pipe->mutex.unlock();
    }

    Locked_pipes(const Locked_pipes&) = delete;
    Locked_pipes& operator=(const Locked_pipes&) = delete;

private:
    const Pipe_link* links_;
    std::uint8_t count_;
};

} // namespace

bool pipe_try_inline(const Task_ptr& block)
{
    const std::uint8_t count = block->pipe_count;
    {
        // All or nothing: nothing is admitted unless every pipe passes, so a failed probe is
        // invisible to every other participant (docs/multi-access-op-design.md §4.4). No user
        // code runs under the mutexes - the critical section is N checks and N admits.
        Locked_pipes locked(block->pipe_links, count);
        for (std::uint8_t i = 0; i < count; ++i)
        {
            const Pipe_link& l = block->pipe_links[i];
            if (l.pipe->queue_head != nullptr || !admissible(*l.pipe, l.mode))
                return false;   // queued work ahead (FIFO) or mode-blocked - defer to the queue
        }
        for (std::uint8_t i = 0; i < count; ++i)
            admit_locked(*block->pipe_links[i].pipe, &block->pipe_links[i]);
        block->pipes_entered = count;   // settle's advance releases these admissions
    }

    // Admitted: run the body inline on this thread (it installs its own access scope). The
    // caller blocks for its duration; the settle's `advance_pipe_links` releases the pipes.
    block->execute(block);
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
    // callback. No admission pass here - the blocking condition still holds.
    auto* node = new Hold_node();
    node->pipe = &pipe;
    node->mode = mode;
    node->on_acquired = std::move(on_acquired);
    node->grant_owner = owner;
    queue_push(pipe, node);
    return false;
}

void pipe_release(Scheduler&, Pipe& pipe, Access mode)
{
    release_and_redispatch(pipe, mode);
}

#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT) || TS_RULE_ON(TS_RULE_IN_TASK_SYNC) || TS_RULE_ON(TS_RULE_ACCESS_RANK)
namespace
{
// Display identity of a pipe's object, for a diagnostic. `buf` must outlive the use (each
// caller keeps one buffer per name it prints in a single message).
const char* pipe_name(const Pipe* pipe, char* buf, std::size_t size) noexcept
{
    return pipe != nullptr ? named_display(pipe->debug_name, buf, size, "<unnamed object>")
                           : "<none>";
}
}
#endif

#if TS_RULE_ON(TS_RULE_ACCESS_RANK)
// `Rule::access_rank` (TODO 6.14): a dynamic await must strictly climb the declared rank
// order. Reported here so the message can name the object.
[[noreturn]] void report_rank_violation(const Pipe* awaited, unsigned held_max,
                                        bool held_unranked) noexcept
{
    char object[96];
    char waiter[96];
    char message[768];
    const unsigned target = pipe_rank(*awaited);
    if (held_unranked)
    {
        std::snprintf(message, sizeof message,
            "access rank: task '%s' awaits '%s' while holding a grant on an object with NO declared "
            "ts::Rank. An unranked object cannot be climbed away from safely, so it forbids dynamic "
            "awaits while held - that is the strict default. Give both objects a ts::Rank (the "
            "awaited one strictly higher), or restructure: declare the object on the node, read a "
            "Versioned snapshot, or stage via Deferred. Per-scope escape: "
            "ts::Relaxed_scope{ts::Rule::access_rank}",
            task_name(Current_task::get(), waiter, sizeof waiter),
            pipe_name(awaited, object, sizeof object));
    }
    else
    {
        std::snprintf(message, sizeof message,
            "access rank: task '%s' awaits '%s' (rank %u) while holding a grant of rank %u - a "
            "dynamic await must strictly CLIMB the declared order, or a wait cycle becomes "
            "representable. Raise the awaited object's ts::Rank above %u, or restructure: declare "
            "the object on the node, read a Versioned snapshot, or stage via Deferred. Per-scope "
            "escape: ts::Relaxed_scope{ts::Rule::access_rank}",
            task_name(Current_task::get(), waiter, sizeof waiter),
            pipe_name(awaited, object, sizeof object), target, held_max, held_max);
    }
    ts::fatal(message);
}
#endif

#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
// ===== circular-wait detector (docs/coroutine-first.md §2) ================================
//
// One process-wide registry of {held pipe -> awaited pipe} edges, recorded by the coroutine
// awaiters at a genuine suspension on a pipe and cleared at resume. A cycle among the edges
// is the suspended-ABBA deadlock; it is checked on every insert under the registry mutex,
// so whichever awaiter inserts the closing edge sees the rest and faults. Cold path only
// (a deferred acquire / a suspending await), so a mutex + vector + DFS is plenty.
namespace
{

struct Wait_edge
{
    const Pipe* held;
    const Pipe* awaited;
    const void* ticket;                  // the recording awaiter (identity for clear)
    const Task_control_block* waiter;    // the suspending task, for the diagnostic
};

std::mutex wait_mutex;
std::vector<Wait_edge> wait_edges;

// Recover the owning `Pipe` from an `Access_context` entry's epoch source (the entry
// stores `&pipe->write_epoch`). Diagnostic-only pointer arithmetic, confined to this TU
// where the layout is known; `offsetof` on `Pipe` is conditionally-supported (non-
// standard-layout) and accepted by every toolchain we build on.
const Pipe* pipe_from_epoch(const std::atomic<std::uint64_t>* epoch) noexcept
{
    return reinterpret_cast<const Pipe*>(
        reinterpret_cast<const char*>(epoch) - offsetof(Pipe, write_epoch));
}


// Does a chain of edges lead from `from` back to `target`? (`held == from` edges step to
// their awaited pipe.) Depth-bounded for safety; the registry is tiny (live suspensions).
bool wait_reaches(const Pipe* from, const Pipe* target, int depth) noexcept
{
    if (from == target)
        return true;
    if (depth > 64)
        return false;
    for (const Wait_edge& e : wait_edges)
    {
        if (e.held == from && wait_reaches(e.awaited, target, depth + 1))
            return true;
    }
    return false;
}

} // namespace

bool circular_wait_record(const Access_context* held, const void* ticket, const Task_control_block* waiter,
                      Pipe* const* awaited, int count)
{
    if (held == nullptr || count == 0)
        return false;
    std::scoped_lock lock(wait_mutex);
    bool recorded = false;
    held->for_each_epoch([&](const std::atomic<std::uint64_t>* epoch)
    {
        const Pipe* held_pipe = pipe_from_epoch(epoch);
        for (int i = 0; i < count; ++i)
        {
            const Pipe* awaited_pipe = awaited[i];
            if (awaited_pipe == nullptr)
                continue;
            if (wait_reaches(awaited_pipe, held_pipe, 0))
            {
                // The closing edge. Find the counterpart edge out of the awaited pipe to
                // name the other task; for a self-cycle (awaiting an object this task
                // holds) there is none.
                const Wait_edge* other = nullptr;
                for (const Wait_edge& e : wait_edges)
                {
                    if (e.held == awaited_pipe)
                    {
                        other = &e;
                        break;
                    }
                }
                // One buffer per name printed in the message: `named_display` may return a
                // pointer into the buffer it was handed.
                char held_buf[96], awaited_buf[96], other_awaited_buf[96];
                char waiter_buf[96], other_waiter_buf[96];
                char message[768];
                if (awaited_pipe == held_pipe)
                {
                    std::snprintf(message, sizeof message,
                        "circular wait: task '%s' holding '%s' awaits the same object - the "
                        "access queues behind the very grant the awaiter holds and the frame never "
                        "resumes; access it under the held grant instead (reentrancy covers the "
                        "writer-owner case)",
                        task_name(waiter, waiter_buf, sizeof waiter_buf),
                        pipe_name(held_pipe, held_buf, sizeof held_buf));
                }
                else
                {
                    std::snprintf(message, sizeof message,
                        "circular wait: task '%s' holding '%s' awaits '%s', while task "
                        "'%s' holding '%s' awaits '%s' - a suspended ABBA deadlock (no thread "
                        "parks; the frames simply never resume). Prefer the access hierarchy: declare "
                        "the object on the node, read a Versioned snapshot, or stage via Deferred "
                        "(docs/coroutine-first.md section 2)",
                        task_name(waiter, waiter_buf, sizeof waiter_buf),
                        pipe_name(held_pipe, held_buf, sizeof held_buf),
                        pipe_name(awaited_pipe, awaited_buf, sizeof awaited_buf),
                        task_name(other != nullptr ? other->waiter : nullptr,
                                  other_waiter_buf, sizeof other_waiter_buf),
                        pipe_name(awaited_pipe, awaited_buf, sizeof awaited_buf),
                        pipe_name(other != nullptr ? other->awaited : nullptr,
                                  other_awaited_buf, sizeof other_awaited_buf));
                }
                ts::fatal(message);
            }
            wait_edges.push_back({ held_pipe, awaited_pipe, ticket, waiter });
            recorded = true;
        }
    });
    return recorded;
}

void circular_wait_clear(const void* ticket) noexcept
{
    std::scoped_lock lock(wait_mutex);
    std::erase_if(wait_edges, [&](const Wait_edge& e) { return e.ticket == ticket; });
}
#endif   // TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)

#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
// The scheduler-side half of the deadlock net (declared in task.h). Defined here rather
// than in scheduler.cpp because this is where the global scheduler holder lives, and the
// null check is load-bearing: a blue thread may wait on a hand-triggered `Signal` before
// any scheduler exists, and asking `global_scheduler()` would create a worker pool as a
// side effect of a safety check.
bool scheduler_quiescent() noexcept
{
    const Scheduler* scheduler = g_fast.load(std::memory_order_acquire);
    return scheduler != nullptr && scheduler->quiescent();
}

// The net fired: the scheduler has been continuously idle with empty queues, nothing is
// registered as completable from off-pool, and this waiter is still blocked. Names its own
// escape, because the failure mode of the counter is a forgotten registration, which
// presents as exactly this message on a correct program.
// Append to a bounded report buffer, tracking the write position. Truncation is fine: this
// is a dying process's last words, not a protocol.
void report_append(char* buffer, std::size_t size, std::size_t& used, const char* format, ...) noexcept
{
    if (used + 1 >= size)
        return;
    va_list args;
    va_start(args, format);
    int written = std::vsnprintf(buffer + used, size - used, format, args);
    va_end(args);
    if (written > 0)
        used = used + static_cast<std::size_t>(written) < size ? used + static_cast<std::size_t>(written) : size - 1;
}

// tier 2 - free wherever the circular-wait registry exists. Its live entries are the tasks
// suspended while holding a grant, in a structure already maintained for the cycle check,
// read at a moment when we are already dying. (This is a post-mortem of edges an independent
// mechanism has already concluded are wedged, not a prediction from them - the distinction
// that separates it from the learned-order detectors that failed to land in Linux.)
void report_held_grant_suspensions([[maybe_unused]] char* buffer, [[maybe_unused]] std::size_t size,
                                   [[maybe_unused]] std::size_t& used) noexcept
{
#if TS_RULE_ON(TS_RULE_CIRCULAR_WAIT)
    std::scoped_lock lock(wait_mutex);
    if (wait_edges.empty())
    {
        report_append(buffer, size, used, "\n  (no task is suspended while holding a grant)");
        return;
    }
    report_append(buffer, size, used, "\n  suspended while holding a grant:");
    for (const Wait_edge& edge : wait_edges)
    {
        char who[96], held[96], awaited[96];
        report_append(buffer, size, used, "\n    task '%s' holds '%s', awaits '%s'",
            task_name(edge.waiter, who, sizeof who),
            pipe_name(edge.held, held, sizeof held),
            pipe_name(edge.awaited, awaited, sizeof awaited));
    }
#endif
}

// tier 3 - the full picture, when the registry is compiled in: every live suspension,
// including the ones holding nothing (a plain task await), which tier 2 structurally cannot
// see. When it is not compiled in, this is where the user learns how to get it.
void report_all_suspensions(char* buffer, std::size_t size, std::size_t& used) noexcept
{
#if TS_SUSPENSION_REGISTRY
    int total = 0;
    for (Suspension_shard& shard : suspension_shards)
    {
        std::scoped_lock lock(shard.mutex);
        for (const Suspension_record* record = shard.head; record != nullptr; record = record->next)
        {
            ++total;
            char who[96], awaited[96];
            const char* what = record->awaited_pipe != nullptr
                ? pipe_name(record->awaited_pipe, awaited, sizeof awaited)
                : named_display(record->awaited_task, awaited, sizeof awaited, "<unnamed task>");
            report_append(buffer, size, used, "\n    task '%s' awaits '%s'",
                named_display(record->task, who, sizeof who, "<unnamed task>"), what);
            for (int i = 0; i < record->held_count; ++i)
            {
                char held[96];
                report_append(buffer, size, used, "%s'%s'", i == 0 ? ", holding " : ", ",
                    pipe_name(pipe_from_epoch(record->held[i]), held, sizeof held));
            }
        }
    }
    if (total == 0)
        report_append(buffer, size, used, "\n  (no task is suspended at all - the wait is on work that never started)");
    else
        report_append(buffer, size, used, "\n  (%d suspended task(s) listed above)", total);
#else
    report_append(buffer, size, used,
        "\n  the suspension registry is compiled out, so tasks suspended without holding a "
        "grant are invisible here. Rebuild with -DTS_SUSPENSION_REGISTRY=1 and reproduce for "
        "the full list of suspended tasks, what each holds and what each awaits");
#endif
}

[[noreturn]] void report_deadlock(const Task_control_block* waited_on) noexcept
{
    char target[96];
    char message[4096];
    std::size_t used = 0;
    report_append(message, sizeof message, used,
        "deadlock: waiting on task '%s', but every worker has been idle with empty queues for "
        "%lld ms and nothing is registered as completable from outside the pool - no thread and "
        "no queue can ever settle it. If this wait IS completed by a non-worker thread (I/O, a "
        "GPU fence, a Signal triggered off-pool, a frame gate), hold a ts::External_wait for its "
        "duration - a missing registration reports a correct program as deadlocked. "
        "ts::set_deadlock_net_window(0ms) disables the net for this process; TS_ENABLED_RULES "
        "drops it from the build.",
        task_name(waited_on, target, sizeof target),
        deadlock_net_window_ms.load(std::memory_order_relaxed));
    report_held_grant_suspensions(message, sizeof message, used);
    report_all_suspensions(message, sizeof message, used);
    ts::fatal(message);
}
#endif


#if TS_RULE_ON(TS_RULE_IN_TASK_SYNC)
// The `sync_wait` diagnostic (declared in task.h, defined here for the `Pipe` layout): a
// `sync()`/`take()` was issued from inside a task. Sharp message when the target is a pipe
// task on a pipe the current scope's grant covers - that shape deadlocks outright (the
// entry sits behind the very grant the waiter holds). The links carry the pipe identities,
// so multi-object jobs get the sharp match too.
//
// The target may already be settled: the rule is about the call, not the incident (TODO
// 6.10), so the message names the escapes rather than describing a park that may not
// happen this run.
[[noreturn]] void blocking_sync_diagnose(const Task_control_block* blk) noexcept
{
    char waiter[96];
#if TS_SAFETY_CHECKS
    if (const Access_context* cur = access_load())
    {
        for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
        {
            const Pipe_link& l = blk->pipe_links[i];
            if (l.pipe != nullptr && cur->holds_epoch(&l.pipe->write_epoch))
            {
                char message[512];
                char object[96];
                std::snprintf(message, sizeof message,
                    "sync()/take() inside task '%s' on an access to '%s', which this task already "
                    "holds - the access queues behind the waiter's own grant, so this deadlocks. "
                    "co_await it, or commit()/access under the held grant",
                    task_name(Current_task::get(), waiter, sizeof waiter),
                    pipe_name(l.pipe, object, sizeof object));
                ts::fatal(message);
            }
        }
    }
#endif
    char message[512];
    std::snprintf(message, sizeof message,
        "sync()/take() inside task '%s' - a blocking wait inside a task occupies a worker and "
        "risks pool-exhaustion deadlock, whether or not the target happens to be settled right "
        "now. Use co_await, or try_take() for the non-blocking read; if the wait is genuinely "
        "bounded by something we cannot see, declare it with "
        "ts::Relaxed_scope{ts::Rule::in_task_sync}",
        task_name(Current_task::get(), waiter, sizeof waiter));
    ts::fatal(message);
}
#endif

// The body-boundary diagnostic: a user body let an exception escape. Declared in task_block.h,
// which explains why it is defined out of line here rather than inline there.
[[noreturn]] void escaped_exception_diagnose(const char* what) noexcept
{
    char who[96];
    char message[512];
    std::snprintf(message, sizeof message,
        "task '%s' let an exception escape its body%s%s - macrame does not carry an exception "
        "across a task boundary: unwinding runs through library frames that hold grants, lock "
        "counts and refcounts, and that may be compiled with no exception support at all. "
        "Handle it inside the body",
        task_name(Current_task::get(), who, sizeof who),
        what != nullptr ? ": " : "",
        what != nullptr ? what : "");
    ts::fatal(message);
}

} // namespace detail
} // namespace ts

