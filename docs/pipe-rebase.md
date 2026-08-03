# Pipe rebase — design of record (pipes-as-edges, reader/writer)

Consolidates TODO 1.14 (+ its two addenda), the task-internals §6 retraction addendum,
and the `FPipe` note in task-systems-comparison.md into one design. Companion:
`docs/pipe-rebase-tests.md` (the test plan). Status: design, 2026-07-31, branch
`pipe-rebase`. Read this before touching `src/guarded.cpp` / the pipe.

The tests are labelled R1..R10 to match the concern labels here.

## 0. OUTCOME (2026-08, author decision): the evolved mutex pipe

The lock-free tail chain (§4–§5 below) was implemented, stress-tested, and **retired**.
This section is the design of record going forward; §4–§5 stay as the engineering record
of what was tried and why it was withdrawn. The two goals that now drive the work:

1. **(important) Collapse `Deferred::commit()` / `commit_async()` into one auto-dispatching
   `commit()`** — needs only an always-on grant-ownership answer ("does `current_task`
   hold this pipe's write grant?"), which never required the rebase.
2. **Simplify/optimize the pipe internals, in particular multi-object access.**

### 0.1 Why the chain was retired (the evidence)

- **Perf: the mutex was never the bottleneck.** The R10 gate came back negative: 22
  producers on ONE pipe measured 798 ns/op vs ~1050 ns/op uncontended — per-object mutexes
  with nanosecond holds barely degrade under contention. The lock-free half of the rebase
  fails its own perf gate.
- **Every serial (UE-verbatim) piece worked immediately; every reader-group piece burned.**
  Four distinct protocol bugs in the reader-run/join machinery (walk double-claim, three
  custody/lifetime UAFs, claim-without-fire — the last still open at retirement), plus the
  tenure/era ABA that exists only because the chain is lock-free. Late reader join is a
  counter problem; a counter under a lock is one line, a lock-free counter + FIFO + group
  close kept re-deriving research problems.
- **The author's semantic constraint** — reader N's completion must not depend on earlier
  readers — is native to counter admission and fought by every chained-completion scheme.
- The genuinely valuable inventions were all task-side and survive: **pipe turns as
  `num_locks` prerequisites** (the `pipe_count` trigger in `release()`), **pipes entered
  last** (§5.5), **frozen-at-launch** enforcement, **embedded per-line storage** (one
  allocation, zero closures), the test/TSan/bench net.

### 0.2 The design: one mutex per pipe, rebuilt internals

```cpp
struct Pipe
{
    std::mutex mutex;
    Pipe_link* head = nullptr;                 // intrusive FIFO of embedded links
    Pipe_link* tail = nullptr;                 //   (Job / std::deque / closures: deleted)
    int active_readers = 0;                    // late reader join = ++active_readers
    bool writer_active = false;
    std::atomic<Task_control_block*> writer_owner{ nullptr };   // ALWAYS-ON (goal 1): the
                                               // block holding the write grant; written
                                               // under `mutex` (plus the graph write
                                               // handoff, inside an exclusive window);
                                               // read lock-free by the ownership check
    std::condition_variable idle;              // drain (verdict: safe as-is, notify under lock)
    // write_epoch / graph_refs / debug_name as today (TS_SAFETY_CHECKS gating unchanged)
};
```

(`Pipe::last_write` from the first draft is dropped: a later unrelated async write
pollutes it, so the `Deferred` contract must not key off it. The ordering mechanism is
the enqueue-and-record seam below; a general last-write facility can return if a second
consumer appears.)

- **`Pipe_link` slims to a queue node**: `next` (plain pointer — the queue is
  mutex-guarded), `owner`, `pipe`, `mode`, `index`, `priority`. Deleted: `role`, `gate`,
  `group_target`, `join_pin`, `turn_claim`, `tenure`, `prev_owner`, the tagged-word
  encodings, alignas(64) (re-evaluate under TODO 4.8 with measurements). Storage stays
  embedded: single-object tasks carry one link in the wrapper, multi-object
  `Piped_executable<Body, R, N>` carries N, graph nodes use the compile()-time slab —
  the pipe hot path still allocates nothing.
- **Admission** (under the mutex; submission/execution outside, as today): a granted turn
  fires `release(link.owner)` — the `pipe_count`/`num_locks` trigger, unchanged. The last
  turn dispatches through `dispatch_ready` (priority, inline, cancellation, worker-less
  uniform). Reader/writer/FIFO rules identical to the current pipe. The queue holds one
  ref on `owner` per queued link (taken at enqueue, released after the turn fires).
  **Turn-firing happens OUTSIDE the mutex** (the trap): `release()` reaching zero
  dispatches — a scheduler submit, possibly a wake syscall, and in worker-less mode the
  submit EXECUTES the body in-call, so a body releasing the same pipe would self-deadlock
  on the held mutex. Admission collects the granted links under the lock and fires their
  releases after unlock — the same shape as today's `submit_admitted`.
- **Multi-object, unified (goal 2)**: one wrapper allocation with embedded `(pipe, mode)`
  bindings; `num_locks = launch + ordinary + P`; at the `now == pipe_count` trigger, the
  sequential canonical cascade enqueues link[0]; each admission fires `release(owner)` and
  enqueues the next link; the last dispatches. This one path serves dynamic multi-object
  `async` (deleting `Multi_async_state`, its `std::map`, and the `on_acquired` closures)
  AND graph nodes (deleting `acquire_next`'s pipe walk, `preheld`, `handoff_target`,
  `mark_preheld`; a node's cascade starts at data-ready). The explicit graph handoff goes
  away; its optimization is recovered only when the successor's link is already queued at
  the predecessor's release (release + next admission in one mutex pass) — NOT guaranteed
  (a successor whose cascade has not reached this pipe yet queues later), and an async
  write may now interleave between two conflicting nodes where the handoff used to splice
  them airtight. Behavior change accepted (the old whole-run reservation already allowed
  gaps elsewhere); re-measure the graph benchmark. `write_epoch` parity is preserved by
  the normal release/acquire bumps (+1 each, +2 total across a write→write boundary —
  same parity as the old handoff's +2). `writer_owner` hand-off likewise becomes natural
  (release clears it, the next admission sets it).
- **Held grants** (`pipe_acquire`/`pipe_release`, mode-aware) remain the coroutine-guard
  primitive, as on master. Reader holds join `active_readers`; writer holds set
  `writer_owner`.
- **Goal 1, the unified verb** (ladder amended per the author's design review):
  `Deferred::commit(opts = {})` —
  1. `pipe.writer_owner == current_task.get()` → the caller already holds the write grant
     (graph node / async write body): apply inline under it; return a pre-settled
     `Task<void>` sentinel (one shared static settled block, no per-call allocation).
     **Sentinel ordering contract**: the pre-settled task provides no happens-before edge
     (it settled before the apply) — observers of the data order through the object's
     pipe; the handle only answers `is_done`/`sync` truthfully.
  2. anyone else → enqueue as an ordinary pipe write (the old `commit_async` body);
     return its task. The opportunistic front-free try-inline arm from the first draft
     is DROPPED: it would silently turn fire-and-forget commit latency into a full batch
     apply on the caller. May be relaxed later, behind evidence.
  `commit_async` is removed from the public API (goal 1 delivered). The destructor's
  in-flight check keys off the **enqueue-and-record seam**: the enqueue path stores the
  new write's block into `Deferred::last_commit_` while still under the pipe's mutex —
  atomic enqueue+record is what makes the external `commit_mutex_` deletable (a plain
  post-enqueue store could lag FIFO order and let the settled-check miss a pending
  write). **Nested-grant contract**: sub-work running under a parent's INHERITED write
  grant is not the holder (`writer_owner` is the parent) — its commit() would enqueue
  behind the very grant it waits out; fatal under `TS_SAFETY_CHECKS`
  (`Access_context::holds_write_epoch`), call commit() from the grant-holding task.
  `Versioned::publish()` unification rides the same mechanism later — out of scope for
  this pass.
- **Non-goals of this pass**: writer retraction (parked, R4 — pipe order is still not
  prerequisite edges; a writers-only chain behind the mutex admission remains possible
  later), the atomic reader fast path (a compatible future optimization: pack
  `{reader_count, writer_active, queue_nonempty}` into one word and CAS-admit readers on
  quiet pipes, falling back to the mutex), standalone public `Pipe`, `publish()`
  unification.

### 0.3 What carries over from the chain work

Landed and kept regardless of pipe core: the `release()` trigger branch + `pipe_count` /
`pipes_entered` block fields, pipes-entered-last, the frozen-at-launch enforcement (+ death
test), the embedded-storage wrappers, the R10 benchmark + baseline, the whole pipe test
suite and TSan stages (implementation-independent), and the OOM-bounded hammer fixtures.
Deleted with the chain: `pipe_tail.cpp`, the `TS_PIPE_TAIL` flag, the tagged tail word,
tenure/era, the reader-run five-field protocol, the hammer test's tail-only diagnostic.

### 0.4 Sequencing and acceptance

1. Goal 1 first on the current mutex core: `writer_owner` + `last_write` + unified
   `commit()` (+ guide/design doc updates — public API change).
2. Internals swap: intrusive link queue + trigger admission + unified multi-object
   (async, then graph). Same seam, same tests.
3. Retire `TS_PIPE_TAIL` + `pipe_tail.cpp` + chain-only fields/tests; CLAUDE.md pipe
   paragraphs updated at merge.

Acceptance per step: full suite 0 failures both configs that exist at that step; Shipping
compiles; the 10× graph-async-hammer loop clean (it must become boring — there is no
lock-free protocol left to race); full WSL TSan campaign clean; R10 bench non-regression
vs the recorded baseline.

## 1. Goal and non-goal

Replace the mutex-guarded reader/writer deque (`detail::Pipe`, `src/guarded.cpp`) with a
lock-free tail-chain where pipe ordering IS dependency edges on the existing block
machinery (`Task_control_block`), and add an always-on grant-ownership field. The payoff
is architectural — inline-safety (nothing dispatches under a lock), feature unlocks
(auto-dispatching `publish`/`commit`, reentrant `access`, writer retraction), and code
simplification (the `Deferred::commit_mutex_` class of external-ordering hazard
dissolves). Perf goal is **non-regression only** (per-object mutex contention is naturally
low; R10).

Non-goal for v1: the auto-dispatch verbs, writer retraction, and the graph/`when_all`
rebase onto `num_locks`. Those are follow-ups this rebase ENABLES; v1 lands the pipe core
+ the `writer_owner` field behind the unchanged pipe seam.

## 2. What we replace

`struct Pipe { std::mutex mutex; std::condition_variable idle; std::deque<Job> jobs; int
active_readers; bool writer_active; ... }` with FIFO admission under the mutex; async jobs
carry a block, graph reservations carry an `on_acquired` callback; everything dispatches
UNDER the mutex (`dispatch` + `submit_admitted`). The seam
(`pipe_enqueue`/`pipe_acquire`/`pipe_release`/`pipe_try_inline`/`wait_until_idle`) is kept
verbatim so the graph, multi-object `async`, `Deferred`/`Versioned` are untouched during
the swap (§9).

## 3. Prior art

### 3.1 UE `FPipe` (the writer chain, verbatim mechanics)

Confirmed against `ue6-main` (Pipe.{h,cpp}, TaskPrivate.{h,cpp}). The whole serial pipe is
three fragments:

- **Push** = `LastTask.exchange(&Task, acq_rel)` — the sole linearization point. `Task` is
  `AddRef`'d BEFORE the exchange (the pipe's own ref on the tail). The returned previous
  tail's ref is TRANSFERRED to the pusher.
- **Edge** = `prev->AddSubsequent(new)` + `new->Prerequisites.Push(prev)` and one extra
  `NumLock` on the piped task (a piped task tolerates `NumLocks <= 1`), released when
  `prev`'s `Close()` unlocks its subsequents. If `AddSubsequent` fails (prev already
  closed), no edge is needed and prev's ref is released.
- **Clear** = on completion, `LastTask.compare_exchange(self -> nullptr)`; a separate
  `TaskCount` tracks emptiness; `WaitUntilEmpty` polls it + an `FEventCount`. The last
  `TaskCount` decrement takes a LOCAL shared-ref to the event first (`// use-after-free
  territory!` — the waiter may free the pipe the instant the count hits zero).

The elegance we inherit for writers: nobody ever does "load the tail, then inc its
refcount" — `exchange` atomically hands the existing ref to the exchanger. No hazard
pointer needed. This holds for our writer chain and for a reader that STARTS a new group;
it does NOT hold for a reader JOINING an open group (§5.2, the one novel hard part).

### 3.2 Queued reader/writer locks and read indicators

Our generalization (serial → reader/writer) is a task-ified queued RW-lock. The literature
decomposition we follow:

- **KSUH** (Krieger, Stumm, Unrau, Hanna) queue RW-lock: a reader enters iff every node
  ahead of it in the queue is a reader; readers form concurrent groups; a writer waits for
  the readers ahead to drain. Reader count / next-writer are implicit in the queue.
- **SNZI** — Scalable NonZero Indicator (Ellen, Lev, Luchangco, Moir, PODC'07): a
  linearizable, non-blocking "is the count > 0" object, cheaper than a full counter,
  with a `arrive`/`depart` interface and a zero-transition signal. This is exactly the
  "active readers" primitive.
- **Scalable Reader-Writer Locks** (Lev, Luchangco, Olszewski, SPAA'09): the canonical
  decomposition = a SNZI **read indicator** + a **writer serialization** lock. Our design
  is this decomposition, expressed as completion edges rather than spin state.

Mapping: **writers** = the FPipe chain (writer serialization, as completion edges).
A **reader group** = a SNZI read indicator (a sentinel block whose `num_locks` above
`execution_flag` counts arrived readers; the zero-transition releases the next writer).
The single-counter indicator is fine for per-object pipes (low fan-in); SNZI's tree
variant is a future scale lever if a hot object ever needs it.

## 4. The model — RETIRED (see §0; kept as the engineering record)

### 4.1 The tail

`Pipe` keeps one `std::atomic<std::uintptr_t> tail` holding a **tagged pointer** — an
aligned `Task_control_block*` with bit 0 as the tag: `0` (the whole word) = idle, else the
pointer with bit 0 `= 0` a writer, `= 1` a reader head/group. Blocks are ≥8-byte aligned,
so bit 0 (bits 0–2) is free. So `tail.exchange(pack(block, tag))` / `compare_exchange` are
single-word atomic ops — the `{block, tag}` notation below is that packed word, nothing
non-atomic. (UE uses the same shape: `std::atomic<FTaskBase*> LastTask`, Pipe.h:136.) Plus
a `TaskCount` (emptiness, §7). The `write_epoch`/`graph_refs`/`debug_name` fields carry over
(harness-gated as today).

### 4.2 Writers = FPipe chain

A writer W: `AddRef(W)`; `prev = tail.exchange({W, WRITER})`; edge W after `prev` (or run
immediately if `prev` is null / already completed), transferring `prev`'s ref into W's
prerequisites. Verbatim §3.1. Serial by construction; retractable by the single
prerequisite edge (R4, parked).

### 4.3 Reader group — the two gates, and two candidate designs

A reader group has two gates: a **start gate** (all its readers run after the previous
group) and an **end gate** (the next writer runs after all its readers). The group's
concurrency is that its readers share the start gate and run at once. The question is how
readers get admitted into a group without racing the group's close/complete. Two designs;
**B is the leading candidate** (it sidesteps the R1 use-after-free that A must solve).

#### 4.3.A — sentinel with arrival-time join (the SNZI approach)

A bodyless sentinel block is the group; readers `arrive`/`depart` on its `num_locks` (a
SNZI read indicator — Ellen/Lev/Luchangco/Moir). `num_locks` = `execution_flag` +
open-lock (dropped when a writer closes the group) + one per arrived reader; it completes
at the zero-transition (closed and drained). Each reader edges on the sentinel's `pred`
(start gate); the next writer edges on the sentinel (end gate). **The problem (R1, §5.2A):
arrival is a load-then-inc that can use-after-free** — a reader loads the tail = sentinel S,
but S can be closed + drained + freed before the reader's `fetch_add`. Fixing it needs
hazard pointers or a packed/split reference count on the tail (DWCAS). That is the
expensive, bug-prone core, and the reason to prefer B.

#### 4.3.B — chain readers as subsequents, head-reader walk (recommended)

**Every** task — reader or writer — chains on the tail by `exchange` + `AddSubsequent`,
exactly like a writer (UE-verbatim; the ref transfers, so there is **no load-then-inc and
no UAF**). Reader concurrency comes from an ad-hoc forward walk:

- The previous writer completes and unlocks its immediate subsequent — the **head** reader.
- The head reader walks the `subsequent` chain forward, unlocking consecutive readers until
  it hits a writer or the chain end. So the whole reader run starts together.
- **Lifetime-safe** because a walked reader is still *locked* (nobody else unlocks it),
  hence alive; the walker reads `Ri.subsequent` *before* unlocking `Ri` (grab-next-then-
  release), so it never touches a node after letting it run. No hazard pointers, no DWCAS.

Reframed: this is the sentinel-as-END-gate, but **populated by the head reader's
ownership-safe walk over locked nodes instead of by concurrent arrival-time joins.** The
sentinel (a group barrier, below) is created single-threaded by the walker, never
concurrently `arrive`d — which is exactly what removes the UAF.

Two subtleties, both resolved with UE-proven primitives (not new hazards):
1. **Walk-vs-late-arrival** — a reader `R4` that exchanged the tail but has not yet linked
   `AddSubsequent(R3, R4)` when the walker reaches `R3` (sees `R3.subsequent == null`).
   Resolution = UE's `Subsequents.Close()`: the walker CAS-closes `R3`'s slot; `R4`'s
   `AddSubsequent` then fails and `R4` starts the next group. "Straggler loses concurrency,
   correctness holds" — same class and rarity as A's backout.
2. **Terminator wait** — the next writer `W` must wait for *all* n readers. If `W` is
   already linked when the walker reaches the end, the walker retargets each reader's unlock
   at `W` and sets `W`'s lock to n. If `W` has not arrived, the walker CAS-installs a small
   **group barrier** block `G` (lock = n; each reader unlocks it) at the chain end, and `W`
   later chains behind `G`. `G` is a sentinel, but built and filled by the single-threaded
   walker over locked (safe) nodes — never concurrently joined.

Cost trade vs A: B has no arrival-time shared-counter contention (readers just exchange the
tail, unavoidable) and no UAF; it pays an O(n) serial unlock walk by the head reader (tail
readers start a few pointer-chases late — negligible for the small groups pipes see) and at
most one barrier alloc per group that a writer follows. B uses only exchange +
`AddSubsequent`/`Close`, all UE-proven. **Recommendation: prototype B.**

### 4.4 Lone-reader elision (R5)

Rides on the §4.3 choice. Under **B** it is trivial and needs no special case: a lone reader
is a group of one — the head reader walks, finds no successor reader, and installs no
barrier until a writer arrives, so the common case (a single read between writes) allocates
only the reader's own block. Under **A**, the first reader's block doubles as the sentinel
(no separate alloc), at the cost of an arrive-vs-representative-settle race on top of the
join UAF. Either way the escape, if the race proves too sharp, is a pooled dedicated
sentinel per group off the free-list (one alloc, simpler race).

## 5. Protocols (the careful core) — RETIRED (see §0; kept as the engineering record)

Pseudocode lines are numbered for reference.

### 5.1 Writer arrival (both designs)

```
W1  AddRef(W)                                  // pipe's tail ref
W2  prev = tail.exchange(pack(W, WRITER))      // linearization point (acq_rel)
W3  if prev == 0:            submit W ready     // idle pipe
W4  elif prev is WRITER:     edge W after prev  // FPipe chain; ref transfers
W5  elif prev is READER:     AddSubsequent(prev, W) / edge W after the reader chain
W6                           // (design A: after the sentinel; design B: after the
W7                           //  group's terminator — see 5.2B terminator handling)
```
If `AddSubsequent` fails (prev already completed), no edge is needed and W runs immediately
(UE §3.1). The push is bracketed in `AddRef(W)`/`Release(W)` (R9, §9): prev can complete,
run, and free W before this returns.

### 5.2A Reader arrival — sentinel join (NOT recommended; shown for the trade)

```
A1  retry:
A2    t = tail.load(acquire)
A3    if t is READER sentinel S:
A4        S.num_locks.fetch_add(1)             // SNZI arrive -- *** load-then-inc UAF ***
A5                                             // S may be freed between A2 and A4 (R1)
A6        TS_PIPE_RACE_DELAY(join_after_add)
A7        if tail.load(acquire) == pack(S, READER):
A8            edge R after S.pred; return       // committed
A9        else: S.num_locks.fetch_sub(1); goto retry   // writer closed S -> back out
A10   else:   // WRITER or idle -> start a new group
A11       R.num_locks = execution_flag + open + 1; AddRef(R)
A12       if tail.compare_exchange(t -> pack(R, READER)):
A13           edge R after t; return            // t's ref transfers to R's prereq
A14       else: goto retry
```
Line A4 is the R1 UAF (§4.3.A): it requires a hazard pointer or a packed-count tail to be
safe. This is why B is preferred.

### 5.2B Reader arrival — chain + head-walk, realized as `Pipe_link` (IMPLEMENTED)

The implemented form. Every pipe-accessing task is an **ordinary dynamic task** whose
readiness includes its pipe turns as `num_locks` prerequisites; what physically sits in a
pipe's line is a small embedded **`Pipe_link`** per (task, object) — because a line entry
needs its own successor slot, and a block has one. All pipe storage is sized at creation:
no allocation on any pipe hot path.

#### 5.2B.0 The `Pipe_link` (`include/ts/detail/pipe_link.h`)

```cpp
enum class Link_role : std::uint8_t { serial, head, member };   // set by the walk; serial default

struct alignas(8) Pipe_link
{
    std::atomic<std::uintptr_t> next{ 0 };   // this line's successor: open / closed / group_end / Pipe_link*
    Task_control_block* owner;               // the task this entry admits
    Pipe* pipe;                              // the line this entry sits in
    Task_control_block* prev_owner;          // custody chain: owned ref on the predecessor (see 5.2B.5)
    Pipe_link* group_target;                 // head: the group's LAST link; member: the group's head
    std::atomic<std::uint32_t> gate{ 0 };    // head only: group countdown (members + head)
    std::uint8_t index;                      // position in owner's link array (drives the cascade)
    Access mode;                             // reader / writer in this line
    bool is_head;                            // reader that walks at its turn (set at push, 5.2B.1)
    Link_role role;                          // interpretation of gate/group_target
};
```

`next` is a tagged word: `0` = open (no successor yet), `1` = closed (retired — a late
pusher runs immediately), `2` = group_end (a closed reader group ends here — a late pusher
links on and HEADS a new group), else an aligned `Pipe_link*`. The block gains
`Pipe_link* pipe_links` + `std::uint8_t pipe_count` + `std::uint8_t pipes_entered`
(replacing the earlier per-block `pipe_next`/`pipe_terminator`). Links live in the one
allocation that already exists: a `Piped_executable<Body, R, N>` wrapper for `async`
(N = pack size, compile-time), the graph's per-node slab for nodes (allocated at
`compile()`, re-armed per run).

**Compaction pass (review after the first running version):** check every field is
actually required and how the layout can be compacted — the `owner` redundancy across a
task's links, `gate`/`group_target` width (a union candidate), `index`/`mode`/flags
packing, and the per-link `pipe` pointer vs deriving it. Deliberately NOT done up front;
correctness and readability first.

#### 5.2B.1 Turn = prerequisite (the `pipe_count` trigger)

`num_locks` at launch = 1 (launch) + O (ordinary prereqs) + P (pipes); the block stores
`pipe_count = P`. `release()` grows one branch:

```cpp
if (now == 0)                       dispatch_ready(blk, gen);   // all locks (incl. turns) met
else if (now == execution_flag)     blk->complete();            // post-execution: nested done
else if (now == blk->pipe_count)    pipe_enter_first(blk);      // only pipe locks left -> enter line 0
```

`fetch_sub` return values are unique, so exactly one release crosses `pipe_count` — no
claim, no flag; a `pipe_count == 0` task never reaches the third branch (`now > 0` there).
Note: the review-chat compression that folded the `now == 0` dispatch into the
`now == pipe_count` branch was wrong for pipe tasks (a task counting P turns down to 0
would never dispatch); the implemented three-branch form above is the corrected version.
Callers with no ordinary prerequisites (`async`, graph nodes at data-ready) skip the
trigger and call `pipe_enter_first` directly with `num_locks` pre-set to P.

#### 5.2B.2 The cascade (`link_turn`)

A link's turn arrives (its line predecessor retired, or the line was free at push):

```cpp
void link_turn(Pipe_link& l)
{
    if (l.mode == Access::read_write)  epoch_open(*l.pipe);    // TS_SAFETY_CHECKS
    else if (l.is_head)                walk_group(l);          // form the reader group (5.2B.4)
    Task_ptr keep(l.owner);
    if (l.index + 1 < l.owner->pipe_count)
        pipe_enter_link(l.owner->pipe_links[l.index + 1]);     // sequential canonical cascade
    Task_control_block::release(keep);                         // the pipe-turn prerequisite
}
```

Holding turn k while waiting only for later-canonical turns — no cycle can form (ordered
acquisition), and §5.5 guarantees no ordinary prerequisite is ever waited on while a turn
is held. Nested `link_turn` depth via an idle next line is bounded by `pipe_count`.

#### 5.2B.3 Head identification (unchanged, now per-link)

A reader link is the group **head** iff the predecessor it exchanged out of the tail is a
writer or idle (`0`); it is a member iff the predecessor is a reader. Purely local, from
the single exchange result (the tail word carries the reader bit). One addition: a pusher
whose link-CAS lands on `group_end` (a closed group's end) KNOWS its group predecessor is
a finished group and upgrades itself to head — stragglers form a fresh group instead of
serializing.

#### 5.2B.4 The walk — allocation-free

No barrier block, no member vector. The group's state lives on the **head's link**:

- **Pass 1** follows `next` over consecutive reader links, counting. Ends at: a writer's
  link (stays chained — it needs no gate; its turn is simply not dispatched until the
  group retires), or open chain end — then CAS `open -> group_end` closes the group
  (a lost CAS means a late reader linked; re-read and absorb). A lone reader (`n == 1`
  with open end) returns untouched — serial advance, zero group cost (R5).
- The head's link is armed: `role = head`, `gate = n` (members + head), `group_target =
  last`. Each member: `role = member`, `group_target = &head`, plus an owned ref on the
  head's owner (keeps the gate alive however member settle order interleaves).
- **Pass 2** re-traverses head→last dispatching each member's `link_turn` (their owners'
  turn prerequisites release; owners run when fully ready). Grab-next before dispatch;
  the gate cannot reach 0 during pass 2 (the head's own departure is pending), so the
  traversed links are stable.

#### 5.2B.5 Advance — my settle retires my lines

Settle (after nested tasks — the §8 invariant holds structurally) advances every entered
link; there is no `pipe_release` and no externally-completed entity anywhere:

```cpp
void advance_pipe_links(Task_control_block* blk)      // called by the pipe task's settle hook
{
    for (std::uint8_t i = 0; i < blk->pipes_entered; ++i)
        link_advance(blk->pipe_links[i]);
}

void link_advance(Pipe_link& l)                        // exactly once per entered link
{
    if (l.mode == Access::read_write) epoch_close(*l.pipe);
    switch (l.role)
    {
    case Link_role::serial:  chain_retire(l); break;
    case Link_role::head:    group_depart(l, l); break;              // my own gate
    case Link_role::member:  group_depart(*l.group_target, l); break; // the head's gate
    }
    release prev_owner custody ref;                    // 5.2B.6
    l.pipe->task_count.fetch_sub(1, release);          // LAST pipe touch (drain discipline, §7)
}
```

`group_depart(head, departing)` decrements `head.gate`; the last one out calls
`chain_retire(*head.group_target)` — retiring the group at its LAST link. `chain_retire`
is the serial hand-off: exchange `next -> closed` (a waiting successor's turn fires;
`group_end`/open mean none), then CAS the tail from this link to idle (dropping the line's
ref) if still ours. Intermediate group links are never touched again.

#### 5.2B.6 Lifetime — the custody chain (corrects the reviewed "dec after CAS")

The review chat proposed releasing the exchanged-out predecessor's ref immediately after
the link-CAS resolves. That is a use-after-free: a mid-chain entry whose successor has
already pushed (and dec'd it) is kept alive by nothing but the user's droppable handle
while it waits for its turn — and the walk touches member links whose owners may hold no
other refs. The implemented scheme is the stage-3a custody chain, per link:

- push: `intrusive_inc(l.owner)` — the line's ref on this entry;
- the tail exchange TRANSFERS the predecessor's line ref to the pusher, stored as
  `l.prev_owner` and released at `l`'s advance — so every queued entry is kept alive by
  its successor, anchored by the tail's ref on the newest entry;
- `chain_retire`'s successful tail-clear CAS releases the entry's own line ref; a failed
  CAS means a successor adopted it.
- group members additionally hold a walk-granted ref on the head's owner (dropped at
  depart), because the custody chain releases in settle order, not chain order, and the
  gate lives on the head.

#### 5.2B.7 Inline safety (unchanged argument)

Pass 1 dispatches nothing; pass 2 and `chain_retire` dispatch turns, which only
`release()` — the body dispatch at count 0 goes through `dispatch_ready`, whose bounded
per-thread trampoline covers inline tasks, and worker-less submit-executes ride the serial
trampoline. The head's stack depth is O(1) in group size; a cascade nests at most
`pipe_count` frames.

The TSan-critical race points: walk-close vs late-join (`open -> group_end` CAS), the
straggler's `group_end -> me` CAS vs group retirement (`group_end -> closed`), and the
drain last-decrement (§7). `TS_PIPE_RACE_DELAY` hooks sit at the walk close and the push
link-CAS.

#### 5.2B.8 Link reuse across graph runs — the tenure/era ABA (implemented fix)

The graph re-arms its links per run; an out-of-band async pusher can straddle the
re-arm: it exchanges the tail in tenure g (getting the node's link as predecessor),
stalls, and completes its slot-CAS against the RE-OPENED slot of tenure g+1 — linking
into the wrong tenure. The new node meanwhile chained behind the pusher's stale tail
position, closing a wait cycle: a reliable native deadlock (~60% per suite run in the
`graph async hammer` test). One-shot async links cannot hit this (no reuse).

Two-layer fix, both keyed on a per-link `tenure` counter bumped at re-arm:

1. **Era bits in the tail word** — the pusher's staleness is decided by ONE atomic: the
   tail exchange. `Pipe_link` is alignas(64) (also kills false sharing between slab
   neighbours), freeing 6 low bits: bit 0 = reader tag, bits 1–5 = the link's tenure
   mod 32 at push time. After the exchange the pusher compares the word's era against
   the predecessor's current tenure: a mismatch means the predecessor tenure it queued
   behind has provably retired — run immediately (the custody adoption stays balanced:
   the old tenure's line ref transferred at the exchange and is released at the
   pusher's advance, exactly as the retired-predecessor branch).
2. **Tenure in the `next` open word** — `open(tenure) = (tenure << 2) | 1`, so a slot
   CAS from a stale read fails against the re-armed word regardless of timing; any
   unexpected slot value (closed, other-tenure open, foreign link) resolves to
   run-now when the era check said stale, and remains the invariant fatal when it
   said current.

Residual (documented, accepted): the era is mod 32, so a pusher preempted across an
exact multiple of 32 re-arms of the same link, waking inside the reopened window, could
still mis-link — that requires 32 complete graph runs (with their `sync()` barriers)
inside one preempted push. The full-width tenure in the open word narrows it further to
that pusher also CASing a stale-read word. Comparable to accepted non-wrap assumptions
elsewhere; revisit only if a 128-bit tail (DWCAS) ever becomes warranted.

### 5.5 Pipes are entered last (invariant)

A task enters a pipe's line only when its pipe turns are its only unmet locks: the
`pipe_count` trigger fires after every ordinary prerequisite resolved, and the
prerequisite set is frozen at launch (enforced — `Task_builder::after()` post-launch and
`add_prerequisite` on an unlocked/running successor are fatal under `TS_SAFETY_CHECKS`).
Consequences: an entry in a line waits only on line turns (never an ordinary edge — the
deadlock the alternative allows: hold a turn, wait on a prerequisite that is queued behind
that very turn), and pipe FIFO is readiness-ordered for prerequisite-carrying tasks (UE's
documented caveat; today's pipe jobs are all prerequisite-free, so their FIFO stays
call-ordered).

### 5.3 Completion (summary)

A task's settle advances every entered link (5.2B.5): serial links hand the line to their
successor's turn; group members depart the head's gate, the last one out retiring the
group at its last link. The tail-clear CAS (this link -> idle) drops the line's ref when
no successor superseded it. `task_count` decrements once per link, as the advance's last
pipe touch, and drives `wait_until_idle` (§7).

### 5.4 Why this is deadlock-free and correctly ordered

Ordering is the chain on the tail (each entry after its predecessor); within a reader
group, readers' turns fire together at the head's turn and the next entry waits on the
group gate. FIFO-by-arrival is the tail exchange order per line; §5.5 confines a queued
entry's waits to later-canonical turns, and the sequential cascade acquires lines in one
global (address) order — the classic ordered-acquisition argument, now covering graph
nodes and multi-object `async` uniformly because they ARE the same mechanism. No lock is
held across dispatch; inline and worker-less chains ride the existing bounded trampolines.

## 6. R2 — reservations dissolved: multi-object `async` and graph nodes ride the links

The reservation concept (`pipe_acquire` hold + explicit `pipe_release`) is DELETED, not
reimplemented. Both former users become ordinary tasks with N links:

- **Multi-object `ts::async(fn, objs...)`**: the builder dedups (write-wins) and sorts the
  (pipe, mode) set by pipe address at build time — the canonical order — into a
  `Piped_executable<Body, R, N>` (N = pack size; one allocation as before, now including
  the links). `num_locks = n`, `pipe_enter_first`, done: turns cascade, the body runs
  under the full `Access_context` when the last turn releases, settle advances all links.
  `Multi_async_state`, `multi_acquire`, and the per-call `std::map` are deleted.
- **Graph nodes**: per-node links in a graph-owned slab, bound at `compile()` (the node's
  `pipe_indices` are ascending over the address-sorted `distinct_pipes_`, so link order is
  canonical), re-armed per run — re-runs stay allocation-free. Data prerequisites keep
  `remaining_deps` (v1 scope cut); `on_data_ready` seeds `num_locks = pipe_count` and
  calls `pipe_enter_first` (or dispatches directly for object-free nodes). The node block's
  settle hook (`graph_node_completed`) advances the links first, then runs the graph
  bookkeeping. Deleted: `acquire_next`'s pipe path, `pipe_acquire`/`pipe_release`,
  `preheld`/`handoff_target`/`mark_preheld`, `Pipe::held`.
- **The node→node handoff is now just FIFO**: a successor node's link is queued right
  behind the predecessor's in the shared line; the predecessor's retire dispatches its
  turn directly. No `preheld` mask, no `write_epoch += 2` elision — every write window
  opens at the writer link's turn and closes at its advance (parity preserved).
- **Reader-node ∥ async-reader overlap** is automatic: both are reader links, grouped by
  the walk like any others. **Gap-freeing** is automatic: a node's links exist in a line
  only from data-ready to settle.

The settle-must-advance-links contract concerns only library-internal code (users cannot
create pipe tasks except via the two factories); it is stated in comments at both creation
sites (the `async` factory and `graph_node_completed`) rather than tracked as a risk.

### 6.1 Memory / perf accounting

| | before (mutex pipe) | after (links) |
|---|---|---|
| single-object async | block + mutex/deque `Job` | block + 1 embedded link (0 extra allocs) |
| multi-object async (n) | block + `Multi_async_state` + map + n deque Jobs + closures | block + n embedded links (**one** alloc total) |
| graph node per run | 0–n reservation closures when contended | **0** (links re-armed in slab) |
| reader group | mutex bookkeeping | **0** (no barrier, no member vector) |
| ops per access (uncontended) | 2 mutex lock/unlock + deque ops | a handful of relaxed/acq-rel atomics |

One deliberate cost: dispatch goes through `release`/`dispatch_ready` (one extra
`fetch_sub` vs a direct submit), buying a single uniform path — priority, inline,
cancellation, worker-less all ride the standard machinery. Priced by `bench_pipe_contention`
(§13.1).

### 6.2 Known compromises

1. **Sequential cascade latency.** Line k+1 is entered only at turn k, so: (a) waits SUM
   instead of overlapping — the task waits out P1's line, then P2's from that moment;
   parallel registration would wait max, not sum; (b) late FIFO positions — the position
   in line k+1 reflects turn-k time, not request time, so global request order is not
   respected across lines (per-line FIFO holds; no starvation); (c) held-line idling —
   the already-held P1 turn blocks P1's line for the whole downstream wait (inherent to
   exclusive multi-holds, not to the cascade). Mitigating fact: these are exactly today's
   `multi_acquire` semantics, so v1 is behavior-neutral.

   **Parallel registration with per-pipe push-locks (designed follow-up, evidence-gated).**
   At the `pipe_count` trigger, instead of the sequential cascade: acquire each involved
   pipe's push-lock in canonical (address) order, push all n links, release all.
   Consistency argument: two tasks sharing >= 2 pipes contend on those pipes' push-locks,
   so their registration transactions serialize and in-line orders can never invert (the
   deadlock mechanism); ordered lock acquisition makes the locks themselves cycle-free.
   Single-object pushes never take the lock (one shared line cannot form an inversion), so
   the hot path stays lock-free. Fixes: waits overlap (latency = max over lines, not sum)
   and FIFO positions reflect request time. Does NOT fix held-line idling (holding k turns
   while awaiting the rest is inherent to all-or-nothing acquisition; only shortened).
   Price: n short spin-lock pairs per multi-access, one lock byte per `Pipe`, and a second
   deadlock-freedom argument (serialized transactions instead of ordered acquisition). The
   upgrade is localized to the registration step (`pipe_enter_first` + the cascade in
   `link_turn`); links/advance/walk/trigger untouched — deferring costs nothing
   architecturally. Gate: a contended multi-object async latency fixture showing the
   sequential sum-of-waits actually biting.
2. **Tagged words** (`open`/`closed`/`group_end`/pointer in `next`; the reader bit in
   `tail`) — concentrated behind named constants and helpers, used nowhere else.
3. **The graph keeps `remaining_deps`** — two counting systems until the full
   graph-on-`num_locks` rebase (which this design makes mechanical).
4. **Residual straggler serialization** — a reader linking onto a still-open run the walk
   just passed is absorbed by the re-read loop or serialized (never lost); the `group_end`
   case upgrades to a new head.
5. **Cancelled tasks still take their turns** (today's semantics); the skip-lines
   optimization interacts with `pipes_entered` accounting and is deferred.
6. **Priority never jumps a line** — see the pipe-rebase addendum under TODO D6 (master):
   the pipe instantiation of priority propagation gets re-expressed on the chain when D6
   lands.
7. **Inline nodes on contended objects** now run on whichever thread releases the last
   turn (previously deferred to the queue when any acquire was contended) — still a
   documented-nondeterministic thread; noted, not user-visible as a contract change.

## 7. R6 — `wait_until_idle`

An atomic `TaskCount` tracks emptiness (incremented on every push, decremented at every
completion), SEPARATE from the tail (`LastTask` is the newest task; `TaskCount` counts all
outstanding). The subtle part is the destruction race: the waiter can free the whole pipe
the instant the count hits zero, so the last decrementer must not touch the pipe after the
decrement that could unblock the waiter.

UE's fix: `TaskCount` + a `TSharedRef<FEventCount>` — a HEAP, SHARED drain event that
outlives the pipe; the last decrementer takes a local copy of the shared ref BEFORE the
`fetch_sub` (`// use-after-free territory!`) and touches only that afterward. (The
`ue6-main` clone here is squashed to one commit — no history/messages — so the fix's
original context is not recoverable locally; the public GitHub blame can be checked on
request. The event exists because UE's `WaitUntilEmpty` is a general API that must not burn
a core.)

**Simpler for us.** Our only waiter is `~Guarded` (teardown, not hot), so we can drop the
event entirely: `~Guarded` **spin-yields on `TaskCount == 0`**, no event object, no shared
ref, no `// UAF` dance — the last job's `fetch_sub` to zero is its final pipe touch, and the
waiter frees the pipe only after observing zero, so there is nothing to free out from under
the signaler. Add an eventcount only if a public, non-teardown `Pipe::wait_until_idle` is
later exposed (TODO 1.7). Also verify during the rewrite: the CURRENT CV-under-mutex
`wait_until_idle` likely has the same latent destroy-during-`notify_all` race UE fixed
(waiter wakes, `~Guarded` destroys the CV/mutex while the signaler is still inside
`notify_all`) — the count-based drain cures it.

## 8. R3 — grant-ownership `writer_owner`

An always-on `Pipe::writer_owner` (a `Task_control_block*`, or the block identity) naming
the writer that CURRENTLY holds the write grant — NOT the tail (the tail may be a queued
future writer that has not started). Set when a write representative's body/hold BEGINS
(grant acquired), cleared at its completion/release, and TRANSFERRED at the graph write
handoff (the successor becomes the owner without a release/re-acquire — §6a bumps
`write_epoch += 2`; `writer_owner` moves in the same step). Compared against `current_task`
(an existing always-on TLS):

- `writer_owner == current_task` → I already hold the grant → run under it (reentrant).
- head free → inline-acquire, run, chain the tail.
- writer active, owner ≠ me → someone else holds it → async.

This is the always-on, behavior-relevant half of the "hybrid harness" (the diagnostic half
— `write_epoch`, `graph_refs`, full `Access_context` — stays `TS_SAFETY_CHECKS`-gated). It
must be correct on every path: graph handoff (transfer), inline `access` (owner = the
inline block for its duration), multi-object holds (per-pipe owner), worker-less/retraction
(owner = whatever block runs, on whatever thread). Tests F1–F4 (Wave 2). It UNLOCKS
(follow-up PRs) auto-dispatching `publish()`/`commit()`, reentrant `access`, and the
scheduler avoiding the same-object blocking-sync deadlock by running-inline when the owner
is the waiter.

## 9. R9 — push-UAF bracket

Entering a line lets the predecessor retire, dispatch the new entry's turn, run and settle
its owner, and FREE it before the push returns (UE: `// Use-after-free territory`). The
push path holds its own ref on the entered owner across the whole operation (the caller's
handle or the line ref taken before the exchange), and the custody chain (5.2B.6) keeps
every queued predecessor alive. Test G3.

## 10. The seam (narrowed by design)

Surviving: `Pipe::wait_until_idle()`, `pipe_epoch(pipe)`, `pipe_try_inline` (single-object
`access` fast path). DELETED flag-on: `pipe_enqueue` as a generic entry (the `async`
factory binds links and calls `pipe_enter_first`), and `pipe_acquire`/`pipe_release`
entirely (§6 — reservations dissolved; the graph talks to the pipe only through links).
The mutex-pipe versions remain under `!TS_PIPE_TAIL` until the default flips.
`Deferred`/`Versioned` are untouched (they use `Guarded::async`/`access`); the
tail-as-last-write handle (§11) stays a follow-up.

## 11. R8 — Deferred/Versioned ordering (follow-up)

The atomic tail IS the last-job handle. Expose the WRITE-push's returned handle (captured
at `commit_async` time, not re-read from the tail later — the tail may be a reader sentinel
by then), so `Deferred` records its last commit unambiguously and the `commit_mutex_`
load+store race is dissolved. `Versioned`'s phase-3 resync becomes a read representative on
the tail (already is, effectively). Internals-only; the public API/contracts do not change.

## 12. R4 — writer retraction (parked)

Writer retraction "falls out" (a writer chains via one prerequisite edge, so `retract`
walks it), but is UNSAFE when the retractor already holds a conflicting grant on the same
pipe (it would run a second writer under the held grant — the exact blocking-sync deadlock,
now silently executed). Gate: retract a pipe writer only when `writer_owner != current_task`
and no reader-hold by the retractor. Since v1 ships `writer_owner`, the guard is available —
but writer retraction itself is DEFERRED (undecided; simpler to keep pipe blocks
non-retractable for now, per the R4 decision). Reader retraction does NOT map (a reader has
no single ordering edge — N predecessors via the group) and stays a separate open question.
Revisit both when a real need appears.

## 13. Sequencing and risks

1. Consolidate design (this doc) + baseline the reader-heavy contention benchmark (R10 —
   prove non-regression; the gate override is architectural, so baseline, don't gate).
   **Baseline recorded** (`bench_pipe_contention`, 22 hw threads all on one pipe —
   machine-specific, track the ratio not the absolute): 100% rd 1.25 M/s (798 ns/op),
   90% rd 0.93 M/s (1069 ns/op), 50% rd 0.85 M/s (1179 ns/op). The single-mutex ceiling
   under 22 producers is what the tail must hold or beat; the uncontended `ts_write`
   (1062 ns/op) / `ts_read` (1035 ns/op) numbers are the not-to-regress floor.
2. **Writer chain first** — implement the tail + writer arrival/clear + `wait_until_idle`,
   validate with WRITE-ONLY workloads (the UE-exact core; de-risks ref-transfer/lifetime
   before reader-join). TSan.
3. **Reader group** — design B (chain + head-walk, §4.3.B/§5.2B): reader chaining, the
   head-walk, the group barrier + terminator, lone-reader elision (§4.4). The hard part;
   race-delay hooks + the C/D tests + `stress_pipe_rw` under TSan. (Gated on the Q1
   decision.)
4. **Reservations** (§6a) behind the seam; the E-tests + `stress_pipe_reservation`.
5. **`writer_owner`** (§8) + F-tests.
6. Shipping compile (behavior half stays, diagnostic half compiles out) + full TSan +
   ASan/`--stress` on Windows.
7. Update CLAUDE.md scheduler/Guarded paragraphs; hand back to the review session.

Risks: the reader-group races (§5.2B — walk-vs-late-arrival, terminator install) are the
crux; race-dense, Windows has no TSan (WSL only), so the hooks + high-iteration stress carry
the load. The head-walk's O(n) is a latency skew for large groups (rare). The reservation
path (§6) is a second consumer of the tail and most of the risk after the reader protocol.

## 14. Open questions — RESOLVED (2026-08)

1. §4.3 reader design: **B chosen** (author's chain + head-walk), realized as `Pipe_link`
   (§5.2B) after the further unification round: pipe turns as `num_locks` prerequisites
   (author's counter scheme), links embedded (author's no-dynamic-alloc requirement).
2. §4.4 elision: free under the link walk (`n == 1` returns untouched).
3. §6: superseded — not (a) behind-the-seam, but the full dissolution (§6), because the
   link model made (b)'s simplification the EASIER path, not the harder one.

Sources: [SNZI (PODC'07)](https://dl.acm.org/doi/10.1145/1281100.1281106) ·
[Scalable Reader-Writer Locks (SPAA'09)](https://people.csail.mit.edu/mareko/spaa09-scalablerwlocks.pdf) ·
UE `FPipe` (`ue6-main` Engine/Source/Runtime/Core/{Public,Private}/Tasks/Pipe.{h,cpp}).
