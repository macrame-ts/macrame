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

## 5. Protocols (the careful core)

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

### 5.2B Reader arrival — chain + head-walk (recommended)

```
B1  Arrival (any reader R, identical to a writer's push):
B2    AddRef(R)
B3    prev = tail.exchange(pack(R, READER))     // exchange only -- NO load-then-inc, no UAF
B4    if prev == 0: mark R the head; submit R ready   // idle pipe -> R is the group head
B5    elif AddSubsequent(prev, R): edge R after prev   // linked; R starts when prev unlocks it
B6    else: prev already completed -> R is a new head; submit R ready

B7  Head-reader walk (runs when a reader that is a group HEAD gets the grant):
B8    n = 1; last = self
B9    loop:                                     // pass 1: find extent, close the chain
B10     nxt = last.subsequent                   // read BEFORE any unlock (grab-next)
B11     if nxt is a READER (and linked): last = nxt; n += 1; continue
B12     if nxt is a WRITER W: terminator = W; break
B13     if nxt == null: if CAS_close(last.subsequent): terminator = install G(lock=n); break
B14                     else: nxt = last.subsequent; continue   // a late reader linked; re-read
B15   set each group reader's unlock-target to `terminator`; set terminator.lock = n
B16   pass 2: unlock self..last in order (each may now run; grab-next already done in pass 1)
```
Only a **head** reader walks (a reader unlocked *by* the walk is a member, flagged, and does
not walk). B3 is the whole arrival — one exchange, no UAF. B13/B14 is the walk-vs-late-
arrival close race (UE `Close`). The `TS_PIPE_RACE_DELAY` hooks go at B10/B13 and at the
writer's `AddSubsequent` (5.1 W5) for the TSan campaign.

The TSan-critical race points:

- **Design A only — join-vs-UAF (R1):** line A4's `fetch_add` on a possibly-freed sentinel
  (§4.3.A). The blocker that makes A expensive.
- **Design B — walk-vs-late-arrival:** B13/B14, the walker closing the chain end while a
  reader is mid-`AddSubsequent`. UE `Close` semantics; the late reader becomes a new head.
- **Design B — terminator install race:** the walker's `CAS_close`/`install G` (B13)
  racing a writer's `AddSubsequent` (W5): whoever wins the `last.subsequent` CAS decides
  whether the writer chains behind `G` or is found directly by the walker.
- **Both — last-decrement / drain (R6):** §7.

The race-delay hooks (`TS_PIPE_RACE_DELAY`, no-op in normal builds) sit at B10/B13 (design
B) or `join_after_add` (design A) and at the writer's `AddSubsequent` for the TSan campaign
(pipe-rebase-tests §3.3).

### 5.3 Completion

- A reader finishing: `num_locks.fetch_sub(1)` on the group barrier / representative
  (design B: the barrier `G` or the retargeted terminator; design A: the sentinel). When
  the count reaches its base, the barrier settles → releases the next writer → the writer
  runs.
- A writer finishing: `settle` → releases its single subsequent (the next head reader or
  writer), UE-style. Then `tail.compare_exchange(self -> 0)` if it is still the tail (pipe
  now idle); else the next pusher already took the tail (ref transferred).
- `TaskCount` decrement (every task, at its completion) drives `wait_until_idle` (§7).

### 5.4 Why this is deadlock-free and correctly ordered

Ordering is the chain on the tail (each task edges on its predecessor); within a reader
group, readers share the head's start gate and run concurrently; the next writer waits on
the group barrier / terminator. FIFO-by-arrival is the tail exchange order (UE's
guarantee). No lock is held across dispatch — a completing block releases edges outside any
pipe lock, so a worker-less inline chain rides the bounded trampoline (R7), and `access`
inline never stacks under a lock.

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

## 14. Open questions (for review)

1. **§4.3 reader design — A (arrival-join sentinel) vs B (chain + head-walk).** B is
   recommended: it structurally avoids the R1 load-then-inc UAF (readers only `exchange`,
   like writers), trading it for a head-reader O(n) walk over UE-proven `AddSubsequent`/
   `Close` primitives. A needs hazard pointers or a packed-count tail to make line A4 safe.
   *Author's raw idea = B; analysis says nothing serious against it.* **Decision pending —
   prototype B first?**
2. §4.4 elision — under B this is trivial (a lone reader is a group of one, no barrier until
   a writer arrives, no special case). Confirmed; rides on Q1.
3. §6 — v1 = (a) minimal (graph untouched behind the seam); (b) is the follow-up. Confirmed.

Sources: [SNZI (PODC'07)](https://dl.acm.org/doi/10.1145/1281100.1281106) ·
[Scalable Reader-Writer Locks (SPAA'09)](https://people.csail.mit.edu/mareko/spaa09-scalablerwlocks.pdf) ·
UE `FPipe` (`ue6-main` Engine/Source/Runtime/Core/{Public,Private}/Tasks/Pipe.{h,cpp}).
