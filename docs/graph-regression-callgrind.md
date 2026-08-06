# Graph `execute()` machinery — callgrind cost map (untracked)

Deterministic instruction-count profiling of the current `master` node-block
machinery: where `graph.execute()` spends its instructions per node, and the fat
targets a future optimization would attack. Baseline (`1652818`) diff follows as
secondary context. Tool: valgrind callgrind 3.26 (exact instruction counting), WSL
Ubuntu 26.04 / clang 21.

> Status: measurement report for review. Not committed. Delete once folded into the
> TODO / a fix lands.

## 1. Method

- **Build** (`master`, `1ff7781`), shipping-like so callgrind sees only real machinery:
  `clang++ -std=c++23 -O2 -g -fno-exceptions -pthread -DTS_SAFETY_CHECKS=0 -DTS_PROFILING=0`
  over the `tsan/run.sh` library TU set (`scheduler, worker_thread, guarded,
  static_task_graph, access, fatal`) + a driver, `-latomic` (16-byte `Task_entry`
  atomic). Harness + trace compiled out (`Trace_*_scope`, `stamps.*` are no-ops).
- **Driver**: 1000 object-free empty-lambda nodes (`add_node(ts::Named{nullptr}, []{})`),
  `compile()` once, `execute().sync()` × 200. **Worker-less scheduler**
  (`Scheduler_config{.single_threaded = true}`) → the whole machinery call tree runs
  deterministically on one thread, no worker noise. 200 000 node-executes measured.
  Empty bodies ⇒ machinery dominates.
- `valgrind --tool=callgrind --dump-instr=yes`, then `callgrind_annotate --auto=yes`.
  Deterministic: re-run reproduced 129,583,340 Ir exactly.

## 2. Headline: per-node cost of `graph.execute()`

Total program **129,583,340 Ir**, splitting into:

- **steady-state execute loop = 118,510,161 Ir ⇒ 592.6 instructions per node-execute**
  (the number that matters — pure per-node graph machinery)
- one-time setup/startup = 11,073,179 Ir (`compile()` once + dynamic-loader/PLT
  resolve + TLS init + MPMC-queue ctor + `detect_cycles`) — not per-node, ignore for
  optimization.

So **each trivial graph node costs ~593 instructions of framework machinery** end to
end (dispatch → body shim → completion → successor wake → re-arm amortized). With an
empty body, ~2.7% of that is the body invoke; the other ~97% is pure framework.

## 3. The per-node cost map (current master)

Steady-state execute loop, self-cost bucketed by phase, ins/node = Ir / 200 000:

| ins/node | % | phase | what it is |
|---:|---:|---|---|
| **133.8** | 22.6% | **`Task_ptr` reference counting** | ctor/dtor/copy/move atomics: the dispatch-hop ref released into / adopted out of the queue, plus the `current_task` swap in `run_graph_node`. ~4 atomic inc/dec + null-check scaffolding per node |
| **80.0** | 13.5% | scheduler: `submit` + `run_serial` | worker-less inline FIFO trampoline (enqueue + pop + invoke). Structural |
| **75.1** | 12.7% | **completion notify: `std::mutex` + condvar broadcast** | `settle()` takes `scoped_lock(mutex)` and calls `done_cv.notify_all()` **per node** — `pthread_mutex_lock` 31 + `unlock` 21 + `cond_broadcast`/`notify_all` 21. Nothing waits on a node's condvar (only the run's `done` is `sync()`ed) |
| **68.1** | 11.5% | `execute()` top: per-run re-arm | the `for (nodes_)` loop resets ~10 fields/node/run (`remaining_deps`, `completed`, `ready`, `num_locks`, `token` copy, flags…) + `make_bare_block()` done handle + `add_nested` |
| **67.1** | 11.3% | **`settle()` self** | constructs+destructs an empty `std::vector<move_only_function>` `conts` (+ its memset), sets flags, moves out `continuations`/`nested_parent` — all no-op payload for a graph node |
| **51.0** | 8.6% | `run_graph_node` self | the `num_locks` self-lock `store(execution_flag+1)` + `fetch_sub`, `reinterpret_cast`, cancel-check, no-op `Trace_*_scope` brackets (refcount of its `current_task` swap is in the refcount row) |
| **39.0** | 6.6% | dispatch trampoline | `dispatch_ready` → `submit_ready(Task_ptr by value)` → `run_block_dispatch(adopt)` |
| **38.0** | 6.4% | `node_complete` | successor `remaining_deps.fetch_sub` loop + `remaining_nodes.fetch_sub` + ready-successor dispatch |
| **16.1** | 2.7% | node body invoke | the empty lambda through `move_only_function::_S_invoke` |
| **11.0** | 1.9% | `global_scheduler()` per dispatch | the lock-free `g_fast` read, re-resolved on every node instead of cached |
| **9.9** | 1.7% | allocation | `make_bare_block()` done-handle malloc/free — one per **run** (0.01/node), rest is libc mem |
| **3.0** | 0.5% | `graph_node_completed` + `advance_pipe_links` | completion entry hop + pipe-link retirement (loops over zero links for object-free nodes) |

### Grouped by logical phase

- **Node completion / settle** — `settle` self 67.1 + mutex/condvar notify 75.1 +
  `node_complete` 38.0 + `graph_node_completed` 3.0 = **~183 ins/node (31%)**. The
  single fattest area, and the most removable: a graph node block has exactly one
  `on_complete` (the graph's), never external `sync()` waiters or continuations, yet
  it pays the full generic `Task_control_block` completion primitive — a `std::mutex`,
  a `condition_variable::notify_all`, and an empty continuations vector — every node.
- **Reference counting** — **~134 ins/node (23%)**. ~4 atomic inc/dec per node from
  the ownership-carrying dispatch (`Task_ptr` released into and adopted back out of the
  queue) and the `current_task` swap.
- **Scheduler + dispatch** — `submit`/`run_serial` 80 + trampoline 39 +
  `global_scheduler` 11 = **~130 ins/node (22%)**.
- **Per-run re-arm** — `execute()` top **68 ins/node (11.5%)**, amortized: ~10 field
  resets/node/run.
- **`run_graph_node` self-lock + shim** — **51 ins/node (8.6%)**.

## 4. Fat targets — what a future optimization would attack

Ordered by payoff × safety (all instruction-count, so a lower bound on wall-clock for
the atomic-heavy ones):

1. **Slim the graph-node completion path (~183/node, biggest lever).** A graph-node
   block never has external `sync()` waiters or `continuations`; it is driven solely by
   `on_complete`. Giving graph nodes a completion that skips the `std::mutex` +
   `done_cv.notify_all()` + `conts` vector — fire `on_complete` directly under the
   existing atomic `completed`/`ready` flags — would reclaim most of the 75/node condvar
   traffic and much of the 67/node `settle` self. The generic `settle` stays for
   externally-awaited tasks (`async`/`launch`/coroutines). This is the standout finding:
   the per-node **`pthread` mutex+condvar is 12.7% of the entire cost and wakes nobody**.

2. **Borrowed-pointer dispatch for functor nodes (~134/node refcount, big lever).** The
   block is owned by `Run_state` for the whole run and provably outlives every dispatch,
   so the queue need not own a `Task_ptr` ref. A raw-pointer dispatch for the common
   object-free / zero-nested-frame functor node (the baseline did exactly this) removes
   the dispatch-hop inc/dec and the `Task_ptr` scaffolding. Keep the ownership-carrying
   path for async/coroutine blocks that can outlive their launcher.

3. **Cache the scheduler pointer (~11/node, trivial).** `execute()` already stores
   `run.scheduler`; the dispatch path should use it instead of calling
   `global_scheduler()` per node. ~2% for near-zero risk.

4. **Trim the re-arm loop (~68/node, moderate).** ~10 separate relaxed stores + a
   `Cancellation_token` copy per node per run. Batch the flag resets (a single memset of
   a POD sub-block, or a packed flags word) and skip the `token` copy when the run token
   is empty.

5. **Skip `advance_pipe_links` when `pipe_count == 0` (~2/node, trivial).**

Realistic reclaim from (1)+(2)+(3) alone is on the order of ~250–300 ins/node — roughly
half the per-node machinery — for the object-free functor-node case, without touching
the async/coroutine paths.

Note on atomics: several hot phases are `lock`-prefixed RMWs (refcount inc/dec, the
`num_locks` self-lock `store`/`fetch_sub`, `remaining_deps`/`remaining_nodes`
`fetch_sub`). Uncontended here (single thread), so instruction count captures only their
issue cost; on a contended multi-worker frame their cache-coherence cost is *additional*,
so these rows understate real wall-clock weight — another reason the refcount and
completion levers are attractive.

---

## 5. Baseline diff (secondary context) — is current heavier than the old block, and where

Same driver/build on `1652818` (callback composition, `std::deque` pipe, no `ts::Named`):

| | Ir | ins/node |
|---|---|---|
| baseline `1652818` | 104,450,328 | 522.3 |
| current `master`   | 129,583,340 | 647.9 |
| **delta** | **+25,133,012 (+24.1%)** | **+125.7 ins/node** |

(Per-node here over all 200 000 node-execs incl. amortized one-time setup; §2's 592.6
is the steady-state-only figure. The two denominators differ; both are internally
consistent.)

The regression reproduces in instruction count — real retired work, not an
invisible cache/atomic effect, so callgrind is the right tool. The ~24% on this
pure-machinery microbench vs the reported ~10% wall on real frames is consistent: real
node bodies dilute the fixed ~+126 ins/node.

**Where the +126/node came from** (source-verified, not just symbol names):

- **NOT the `execution_flag` self-lock.** `run_graph_node` is **byte-identical**
  between the commits (same `num_locks.store(execution_flag+1)` / `fetch_sub` /
  `complete()`; baseline only carries an extra `gen` param). That path predates
  coroutine-first — the leading hypothesis is false.
- **NOT the scheduler** (`submit`/`run_serial` self-costs identical) and **NOT the
  completion self/`node_complete`**, which are actually *cheaper* on current (settle
  self −4.2M, node_complete −1.6M — partly refcount relocating out; see below).
- **It is the unified ownership-carrying dispatch trampoline.** Baseline queued a
  **borrowed raw `Node*`** (`run_node → submit(&node_trampoline, &node) → node_trampoline
  → block->execute`, zero reference counting). Current routes every node through the
  generic block path (`dispatch_ready → submit_ready(Task_ptr by value = atomic INC) →
  run_block_dispatch(adopt + dtor = atomic DEC) → execute`), adding +1 atomic inc + 1
  dec + `Task_ptr` ctor/dtor per node. Secondary: `global_scheduler()` per dispatch
  (+11/node) vs baseline's cached `run.scheduler`; unconditional `advance_pipe_links`
  (+3/node); `execute`/`graph_node_completed` indirection.

Attribution caveat: the `Task_ptr` refcount trio shows +26.7M gross self, but part is
debuginfo inlining-attribution relocation — the identical `current_task` swap is
attributed to `run_graph_node`/`acquire_next`/`settle` **self** in the baseline binary
and to standalone `Task_ptr` symbols in the current one (which is why baseline's
`acquire_next` 7.2M and `settle` 17.6M look inflated). The inlining-independent truths
are the grand total (+25.1M) and the source diff. The mechanistic net, grouping to
cancel the relocation: dispatch-machinery functions are ~net-neutral, the refcount
inc/dec on the dispatch hop is the genuine new cost, plus `global_scheduler` (+2.2M)
and `execute` (+1.6M), offset by cheaper settle/node_complete (−5.8M).

The baseline finding aligns with the current-version map: the biggest *absolute* cost
today (node completion, ~183/node) is largely present in both versions — it is
inherent to the generic `Task_control_block` completion primitive, not the regression.
The regression proper is the +126/node of ownership-refcount the transformation added;
the completion path is the larger standing target that predates it. Both are addressed
by items 1–2 in §4.
