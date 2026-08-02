#pragma once

#include "ts/access.h"

#include <atomic>
#include <cstdint>

// Race-widening hook for the TSan campaign: a no-op normally, a short yield/spin when a
// test build defines it (docs/pipe-rebase-tests.md §3.3). Placed at the walk's close CAS
// and the push's link CAS.
#ifndef TS_PIPE_RACE_DELAY
#define TS_PIPE_RACE_DELAY(point)
#endif

namespace ts
{
namespace detail
{

struct Task_control_block;
struct Pipe;

// How a link participates in a reader run. `serial` until its turn (writers stay serial
// for life); a front reader arms itself `head` at its turn; `member` = a grouped reader
// (walked by the head, or lazily joined) whose departure counts down the head's gate.
enum class Link_role : std::uint8_t
{
    serial,
    head,
    member
};

// A task's entry in ONE pipe's line (docs/pipe-rebase.md §5.2B). Embedded in the owning
// task's allocation -- `Piped_executable` for `async`, the graph's per-node slab -- never
// heap-allocated on its own; the pipe hot path allocates nothing.
//
// `next` is a tagged word -- the ONE place the line's structure lives:
//   `link_open`    no successor yet (a pusher CASes itself in)
//   `link_closed`  this entry retired; a pusher finding it runs immediately
//   else           an aligned `Pipe_link*`: the successor waiting for this entry to retire
//
// Review note (author): after the first running version, audit every field and compact the
// layout -- the `owner` redundancy across a task's links, the `gate`/`group_target`/
// `join_pin` group-state width, `index`/`mode`/role packing, and `pipe` vs deriving it.
// Correctness and readability first.
struct alignas(8) Pipe_link
{
    std::atomic<std::uintptr_t> next{ 0 };

    Task_control_block* owner = nullptr;   // the task this entry admits
    Pipe* pipe = nullptr;                  // the line this entry sits in
    // Custody chain (§5.2B.6): the exchanged-out predecessor's line ref, adopted at push and
    // released at this link's advance -- every queued entry is kept alive by its successor,
    // anchored by the tail's ref on the newest.
    Task_control_block* prev_owner = nullptr;
    // Group state, by `role`: head -> the run's retire point (its newest grouped link; the
    // gate-0 departure retires the run there); member -> the run's HEAD (whose gate this
    // link departs). Null for serial links. Atomic: a lazy joiner reads its predecessor's
    // role/target and extends the head's (see `try_join` in pipe_tail.cpp).
    std::atomic<Pipe_link*> group_target{ nullptr };
    // Head only: outstanding departures of the run (head + members + joiners). Armed to 1
    // at the head's turn -- a nonzero gate is what makes an active run JOINABLE; a joiner's
    // conditional increment fails at 0 (run retiring) and falls back to waiting serially.
    std::atomic<std::uint32_t> gate{ 0 };
    // Join handshake on THIS link as a join predecessor: 0 = free, 1 = my chain successor
    // is joining through me, 2 = departed (closed to joins). The successor's pin-acquire
    // syncs with this link's role/target publication; the advance's close (0 -> 2) waits
    // out a joiner mid-protocol. Readers only; writers never carry joiners.
    std::atomic<std::uint8_t> join_pin{ 0 };

    std::atomic<Link_role> role{ Link_role::serial };
    std::uint8_t index = 0;                // position in the owner's link array (drives the cascade)
    Access mode = Access::read_only;       // this line's access mode
};

// `next` sentinels. Real links are >= 8-aligned, so small integers cannot collide.
inline constexpr std::uintptr_t link_open = 0;
inline constexpr std::uintptr_t link_closed = 1;

inline bool is_link_word(std::uintptr_t word) noexcept { return word > link_closed; }
inline Pipe_link* link_of(std::uintptr_t word) noexcept { return reinterpret_cast<Pipe_link*>(word); }

} // namespace detail
} // namespace ts
