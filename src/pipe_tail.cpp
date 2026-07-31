// Design-B tail-chain pipe (docs/pipe-rebase.md §4-5). Selected by TS_PIPE_TAIL=1; the
// current mutex-guarded deque pipe lives in guarded.cpp under `#if !TS_PIPE_TAIL`.
//
// STAGE 3a -- the writer serial chain + reservations, WITHOUT the reader head-walk yet:
// every pipe task (readers included) chains after its predecessor and runs alone, exactly
// like UE `FPipe`. This validates the tail + the UE refcount protocol + the drain; reader
// concurrency (the §5.2B head-walk) lands in stage 3b, at which point the reader-concurrency
// tests (currently expected to fail under this flag) pass.

#include "ts/guarded.h"
#include "ts/scheduler.h"

#if TS_PIPE_TAIL

#include <atomic>
#include <cstdint>
#include <functional>

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

void run_pipe_block(void* data);

// Dispatch a ready pipe block: hand a fresh ref to the scheduler queue; `run_pipe_block`
// adopts it back.
void pipe_submit(Task_control_block* block)
{
    intrusive_inc(block);
    global_scheduler().submit(&run_pipe_block, block, static_cast<Priority>(block->flags.priority));
}

// Advance the chain past `self` (completed or released): dispatch its single successor,
// clear the tail if `self` is still it (dropping the tail ref), decrement `task_count`.
void pipe_advance(Pipe& pipe, Task_control_block* self)
{
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

// `on_complete` hook (fires at settle, after the body): advance the pipe.
void pipe_on_complete(Task_control_block* self)
{
    pipe_advance(pipe_of(self), self);
}

// Trampoline for an admitted pipe block. A reservation fires its `on_acquired` and HOLDS
// (no body, no advance until `pipe_release`). A normal task runs its body; settle then
// fires `pipe_on_complete`.
void run_pipe_block(void* data)
{
    Task_ptr block(static_cast<Task_control_block*>(data), Adopt_ref{});

    if (block->flags.pipe_reservation)
    {
        Pipe& pipe = pipe_of(block.get());
        pipe.held.store(block.get(), std::memory_order_release);   // pipe_release finds it here
        if (block->result_ptr)
        {
            auto* on_acquired = static_cast<std::move_only_function<void()>*>(block->result_ptr);
            (*on_acquired)();
        }
        return;   // held; `pipe_release` advances + retires it
    }

    block->execute(block, /*gen*/ 0);   // Executable::run -> body -> complete -> settle -> on_complete
}

// Shared push: exchange the tail, link after the predecessor (or run now if idle / the
// predecessor already completed). `raw` must ALREADY carry a dedicated tail ref (the
// caller inc'd it, or transferred one in) -- this function does not add it. The
// exchanged-out predecessor's tail ref transfers into our `prerequisites` (keeping it
// alive until we settle). Returns whether we were admitted synchronously (idle / prev done).
bool pipe_push(Pipe& pipe, Task_control_block* raw, bool reader, bool dispatch_on_admit)
{
    pipe.task_count.fetch_add(1, std::memory_order_relaxed);

    std::uintptr_t prev_word = pipe.tail.exchange(pack(raw, reader), std::memory_order_acq_rel);
    if (prev_word == 0)
    {
        raw->flags.pipe_head = reader;   // idle -> a reader is its group's head
        if (dispatch_on_admit)
            pipe_submit(raw);
        return true;   // admitted synchronously
    }

    Task_control_block* prev = tail_block(prev_word);
    bool prev_reader = (prev_word & reader_bit) != 0;
    // Adopt prev's transferred tail ref as our backward link, set BEFORE we can be
    // dispatched (so it can't race our own settle clearing prerequisites).
    raw->prerequisites.push_back(Task_ptr(prev, Adopt_ref{}));

    Task_control_block* expected = nullptr;
    if (prev->pipe_next.compare_exchange_strong(expected, raw,
            std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        raw->flags.pipe_head = reader && !prev_reader;   // §5.2B.1: head iff predecessor is a writer
        return false;   // linked; prev's advance dispatches us
    }

    // prev already completed (pipe_next == pipe_closed) -> run now, we head a new group.
    raw->flags.pipe_head = reader;
    if (dispatch_on_admit)
        pipe_submit(raw);
    return true;
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
    Task_ptr node = make_bare_block();   // ref 1 = the tail ref (consumed by pipe_push's model)
    Task_control_block* raw = node.get();
    raw->flags.pipe_job = true;
    raw->flags.pipe_reader = (mode == Access::read_only);
    raw->flags.pipe_reservation = true;
    raw->dispatch_arg.store(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&pipe)), std::memory_order_relaxed);

    // Heap the callback so the reservation node can fire it on admission.
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
    // Deferred: prev's advance will dispatch `raw` -> run_pipe_block fires the callback.
    return false;
}

void pipe_release(Scheduler&, Pipe& pipe, Access)
{
    Task_control_block* held = pipe.held.exchange(nullptr, std::memory_order_acq_rel);
    if (held == nullptr)
        return;
    Task_ptr keep(held);   // hold alive across advance + settle (advance may drop the tail ref)
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
    return false;   // stage 3a: never inline; always enqueue (inline fast path is a follow-up)
}

} // namespace detail
} // namespace ts

#endif // TS_PIPE_TAIL
