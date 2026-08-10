#pragma once

#include "ts/access.h"

#include <cstdint>

namespace ts
{
namespace detail
{

struct Task_control_block;
struct Pipe;

// A task's entry in one pipe's admission queue (docs/pipe-rebase.md §0.2). Embedded in the
// owning task's allocation - `Piped_executable` for `async`, the graph's per-node slab -
// never heap-allocated on its own, so the pipe hot path allocates nothing. All fields are
// written either once at bind time or under the owning pipe's mutex; there is no lock-free
// protocol on a link.
//
// Review note (author): after the first running version, audit every field and compact the
// layout - the `owner` redundancy across a task's links, `pipe` vs deriving it, and
// `index`/`mode` packing. Correctness and readability first.
struct Pipe_link
{
    Pipe_link* next = nullptr;             // intrusive FIFO under the pipe mutex; reused for
                                           // the granted list collected by one admission pass
    Task_control_block* owner = nullptr;   // the task this entry admits; null marks a hold
                                           // entry (`pipe_acquire`'s node - see guarded.cpp)
    Pipe* pipe = nullptr;                  // the pipe this entry queues on
    std::uint8_t index = 0;                // position in the owner's link array (drives the
                                           // sequential canonical cascade)
    Access mode = Access::read_only;       // this pipe's access mode
};

} // namespace detail
} // namespace ts
