#pragma once

#include "ts/access.h"
#include "ts/guarded.h"

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

    // The chunk counters must outlive every helper: the caller returns as soon as
    // `completed == chunks`, but a helper that ran the last chunk still does one
    // more `next.fetch_add` to find there's no work left. Heap them via shared_ptr
    // (helpers hold a ref) so that final touch never lands on a freed stack slot.
    struct Counters
    {
        std::atomic<int> next{ 0 };
        std::atomic<int> completed{ 0 };
    };
    auto counters = std::make_shared<Counters>();

    auto drain = [&chunk_fn, chunks](Counters& c)
    {
        int i;
        while ((i = c.next.fetch_add(1, std::memory_order_relaxed)) < chunks)
        {
            chunk_fn(i);
            c.completed.fetch_add(1, std::memory_order_release);
        }
    };

    int helpers = std::min(chunks, static_cast<int>(std::thread::hardware_concurrency())) - 1;
    for (int h = 0; h < helpers; ++h)
        ts::detail::submit_closure(ts::default_scheduler(), [counters, ctx, chunk_fn, chunks]
        {
            // chunk_fn / ctx are only touched while claiming a chunk (i < chunks),
            // which can only happen before the caller returns -- so capturing them
            // is safe even though the helper may outlive parallel_for.
            auto run = [&]
            {
                int i;
                while ((i = counters->next.fetch_add(1, std::memory_order_relaxed)) < chunks)
                {
                    chunk_fn(i);
                    counters->completed.fetch_add(1, std::memory_order_release);
                }
            };
            if (ctx)
            {
                ts::Access_scope scope(*ctx);   // inherit the node's access grant
                run();
            }
            else
                run();
        });

    drain(*counters);   // the calling thread participates; its context is already installed

    while (counters->completed.load(std::memory_order_acquire) < chunks)
        std::this_thread::yield();
}

} // namespace sample
