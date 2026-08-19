# Deferred/Versioned: development state handoff

(User-facing usage: [guide.md](guide.md) §9; condensed rationale:
[design.md](design.md) §6.)

Written 2026-07 at the wrap of the staged-write development push (commits
`99b1b25`..`785fc93`). Intended to be absorbed by the main development session:
what shipped, the load-bearing mechanisms and their WHY, verification state,
and the ranked plan for what comes next. Design history and rationale live in
`docs/command-buffer-design.md` (§7 = outcome); this doc is the operational
summary + the forward plan.

## 1. What shipped (commit by commit)

- `99b1b25` — the layer: `Deferred<T>` + `Versioned<T>` + physics sample,
  14+15 tests, 4 death scenarios.
- `71e46b1` — TSan stress stages (deferred/versioned/physics); regenerated the
  stale `vcxproj.filters`.
- `c20cd9c` — `Recorder` empty state made real (move nulls the source; staging
  on empty is fatal).
- `e33d491` — `Parallel_recorder` (per-worker slots via the scheduler's
  existing `current_worker_index` TLS + an overflow lane); shared staging
  machinery split into `journal.h`; physics gameplay stages per-body drag via
  `parallel_for`.
- `317b998` — journal slot free-list (recorder dtors recycle; live slots
  bounded by peak concurrent recorders) + `max_slots` (4096) fatal diagnostic.
- `443ff85` — typed-tier design notes settled ahead of implementation
  (intermix, record stream, fences; doc §7.3).
- `465548e` — the ordering contract stated once in `journal.h`; slot-reuse
  note demoted to a footnote under it.
- `791eaba` — blackboard pattern recipe (doc §7.4) + single-file sample.
- `785fc93` — single-publisher enforcement at flip entry + the
  resync-enqueue-before-gate ordering fix (pre-fix test demonstrated a lost
  write).
- `ef6cbb7`/`de45b77` — `Versioned::read` became attended (`access` →
  caller-owned `Access_op`, zero-alloc read-mostly front, copy-out-and-discard
  correct by construction); publish batch moved to the single `batch_` member
  slot (removed a `make_shared` per publish).
- review batch (2026-08-19) — in-place capacity-reusing `Journal::cut(out&)`
  (steady-state publishes/commits allocate no batch; `Versioned::batch_` +
  `Deferred::commit_batch_`); allocation-free `discard()` via
  `Journal::clear_staged()`; `~Journal` fatal on recorder-outliving-journal
  (contract 8, death test `recorder_outlives_journal`).

## 2. File map

| file | contents |
|---|---|
| `journal.h` | `detail::Journal<T>` (slots, cut, free-list, `max_slots`), `Recorder<T>`, `Parallel_recorder<T>`, THE ORDERING CONTRACT comment |
| `deferred.h` | `Deferred<T>`: `recorder()`, `parallel_recorder()`, `commit(opts)` (auto-dispatching: inline under the caller's held write grant, else one enqueued write; bound object implicit — see contract 7a), `discard()` |
| `versioned.h` | `Versioned<T>`: `read()`, `recorder()`, `parallel_recorder()`, `publish(opts)`, `publish_into(T&)`, `state()`, `Resync{replay,copy,overwrite}`, `set_copy`, `set_divergence_check`, `discard()`; `ts::publish_fn(v)` for graph flip nodes |
| `sample/physics.cpp` | machine/extract decomposition fixture (sealed `Guarded<Physics_world>`, `Deferred` inputs, `Versioned` poses, id reservation, batch extract, parallel drag staging); determinism self-check |
| `sample/blackboard.cpp` | single file (extern'd in main.cpp, no header — by design): blackboard recipe with key-change subscriptions; determinism self-check |
| `tests/deferred_tests.cpp` | 22 cases incl. churn/reuse/parallel/overflow |
| `tests/versioned_tests.cpp` | 20 cases incl. replay invariant, resync overlap, mixed-publish legality |
| `tests/tests.cpp` | 8 death scenarios (see §5) |
| `tsan/tsan_main.cpp` | `stress_deferred` (+parallel recorder), `stress_versioned`, `stress_physics`, blackboard stage |
| docs | `command-buffer-design.md` §7.1–7.4; CLAUDE.md bullet; TODO 3.1 #7 arena note |

## 3. The contracts (what users may rely on)

1. **Stage is grant-free** — touches only journal storage; safe from any
   thread, anytime; never contends with readers of the target.
2. **Snapshot semantics** — readers see none of a batch before the
   commit/publish, all of it after.
3. **Cut at execution** — a commit/publish captures what was staged when it
   RUNS; stragglers ride the next one; a cancelled commit retains commands.
4. **Ordering** (`journal.h`, the contract comment): intra-recorder FIFO is
   semantic — build on it. Cross-recorder order is ARBITRARY — never build
   semantics on it (same-key cross-producer writes are a conflict; fix =
   single writer per key / commutative commands / future sort keys) — but
   DETERMINISTIC given a deterministic mint/destroy sequence, so runs
   reproduce, bugs included. `Parallel_recorder` is the explicit, localized
   surrender of cross-thread reproducibility (per-worker placement); batch
   order is still fixed at the cut, so `Versioned` replay stays exact.
5. **`Versioned`: no read-your-writes** — outputs arrive as the NEXT version;
   fresh readers order after the publish. `publish()` completes at the swap.
5a. **`read()` is attended (2026-08-18)**: it returns the caller-owned
   `Access_op` via the front's `access` verb, not a heap `Task` via `async`.
   The front is read-mostly by design (concurrent readers, one ~ns swap per
   cycle), so the inline arm hits nearly always — zero-alloc reads — and a
   discarded copy-out read is correct by construction (the op temporary's
   destructor waits out the settle; the old detached form let it race).
   Leavers use the detached spelling `v.state().async(fn)`.
   `Access_options{.queued}` passes through for heavy read bodies.
6. **Resync policies**: `replay` (default; commands must be deterministic —
   capture RNG/time at stage time; both applications see bit-identical
   pre-states) / `copy` / `overwrite` (writer rewrites everything).
   `set_divergence_check(hash)` = bitwise replica compare after replay, fatal
   on mismatch (valid: no FP drift on one binary; partial hashes fine).
7. **Lost writes are fatal** (`TS_SAFETY_CHECKS`): staged-but-unapplied at
   destruction; `discard()` is the escape. Also fatal: destroying a `Deferred`
   with an enqueued `commit()` still in flight — sync the returned task first
   (the pending pipe job uses the `Deferred`; a silently blocking destructor
   would hide the bug). Compliant destruction is non-blocking: pipe writes are
   FIFO, so the last commit settling means no job references the `Deferred`
   (shipping builds keep the pipe wait as the safety net for the violation
   case; inline commits finish in-call and never pend).
7a. **One `commit()`, auto-dispatching (2026-08, the pipe-outcome rework —
   `commit_async` removed).** Called by the task holding the target's write
   grant (`Pipe::writer_owner == current_task`) it applies inline under that
   grant and returns a shared pre-settled task — which carries NO
   happens-before edge (it settled before the apply; observers order through
   the object's pipe). Any other caller's commit is one enqueued write whose
   handle is recorded into `last_commit_` atomically with the enqueue (under
   the pipe mutex — the property the deleted `commit_mutex_` enforced
   externally). Misuse contract: nested sub-work under an INHERITED write
   grant must not call `commit()` (it is not the holder; the enqueued write
   would queue behind the very grant it waits out) — fatal under
   `TS_SAFETY_CHECKS`; commit from the grant-holding task instead.
8. **Recorder lifetime**: mint per producer, prefer setup-time; dtors recycle
   slots (free-list; staged-but-uncut commands survive release); >4096 alive
   recorders is fatal (`Journal::max_slots` — catches mint-and-retain); a
   reused slot keeps its position (observable only to contract-violating
   programs); recorders must not outlive their `Deferred`/`Versioned` — now
   **enforced**: `~Journal` (under `TS_SAFETY_CHECKS`) fatals if any slot is
   still outstanding (`slots_.size() > free_.size()`), catching the recorder
   that would otherwise `release_slot` on a destroyed journal. The journal dies
   during its owner's member destruction, before the outliving recorder's own
   dtor runs, so the fatal precedes the UAF. Death test
   `recorder_outlives_journal`.
9. **Single publisher, enforced**: a graph/inline publish catching an
   unresolved dynamic publish fatals at flip entry. Legal: dynamic↔dynamic
   (chain), synced-publish-then-run (deterministic — see §4.3), publish
   arriving mid-flip (chains behind the flip). Rejected: fire-and-forget
   publish racing a flip.

## 4. Load-bearing mechanisms (maintainer notes — the WHYs)

1. **Three-phase publish; only the swap holds the write grant.** Phase 1
   (cut + apply to shadow) is grant-free (shadow unobservable); phase 2 swaps
   the replicas' CONTENTS (front address stable ⇒ zero graph/pipe/harness
   changes); phase 3 (resync) runs as a **pipe READ job** — overlaps readers
   of the new version, and pipe FIFO holds the next writer (incl. a graph
   flip's acquire) behind it. The pipe IS the shadow-ownership chain.
   Consecutive publishes chain phase 1 after the previous resync via `chain_`
   (`seq_mutex_` guards only the handoff). An empty publish is a resync fence.
2. **The `front == shadow` invariant** at every publish start is what makes
   staged deltas equivalent to full state. Replay restores it by applying the
   same batch to the new shadow (identical pre-states). The shadow is
   `T shadow_{}` — value-init to match `Guarded`'s `instance_()`; a
   default-init shadow was the first real bug (replicas born diverged; RMW
   commands exposed it; the divergence check would have caught it at first
   publish).
3. **Resync is enqueued BEFORE the publish gate triggers** (swap body order:
   `swap_replicas; start_resync; swapped.trigger()`). Load-bearing: anything
   ordered after the publish task (a `sync()`, an `.after`) finds the resync
   already on the pipe, so a following flip FIFO-orders behind it — this is
   what makes `publish().sync(); g.execute()` deterministically legal and
   what the flip-entry enforcement check relies on. Pre-fix, the amplified
   test lost a write through this window.
4. **Flip-entry enforcement** (`publish_into`): under one `seq_mutex_` hold,
   `chain_.is_done()` check (fatal if false) + install the flip's
   `shadow_ready`. The entry-install simultaneously makes a dynamic publish
   arriving mid-flip chain behind the flip. Empty-batch path must still
   trigger the signal (the chain always resolves).
5. **`Parallel_recorder` lane routing**: worker index + 1 when the caller is a
   worker of the one process-wide pool (`current_worker_index >= 0`) and the
   index fits, else the overflow lane (0) — which absorbs the `parallel_for`
   caller's own share, inline bodies, and external threads. (Since the
   single-global collapse there is one pool with workers, so "am I a worker of
   the bound scheduler" reduces to `current_worker_index >= 0`.) One TLS read +
   two compares per stage.
6. **Per-slot mutex exists only for the dynamic stage-vs-cut race**;
   uncontended in one-producer use; graphs edge-order it away entirely. It is
   the thing the arena/record-stream rebase can remove.
7. **`commit()` dispatch (was `commit()` vs `commit_async()`)**: one verb since
   the 2026-08 rework — `Pipe::writer_owner == current_task` picks the inline
   under-grant apply (still `access_check`-verified on the bound instance, the
   graph-node / sim-boundary form); every other caller gets one ordinary pipe
   write. See contract 7a for the sentinel-ordering and nested-grant rules.
8. **Producer→commit/flip edges are hand-wired** (`.after`); `Access::append`
   derivation was analyzed and deliberately NOT added (avoidable lattice
   dimension; staging needs no grant at all). Revisit only when the
   completeness hazard bites in practice.
9. **One reused batch slot, filled in place.** `Versioned` keeps the in-flight
   publish's cut in a single member (`batch_`); one slot suffices because the
   publish chain serializes batch lifetimes — `shadow_ready` triggers only
   after the batch's last use (resync replay / phase-1 apply / the cut itself
   for the empty and cancelled paths), and the next publish's cut into the slot
   gates on exactly that signal via `chain_`. The cut is `Journal::cut(out&)`,
   which clears and refills the caller's buffer instead of returning a fresh
   vector, so the capacity is genuinely reused and steady-state publishes
   allocate no batch. `Deferred` does the same through a `commit_batch_` member,
   clearing it after each apply (it applies once — no replay — so the command
   captures are released promptly while the buffer is retained). The batch
   allocation this removed is one vector per publish/commit; the per-command
   closure allocation (§6 arena work) remains the dominant cost.

## 5. Verification state

- **340 checks, 0 failures**; suite loops (10–20×) clean; the one flake found
  (counting resync side effects after `sync()`) was a test bug — fixed with
  the empty-publish fence.
- **Death scenarios**: `deferred_drop_staged`, `versioned_drop_staged`,
  `versioned_divergence`, `versioned_wrong_front`, `versioned_mixed_publish`,
  `recorder_empty_stage`, `parallel_recorder_empty_stage`,
  `journal_slot_overflow`, `recorder_outlives_journal`.
- **TSan clean on WSL** (clang + libstdc++) including: parallel staging racing
  fire-and-forget commits; stagers + readers + 200 chained publishes with the
  divergence hash reading both replicas; both samples.
- **Determinism**: both samples run twice and compare bitwise
  (poses hash / full stats). Physics: staged inputs + keyed apply + replay ⇒
  bit-identical regardless of thread timing, including the nondeterministic-
  placement drag staging (one command per body; disjoint bodies commute;
  thrust-before-drag fixed by recorder creation order).
- Not done: `mem_profile` coverage for the journal (no measured alloc
  numbers — the docs assert the closure-tier costs; see §6 step 1).

## 6. Future work — the ranked plan

**Main chain (each gates the next):**

1. **Baseline**: add the journal to `mem_profile` — print the closure-tier
   alloc numbers the docs currently assert. Opens the arena work with a
   before/after table.
2. **Minimal per-journal arena** (entry path into TODO 3.1 #7, NOT the full
   frame-arena ambition): chunked bump allocation, per-slot chunk chains
   (single producer ⇒ lock-free bump; the cut becomes a chain-head exchange
   and likely kills the per-slot mutex), chunks recycled via a per-journal
   pool. Design decisions to settle first, on paper:
   - **Batch lifetime vs chunk recycling**: chunks return when the BATCH is
     destroyed — after one apply (`Deferred`) but after the RESYNC
     (`Versioned`, applied twice). Therefore records need
     **destroy-at-batch-drop, NOT UE's `ExecuteAndDestruct`** — replay would
     re-invoke destroyed closures. This is the known trap.
   - Per-journal pool vs shared frame arena (lean: per-journal now; rebase
     later doesn't conflict).
   - Chunk size + oversized-capture overflow records; check the chunk
     interface against `parallel_gather_apply` (TODO 7.x) on paper only.
3. **Record-stream slots with closure records only** — `[header|payload]…` is
   the §7.3 layout with one record kind; it can land BEFORE the typed API.
   Kills per-command heap allocs and capacity retention with zero call-site
   churn. TSan pass mandatory (stage/cut concurrency changes).
4. **Typed tier** (`Deferred<T, Cmd>` / `Versioned<T, Cmd>`): second record
   kind + `stage(Cmd)` overload. Design settled in §7.3: closure intermix in
   one stream, closures fence the hooks.
5. **Riding the typed tier**: commit-time hooks (sort / dedup-last-wins-by-key
   / reduce); **sort keys for `Parallel_recorder`** (restores cross-thread
   determinism — the one real gap in parallel staging); `Set_cmd` dirty set
   (upgrades the blackboard's polling-diff notification to push); keyed
   closures only if a case forces it.
6. **Render-queue fixture with a sorted commit** — the doc's original proving
   use case, deliberately saved to validate the typed tier.

**Batch-along (fold into whatever touches these files next):** default
bytewise hash/copy for trivially-copyable `T`; `Versioned` ctor arg forwarding
(currently default-construct only).

**Parked, awaiting a forcing use case:** `Access::append` derivation /
`add_flush_node`; cross-target commands (physics says: decompose instead);
reserved handles from `stage()` (id-allocator pattern is the documented
answer); `read_pair()` for interpolation; triple-buffer `Versioned` (the name
already permits it silently); parallel apply inside a commit (API-invisible;
only if a flush becomes the critical path).

## 7. Selection guidance (when to use what — condensed from the fixtures)

- Per-frame delta is DATA (poses, facts, events) → `Versioned<T>`.
- Producing version N+1 is heavy COMPUTATION over N (physics sim) → keep ONE
  instance behind `Guarded`, `Deferred` its inputs, `Versioned` its output
  EXTRACT (the machine stays sealed; count its grant holders — physics has 1,
  the sim's write; even its scene query reads the extract).
- Delta ≈ whole state, rebuild cheap → `shared_ptr<const T>` swap, no journal.
- One logical producer parallelized internally → `Parallel_recorder`
  (commutative / per-key-single commands) or index-keyed `Recorder` lanes
  (raw determinism).
- Blackboard/observer shapes → §7.4 recipe; no new types.
