# Pipe rebase — design of record (pipes-as-edges, reader/writer)

Consolidates TODO 1.14 (+ its two addenda), the task-internals §6 retraction addendum,
and the `FPipe` note in task-systems-comparison.md into one design. Companion:
`docs/pipe-rebase-tests.md` (the test plan). Status: design, 2026-07-31, branch
`pipe-rebase`. Read this before touching `src/guarded.cpp` / the pipe.

The tests are labelled R1..R10 to match the concern labels here.

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

## 4. The model

### 4.1 The tail

`Pipe` keeps one atomic **tagged pointer** `tail` (low bits tag an aligned
`Task_control_block*`): `null` (idle), a **writer block** (`WRITER`), or a **reader-group
sentinel** (`READER_OPEN`). Plus a `TaskCount` (emptiness, §7) and a refcounted drain
event (§7). The `write_epoch`/`graph_refs`/`debug_name` fields carry over (harness-gated as
today).

### 4.2 Writers = FPipe chain

A writer W: `AddRef(W)`; `prev = tail.exchange({W, WRITER})`; edge W after `prev` (or run
immediately if `prev` is null / already completed), transferring `prev`'s ref into W's
prerequisites. Verbatim §3.1. Serial by construction; retractable by the single
prerequisite edge (R4, parked).

### 4.3 Reader group = SNZI sentinel carrying `pred`

A reader group is represented by a block acting as a SNZI indicator. It carries `pred` =
the previous group's block (a writer or null). Two gates:

- **Start gate** (readers must run after the previous group): each reader takes a
  prerequisite edge on `pred`. A reader that joins after `pred` already completed edges on
  a closed block → `AddSubsequent` fails → the reader is immediately ready (correct: the
  group is already running).
- **End gate** (the next writer must run after all readers): the next writer edges on the
  sentinel; the sentinel completes at the SNZI zero-transition (all readers departed AND
  the group is closed).

`num_locks` on the sentinel: `execution_flag` (it is "executing" — bodyless) + an
**open-lock** (dropped when the group is closed by the next writer's tail exchange) +
one per arrived reader. It completes when the count returns to `execution_flag` — i.e.
closed and drained. This is the existing nested-task counter (§4/§7 of task-internals),
used verbatim; the open-lock is the `+1` self-lock the execution_flag machinery already
provides.

### 4.4 Lone-reader elision (R5)

We do NOT allocate a separate sentinel for a lone reader. The **first reader's own block
doubles as the group representative** (every block has `num_locks`): it runs its read body
AND serves as the SNZI indicator. A second reader `arrive`s on it (a nested lock) and edges
on its `pred`; the representative completes when its body is done AND all joined readers
have departed — exactly nested-task semantics. So the common case (a single read between
writes) allocates only the reader's own block, no sentinel. Promotion to a "real" sentinel
is unnecessary: the representative IS the sentinel. The cost moves to the join protocol
(§5.2) rather than an allocation. (If this over-complicates the arrive-vs-representative-
settle race in practice, fall back to a pooled dedicated sentinel per group and accept the
alloc — flagged as the escape.)

## 5. Protocols (the careful core)

### 5.1 Writer arrival

```
AddRef(W)                                   // pipe's tail ref
prev = tail.exchange({W, WRITER})           // linearization point (acq_rel)
if prev == null:      submit W ready        // idle pipe
elif prev is WRITER:  edge W after prev      // FPipe chain; ref transfers
elif prev is READER_OPEN sentinel S:
     edge W after S                          // W waits for the group's end gate
     drop S's open-lock                      // CLOSE the group (may complete S now)
```
Closing = dropping the open-lock, sequenced AFTER the exchange that made the tail no longer
point at S (so new readers see a changed tail and back out, §5.2).

### 5.2 Reader arrival (join / new group / backout) — the novel race

```
retry:
  t = tail.load(acquire)
  if t is READER_OPEN sentinel S:
      S.num_locks.fetch_add(1)               // SNZI arrive (tentative join)
      TS_PIPE_RACE_DELAY(join_after_add)     // widen the window under test builds
      if tail.load(acquire) == {S, READER_OPEN}:
          edge R after S.pred                 // committed; start gate = pred
          return
      else:                                   // a writer closed S: back out
          S.num_locks.fetch_sub(1)            // SNZI depart (may complete S)
          goto retry
  else:   // t is WRITER or null -> start a new group
      make representative R the sentinel (pred = t; num_locks = execution_flag + open + 1)
      AddRef(R)
      if tail.compare_exchange(t -> {R, READER_OPEN}):
          edge R after t (its pred)            // ref of t transfers to R's prereq
          return
      else: goto retry
```

Only the JOIN path (tentative-arrive + re-validate + backout) is novel — UE never joins,
it always exchanges. This is the SNZI `arrive` guarded by the tail: a reader commits to a
group only if the tail still names it open; a writer's close (a tail exchange) is observed
by the re-validate, forcing a backout. Three race points, all TSan-critical:

1. **join-vs-close** — the `fetch_add` / re-validate / backout above. If the writer's
   exchange lands between our add and re-validate, we back out and re-form after the
   writer (correct ordering). The backout's `fetch_sub` may be the one that completes S
   (if we were the last arrived and the group is now closed) — completion must be
   idempotent (it is: `settle` under the block mutex).
2. **join-vs-representative-settle** (R5 elision) — a reader arriving on the lone
   representative R after R's read body returned but before R settled. The open-lock keeps
   R from settling while the group is open, and the tail re-validate rejects a join once R
   is closed; the surviving window is the same shape as nested-task registration racing
   body-completion, which the execution_flag self-lock already covers.
3. **last-decrement / drain** (R6) — §7.

The race-delay hooks (`TS_PIPE_RACE_DELAY`, no-op in normal builds) at `join_after_add`
and `close_before_drop` widen 1 and 2 for TSan (pipe-rebase-tests §3.3).

### 5.3 Completion

- A reader (or the representative's body) finishing: `num_locks.fetch_sub(1)` (SNZI
  depart). If it returns to `execution_flag` (closed and drained), the sentinel settles →
  releases the next writer's edge → the writer runs.
- A writer finishing: `settle` → releases its single subsequent (the next group's
  representative or writer), UE-style. Then `tail.compare_exchange(self -> null)` if it is
  still the tail (pipe now idle); else the next pusher already took the tail (ref
  transferred).
- `TaskCount` decrement (every representative, at its completion) drives `wait_until_idle`
  (§7).

### 5.4 Why this is deadlock-free and correctly ordered

Ordering is the group chain on the tail (each group edges on its predecessor); within a
reader group, readers share the `pred` start gate and run concurrently; the next writer
waits on the group's SNZI zero-transition. FIFO-by-arrival is the tail exchange order
(UE's guarantee), preserved for groups. No lock is held across dispatch — a completing
block releases edges outside any pipe lock, so a worker-less inline chain rides the bounded
trampoline (R7), and `access` inline never stacks under a lock.

## 6. R2 — reservations / graph, and a simplification

The graph and multi-object `async` use `pipe_acquire` (held, mode-aware, sync-or-deferred)
+ `pipe_release` (explicit) + the direct node→node handoff (`preheld`, `write_epoch += 2`).
On the tail model a **reservation is a group representative whose completion is deferred to
an explicit `pipe_release`** instead of body-return — otherwise identical. So:

- `pipe_acquire(mode, on_acquired)` = attach a held representative (a reader joins/forms a
  reader group; a writer chains). Return true if it is immediately at the head (idle-
  compatible tail), else false and fire `on_acquired` when its start gate opens.
- `pipe_release(mode)` = settle the held representative (SNZI depart for a reader; writer
  settle), advancing the chain.

**Two implementation paths:**

- **(a) Minimal (v1).** Keep the seam signatures and the graph's explicit
  acquire/handoff/preheld logic exactly; reimplement `pipe_acquire`/`pipe_release` as
  held representatives on the tail. The handoff stays explicit (skip release + re-acquire,
  bump `write_epoch += 2`). The graph is UNTOUCHED behind the seam — the handoff mandate.

- **(b) Ambitious (follow-up this rebase ENABLES).** The explicit per-node
  acquire/release/handoff/preheld machinery largely DISSOLVES: a serial X-chain of writer
  nodes A→B→C naturally forms a writer chain on X's tail (each edges on the previous), so
  the "handoff" (B skips re-acquiring X because it is already the head after A) is the
  natural edge chain — no `preheld` mask. Gap-freeing (async slips in when a node releases)
  is automatic (the tail is free between representatives). Reader-node ∥ async-reader
  overlap is automatic (both are reader-representatives on the tail). This is the
  graph/`when_all` rebase onto `num_locks` already on the roadmap (task-internals "open
  items"); folding it in couples the graph scheduling to the pipe tail (canonical-order
  push for multi-object deadlock-freedom must be preserved). Deliberately DEFERRED — v1
  proves the core behind the seam; (b) is a separate PR.

Recommendation: v1 = (a). Note (b) as the marquee simplification the rebase unlocks; the
E-tests (writer handoff, reader-node ∥ async, node ∥ async no-race) are the regression
guard for either path.

## 7. R6 — `wait_until_idle`

Copy UE's `TaskCount` + eventcount: increment on every representative push, decrement at
its completion; `~Guarded`'s wait polls `TaskCount == 0` armed on an eventcount, with the
double-check to avoid a lost wakeup. The drain event is a REFCOUNTED object (a
`Ref_ptr`/`shared_ptr`) so a waiter may free the pipe the instant the count hits zero: the
last decrementer takes a local ref to the event BEFORE the decrement (`// use-after-free
territory`), and touches only that ref afterward. This is the R6 hazard, verbatim from UE.

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

Registering a new block as an edge on the previous tail lets the previous block complete,
release the new block, run it, and FREE it before the enqueue returns (UE:
`// Use-after-free territory, do not touch any of the task's properties here`). Our
`pipe_enqueue` (and the acquire/reader-join paths) must bracket the push in an `AddRef` /
`Release` on the pushed block, exactly as UE's `TryUnlock` does. Test G3.

## 10. The seam

Preserved verbatim so nothing else in the library changes during the core swap:
`pipe_enqueue(scheduler, pipe, mode, block, priority)`,
`pipe_acquire(scheduler, pipe, mode, on_acquired) -> bool`,
`pipe_release(scheduler, pipe, mode)`,
`pipe_try_inline(scheduler, pipe, mode, block) -> bool`,
`Pipe::wait_until_idle()`. `pipe_epoch(pipe)` (the harness grant-epoch source) stays.
`Deferred`/`Versioned` gain access to the tail-as-last-write handle (§11), which lets the
`commit_mutex_` go (follow-up), but their public API is untouched.

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
2. **Writer chain first** — implement the tail + writer arrival/clear + `wait_until_idle`,
   validate with WRITE-ONLY workloads (the UE-exact core; de-risks ref-transfer/lifetime
   before reader-join). TSan.
3. **Reader group** — the SNZI sentinel + join/close/backout (§5.2) + lone-reader elision
   (§4.4). The hard part; race-delay hooks + the C/D tests + `stress_pipe_rw` under TSan.
4. **Reservations** (§6a) behind the seam; the E-tests + `stress_pipe_reservation`.
5. **`writer_owner`** (§8) + F-tests.
6. Shipping compile (behavior half stays, diagnostic half compiles out) + full TSan +
   ASan/`--stress` on Windows.
7. Update CLAUDE.md scheduler/Guarded paragraphs; hand back to the review session.

Risks: the join-vs-close/settle races (§5.2) are the crux — race-dense, Windows has no
TSan (WSL only), so the hooks + high-iteration stress carry the load. The elision (§4.4)
may complicate the arrive-vs-settle race enough to prefer a pooled sentinel — decide during
step 3. The reservation path (§6) is a second consumer of the tail and most of the risk
after the join protocol.

## 14. Open questions (for review)

1. §5.2 join protocol — is the tentative-arrive + tail-revalidate + backout the shape we
   want, or should a joining reader instead CAS a per-sentinel state word (closed-bit +
   count) à la a classic queued RW-lock, keeping the tail pointer-only? The former reuses
   `num_locks` (one fewer field); the latter localizes the race to the sentinel (arguably
   easier to reason about / TSan). Leaning tentative-arrive; want a second opinion.
2. §4.4 elision — first-reader-as-representative (no sentinel alloc, harder settle race) vs
   a pooled dedicated sentinel per group (one alloc off the free-list, simpler race). v1
   default = elision, escape = pooled. Agree?
3. §6 — confirm v1 = (a) minimal (graph untouched behind the seam); (b) is the follow-up.

Sources: [SNZI (PODC'07)](https://dl.acm.org/doi/10.1145/1281100.1281106) ·
[Scalable Reader-Writer Locks (SPAA'09)](https://people.csail.mit.edu/mareko/spaa09-scalablerwlocks.pdf) ·
UE `FPipe` (`ue6-main` Engine/Source/Runtime/Core/{Public,Private}/Tasks/Pipe.{h,cpp}).
