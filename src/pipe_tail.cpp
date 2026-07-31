// Design-B tail-chain pipe (docs/pipe-rebase.md §4-5). Selected by TS_PIPE_TAIL=1; the
// current mutex-guarded deque pipe lives in guarded.cpp under `#if !TS_PIPE_TAIL`.
//
// Writers chain FPipe-style on an atomic tagged tail. A reader is admitted the same way, but
// the group HEAD (its predecessor is a writer or idle, §5.2B.1) runs a forward WALK when it
// starts: it collects the contiguous run of following readers, dispatches them to run
// concurrently, and points them all at a terminator (the next writer, or a freshly installed
// barrier) that each reader decrements on completion -- the next writer runs only once the
// whole group has drained. A LONE reader (the walk finds no follower) installs no barrier and
// advances serially, which is the R5 elision.

#include "ts/guarded.h"
#include "ts/scheduler.h"

#if TS_PIPE_TAIL

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts
{
namespace detail
{
namespace
{

constexpr std::uintptr_t reader_bit = 1;

std::uintptr_t pack(Task_control_block* block, bool reader)
{
    return reinterpret_cast<std::uintptr_t>(block) | (reader ? reader_bit : 0);
}
Task_control_block* tail_block(std::uintptr_t word)
{
    return reinterpret_cast<Task_control_block*>(word & ~reader_bit);
}

Pipe& pipe_of(Task_control_block* block)
{
    return *reinterpret_cast<Pipe*>(
        static_cast<std::uintptr_t>(block->dispatch_arg.load(std::memory_order_acquire)));
}

bool is_reader_task(Task_control_block* b)   // an async reader eligible to join a group
{
    return b->flags.pipe_reader && !b->flags.pipe_reservation;
}

void run_pipe_block(void* data);
void pipe_advance(Pipe& pipe, Task_control_block* self);

// Dispatch a ready pipe block: hand a fresh ref to the scheduler queue; `run_pipe_block`
// adopts it back.
void pipe_submit(Task_control_block* block)
{
    intrusive_inc(block);
    global_scheduler().submit(&run_pipe_block, block, static_cast<Priority>(block->flags.priority));
}

// A reader group's terminator reached zero (all its readers drained). A writer terminator
// runs its body (its own completion advances the pipe); a barrier terminator has no body, so
// it advances directly here and then settles (releasing its backward ref).
void dispatch_terminator(Pipe& pipe, Task_control_block* term)
{
    if (term->execute != nullptr)
    {
        pipe_submit(term);   // a writer -> run it
        return;
    }
    Task_ptr keep(term);     // barrier: hold across advance + settle (advance may drop the tail ref)
    pipe_advance(pipe, term);
    term->complete();        // settle: releases the backward prerequisite ref, retires the barrier
}

// Advance the pipe past `self` (completed or released). A GROUPED reader (its walk set a
// terminator) just decrements that terminator -- it never touches the tail or its own
// successor, the walk owns those. Everything else (a writer, a barrier, or a LONE reader)
// dispatches its single chain successor and clears the tail if still ours.
void pipe_advance(Pipe& pipe, Task_control_block* self)
{
    Task_control_block* term = self->pipe_terminator.load(std::memory_order_acquire);
    if (term != nullptr)   // grouped reader
    {
        pipe.task_count.fetch_sub(1, std::memory_order_release);
        if (term->num_locks.fetch_sub(1, std::memory_order_acq_rel) == 1)
            dispatch_terminator(pipe, term);
        return;
    }

    Task_control_block* succ = self->pipe_next.exchange(pipe_closed, std::memory_order_acq_rel);
    if (succ != nullptr && succ != pipe_closed)
        pipe_submit(succ);   // was linked and waiting -> run it now

    std::uintptr_t expected = pack(self, self->flags.pipe_reader);
    if (pipe.tail.compare_exchange_strong(expected, 0,
            std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        intrusive_dec(self);   // still the tail -> release its tail ref (idle now)
    }
    // else: a later pusher exchanged `self` out and adopted its tail ref as a prerequisite.

    pipe.task_count.fetch_sub(1, std::memory_order_release);   // drain (§7); last touch of the pipe
}

// The head reader's forward walk (§5.2B). Collects the contiguous run of async readers after
// `head`, sets up a terminator, points every group reader at it, and dispatches the members
// to run concurrently. Iterative -- pass 1 only reads pointers (no dispatch, no recursion),
// pass 2 dispatches through the scheduler (inline members ride the block's own trampoline).
void walk_group(Pipe& pipe, Task_control_block* head)
{
    std::vector<Task_control_block*> members;   // followers (head excluded; it runs its own body)
    Task_control_block* last = head;
    Task_control_block* terminator = nullptr;

    for (;;)
    {
        Task_control_block* nxt = last->pipe_next.load(std::memory_order_acquire);
        if (nxt != nullptr && nxt != pipe_closed && is_reader_task(nxt))
        {
            members.push_back(nxt);   // a follower reader -> a member
            last = nxt;
            continue;
        }
        if (nxt != nullptr && nxt != pipe_closed)
        {
            terminator = nxt;   // a writer (or a reservation) already chained -> the terminator
            break;
        }
        // nxt == null: chain end. A lone reader (no members) installs NO barrier and advances
        // serially -- the R5 elision. Otherwise install a barrier and try to close the end.
        if (members.empty())
            return;   // lone reader: leave `head` serial (pipe_terminator stays null)

        Task_ptr barrier = make_bare_block();       // ref 1 becomes the barrier's tail ref
        Task_control_block* g = barrier.get();
        g->flags.pipe_job = true;
        g->dispatch_arg.store(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)), std::memory_order_relaxed);
        Task_control_block* expected = nullptr;
        if (!last->pipe_next.compare_exchange_strong(expected, g,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            continue;   // a late reader/writer linked after `last` -> re-read and include it
        }
        // Barrier installed. Keep `last` alive as the barrier's backward link, and advance the
        // tail to the barrier so the barrier (not a group reader) manages tail clearing.
        barrier.release();                           // hand ref 1 to the tail
        pipe.task_count.fetch_add(1, std::memory_order_relaxed);
        g->prerequisites.push_back(Task_ptr(last, Adopt_ref{}));   // adopt last's tail ref
        std::uintptr_t last_word = pack(last, true);
        pipe.tail.compare_exchange_strong(last_word, pack(g, false),
                std::memory_order_acq_rel, std::memory_order_relaxed);
        // If that CAS failed a straggler moved the tail past `last`; it linked after `g`
        // (its push follows `last->pipe_next == g`), so the tail correctly names the straggler.
        terminator = g;
        break;
    }

    int n = 1 + static_cast<int>(members.size());              // head + members
    terminator->num_locks.store(n, std::memory_order_relaxed);   // reader gate

    // Pass 2: point head + members at the terminator, then dispatch the members. `head` is
    // already running (it runs its own body after this returns).
    head->pipe_terminator.store(terminator, std::memory_order_release);
    for (Task_control_block* m : members)
    {
        m->pipe_terminator.store(terminator, std::memory_order_release);
        pipe_submit(m);   // dispatch the member (concurrent with the head)
    }
}

// `on_complete` hook (fires at settle, after the body): advance the pipe.
void pipe_on_complete(Task_control_block* self)
{
    pipe_advance(pipe_of(self), self);
}

// Trampoline for an admitted pipe block. A reservation fires `on_acquired` and HOLDS. A
// reader HEAD runs the group walk first. Then the body runs; settle fires `pipe_on_complete`.
void run_pipe_block(void* data)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});

    if (block->flags.pipe_reservation)
    {
        Pipe& pipe = pipe_of(block.get());
        pipe.held.store(block.get(), std::memory_order_release);
        if (block->result_ptr)
            (*static_cast<std::move_only_function<void()>*>(block->result_ptr))();
        return;   // held; `pipe_release` advances + retires it
    }

    if (block->flags.pipe_reader && block->flags.pipe_head)
        walk_group(pipe_of(block.get()), block.get());

    block->execute(block, /*gen*/ 0);   // Executable::run -> body -> complete -> settle -> on_complete
}

// Shared push: exchange the tail, link after the predecessor (or run now if idle / the
// predecessor already completed). `raw` must ALREADY carry a dedicated tail ref. The
// exchanged-out predecessor's tail ref transfers into our `prerequisites`. Returns whether we
// were admitted synchronously (idle / prev done). Follows past a barrier the walk may have
// installed at the predecessor's slot.
bool pipe_push(Pipe& pipe, Task_control_block* raw, bool reader, bool dispatch_on_admit)
{
    pipe.task_count.fetch_add(1, std::memory_order_relaxed);

    std::uintptr_t prev_word = pipe.tail.exchange(pack(raw, reader), std::memory_order_acq_rel);
    if (prev_word == 0)
    {
        raw->flags.pipe_head = reader;   // idle -> a reader is its group's head
        if (dispatch_on_admit)
            pipe_submit(raw);
        return true;
    }

    Task_control_block* prev = tail_block(prev_word);
    bool prev_reader = (prev_word & reader_bit) != 0;
    raw->prerequisites.push_back(Task_ptr(prev, Adopt_ref{}));   // adopt prev's tail ref, set before linking

    // Link after `prev`, following its `pipe_next` chain past any barrier/straggler.
    Task_control_block* at = prev;
    for (;;)
    {
        Task_control_block* expected = nullptr;
        if (at->pipe_next.compare_exchange_strong(expected, raw,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // Linked: whoever completes `at` dispatches us. Head iff the immediate predecessor
            // (`prev`) is a writer; a follower of a reader is a member walked by the head.
            raw->flags.pipe_head = reader && !prev_reader;
            return false;
        }
        Task_control_block* nxt = at->pipe_next.load(std::memory_order_acquire);
        if (nxt == pipe_closed)
        {
            // `at` already completed -> run now; we head a new group.
            raw->flags.pipe_head = reader;
            if (dispatch_on_admit)
                pipe_submit(raw);
            return true;
        }
        at = nxt;   // a barrier / straggler occupies the slot -> follow to the real end
    }
}

} // namespace

void pipe_enqueue(Scheduler&, Pipe& pipe, Access mode, Task_ptr block, Priority priority)
{
    Task_control_block* raw = block.get();
    raw->flags.pipe_job = true;
    raw->flags.pipe_reader = (mode == Access::read_only);
    raw->flags.priority = priority;
    raw->on_complete = &pipe_on_complete;
    raw->dispatch_arg.store(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)), std::memory_order_relaxed);

    intrusive_inc(raw);   // dedicated tail ref (the caller's Task<R> handle keeps its own)
    pipe_push(pipe, raw, mode == Access::read_only, /*dispatch_on_admit*/ true);
}

bool pipe_acquire(Scheduler&, Pipe& pipe, Access mode, std::move_only_function<void()> on_acquired)
{
    Task_ptr node = make_bare_block();
    Task_control_block* raw = node.get();
    raw->flags.pipe_job = true;
    raw->flags.pipe_reader = (mode == Access::read_only);
    raw->flags.pipe_reservation = true;
    raw->dispatch_arg.store(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)), std::memory_order_relaxed);

    auto* cb = new std::move_only_function<void()>(std::move(on_acquired));
    raw->result_ptr = cb;

    node.release();   // `make_bare_block`'s ref becomes the node's dedicated tail ref
    bool admitted = pipe_push(pipe, raw, mode == Access::read_only, /*dispatch_on_admit*/ false);

    if (admitted)
    {
        pipe.held.store(raw, std::memory_order_release);
        delete cb;                 // synchronous acquire: on_acquired is not called
        raw->result_ptr = nullptr;
        return true;
    }
    return false;   // deferred: prev's advance dispatches `raw` -> run_pipe_block fires the callback
}

void pipe_release(Scheduler&, Pipe& pipe, Access)
{
    Task_control_block* held = pipe.held.exchange(nullptr, std::memory_order_acq_rel);
    if (held == nullptr)
        return;
    Task_ptr keep(held);   // hold alive across advance + settle
    if (held->result_ptr)
    {
        delete static_cast<std::move_only_function<void()>*>(held->result_ptr);
        held->result_ptr = nullptr;
    }
    pipe_advance(pipe, held);
    held->complete();      // settle: releases the backward prerequisite ref, retires the node
}

bool pipe_try_inline(Scheduler&, Pipe&, Access, const Task_ptr&)
{
    return false;   // inline fast path is a follow-up (stage 5)
}

} // namespace detail
} // namespace ts

#endif // TS_PIPE_TAIL
