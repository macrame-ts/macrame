# Graph `execute()` regression — callgrind root-cause (untracked)

Deterministic instruction-count profiling of the ~10% `graph.execute()` regression
between the pre-transformation baseline and current `master`. Tool: valgrind
callgrind 3.26 (exact instruction counting), on WSL Ubuntu 26.04 / clang 21.

> Status: measurement report for review. Not committed. Delete after the finding
> is folded into the TODO / a fix lands.

## 1. Method

- **Two throwaway Linux-local clones**, built identically:
  - baseline = `1652818` (callback composition, `std::deque` pipe, no `ts::Named`)
  - current = `master` tip (`1ff7781`; coroutine-first, evolved pipe, `ts::Named`)
- Build (both): `clang++ -std=c++23 -O2 -g -fno-exceptions -pthread -DTS_SAFETY_CHECKS=0 -DTS_PROFILING=0`
  over the same library TU set as `tsan/run.sh` (`scheduler, worker_thread, guarded,
  static_task_graph, access, fatal`) + a driver, `-latomic` (16-byte `Task_entry`
  atomics need it on libstdc++). Shipping-like: harness + trace compiled out, so
  callgrind sees only the real node machinery (trace `mark_*`/`fold`, `Trace_*_scope`
  are no-ops).
- **Matched driver** per version: build 1000 object-free empty-lambda nodes
  (`add_node([]{})` on base; `add_node(ts::Named{nullptr}, []{})` on current),
  `compile()` once, then `execute().sync()` × 200. **Worker-less scheduler**
  (`Scheduler_config{.single_threaded = true}`) so callgrind sees the whole
  machinery call tree deterministically on one thread, no worker-thread noise.
  Empty bodies ⇒ machinery dominates. 200 000 node-executes measured.
- The only measurement-harness adjustment was `-latomic` (identical on both) — no
  library-behavior change on either clone.

## 2. Headline number — the regression reproduces in instruction count

| | instructions (Ir) | per node-exec |
|---|---|---|
| baseline `1652818` | 104,450,328 | 522.3 |
| current `master`   | 129,583,340 | 647.9 |
| **delta**          | **+25,133,012 (+24.1%)** | **+125.7 ins/node** |

So this is **not** a cache / branch-mispredict / atomic-contention effect invisible
to instruction counting — the extra work is real, retired instructions. Callgrind is
the right tool.

Reconciling +24% here with the reported ~10% wall-clock: this microbenchmark is
100% machinery (empty nodes), so the fixed per-node overhead shows at full weight.
On a real frame the node bodies do actual work that dilutes the fixed ~+126 ins/node,
landing the observable delta lower — consistent with ~10%. One caveat pointing the
other way: the new cost includes two **atomic** RMWs/node (see §4); those are
uncontended in this single-thread run, so on a contended multi-worker frame their
cache-coherence cost is *additional* to the instruction count — instruction count is
a lower bound there.

## 3. What it is NOT (hypotheses falsified)

- **NOT the `execution_flag` self-lock completion path.** `Static_task_graph::run_graph_node`
  is **byte-identical** between the two commits — same `num_locks.store(execution_flag+1)`,
  same body bracket, same `fetch_sub`/`complete()`. The only textual difference is the
  baseline's extra `std::uint64_t gen` param (`claim(gen)`). Self-cost: 10.2M (cur) vs
  10.4M (base) — neutral. The self-lock predates the transformation; the leading
  hypothesis is wrong.
- **NOT the scheduler.** `Scheduler::submit` self = 2.20M both; `Scheduler::run_serial`
  self = 10.60M both. Identical.
- **NOT completion/settle.** These got *cheaper* on current: `settle` self 13.4M vs
  17.6M (−4.2M), `node_complete` 6.0M vs 7.6M (−1.6M), `graph_node_completed` inclusive
  8.24M vs 8.63M. (Some of that is refcount work relocating out of these functions'
  self-cost — see the attribution caveat in §4.)

## 4. Root cause — the unified ownership-carrying dispatch trampoline

The regression is entirely in the **per-node dispatch hop**, and it is a deliberate
structural change from coroutine-first: every ready node now dispatches through the
one generic block trampoline (shared with `async`/`launch`/coroutines/the pipe
cascade) instead of the graph's old bespoke one.

**Baseline** — dispatch carries a **raw `Node*`**, zero reference counting:
```
on_data_ready → acquire_next → run_node
    → submit(&node_trampoline, &node, prio)       // bare pointer into the queue
node_trampoline(void* node) → block->execute(block, gen)
```

**Current** — dispatch carries an **owned `Task_ptr` through the queue**:
```
on_data_ready → dispatch_ready(node.block)
    → submit_ready(Task_ptr block)                // by value: copy ctor = atomic INC
        → submit(&run_block_dispatch, block.release(), prio)   // release into queue
run_block_dispatch(void* d):
    Task_ptr block(d, Adopt_ref{});               // adopt
    block->execute(block); … }                    // dtor at scope end = atomic DEC
```
Net new work per node vs baseline: **+1 atomic increment + 1 atomic decrement +
`Task_ptr` construct/destruct scaffolding**, on the dispatch hop that baseline did
with a bare pointer.

### Per-function net-delta (self Ir, renamed pairs merged, ×200k = per node)

| Δ Ir | Δ/node | cur | base | function |
|---:|---:|---:|---:|---|
| +11,698,957 | +58.5 | 11.70M | ~0 | `Task_ptr::~Task_ptr` |
| +10,039,396 | +50.2 | 10.04M | 0 | `Task_ptr::operator=(Task_ptr&&)` |
| +5,400,000  | +27.0 | 5.40M | 0 | `Task_control_block::dispatch_ready` |
| +5,000,000  | +25.0 | 5.00M | 0 | `Task_ptr::operator=(const Task_ptr&)` |
| +2,400,000  | +12.0 | 2.40M | 0 | `run_block_dispatch` |
| +2,200,000  | +11.0 | 2.20M | ~0 | `global_scheduler()` |
| +1,602,003  | +8.0  | 13.62M | 12.02M | `execute` |
| +600,000    | +3.0  | 1.60M | 1.00M | `graph_node_completed` |
| +600,000    | +3.0  | 0.60M | 0 | `advance_pipe_links` |
| −200,000    | −1.0  | 10.20M | 10.40M | `run_graph_node` (identical source) |
| −1,200,000  | −6.0  | 0 | 1.20M | `node_trampoline` (baseline-only) |
| −1,600,000  | −8.0  | 6.00M | 7.60M | `node_complete` |
| −4,200,000  | −21.0 | 13.40M | 17.60M | `settle` |
| −7,200,000  | −36.0 | 0 | 7.20M | `acquire_next` (baseline-only) |
| | | | | (sum = **+25.13M**) |

**Attribution caveat (important, and it does not change the verdict).** The `Task_ptr`
refcount trio shows +26.7M gross, but that overstates the *new* work: the identical
`current_task` swap inside `run_graph_node` is inlined-and-attributed to `run_graph_node`
/`acquire_next`/`settle` **self** in the baseline build, and to the standalone `Task_ptr`
symbols in the current build (a debuginfo inlining-attribution difference between the two
binaries). That is exactly why baseline's `acquire_next` (7.2M) and `settle` (17.6M)
look inflated and current's look smaller — the same refcount instructions moved symbol.
The **inlining-independent** truths are the grand total (+25.1M) and the source diff
above; the per-symbol rows are directional, not literal.

Grouping the net delta by mechanism (this *is* inlining-robust — it nets the relocation):

- **Dispatch-machinery functions** — baseline `{acquire_next −7.2M, node_trampoline
  −1.2M}` removed vs current `{dispatch_ready +5.4M, run_block_dispatch +2.4M,
  advance_pipe_links +0.6M, graph_node_completed +0.6M}` added ⇒ **~net-neutral (+0.6M)**
  at the named-function level.
- **Refcount traffic** (`Task_ptr` trio, net of the relocation) ⇒ the **dominant
  positive**: the dispatch-hop inc/dec that baseline's raw-`Node*` trampoline never paid.
- **`global_scheduler()` per dispatch** ⇒ **+2.2M (+11/node)**: current calls
  `global_scheduler().submit(...)` on every node; baseline cached `run.scheduler->submit(...)`.
- **`advance_pipe_links` unconditional** ⇒ **+0.6M (+3/node)**: `graph_node_completed`
  calls it every completion even when `pipe_count == 0` (the object-free node case).
- **`execute` top-level** ⇒ **+1.6M (+8/node)**.
- **Offset**: cheaper `settle` + `node_complete` ⇒ **−5.8M**.

## 5. Verdict

The +125.7 instructions/node (+24.1% of pure machinery; ≈ the reported ~10% wall on
real frames once node bodies dilute it) is **the unified ownership-carrying dispatch
trampoline** introduced by coroutine-first: each ready node is now released into and
adopted back out of the scheduler queue as a reference-counted `Task_ptr` (one atomic
increment + one atomic decrement + `Task_ptr` ctor/dtor per node), where the baseline
queued a borrowed raw `Node*` and did no reference counting. Secondary, smaller
contributors: `global_scheduler()` re-resolved per dispatch (+11/node), an
unconditional `advance_pipe_links` on object-free nodes (+3/node), and the extra
`graph_node_completed`/`execute` indirection. It is **not** the `execution_flag`
self-lock (byte-identical to baseline), **not** the scheduler (identical), and **not**
the completion/settle path (measurably cheaper on current).

**Tolerable-or-fixable:** largely fixable, low-risk, and it recovers most of the delta:

1. **Cache the scheduler pointer** on the dispatch path (`run_.scheduler`) instead of
   `global_scheduler()` per node — trivial, ≈ −2.2M (−11/node, ~9% of the regression).
2. **Borrowed-pointer dispatch fast-path for object-free / zero-nested-frame functor
   nodes** — the block is owned by `Run_state` for the whole run and provably outlives
   every dispatch (the baseline relied on exactly this to queue a raw `Node*`), so the
   queue does not need to own a ref. Restoring a raw-pointer trampoline for the common
   functor-node case reclaims the refcount trio — the single big lever (~+100/node).
3. **Skip `advance_pipe_links` when `pipe_count == 0`** — ≈ −3/node.

The transformation traded per-node atomic refcount churn for one cascade serving nodes,
multi-object `async`, and coroutines. The cost is real and instruction-count-visible;
most of it is reclaimable for the graph's functor-node fast path without giving up the
unified cascade for the paths that genuinely need block ownership.
