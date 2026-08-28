<!-- Design for API-stability item M1, option (b): a caller-owned operation handle for
     multi-object access. Companion to docs/internals/access-op-design.md (single-object) and
     docs/internals/pipe-rebase.md section 0 (the cascade). -->

# Multi-object `Access_op`: design

**Decision (author, 2026-08-21):** M1 option **(b)** - `ts::access(fn, objs...)` returns a
real caller-owned operation over N pipes, with an opportunistic inline path, rather than
forwarding to `ts::async` and returning a heap `Task<R>`.

## 1. What is wrong today

Two verbs with one name and incompatible handles:

```cpp
auto a = numbers.access(fn);          // Access_op<T, Body> - caller-owned, pinned, zero-alloc
auto b = ts::access(fn, combat, economy);   // Task<R>      - heap block, refcounted, detached
```

`ts::access` forwards verbatim to `ts::async` (`guarded.h`), so the multi-object form is not
opportunistic at all - it always enqueues, and it always allocates. The name promises the
single-object contract ("runs on the calling thread when the object is free") and does not
keep it.

## 2. What already exists, and what it costs to reuse

Three pieces are in place, which is what makes (b) tractable:

1. **The caller-owned op.** `Access_op<T, Body>::State` derives from `Task_control_block`,
   embeds one `Pipe_link`, and flattens the access context into plain members
   (`pipe`/`inst`/`epoch`/`rank`) rather than capturing a closure. Custody is
   `Flags::caller_owned`: the machinery never takes a reference.
2. **The N-ary cascade.** Multi-object `async` already binds N links in canonical
   (pipe-address) order, seeds `num_locks = N`, and enters link 0; each granted turn releases
   one lock and enters the next, and the last dispatches. Deadlock-freedom comes from the
   canonical order, and it is the *same* cascade graph nodes use.
3. **An N-ary release path.** `advance_pipe_links` already loops `[0, pipes_entered)`
   (`src/guarded.cpp`), and `pipes_entered` is the cascade's progress marker.
   `pipe_try_inline` simply sets it to `1`.

**The release side therefore needs no change.** An N-ary inline admission sets
`pipes_entered = N` and settle releases all N, admitting each pipe's successors, exactly as it
does for the queued cascade.

What must be built is the storage (section 3) and the inline probe (section 4).

## 3. Storage

Per 7.1 this is one type, declared `template<typename... Args>` and split internally into the
objects (all but last) and the body (last), so `Access_op<T, Body>` keeps its present meaning:

```cpp
template<typename... Args>       // Access_op<Combat, Economy, Fn>; N=1 unchanged
class Access_op
{
    struct State : detail::Task_control_block
    {
        detail::Result_storage<result_type> storage;

        // Canonical (pipe-address) order - drives the cascade.
        detail::Pipe_link links[N];
        detail::Pipe*     pipes[N];
        Access            modes[N];

        // Declaration order - drives the invoke and the access context.
        std::tuple<detail::Mode_ref_t<Modes, Ts>*...>   insts;
        const std::atomic<std::uint64_t>*               epochs[N];
        unsigned                                        ranks[N];

        alignas(Body) unsigned char body_store[sizeof(Body)];
        bool bound, started, consumed, queued, settle_synchronously;
    };
    State state_;
};
```

**Two orderings, deliberately.** The links are sorted by pipe address because the cascade's
deadlock-freedom depends on a globally canonical order. The instances stay in declaration
order because that is the order `fn` takes its parameters. Today's heap path has the same
split - the closure captures declaration order while the links are sorted - so this is not a
new hazard, but it is the one place a reader will trip, and it earns a comment at the
declaration.

**Flattening the closure is the point.** The heap path builds a lambda capturing
`instances`/`epochs`/`ranks` tuples; that capture is what forces the allocation. Storing them
as members is the same technique `Access_op` already uses at N=1, applied N-ary.

**Cost, stated plainly.** The op carries N links plus N pipe pointers, epochs and ranks. For
the common N=2 that is on the order of 100-150 bytes of *caller* storage - a stack frame or a
class member - against one heap block today. Zero allocation is the trade, and the op is
non-movable for the same reason `Access_op` is: its address is the identity the pipe links
point at.

## 4. The inline path: all-or-nothing under simultaneous locks

The single-object arm locks one pipe, checks admissibility, admits, unlocks, then runs the
body. The N-ary generalization has one real question: what to do when some pipes are free and
others are not.

### 4.1 What this codebase already decided

The question is not new here. Three mechanisms already face it, and all three give the same
answer - for the **blocking** case:

| mechanism | strategy |
|---|---|
| graph node's declared set | sequential, canonical (pipe-address) order |
| multi-object `async` cascade | sequential, canonical order |
| `Multi_access_awaiter::drive()` - `co_await ts::read_write(a, b)` | sequential, canonical order |

Each acquires one pipe at a time and **holds what it has while waiting for the next**. That is
textbook hold-and-wait, made safe by the globally canonical order - the same order all three
use, which is why none of them can deadlock against the others.

For the **non-blocking** case only N=1 exists: `pipe_try_inline`.

So the two are answering different questions, and the answers differ for a reason:

- *"Acquire N, and I am willing to wait"* -> sequential canonical hold-and-wait.
- *"Acquire N only if I never have to wait"* -> all-or-nothing, because a sequential *try* has
  nowhere to put the partial set. It would have to roll back, which is the option rejected
  below.

The all-or-nothing probe is therefore **the N-ary generalization of `pipe_try_inline`**, not a
competitor to the cascade. It does not change how anything is acquired when waiting is on the
table.

### 4.2 The same conclusion, reached independently

`docs/internals/static-order-checking-and-ww-mutex.md` §2.4 already evaluated batch multi-acquire for a
different purpose (the suspended-ABBA hole) and arrived at the same shape, as the cheaper
alternative to a full `ww_mutex`:

> a runtime-length second batch acquisition, taken before any mutation, in canonical
> pipe-address order, all-or-nothing [...] deadlock-free by the same argument as
> `multi_acquire` **provided the node holds nothing else across it**

The proviso is worth keeping in view - it is exactly the mixed-hold case in 7.2.

### 4.3 What the field does

The standard taxonomy is prevent / recover / detect (the table in
`static-order-checking-and-ww-mutex.md` §2.4 uses the same split):

- **Ordering (resource hierarchy)** - the classic prevention. Its precondition is that you
  know the whole set before taking any of it; the literature notes this "may rule out dynamic
  transactions". Our set is a compile-time parameter pack, so the precondition holds and this
  is what the cascade already does.
- **`std::lock` / `std::scoped_lock`** - try-and-back-off, deadlock-avoiding without an order.
  This is what the probe uses for its brief critical section, so the probe does not have to
  re-implement ordering.
- **Conservative (static) two-phase locking** - acquire the entire set at transaction start or
  none of it. Deadlock-free by construction, at the cost of concurrency. This is precisely
  "probe and commit".
- **`ww_mutex` (Linux DRM/GPU)** - wound-wait with backoff and retry. It exists specifically
  because GPU buffer sets *cannot* be ordered statically: "there is no way to guarantee that
  buffers appear in a execbuf/batch in the same order in all contexts". That is not our
  situation, and the mechanism requires revocable grants, which this library does not have.
- **Detect-then-abort** (most databases) - also needs revocation.

The two techniques designed for *unknown or incrementally discovered* sets are the two we
cannot use and do not need. For a statically known, canonically ordered set, ordering (when
waiting) and all-or-nothing (when not) are the textbook answers.

### 4.4 The rejected alternative

**Rejected: acquire-then-roll-back.** Admitting on pipes 1..k and releasing them when pipe k+1
turns out to be busy is observationally a zero-length access, so it is not *incorrect* - but a
release runs the admission pass, so it can admit and fire waiters that must then be raced. It
turns a failed probe into visible scheduling work.

**Chosen: probe and commit under all N mutexes at once.**

```
scoped_lock over all N pipe mutexes          // compile-time N; std::lock avoids deadlock
if every pipe i has queue_head == nullptr && admissible(pipe_i, mode_i):
        admit all N
        pipes_entered = N
        (unlock)
        run the body inline on this thread    // settle releases all N
else:
        (unlock)
        fall through to the queued cascade    // unchanged: num_locks = N, pipe_enter_first
```

Three properties make this safe:

- **No user code under the mutexes.** The critical section is N admissibility checks and N
  admits. The body runs after unlocking - admission is what excludes other work, not the
  mutex. Holding a pipe mutex across a user body would block every operation on that object
  and would self-deadlock the moment the body touched the same pipe.
- **No lock-ordering requirement for the probe.** `std::scoped_lock`'s multi-mutex constructor
  uses `std::lock`, which is deadlock-avoiding. The canonical order still governs the queued
  cascade; the probe does not need to duplicate it.
- **No partial state on failure.** Nothing is admitted unless every pipe passes, so a failed
  probe is invisible to every other participant.

**Fairness is preserved per pipe.** Bailing when any pipe has a non-empty queue is the direct
analogue of the single-object rule "never jumps queued work". The op takes the inline arm only
when it would displace nobody on any object.

## 5. Why deadlock-freedom is unaffected

The queued path is untouched: links are entered one at a time in canonical order, which is what
makes a cross-object wait cycle unrepresentable, and it is the same cascade graph nodes use -
so nodes and multi-object accesses still cannot deadlock against each other.

The inline path never holds a *partial* set of admissions across a wait: it either holds all N
(and is running, not waiting) or holds none. A held-all-N inline body is exactly a graph node
holding its declared set, which the model already permits.

## 6. Refire, rebind, custody

Unchanged from `Access_op`, because they key off state the cascade already resets: `start()` on
a settled op clears `pipes_entered = 0` and `num_locks = 0` before refiring, so the same
storage re-enters the pipes with no allocation. `Flags::caller_owned` keeps the machinery from
ever taking a reference, and the destructor's wait-out-if-in-flight logic is arity-independent.

## 7. Open decisions

### 7.1 One type - RESOLVED (author, 2026-08-21): unify, without breaking existing spellings

The constraint is that the type is *named*, not just `auto`: the whole point of a caller-owned
op is holding one as a class member, which means writing `ts::Access_op<Inventory, Fn>` in a
header. Any unification must leave that spelling meaning exactly what it means today.

It can. The current declaration is already **objects first, body last**:

```cpp
template<typename T, typename Body> class Access_op;      // today
```

so the N-ary form is the natural extension of the same convention:

```cpp
ts::Access_op<Inventory, Fn>                  // N=1 - unchanged, still compiles, same meaning
ts::Access_op<Combat, Economy, Fn>            // N=2 - new
```

C++ requires a parameter pack to come last, so the declaration is `template<typename... Args>`
with the class splitting `Args` into "all but last" (the objects) and "last" (the body). That
is a small, contained metaprogram; every existing explicit spelling keeps working unchanged
because it was already in objects-then-body order.

**No deprecation, no alias, no churn in user code or in ours.** The only files that change are
the ones implementing the split.

Verify with a static assertion that `Access_op<T, Body>` still names the single-object
specialization, so the compatibility claim is enforced rather than asserted in prose.

### 7.2 Reentrancy and mixed holds - lend, rather than a documented restriction

Single-object `access` has a reentrant arm: `writer_owner == current_task` runs the body inline
under the already-held grant. The N-ary question is what to do when the caller holds *some* of
the objects - holds A, wants A and B.

**What the earlier draft proposed, and why it was wrong.** It proposed falling back to the
queued cascade and calling the case governed by the existing rules. Tracing it through:

- The caller holds A's write grant and `co_await`s the op.
- The op's link on A queues *behind the caller's own hold*.
- A's turn is never granted, so the op never completes, so the caller never resumes and never
  releases A. Deadlock.

The rules do catch it, so it is not a *safety* hole. `Rule::circular_wait` is defined as
exactly this shape - a held-grant to awaited-pipe wait cycle - and fatals with a named cycle.
Where it does not fire, the `Multi_access_awaiter` comment records a known diagnostic gap for
multi-object waits (a cross-waiter ABBA through a later-blocked pipe goes unrecorded), and
`Rule::deadlock_net` reports it instead: a coarser, window-delayed report rather than an
immediate named cycle. `Rule::access_rank` fires earlier still if the objects carry declared
ranks and the await climbs out of order. So the outcome is always a fatal, never a silent hang.

**But a detected fatal is still a rejected program, and the graph accepts this one.** A node
holding objects can `co_await inner.execute()` over those same objects today: `bind_links_for_run`
intersects the inner graph's access set with the caller's `Access_context` and re-binds the link
slab to only the objects the run must actually take turns on. A lent object's turn is simply
never taken, because an awaited inner run is *contained* in the caller's grant window, which is
already the exclusion it needs.

That containment argument applies verbatim to an awaited multi-object access. So the divergence
is not justified by anything structural - it is just an unimplemented case. Two mechanisms over
the same pipes with different rules is exactly the kind of inconsistency that makes a library
feel arbitrary.

**Proposal: apply the lend protocol.** For each object, ask `current_access->grants(inst, mode)`
- the same predicate, keyed by instance address, that the graph uses. A granted object is lent:
no link entered, `num_locks` reduced accordingly. The rest go through the normal path.

Two things fall out for free:

- **The reentrant arm is subsumed.** All objects lent means nothing to acquire, so the body
  runs inline under the held grants - which is precisely what single-object reentrancy does,
  generalized, with no separate code path.
- **The inline probe narrows correctly.** It probes only the non-lent pipes, so a caller
  holding some objects gets the inline arm more often, not less.

**Inherited fatal.** The graph's read-holder-where-inner-writes case applies unchanged: a
caller holding only READ on an object the body writes can neither lend (the grant is too weak)
nor queue (it would deadlock behind itself). That must fatal, with the graph's message shape.

**Cost.** One `grants()` call per object at fire time, on a path that already walks the objects
to sort them. The savings are larger than the cost: every lent object is a pipe operation not
performed.

### 7.3 An upper bound on N - RESOLVED (author, 2026-08-21): yes

`pipes_entered` is a `std::uint8_t`, so the cascade already caps a task at 255 pipes - a
silent bound inherited from a field width, which is not a design. The op carries a
`static_assert` at a human number instead: **8**, matching `Access_context::max_entries`, so
the arity limit and the grant-context limit are one number a user can hold in their head.
Exceeding it is a compile error naming both.
## 8. Staging

1. **Caller-owned N-link op, queued dispatch only.** Fixes the API defect - one return type,
   zero allocation - without touching admission. `ts::access` still always enqueues, i.e. no
   behaviour change, so the risk is contained to storage and lifecycle.
2. **The all-or-nothing inline arm** (section 4). This is what earns the name `access`.
3. **Documentation of the mixed-hold restriction** (7.2), or lifting it if the lend protocol is
   generalized later.

Phase 1 alone is a shippable improvement, which matters if the tag arrives before phase 2.

**As landed (2026-08).** All three phases, in that order, plus M6 from
the API-stability pass (the option `name` fields became `ts::Named`, so a multi-object verb
can carry a call site). Three notes where the implementation differs from the sketch above:

1. **Where the modes live.** §3 shows the instances as a typed tuple and the modes as a
   compile-time pack threaded in from the verb. As landed, the op recomputes each position's
   mode from its own template arguments - `detail::Op_traits` splits `Args...` and applies the
   same tier rules the verbs use - so nothing has to be threaded through, and a *tagged*
   position carries its declared mode in the type (`Access_op<Access_arg<T, M>, ..., Body>`,
   produced only by the tagged verb's deduced return type). The instances are stored as
   `const void*` in declaration order, cast back per position at the invoke, which serves the
   body, the access context and the lend check from one array.
2. **The probe's lock.** §4.4 specifies `std::scoped_lock` over a compile-time N. The lend
   protocol makes the set a *runtime* count - the lent objects are not probed - so the probe
   instead locks the bound pipes in ascending address order, which is the order they are
   already sorted into. That is deadlock-free by the same argument the cascade uses, and
   nothing else in the library ever holds two pipe mutexes at once.
3. **The guard-rule exemption needed updating.** `reentrant_under_held_grant` (the one
   exemption to `Rule::await_under_guard`) recognized a reentrant access by every bound pipe
   being write-owned by the running task. An all-lent op binds no pipe at all, which is also
   what a bare task looks like, so the fact is now recorded on the block
   (`Flags::all_lent`, a spare bit) and the rule reads that.

## 9. Test plan

The single-object `Access_op` cases apply at N>1 (bind/start/refire/dtor-waits/cancellation/
double-consume/never-started). Additional, arity-specific:

- **all-free runs inline** - the body observes the calling thread's id.
- **one pipe busy defers** - the body runs off-thread, and no partial admission is observable
  from a concurrent reader on the free pipe.
- **queued work is never jumped** - an entry queued on any one pipe suppresses the inline arm.
- **readers overlap** - two multi-object read ops over the same pair run concurrently.
- **canonical order holds** - two ops over the same objects declared in opposite orders do not
  deadlock under stress.
- **duplicate object stays fatal** - a death test, as today.
- **cross-arity ordering** - a multi-object op against a graph node over shared objects, under
  stress, to confirm the shared cascade still serializes them.

The last two want a TSan run: they are the cases where a partial-admission bug would surface as
a race rather than a failure.
