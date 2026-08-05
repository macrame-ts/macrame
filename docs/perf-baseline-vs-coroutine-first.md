# Performance & footprint: pre-transformation baseline vs coroutine-first

Honest before/after of the task-system library across the coroutine-first + evolved-pipe
transformation.

- **Baseline (before):** `1652818` — callback composition (`then`/`when_all`/task builders/
  `after`), retraction, reusable tasks, the `std::deque`-based pipe. This is `origin/master`.
- **Current (after):** `1a48e06` — tip of local `master`, 96 commits ahead. Coroutine-first
  (`co_await`, retraction removed), evolved pipe.

`master` descends from `1652818` (`git merge-base --is-ancestor` confirmed). All builds and
runs done in an isolated worktree, one build's heavy work at a time, on the same quiet
machine (22 hw threads, MSVC `clang-cl`/`cl` toolchain 14.51, VS 18).

Method: Release x64, each benchmark = the in-tree harness (4 measured reps × 200 ms target,
median reported with min/max spread). Each `--bench` run repeated 3×; the tables use the
median run's median. Footprint from a `TS_MEM_PROFILE` build (global `operator new`
counters + `sizeof`). "M/s" higher = better; "ns/op" lower = better.

---

## 1. Build status

| Commit | Config | Result | Notes |
|---|---|---|---|
| `1652818` baseline | Release x64 | **Clean**, ~55 s, exit 0 | No warnings. Builds on the current toolchain with **zero surgery** — a positive finding. |
| `1a48e06` current | Release x64 | **Clean**, ~76 s, exit 0 | No warnings. |

**Finding (1a):** the *default* builds of both commits are clean. The only build that needed
a fix was the dev-only allocation profiler (`src/mem_profile.cpp`, gated behind
`TS_MEM_PROFILE`, excluded from normal builds). On **current**, its `TS_MEM_PROFILE` branch
had rotted against two API changes made during the transformation and would not compile:
- `Guarded<int> g{ 0 }` — the ctor now requires a leading `ts::Named` (`{ ts::Named{}, 0 }`).
- `graph.add_node(fn, obj)` — `add_node` now requires a leading `ts::Named`/`{}`.

Three-line mechanical fixes (measurement-only, applied in-worktree, then discarded) got it
building. The baseline profiler compiled unmodified. Worth repairing the current profiler in
tree so it doesn't bit-rot further.

**Method note (1b):** to time the retraction micro (below) the baseline needed a
`bench_launch_sync` that baseline's suite lacked — I added the *identical* body current
already has (`ts::launch([]{return 1;}).sync()`) to the baseline benchmark file only, a
measurement-harness addition that does not touch library behavior. The current build was left
unmodified.

---

## 2. Common workloads (present and comparable at both commits)

Same benchmark source at both commits (byte-identical for the scheduler set; `fork-join` is
literally identical). Median ns/op (lower = better), median of 3 runs.

| Workload | Baseline ns/op | Current ns/op | Δ | Verdict |
|---|---|---|---|---|
| throughput, spin | 405 | 375 | −7% | wash (both ~noisy ±15%) |
| throughput, s+block | 488 | 471 | −3% | wash |
| throughput, handoff | 518 | 511 | −1% | wash |
| wake latency, spin | 2285 | 2067 | −10% | wash (spin wake very noisy) |
| **wake latency, s+block** | 655 | 728 | **+11%** | **current slower** |
| wake latency, handoff | 637 | 739 | +16% | current slower (noisy) |
| contention (22 prod), spin | 380 | 369 | −3% | wash |
| contention, s+block | 409 | 415 | +2% | wash |
| contention, handoff | 411 | 413 | 0% | wash |
| fork-join (raw submit), spin | 332 | 336 | +1% | wash |
| fork-join, s+block | 368 | 370 | +1% | wash |
| fork-join, handoff | 371 | 368 | −1% | wash |
| ts_write (async write) | 1180 | 1161 | −2% | wash |
| **ts_read (async read)** | 1117 | 840 | **−25%** | **current faster** |
| **graph execute** (3-node) | 5500 | 6026 | **+10%** | **current slower** |
| harness (`TS_CHECK`) | 1.3 | 1.3 | 0% | wash |

**Readings:**
- **2a. Scheduler core is a wash.** Raw throughput, contention, and worker→worker fork-join
  are within run-to-run noise across all three idle policies. The transformation did not move
  the scheduler's submit/dispatch cost. (`fork-join` here is the raw `sched.submit` fan-out —
  no task blocks, no retraction — so it isolates scheduler throughput, unchanged.)
- **2b. Async read is materially faster (−25%)** and **allocates half as much** (§4) — the
  evolved pipe's win, consistent across all runs.
- **2c. Graph execute regressed ~10%**, consistently (baseline 5462–5579 ns, current
  5855–6062 ns), despite one fewer allocation and 8 fewer bytes per run. Small absolute
  cost, but real and reproducible; worth a look.
- **2d. Wake latency on the blocking policies (`s+block`, `handoff`) regressed ~10–16%.**
  Noisier than the throughput series but the `s+block` regression held across runs.

---

## 3. Workloads that changed shape (reconstructed comparisons)

These are **not identical benchmarks** — the old callback expression was removed and the
coroutine expression added. Closest honest logical match, same logical work each way.

### 3a. Chain of N dependent steps — `then` chain vs coroutine chain

- **Baseline (`then`):** one producer, then 50× `.then([](int v){ return v+1; })`, one
  `sync()`. 50 chained continuations.
- **Current (`coro chain`):** `for k in 0..50: v = co_await ts::launch([v]{ return v+1; })`.
  50 sequential awaited launches.

| | Baseline `then` | Current `coro chain` | Δ |
|---|---|---|---|
| ns per dependent step | 1424 | 2036 | **+43%** (current slower, multi-worker) |

Caveat: not the same mechanism. The coroutine chain awaits 50 *separate scheduled launches*
and pays a cross-worker resume per stage when work migrates. The suite's decomposed section
shows this clearly: the same coroutine chain **pinned to 1 worker is ~340 ns/stage**, vs
~1490–1580 ns/stage on the full pool. The per-step regression is work-migration + resume
round-trip, not raw overhead. Fewer allocations though (§4).

### 3b. Join of N prerequisites — `when_all` vs coroutine join

- **Baseline (`when_all`):** `when_all(4× async read).then(sum).sync()`.
- **Current (`coro join`):** `co_return co_await t0 + co_await t1 + co_await t2 + co_await t3`
  over 4 async reads.

| | Baseline `when_all` | Current `coro join` | Δ |
|---|---|---|---|
| ns per 4-way join | 5245 | 4627 | **−12%** (current faster) |

The coroutine join is both faster and **less than half the allocations** (§4). This is the
clearest reconstructed win.

---

## 4. Footprint

From the `TS_MEM_PROFILE` build (allocs/op and bytes/op charged through global
`operator new`; op = one full measured closure). Join/chain rows use fan-in 2 in the
profiler (vs 4 in the timed bench).

| Metric | Baseline | Current | Δ |
|---|---|---|---|
| **`sizeof(Task_control_block)`** | 296 B | 288 B | −8 B (−3%) |
| launch (`launch().sync()`) | 1.00 allocs / 592 B | 1.00 allocs / **304 B** | bytes **−49%** |
| async write | **2.00 allocs** / 408 B | **1.00 alloc** / 352 B | allocs **−50%** |
| async read | **2.00 allocs** / 408 B | **1.00 alloc** / 352 B | allocs **−50%** |
| dependent step (`then` vs `coro chain`, 1 link) | 5.01 allocs / 744 B | **2.87 allocs** / 1144 B | allocs −43%, bytes +54% |
| join (`when_all` vs `coro join`, 2-way) | **9.07 allocs** / 1527 B | **3.80 allocs** / 1515 B | allocs **−58%** |
| parallel_for(64) | 1.00 alloc / 608 B | 1.00 alloc / 664 B | +56 B |
| graph execute (3-node) | 1.00 alloc / 296 B | 1.00 alloc / 288 B | −8 B |

**Readings:**
- **4a. The allocation story is the transformation's strongest result.** `async` halved its
  allocs (2→1); the join went 9.07→3.80 allocs; the control block shrank slightly. `launch`
  keeps its single alloc but nearly halves the bytes.
- **4b. Coroutine frames trade allocation *count* for *bytes*.** The `coro chain` step
  allocates fewer times but ~54% more bytes (the coroutine frame). Net: fewer heap round
  trips, larger blocks.
- **4c. Game-frame allocs/frame is not comparable across commits** — the baseline profiler
  never measured a frame (its `--memprofile` ends at `graph execute`), and the game_frame
  sample itself was heavily rewritten during the transformation (the whole optimised-variant
  work). Current frame footprint, for the record: graph 43.2 allocs/22.8 KB, graph-free
  140.6 allocs/56.1 KB per frame. No honest baseline counterpart exists.

---

## 5. The specific trade: removal of retraction

Confirmed in source: baseline has the full retraction machinery (`retract_or_wait`,
`E_retract_exec`, `retractable`, deep-subtree inline execution); **current has zero
occurrences of `retract`** — it is entirely removed. The old design ran a not-yet-started
retractable task **inline on the blocking waiter**; the new design **suspends** via
coroutines instead of blocking.

### 5a. The pure retraction micro — `launch().sync()`

Single bare task launched and immediately `sync()`ed. On baseline the waiter *retracts* the
unstarted task and runs it inline (no scheduler round trip); on current the task is scheduled
to a worker and the waiter parks until it settles. Identical op both sides.

| | Baseline (retraction, inline) | Current (schedule + park) | Δ |
|---|---|---|---|
| ns/op (steady runs) | ~957 (runs: 1225 / 954 / 957) | ~1222 (runs: 1205 / 1253 / 1223) | **+28% current** |

This is the cost of removing retraction, isolated: ~265 ns of extra scheduler round-trip +
park/unpark on a blocking wait for one unstarted task. Retraction was measurably faster here.

### 5b. Why the trade is not a straight regression

- The fork-join **join** path (§3b) — the actual multi-child pattern — is **faster** under
  coroutines (−12%) and allocates far less (§4, 9.07→3.80). Coroutines join by suspension,
  not by a blocking waiter that needs retraction to stay live.
- Retraction existed primarily to **avoid deadlock under worker exhaustion** (a nested
  fork-join whose blocking `sync()`s could occupy every worker). Coroutines make that
  scenario structurally impossible: a `co_await` releases the worker instead of blocking it,
  so there is no waiter to run work inline *for*. The −28% on 5a buys the removal of an
  entire class of blocking-wait hazard and the retraction complexity that guarded it.

**Verdict (5c):** removing retraction is a **real but small latency regression on one
narrow path** — a top-level blocking `sync()` on a single unstarted task (~+28%, ~265 ns).
It is *not* a regression on the fork-join/join pattern retraction was built to protect, which
is faster and lighter under coroutines. The trade is "slightly slower single blocking-launch
latency" bought with "no worker-exhaustion deadlock class, fewer allocations, simpler core."

---

## 6. Bottom line

On the measured evidence, **current is a net improvement, concentrated in allocation
footprint and the async/join paths, with two small, real, isolated time regressions.**

**Faster / smaller (measured):**
- Async read −25% time and −50% allocations; async write −50% allocations.
- 4-way join −12% time and −58% allocations.
- Control block −8 B; `launch` bytes −49%.
- Scheduler core (throughput / contention / raw fork-join): unchanged (wash) — the
  transformation did not cost scheduler throughput.

**Slower (measured, small, real):**
- `launch().sync()` single blocking wait +28% (~265 ns) — the retraction removal, §5.
- Graph execute +10% (~525 ns), consistent, §2c.
- Wake latency on blocking idle policies +10–16%, §2d.
- Reconstructed dependent-chain per step +43% under multi-worker migration (but −77% pinned
  to one worker; expression changed, §3a).

**Slower-but-bought-safety/simplicity (not a raw regression):**
- The retraction removal (§5): the single-launch latency cost buys elimination of the
  worker-exhaustion deadlock class and the retraction machinery, and the pattern it protected
  (fork-join/join) is faster and lighter under coroutines.

No result shows a large or broad regression. The two genuine time regressions to keep an eye
on are **graph execute (+10%)** and **blocking-wake latency (+10–16%)**; both are small in
absolute terms and neither is on the async/join hot paths that improved.

---

### Appendix — raw run medians (ns/op)

Baseline: throughput spin 354/405/440, s+block 459/488/546, handoff 442/550/518; wake spin
1904/2626/2285, s+block 632/655/663, handoff 637/657/613; contention spin 380/365/392,
s+block 416/409/404, handoff 411/402/417; fork-join spin 332/333/332, s+block 364/372/368,
handoff 370/371/374; launch 1225/954/957; ts_write 1185/1156/1180; ts_read 1117/1072/1616;
then 1424/1317/1431; when_all 5245/5168/5563; graph 5500/5462/5579.

Current: throughput spin 334/375/448, s+block 461/471/551, handoff 511/510/591; wake spin
1367/2068/2181, s+block 728/715/809, handoff 1192/720/739; contention spin 367/369/390,
s+block 412/415/419, handoff 397/413/413; fork-join spin 318/349/336, s+block 357/377/370,
handoff 359/381/368; launch 1205/1253/1223; ts_write 1273/1153/1161; ts_read 919/841/818;
coro chain 2148/1712/2036; coro join 4627/4804/4598; graph 6026/6062/5855.
