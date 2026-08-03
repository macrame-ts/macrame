// The tail-chain pipe (docs/pipe-rebase.md §4-6). Selected by TS_PIPE_TAIL=1; the
// mutex-guarded deque pipe lives in guarded.cpp under `#if !TS_PIPE_TAIL`.
//
// Every pipe-accessing task is an ordinary dynamic task whose pipe turns are `num_locks`
// prerequisites; what sits in a pipe's line is the task's embedded `Pipe_link` for that
// pipe (one successor slot per line membership). Lines are chained through the links'
// tagged `next` words off an atomic per-pipe tail; a link's turn releases one lock on its
// owner (`link_turn`), the owner's settle retires all its links (`advance_pipe_links`).
//
// Reader concurrency is a RUN: the front reader arms itself as the run's head (gate = 1,
// joinable) and walks the already-chained readers behind it (their turns fire together);
// a reader arriving while the run is ACTIVE joins it lazily (`try_join`) -- its turn fires
// immediately, overlapping the run -- with the run retiring at the gate's zero transition.
// The next writer (or a reader that missed the join window) waits serially and is
// dispatched by the run's retire. No pipe operation allocates.

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

// The pipe tail is a tagged `Pipe_link*` (links are 64-aligned -> 6 low bits): bit 0 = 1
// for a reader entry (a pusher classifies its predecessor from the exchanged word alone),
// bits 1-5 = the link's tenure mod 32 at push time (the reuse-era stamp, §5.2B.8), whole
// word 0 = idle. All tagging lives in these helpers.
constexpr std::uintptr_t tail_reader_bit = 1;
constexpr std::uintptr_t tail_tag_mask = 63;

std::uintptr_t pack_tail(const Pipe_link* l)
{
    std::uintptr_t era = (l->tenure.load(std::memory_order_relaxed) & 31u) << 1;
    return reinterpret_cast<std::uintptr_t>(l) | era | (l->mode == Access::read_only ? tail_reader_bit : 0);
}
Pipe_link* tail_link(std::uintptr_t word)
{
    return reinterpret_cast<Pipe_link*>(word & ~tail_tag_mask);
}
std::uint32_t tail_era(std::uintptr_t word)
{
    return static_cast<std::uint32_t>((word >> 1) & 31u);
}

void link_turn(Pipe_link& l);

#if TS_SAFETY_CHECKS
// Write-grant epoch (seqlock parity, see `Pipe::write_epoch`): +1 when a writer link's
// turn arrives (window opens, count goes odd), +1 at its advance (closes, even). Readers
// never bump. The old graph-handoff `+2` elision is gone -- a handed-over line is now an
// ordinary retire + turn pair, so parity is maintained by construction.
void epoch_bump(Pipe& pipe)
{
    pipe.write_epoch.fetch_add(1, std::memory_order_relaxed);
}
#endif

// Lazily join reader `j` to the ACTIVE run its chain predecessor `p` belongs to, firing
// `j`'s turn immediately (the reader-concurrency core: without this, a reader arriving
// after the front reader's turn would serialize). Returns false when there is nothing to
// join -- `p` is a writer, not yet turned, departed, or the run is already retiring -- and
// `j` waits for its serial turn from the run's retire.
//
// Safety, hop by hop: `p`'s allocation is pinned by `j`'s custody ref (taken at the tail
// exchange). `p.join_pin` (0 -> 1) freezes `p`'s departure for the protocol's duration and
// its acquire syncs with `p`'s role/target publication (a joined `p` releases the pin
// AFTER publishing them). The head is reachable while `p` has not departed (`p` holds a
// head ref -- walk- or join-granted -- released only at its departure, which the pin
// blocks). The gate's conditional increment commits the join: once it succeeds the run
// cannot retire before `j` departs, which also makes `j`'s retire-point extension visible
// to the final departer (`j`'s `group_target` store precedes `j`'s own gate decrement).
bool try_join(Pipe_link& p, Pipe_link& j)
{
    if (p.mode != Access::read_only || j.mode != Access::read_only)
        return false;

    std::uint8_t pin = 0;
    if (!p.join_pin.compare_exchange_strong(pin, 1, std::memory_order_acq_rel, std::memory_order_acquire))
        return false;   // departed (2) -- the retire will dispatch us serially

    Pipe_link* head = nullptr;
    switch (p.role.load(std::memory_order_acquire))
    {
    case Link_role::head:   head = &p; break;
    case Link_role::member: head = p.group_target.load(std::memory_order_acquire); break;
    case Link_role::serial: break;   // not yet turned (the walk will absorb us) -- no join
    }
    if (head == nullptr)
    {
        p.join_pin.store(0, std::memory_order_release);
        return false;
    }

    // Commit half 1: conditionally increment the run's gate; 0 means the run is retiring.
    std::uint32_t g = head->gate.load(std::memory_order_acquire);
    for (;;)
    {
        if (g == 0)
        {
            p.join_pin.store(0, std::memory_order_release);
            return false;
        }
        if (head->gate.compare_exchange_weak(g, g + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }

    // Commit half 2: claim j's turn. The head's walk may be claiming j concurrently (j
    // chained behind a link the walk is traversing); exactly one source may fire the turn
    // and account the gate unit. Losing means the walk has j: return our spare gate unit
    // (safe -- the walk's unit for j is outstanding, so the gate cannot hit zero here).
    std::uint8_t claim = 0;
    if (!j.turn_claim.compare_exchange_strong(claim, 1, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        head->gate.fetch_sub(1, std::memory_order_acq_rel);
        p.join_pin.store(0, std::memory_order_release);
        return false;   // the walk fires our turn
    }

    intrusive_inc(head->owner);   // the joiner's ref on the head (released at departure)
    // ORDER INVARIANT (retire-point monotonicity): extend the run's retire point to j
    // BEFORE publishing j as a member. Any later extender first observes membership
    // (role acquire), so its own extension is coherence-later -- the retire point can only
    // move forward along the chain. The reverse order let an extension through j land
    // before this one, REGRESSING the retire point: the run then retired at a stale link,
    // and the true chain end's successors never got their turns (a permanent line wedge).
    j.group_target.store(head, std::memory_order_relaxed);
    head->group_target.store(&j, std::memory_order_release);
    j.role.store(Link_role::member, std::memory_order_release);
    p.join_pin.store(0, std::memory_order_release);

    link_turn(j);   // the joined turn: fires now, overlapping the run
    return true;
}

// Enter `l` into its pipe's line: the line's ref, the tail exchange (the linearization
// point), then an immediate turn (idle line / retired predecessor), a lazy join of an
// active reader run, or a CAS onto the predecessor's `next` slot to wait serially.
void pipe_enter_link(Pipe_link& l)
{
    Pipe& pipe = *l.pipe;
    l.owner->pipes_entered = static_cast<std::uint8_t>(l.index + 1);   // settle advances [0, entered)
    pipe.task_count.fetch_add(1, std::memory_order_relaxed);
    intrusive_inc(l.owner);   // the line's ref on this entry (custody chain, §5.2B.6)

    std::uintptr_t prev_word = pipe.tail.exchange(pack_tail(&l), std::memory_order_acq_rel);
    if (prev_word == 0)
    {
        link_turn(l);   // idle line: the turn fires on the pushing thread
        return;
    }

    Pipe_link& prev = *tail_link(prev_word);
    Task_control_block* prev_owner = prev.owner;
    // The exchanged-out predecessor's line ref transfers to us; released at our advance.
    l.prev_owner = prev_owner;
    // The PUSHING THREAD's own pin on the predecessor, distinct from the custody ref
    // above: the moment the link-CAS below succeeds, the predecessor's retire (on another
    // thread) can dispatch OUR turn, run and settle our owner, and release the custody
    // ref -- freeing `prev`'s allocation while this thread is still inside `try_join`
    // reading it. Taken while the transferred ref is still unambiguously ours (pre-CAS),
    // dropped through the LOCAL once this thread is done touching `prev` (after the CAS,
    // `l` itself may already be settling -- touch neither `l` nor `prev` again).
    intrusive_inc(prev_owner);

    // Reuse-era gate (§5.2B.8): the exchange stamped the tenure the predecessor had when
    // it was pushed; a mismatch with its CURRENT tenure means we straddled a re-arm --
    // the tenure we queued behind has provably retired, so the line is ours now. (The
    // current tenure is stable for a same-era pusher: re-arm requires run quiescence.)
    std::uint32_t prev_tenure = prev.tenure.load(std::memory_order_acquire);
    if ((prev_tenure & 31u) != tail_era(prev_word))
    {
        intrusive_dec(prev_owner);   // drop the pusher's pin (custody ref remains)
        link_turn(l);
        return;
    }
    const std::uintptr_t expected_open = link_open_word(prev_tenure);

    for (;;)
    {
        std::uintptr_t cur = prev.next.load(std::memory_order_acquire);
        if (cur == link_closed || (cur != expected_open && (cur & 3) == 1))
        {
            // Retired -- or an open word of ANOTHER tenure (we straddled a re-arm after
            // the era gate; the same staleness, caught by the full-width tenure).
            intrusive_dec(prev_owner);   // drop the pusher's pin (custody ref remains)
            link_turn(l);   // the line is ours
            return;
        }
        if (cur != expected_open)
        {
            // A real link in a CURRENT-tenure predecessor's slot is impossible: only the
            // unique tail-exchanger of `prev` links there, and that is us.
            ts::fatal("pipe: foreign link in the predecessor's successor slot");
        }
        TS_PIPE_RACE_DELAY(push_link);
        if (prev.next.compare_exchange_weak(cur, reinterpret_cast<std::uintptr_t>(&l),
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // Chained. If the predecessor belongs to an ACTIVE reader run, join it -- the
            // turn fires inside; otherwise the predecessor's retire dispatches us.
            try_join(prev, l);
            intrusive_dec(prev_owner);   // drop the pusher's pin (custody ref remains)
            return;
        }
        // The slot changed under us (spurious failure or a concurrent close) -- re-read.
    }
}

// The front reader's walk: arm this link as the run's (joinable) head, then claim and
// fire the turns of the readers chained behind it, one at a time. Allocation-free -- a
// single pass over the chain. Every claimed member's gate unit is added BEFORE its turn
// fires (the gate cannot drain during the walk anyway: the head's own departure is
// pending). The walk STOPS at the first claim it loses -- that link joined on its own,
// which means it is turned and everything beyond is join territory; a reader that misses
// both paths waits serially and is dispatched by the run's retire.
void walk_group(Pipe_link& head)
{
    // Arm FIRST: a joiner may arrive the moment the gate goes nonzero.
    head.group_target.store(&head, std::memory_order_relaxed);
    head.role.store(Link_role::head, std::memory_order_release);
    head.gate.store(1, std::memory_order_release);

    Pipe_link* m = &head;
    for (;;)
    {
        std::uintptr_t nx = m->next.load(std::memory_order_acquire);
        if (!is_link_word(nx) || link_of(nx)->mode != Access::read_only)
            return;   // chain end, or a writer's link (dispatched by the run's retire)
        m = link_of(nx);

        std::uint8_t claim = 0;
        if (!m->turn_claim.compare_exchange_strong(claim, 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return;   // `m` joined on its own -- turned already; the join chain owns the rest

        head.gate.fetch_add(1, std::memory_order_acq_rel);            // m's unit, before m's turn
        // Retire point BEFORE membership -- the same order invariant as `try_join` (a
        // joiner through `m` must observe the point at `m` before extending past it).
        m->group_target.store(&head, std::memory_order_relaxed);
        head.group_target.store(m, std::memory_order_release);
        m->role.store(Link_role::member, std::memory_order_release);
        intrusive_inc(head.owner);   // the member's ref on the head (released at departure)
        link_turn(*m);
        // Reading m->next after m's turn is safe: a member's departure never touches its
        // own `next`, and the run cannot retire while the head's departure is pending.
    }
}

// A link's turn: its line granted this entry. Pins the owner for the whole turn, opens
// the write window, arms/walks a reader run, enters the owner's next line (the sequential
// canonical cascade), and releases one `num_locks` lock -- the last turn's release
// dispatches the body via the standard `dispatch_ready`.
void link_turn(Pipe_link& l)
{
    // Pin the owner (and with it `l`, which lives in its allocation) FIRST: a walked or
    // joined member can run and settle on another worker while this turn still executes,
    // and its departure can release the last refs on this owner.
    Task_ptr keep(l.owner);
#if TS_SAFETY_CHECKS
    if (l.mode == Access::read_write)
        epoch_bump(*l.pipe);   // write window opens
#endif
    if (l.mode == Access::read_only && l.role.load(std::memory_order_acquire) != Link_role::member)
        walk_group(l);   // front reader: become the run's (joinable) head
    if (static_cast<std::uint8_t>(l.index + 1) < l.owner->pipe_count)
        pipe_enter_link(l.owner->pipe_links[l.index + 1]);
    Task_control_block::release(keep);
}

// Retire a link, handing its line over: close the successor slot, clear the tail if this
// entry is still it (dropping the line's ref; a failed CAS means a pusher adopted the ref
// into its custody chain), and LAST dispatch the waiting successor's turn. Order is
// load-bearing: once the successor's turn fires, its owner can run, settle, and release
// the custody ref on `l.owner` -- freeing the allocation `l` lives in -- so every touch of
// `l` must happen before the dispatch; only captured locals after.
void chain_retire(Pipe_link& l)
{
    Pipe& pipe = *l.pipe;
    std::uintptr_t self_word = pack_tail(&l);
    Task_control_block* owner = l.owner;
    std::uintptr_t nx = l.next.exchange(link_closed, std::memory_order_acq_rel);
    std::uintptr_t expected = self_word;
    if (pipe.tail.compare_exchange_strong(expected, 0,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        intrusive_dec(owner);   // no successor ever pushed (a pushed one moves the tail first)
    if (is_link_word(nx))
        link_turn(*link_of(nx));
}

// A run participant's owner settled: depart the head's gate; the last one out retires the
// run at its newest link (`head.group_target`). The head is pinned by the departing
// member's head ref (or, for the head itself, by its own settle path), released only
// after the retire.
void group_depart(Pipe_link& head, Pipe_link& departing)
{
    Task_control_block* head_owner = (&departing == &head) ? nullptr : head.owner;
    if (head.gate.fetch_sub(1, std::memory_order_acq_rel) == 1)
        chain_retire(*head.group_target.load(std::memory_order_acquire));
    if (head_owner != nullptr)
        intrusive_dec(head_owner);
}

// Retire one entered link at its owner's settle. Exactly once per entered link; the
// `task_count` decrement is the LAST pipe touch (§7 drain discipline -- the waiter may
// free the pipe the instant the count hits zero).
void link_advance(Pipe_link& l)
{
    Pipe& pipe = *l.pipe;
#if TS_SAFETY_CHECKS
    if (l.mode == Access::read_write)
        epoch_bump(pipe);   // write window closes
#endif
    if (l.mode == Access::read_only)
    {
        // Close this link to joiners, waiting out one mid-protocol (its pin hold is a few
        // atomic ops). Joins through OTHER run members remain possible until the gate's
        // zero transition -- which this departure may be the one to cause.
        std::uint8_t pin = 0;
        while (!l.join_pin.compare_exchange_weak(pin, 2, std::memory_order_acq_rel, std::memory_order_acquire))
            pin = 0;
    }
    switch (l.role.load(std::memory_order_acquire))
    {
    case Link_role::serial:
        chain_retire(l);
        break;
    case Link_role::head:
        group_depart(l, l);
        break;
    case Link_role::member:
        group_depart(*l.group_target.load(std::memory_order_acquire), l);
        break;
    }
    if (l.prev_owner != nullptr)
    {
        intrusive_dec(l.prev_owner);   // custody chain: release the predecessor (§5.2B.6)
        l.prev_owner = nullptr;
    }
    pipe.task_count.fetch_sub(1, std::memory_order_release);
}

// The hold node behind `pipe_acquire`/`pipe_release` (the coroutine guards): a bodyless
// block + one link + the grant callback. Its `execute` fires at the link's turn WITHOUT
// completing -- the hold lasts until `pipe_release` settles it (the settle advances the
// link, releasing the line). The one pipe path that allocates, and the only remaining
// held-grant entity; everything else advances at its own settle.
struct Acquire_node
{
    Task_control_block core;
    Pipe_link link;
    std::move_only_function<void()> on_acquired;
    // Turn-vs-caller handshake: 0 initial, 1 turn fired, 2 caller went deferred. Whoever
    // arrives second owns the callback decision (turn second -> invoke; caller second ->
    // report "granted in-call", callback never runs).
    std::atomic<int> state{ 0 };
};

void acquire_node_run(const Task_ptr& c, std::uint64_t gen)
{
    if (!c->claim(gen))
        return;
    auto* node = reinterpret_cast<Acquire_node*>(c.get());
    if (node->state.exchange(1, std::memory_order_acq_rel) == 2)
        node->on_acquired();
    // Deliberately not completing: the grant is held until `pipe_release`.
}

} // namespace

void advance_pipe_links(Task_control_block* blk)
{
    for (std::uint8_t i = 0; i < blk->pipes_entered; ++i)
        link_advance(blk->pipe_links[i]);
}

void pipe_links_on_complete(Task_control_block* blk)
{
    advance_pipe_links(blk);
}

void pipe_enter_first(Task_control_block* blk)
{
    pipe_enter_link(blk->pipe_links[0]);
}

bool pipe_acquire(Scheduler&, Pipe& pipe, Access mode,
                  std::move_only_function<void()> on_acquired, void*& hold)
{
    auto* node = new Acquire_node();
    node->core.destroy = [](Task_control_block* c) { delete reinterpret_cast<Acquire_node*>(c); };
    node->core.execute = &acquire_node_run;
    node->core.on_complete = &pipe_links_on_complete;   // settle (at release) advances the link
    node->core.flags.run_inline = true;                 // a free line grants on this thread, in-call
    node->core.pipe_links = &node->link;
    node->core.num_locks.store(1, std::memory_order_relaxed);   // the turn
    node->on_acquired = std::move(on_acquired);
    node->link.owner = &node->core;
    node->link.pipe = &pipe;
    node->link.mode = mode;
    node->core.pipe_count = 1;

    Task_ptr keep(&node->core);   // refcount 0 -> 1; becomes the hold's ref
    pipe_enter_first(&node->core);
    hold = keep.release();
    return node->state.exchange(2, std::memory_order_acq_rel) == 1;   // 1: turn already fired in-call
}

void pipe_release(Scheduler&, Pipe&, Access, void* hold)
{
    Task_ptr node(static_cast<Task_control_block*>(hold), Adopt_ref{});
    node->complete();   // settle -> on_complete -> the link advances, the line moves on
}

bool pipe_try_inline(Scheduler&, Pipe& pipe, Access mode, const Task_ptr& block)
{
    // The `access` fast path: claim an IDLE line outright and run the body on the calling
    // thread. Any traffic at all falls back to the normal enter. An inline reader arms
    // itself as a joinable head first, so concurrent readers overlap it like any run.
    Pipe_link& l = block->pipe_links[0];
    std::uintptr_t expected = 0;
    if (!pipe.tail.compare_exchange_strong(expected, pack_tail(&l),
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return false;

    block->pipes_entered = 1;
    pipe.task_count.fetch_add(1, std::memory_order_relaxed);
    intrusive_inc(block.get());   // the line's ref, as in `pipe_enter_link`
    if (mode == Access::read_only)
    {
        l.group_target.store(&l, std::memory_order_relaxed);
        l.role.store(Link_role::head, std::memory_order_release);
        l.gate.store(1, std::memory_order_release);
    }
#if TS_SAFETY_CHECKS
    else
        epoch_bump(pipe);         // write window opens (closed by the advance at settle)
#endif
    block->execute(block, /*gen*/ 0);   // body inline on the caller; settle advances the link
    return true;
}

#if TS_SAFETY_CHECKS
// The `retract_or_wait` diagnostic (declared in task.h; the mutex-pipe variant lives in
// guarded.cpp): sharp same-object message when the wait target is a pipe task holding a
// line this scope's grant covers -- that shape deadlocks. The links carry the pipe
// identities, so multi-object jobs get the sharp match too (the mutex pipe could not).
void blocking_sync_diagnose(const Task_control_block* blk) noexcept
{
    if (current_access != nullptr)
    {
        for (std::uint8_t i = 0; i < blk->pipe_count; ++i)
        {
            const Pipe_link& l = blk->pipe_links[i];
            if (l.pipe != nullptr && current_access->holds_epoch(&l.pipe->write_epoch))
            {
                TS_ENSURE(false, "sync() on an access to an object this scope already holds -- "
                    "this deadlocks; use then/when_all or nested tasks");
                return;
            }
        }
    }
    TS_ENSURE(false, "blocking sync() on non-retractable work inside an access scope -- "
        "occupies a worker and risks deadlock; prefer continuations (then/when_all) or "
        "nested tasks");
}
#endif

} // namespace detail
} // namespace ts

#endif // TS_PIPE_TAIL
