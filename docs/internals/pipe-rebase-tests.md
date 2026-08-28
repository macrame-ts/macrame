# Pipe-rebase — test plan

Companion to the pipe rebase (TODO 1.14: pipes-as-edges, UE `FPipe` generalized to
reader/writer, plus the always-on grant-ownership field). This is the reviewed test
design; it is written BEFORE the implementation so the black-box tests land first as a
regression oracle and the rewrite is validated against them.

Status: design agreed 2026-07-31. Implementation of the tests proceeds in two waves
(see §2). The rebase itself is designed in `docs/internals/pipe-rebase.md` (to be written).

## 1. What is being tested (the concerns)

The rebase replaces the mutex-guarded deque pipe (`detail::Pipe`, `src/guarded.cpp`)
with a lock-free tail-chain: writers chain FPipe-style (atomic tail, one prerequisite
edge on the previous holder), a reader batch becomes a reader-group sentinel (a bare
block whose `num_locks` counts active readers), reservations become held bodyless
blocks. The risks these tests must cover (labels track the design discussion):

- **R1** reader-group join / close / backout — the novel hard part (UE never joins; it
  always exchanges). A reader joining an open group does a load-then-register that
  races a writer closing the group and the last reader completing it.
- **R2** reservations / graph — the per-node mode-aware acquire/release/handoff
  (`pipe_acquire`/`pipe_release`, `preheld`, `write_epoch`) must survive the rebase
  bit-for-bit (or be improved; either way these are the regression guard).
- **R3** grant-ownership — `Pipe::writer_owner` (always-on) names the CURRENTLY
  EXECUTING writer (not the queued tail); set at write-body start, cleared at
  completion, transferred at the graph handoff.
- **R4** writer retraction — PARKED (a writer is not retracted while the retractor holds
  another grant). A guard test is planned for when it lands; not in v1.
- **R5** lone-reader elision — a single reader allocates no sentinel; a second reader
  promotes the group to a sentinel.
- **R6** `wait_until_idle` — task-count + eventcount with UE's shared-ref-before-the-
  last-decrement discipline (the waiter may free the pipe the instant the count hits 0).
- **R7** worker-less mode — no pipe mutex, so inline-at-submit chains must ride the
  bounded trampoline, not recurse.
- **R8** `Deferred`/`Versioned` ordering — the atomic tail is the last-write handle;
  the `commit_mutex_` load+store race is dissolved.
- **R9** push-UAF — a predecessor can complete and free the just-enqueued block before
  the enqueue returns (UE brackets the push in an `AddRef`/`Release` on the pushed task).
- **R10** perf — non-regression only (per-object mutex contention is naturally low; the
  payoff is architectural). Covered by a benchmark baseline, not a correctness test.

## 2. Two waves

> **Wave-2 status (2026-08).** The lock-free tail chain this plan was written against was
> implemented and then **retired** ([pipe-rebase.md](pipe-rebase.md) §0.1), so every Wave-2
> row that asserts *chain* internals is **moot**: the reader-group sentinel does not exist,
> nothing is elided, and there are no race-delay hooks to arm. Concretely: **C2**'s hook half,
> **C4**, **D1** and **D3**'s hook half are moot (their black-box halves — C2's stress, D3's
> promotion-vs-close — landed as ordinary tests and remain valuable regardless of the pipe's
> internals), and `Pipe_probe` and the race-delay macro were never needed. The one Wave-2
> group that survived the retirement is **§F** (`writer_owner`), because grant ownership is
> not a chain concept — it landed 2026-08, re-scoped in place. Wave 1 is unaffected: it was
> written black-box precisely so it would outlive whichever implementation shipped, which is
> exactly what happened.

- **Wave 1 — black-box `[bb]`.** Drive only the public `Guarded` / graph / `Deferred` /
  `Versioned` API. These MUST pass on the current (pre-rebase) pipe; they land first and
  become the regression oracle. Any Wave-1 test that fails on the current impl is a test
  bug, not a finding.
- **Wave 2 — white-box `[wb]`.** Assert new internals (sentinel elision, `writer_owner`,
  the race-delay hooks) or new API (unified verbs). These do NOT compile/pass on the
  current impl; they are planned here and added alongside the code that satisfies them.
  Marked `[wb]` throughout; each carries a note on what it needs.

## 3. Test infrastructure

### 3.1 `Rw_probe` (the dual oracle) — `tests/test_util.h`

A guarded "system" every access body drives, giving two independent race detectors:

1. **Explicit invariant.** `atomic<int> active_readers`, `atomic<bool> writer_active`.
   A read body: `++active_readers`, assert `!writer_active`, run, `--active_readers`.
   A write body: assert `active_readers == 0` and CAS `writer_active` false→true (assert
   it was false — no second writer), run, clear. A violation sets an `atomic<bool>` the
   test asserts. Fires on ANY build/scheduler, but its own atomics are synchronized, so
   TSan sees nothing from the bookkeeping alone — hence detector 2.

2. **Unsynchronized payload (for TSan).** A plain, NON-atomic `int payload_[K]` on
   cache-line-separated words. A writer mutates EVERY word; a reader reads ONE word,
   chosen by a rotating index so per-word reader traffic stays low. If the pipe ever
   overlaps a writer with a reader/writer, TSan reports the race on `payload_` directly —
   independent of detector 1's window. The words are separated and reader traffic per
   word kept ≤ ~2 on purpose: TSan keeps only ~4 shadow cells per 8-byte word, so a hot
   word with many concurrent readers would evict the writer's shadow and MISS the race.
   Spreading the conflicting pair onto lightly-trafficked words preserves the shadow.

The probe also records `max_concurrent_readers` (for concurrency-degree asserts) and a
monotonically-checked write counter (writes apply in pipe order).

### 3.2 Window widening — `tests/test_util.h`

Access bodies take a short, index-seeded pseudo-random spin (`Math::random` is banned in
the harness; seed per participant by index). Wider bodies → wider overlap windows →
higher TSan hit-rate. A `tests::tiny_spin(seed)` helper.

### 3.3 Race-delay hooks (T2) — `TS_PIPE_RACE_DELAY(point)` `[wb]`

A macro in the pipe, a no-op in normal builds, a short `yield`/spin under
`TS_PIPE_RACE_TESTING`. Placed at the two known race points:
- `join_after_add` — reader join, AFTER the tentative register, BEFORE the tail
  re-validate.
- `close_before_drop` — writer close, BETWEEN the tail CAS and the open-lock drop.
Highest-leverage lever for C2/D3 under TSan. Landed with the rewrite (Wave 2).

### 3.4 Internal access — `friend struct ts::test::Pipe_probe` `[wb]`

Structural white-box asserts (sentinel-not-allocated for a lone reader, `writer_owner`
identity, sentinel-completes-once) reach pipe internals through a friend probe in
`ts::test`, plus a simple global allocation counter for the elision check (D1).

### 3.5 Homes

- New group `tests/pipe_tests.{h,cpp}` (`run_pipe_tests()`): §A (invariant), §C
  (sentinel protocol), §D (elision), §F (`writer_owner`), §G (lifetime), §H
  (worker-less), §J (priority/cancel).
- §B (FIFO) grows `guarded_tests.cpp` (co-located with the existing FIFO tests).
- §E (reservations) grows `graph_tests.cpp`.
- §I grows `deferred_tests.cpp` / `versioned_tests.cpp`.
- TSan stages grow `tsan/tsan_main.cpp`.

## 4. Catalogue

Each entry: id, `[bb]`/`[wb]`, one line of WHAT, one line of HOW.

### A. Reader/writer invariant (`pipe_tests`)
- **A1** `[bb]` concurrent readers overlap. *How:* `Parallel_gate{k}` — the gate is met
  only if k readers were in flight at once (fails, not hangs, if serialized).
- **A2** `[bb]` writer exclusion (strengthened). *How:* `Rw_probe`; a writer body asserts
  no reader/other-writer active AND mutates the raw payload readers verify — TSan catches
  a true overlap the synchronized asserts might race past.
- **A3** `[bb]` mixed interleave invariant. *How:* many threads issue a randomized mix of
  read/write × access/async on one `Rw_probe`; assert no invariant violation and the
  final value is the exact write count; TSan on the payload.
- **A4** `[bb]` reader-concurrency degrees. *How:* A1 at k ∈ {2,4,8}.

### B. FIFO group ordering (`guarded_tests`)
- **B1** `[bb]` reader-after-writer (exists). *How:* a read after a write sees the write.
- **B2** `[bb]` R,W,R separation. *How:* arrival R₁,W,R₂ with a sequence log — assert
  R₁<W<R₂ and R₂ sees W's value; R₂ must NOT join R₁'s group (writer arrival closes it).
- **B3** `[bb]` writer waits for the open reader group. *How:* a writer launched during a
  live reader group runs only once every reader in that group has drained
  (`Rw_probe`: `active_readers == 0` at write entry).
- **B4** `[bb]` writer FIFO. *How:* a serial write chain composes values in launch order.

### C. Reader-group sentinel / join-close (`pipe_tests`) — R1 core
- **C1** `[bb]` join an open group. *How:* a 2nd/3rd reader joins while the 1st still
  runs; `Parallel_gate` proves the join produced real concurrency.
- **C2** `[bb+wb]` join-vs-close stress (primary). *How:* high thread count, one pipe,
  tiny bodies, randomized reader/writer arrivals, long iterations, `Rw_probe` + payload,
  race-delay hooks armed (Wave 2). Targets the tentative-register / re-validate / backout
  window.
- **C3** `[bb]` backout ordering. *How:* a reader that loses the join to a closing writer
  re-forms after it; assert it runs after the writer's effect, never overlapping it.
- **C4** ~~`[wb]` sentinel completes once~~ — **moot (2026-08)**: no sentinel exists in the
  evolved pipe; a reader group is `active_readers` under the pipe mutex.

### D. Lone-reader elision + promotion (`pipe_tests`) — R5
- **D1** ~~`[wb]` lone reader elided~~ — **moot (2026-08)**: elision was a chain optimization
  (avoid allocating a sentinel block for a single reader). The evolved pipe allocates nothing
  per reader in the first place, so there is nothing to elide or to count.
- **D2** `[bb]` promotion on 2nd reader. *How:* a lone reader in flight, a 2nd joins →
  both concurrent (`Parallel_gate{2}`); a following writer waits for both. (Passes on the
  current impl too — two readers already overlap — so it is `[bb]`.)
- **D3** `[bb+wb]` promotion-vs-close race. *How:* the 2nd reader joins exactly as a
  writer arrives; neither reader ever overlaps the writer; randomized, TSan, hooks.

### E. Reservations / graph (`graph_tests`) — R2
- **E1** `[bb]` writer→writer handoff. *How:* a shared-object serial node chain hands the
  object directly (no release/re-acquire); value + `Rw_probe` invariant; `write_epoch`
  parity under the harness.
- **E2** `[bb]` reader-node ∥ async-reader. *How:* a reader node and a concurrent async
  read overlap (`Parallel_gate`); a writer node excludes both.
- **E3** `[bb]` gap-freeing. *How:* an async on an object in the gap between two nodes
  runs in the gap, not blocked for the whole run.
- **E4** `[bb]` multi-object canonical order. *How:* two multi-object `ts::async`es over
  overlapping objects declared in OPPOSITE orders, hammered — no deadlock, dedup
  write-wins correct.
- **E5** `[bb]` node ∥ async no-race. *How:* a graph run touching X while asyncs hammer X;
  TSan on the object payload (extends `stress_graph_async`).
- **E6** `[bb]` inline node dispatch across handoff. *How:* an inline node chain on one
  object trampolines correctly.

### F. Grant-ownership `writer_owner` (`pipe_tests`) — R3, Wave 2 `[wb]` — **LANDED 2026-08**
- **F1** owner set∥cleared. *How:* inside a write body the test reads `writer_owner ==
  current_task`; outside, `nullptr`. *Landed as written*, plus the other half of the
  invariant: a READER hold must publish NO owner, or `commit()` would take its inline arm
  under a read grant.
- **F2** ~~owner transferred at handoff~~ — **re-scoped**: the explicit graph write handoff
  is deleted with the pipe rebase, so the successor case is release-then-admit. *Landed as:*
  two chained writes each name their OWN block, never the predecessor's, never null.
- **F3** owner under inline `access`. *Landed as written* (an `access` on a free pipe is a
  real admission and publishes its own block), plus the REENTRANT arm, which is the sharper
  case: an `access` from the grant holder runs under that grant and touches the pipe not at
  all, so the owner must remain the OUTER block — republishing it, or clearing it on the
  inner settle, would mis-dispatch `commit()` for the rest of the outer body.
- **F4** per-pipe owner under multi-object write. *Landed as written*, plus an untouched
  third object staying unowned (the per-pipe independence half).
- *Note:* no `Pipe_probe` type was needed in the end — `Guarded_access::pipe()` plus a local
  `owner_of()` helper reads the field directly. The auto-dispatch verb behavior these
  invariants underpin is covered end-to-end in `deferred_tests`.

### G. Lifetime (`pipe_tests`) — R6, R9
- **G1** `[bb]` destructor waits (strengthened). *How:* `Guarded` dropped immediately
  after the last async; loop under ASan/TSan.
- **G2** `[bb]` last-decrement UAF. *How:* many producers, then destroy the `Guarded` as
  the final job completes; the drain event must outlive the pipe. Tight loop.
- **G3** `[bb]` push-UAF. *How:* a predecessor completes and frees the just-enqueued block
  before the enqueue returns; rapid enqueue+immediate-complete churn under ASan.
- **G4** `[bb]` cancelled representative advances. *How:* a cancelled writer still unlocks
  its successor; a cancelled reader still leaves its group; a cancelled sentinel still
  completes. Value + no-hang.

### H. Worker-less mode (`pipe_tests`) — R7
- **H1** `[bb]` A/B/C/D under `Scheduler_scope{ {.single_threaded = true} }`. *How:* run
  the functional bodies on a worker-less scheduler; assert identical results.
- **H2** `[bb]` deep pipe chain worker-less. *How:* a piped write chain (≥100k) must not
  stack-overflow (bounded inline trampoline).
- **H3** `[bb]` blocking sync worker-less drains serial pending. *How:* no deadlock.

### I. Deferred/Versioned ordering (`deferred_tests`/`versioned_tests`) — R8
The existing suites already cover this black-box and are the regression guard as-is:
`deferred: commit under an async write grant` / `straggler rides the next commit` /
`concurrent staging`, and `versioned: reader overlaps the resync` / `concurrent readers
during publishes` / `chained publishes apply exactly once`, plus `stress_deferred` /
`stress_versioned` under TSan. No new Wave-1 tests are needed; the rebase must keep these
green.
- **I1** `[bb]` (covered) commit_async ordering — `deferred: straggler rides the next
  commit` + `stress_deferred`.
- **I2** `[bb]` (covered) publish phase-3 as an edge — `versioned: reader overlaps the
  resync` + `versioned: concurrent readers during publishes`.

Note: the `commit_mutex_` removal + `last_write()` exposure is a Wave-2 / follow-up change
(§R8 in pipe-rebase.md §11); its new-API test lands then.

### J. Priority + cancellation on the pipe (`pipe_tests`)
- **J1** `[bb]` priority. *How:* piped jobs dispatch at their priority but pipe order is
  not reordered across a writer.
- **J2** `[bb]` cancellation down a pipe chain. *How:* overlaps G4.

### TSan stages (`tsan/tsan_main.cpp`)
- **`stress_pipe_rw`** — the interleaved readers/writers stress test: N threads, one
  `Guarded`, randomized async/access read/write, tiny bodies, `Rw_probe` + payload +
  hooks; plus a worker-less variant.
- **`stress_pipe_lifetime`** — churn create/destroy `Guarded`s with in-flight work
  (G1–G3).
- **`stress_pipe_reservation`** — graph nodes + concurrent async on shared objects (E5),
  extending `stress_graph_async`.

## 5. Coverage matrix

| Concern | Tests |
|---|---|
| R1 reader join/close/backout | C1–C4, D3, `stress_pipe_rw` |
| R2 reservation / graph handoff | E1–E6, `stress_pipe_reservation` |
| R3 `writer_owner` | F1–F4 |
| R4 writer-retraction guard | *(parked; add when it lands)* |
| R5 lone-reader elision | D1–D3 |
| R6 `wait_until_idle` UAF | G1, G2, `stress_pipe_lifetime` |
| R7 worker-less | H1–H3 |
| R8 Deferred/Versioned ordering | I1, I2 |
| R9 push-UAF bracket | G3 |
| R10 perf | benchmark baseline (not here) |
| invariant / FIFO / concurrency | A1–A4, B1–B4 |
| priority / cancellation | G4, J1, J2 |

## 6. Wave-1 landing order

1. `Rw_probe` + helpers (`test_util.h`).
2. `pipe_tests` group: A1–A4, C1, C3, D2, G1–G4, H1–H3, J1–J2.
3. §B into `guarded_tests`, §E into `graph_tests`, §I into deferred/versioned tests.
4. `stress_pipe_rw` / `stress_pipe_lifetime` / `stress_pipe_reservation` (TSan).
5. Green on the CURRENT pipe (regression baseline), commit.

Wave 2 (C2 hooks, C4, D1, D3 hooks, F1–F4, race-delay macro, `Pipe_probe`) lands with
the rewrite. **Outcome (2026-08):** only F1–F4 landed (see §F); everything else on that
list died with the chain — see the status note in §2.
