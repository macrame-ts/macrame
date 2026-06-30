#pragma once

#include "access.h"
#include "thread_safe.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace sample
{

// Fork-join over `chunks`, invoking `chunk_fn(chunk_index)`. Helpers run on the
// default scheduler and the calling thread participates, so it is deadlock-free
// regardless of worker availability (the caller alone can drain every chunk;
// helpers only accelerate). Chunks are handed out dynamically, so the work
// load-balances.
//
// Subtasks run under the calling node's access context, so guarded store access
// inside `chunk_fn` passes the harness. This is the intended pattern: a node with
// exclusive (`read_write`) access to a store parallelizes over its disjoint
// elements. (A general `parallel_for` in the core, with proper oversubscription
// instead of a participating spin-wait, is future work -- see docs/TODO.md.)
template<typename Fn>
void parallel_for(int chunks, Fn chunk_fn)
{
    const ts::Access_context* ctx = ts::detail::current_access;

    std::atomic<int> next{ 0 };
    std::atomic<int> completed{ 0 };

    auto drain = [&]
    {
        int i;
        while ((i = next.fetch_add(1, std::memory_order_relaxed)) < chunks)
        {
            chunk_fn(i);
            completed.fetch_add(1, std::memory_order_release);
        }
    };

    auto helper = [&]
    {
        if (ctx)
        {
            ts::Access_scope scope(*ctx);   // inherit the node's access grant
            drain();
        }
        else
            drain();
    };

    int helpers = std::min(chunks, static_cast<int>(std::thread::hardware_concurrency())) - 1;
    for (int h = 0; h < helpers; ++h)
        ts::detail::submit_closure(ts::default_scheduler(), helper);

    drain();   // the calling thread participates; its context is already installed

    while (completed.load(std::memory_order_acquire) < chunks)
        std::this_thread::yield();
}

} // namespace sample
