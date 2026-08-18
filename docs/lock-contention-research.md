# Lock contention in production: research, the generalized case, and the bench

*Research compiled 2026-08-18 (two web sweeps: infrastructure/runtimes and
application software; 41 citable cases). Purpose: ground the library's
contention story in what actually happens in production — how big the problem
is, what shape it takes, which fixes worked — and derive from it a
representative benchmark (`benchmarks/production_contention_bench.cpp`) plus an
honest map of where this library helps and where it does not.*

Companion: [task-systems-comparison.md](task-systems-comparison.md) (the
scheduler-level survey). Sections numbered for reference.

---

## 1. How big a problem is it?

Big, pervasive, and usually invisible until wait-profiled. Quantified damage
from first-party reports:

| System | Number | Source |
|---|---|---|
| memcached (Twitter) | lock functions = 4.2% of CPU at 4 threads, 19.1% at 8, **60% at 16** | [twemcache wiki](https://github.com/twitter/twemcache/wiki/Impact-of-Lock-Contention) |
| .NET `HttpConnectionPool` | **57% of CPU** acquiring one pool lock under load | [dotnet/runtime #70098](https://github.com/dotnet/runtime/issues/70098) |
| Linux futex bucket locks | ~**60% of the wakeup path** waiting on `hb->lock` (240-core NUMA) | [LWN](https://lwn.net/Articles/685769/) |
| InnoDB `kernel_mutex` | **2.8x throughput** recovered by admission control alone (256 threads -> 8) | [Percona](https://www.percona.com/blog/kernel_mutex-problem-cont-or-triple-your-throughput/) |
| PostgreSQL `ProcArrayLock` | **-57% TPS** from 10,000 *idle* connections (snapshot scan under the lock) | [Citus/Freund](https://www.citusdata.com/blog/2020/10/08/analyzing-connection-scalability/) |
| glibc malloc arenas | ~2x throughput drop at high thread counts vs jemalloc/tcmalloc; 2x RSS | [Percona](https://www.percona.com/blog/impact-of-memory-allocators-on-mysql-performance/), [Meta](https://engineering.fb.com/2011/01/03/core-infra/scalable-memory-allocation-using-jemalloc/) |
| Unity allocator lock | **40x anti-scaling**: 0.06 ms/frame at 2 workers -> 2.41 ms at 5 | [Schöner](https://blog.s-schoener.com/2025-01-21-unity-vfx-graph-multithread/) |
| kernel `mmap_lock` | **multi-second serving latencies** at Google fleet scale; Android GC threads in uninterruptible sleep >20 s | [LWN per-VMA locks](https://lwn.net/Articles/924572/) |
| Firefox NSS slot lock | mutex wait 7.1% -> 9.3% of active time, workers at **2.7x less throughput** (a leak grew the under-lock scan) | [Bugzilla 2046401](https://bugzilla.mozilla.org/show_bug.cgi?id=2046401) |
| Go RWMutex (AnyCable) | a **14-minute production freeze**: 820 goroutines behind a write lock held across a blocking call | [Evil Martians](https://evilmartians.com/chronicles/what-could-go-wrong-with-a-mutex-or-the-go-profiling-story) |
| Netflix, Java 21 virtual threads | silent instance **hangs**: carriers pinned by `synchronized` in third-party libs; JDK 24 fix cited at +107% throughput / -64% p99 | [Netflix](https://noise.getoto.net/2024/07/29/java-21-virtual-threads-dude-wheres-my-lock/), [InfoQ](https://www.infoq.com/articles/virtual-threads-after-jdk24/) |
| LMAX (trading) | one uncontended lock made a 500M-increment counter **33x** slower; 2-thread contention **~750x** | [Disruptor paper](https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf) |
| ClickHouse parts mutex (Cloudflare) | >half of query duration waiting on one exclusive mutex whose critical section grew with data shape | [Cloudflare](https://blog.cloudflare.com/clickhouse-query-plan-contention/) |
| UE `UObjectHashTables` lock | game-thread hitches: loader holds the registry lock across a recursive preload for hundreds of ms | [Epic forums](https://forums.unrealengine.com/t/async-loading-stutter-in-large-plas-due-to-scs-preload-and-uobjecthashtables-lock/2688173) |

Three structural findings recur across all 41 cases:

1. **The hot lock guards a process-wide singleton service** — registry, pool,
   queue, cache, metrics aggregate, allocator — on the path of every
   request/frame/allocation.
2. **It is frequently a lock nobody chose** — inside the allocator, the stdlib
   (`java.util.Random`, Go `math/rand`), the classloader, a logging framework,
   the kernel's futex table, a third-party `synchronized`. Contention crosses
   component boundaries (your locks contend inside someone else's hash bucket).
3. **It is routinely misdiagnosed as a CPU problem** — found only by off-CPU /
   wait tooling (thread-state views, mutex profiles, monitor-contention
   events). Every mature ecosystem grew a lock profiler for this reason.

## 2. The two disease classes

The evidence splits cleanly:

- **Class 1 — steady saturation.** Tiny critical sections (sub-us) at very
  high frequency alone hit the ceiling: throughput flattens or *falls* as
  threads are added, CPU burns on handoffs/spinning, mean and tail degrade
  together. memcached, allocator arenas, `ProcArrayLock`, `math/rand`, futex
  buckets, pool borrow/return.
- **Class 2 — the convoy / tail class.** A **rare slow operation under the
  same hot lock** — rehash, eviction, flush, a scan that grew O(n) with state,
  a recursive preload, or simply the holder being preempted — stalls a burst
  of tiny touches. p99/p999 detaches while the mean looks fine; the extreme
  form is a hang. ClickHouse, UE registry, Firefox NSS, Log4j flush, buffer
  pool eviction, plan-cache eviction, AnyCable, Windows lock convoys.

Class 1 caps throughput; Class 2 produces the dramatic incidents. A hot lock
usually carries both diseases at different times.

## 3. Hypothesis verdicts (what a representative model must include)

Tested against the case set:

| # | Hypothesis | Verdict |
|---|---|---|
| a | Hot lock protects a lookup-mostly shared structure | **Confirmed**, refined: the unifying property is *singleton-ness*, not read-mostly-ness (pools/queues are all-write). Corollary: **the read path often secretly writes** (LRU bump, cache-on-lookup, seed advance) — which is what defeats the obvious RW-lock fix. |
| b | Critical sections tiny but very frequent | **Confirmed** as the baseline. Amendment: the worst incidents are when the "tiny" section silently grew with data shape (O(size-of-structure) under the lock is a time bomb). |
| c | The killer is a rare slow op under the same lock | **Confirmed** for Class 2 — one of two diseases, not the only one. |
| d | Threads >= cores when it bites | **Confirmed as the amplifier**: convoys formally require a runnable competitor to preempt the holder (fair locks then give the convoy hysteresis). Not strictly necessary — coherence traffic alone bites at high core counts. |
| e | Bursty arrivals amplify | **Confirmed wherever arrival shape is documented**: bursts synchronize arrivals so the queue on the lock never drains between them; frame-boundary fan-out is the purest form. Steady load finds the ceiling; bursts find the convoy. |

## 4. The fix hierarchy production actually used

In descending observed preference, consistent across every domain:

1. **Stop doing the work under the lock** — cache a snapshot, defer/batch the
   copy, move the slow op out (ClickHouse rounds 2-3, PG14, zone-lock
   batching).
2. **Privatize per thread/core** — allocator thread caches, HikariCP
   thread-local bags, `rand/v2` per-P state, per-CPU page lists.
3. **Shard/stripe by key or instance** — arenas, buffer-pool instances,
   per-VMA locks, per-process futex tables, metric-name shards. (Cloudflare's
   accept-queue data adds the caveat: sharding re-imports burst sensitivity as
   queue imbalance.)
4. **Asymmetric protocols for read-mostly** — RCU/pointer-swap (Uber
   geofence), hot/cold buffer flip (Prometheus), seqlocks (trading).
5. **Async offload to a serial consumer** — Disruptor-based logging, buffered
   metric senders. Relocates the serialization to where it is inherent.
6. **Admission control** when restructuring is off the table
   (`innodb_thread_concurrency`: 2.8x).

RW locks appear mostly as a *first* fix that later proves insufficient
(secretly-writing reads; ClickHouse needed two more rounds).

## 5. The generalized case, and the bench that models it

> Parallel work items (threads >= cores, arriving in bursts/waves) each
> perform private work plus one or two very short exclusive touches of a
> shared singleton — which occasionally executes a much longer maintenance
> operation under the same lock.

`benchmarks/production_contention_bench.cpp` models exactly this, with pure
mocks (plain `std::thread` pool, one `std::mutex`, spin work — deliberately no
library code: it is the baseline exhibit any library-side comparison must
measure against). Elements -> evidence mapping:

| Bench element | Grounding |
|---|---|
| one hot singleton lock | finding 1 of §1 |
| ~200 ns touch amid ~8 us private work (offered load < 1) | §3(b): the healthy sub-saturated baseline that ships every day |
| rare ~100 us slow op under the same lock (`1/1024`, `1/256`) | §2 Class 2 / §3(c) — the element that separates the two diseases |
| 256-item waves (burst release, per-item jitter within the wave) | §3(e): bursts synchronize arrivals; jitter keeps contention workload-shaped, not lockstep |
| threads = cores and = 2x cores | §3(d): oversubscription turns waiting into convoying |
| metrics: throughput + p50/p99/p999 + lock-wait share of thread-time | the units production postmortems are written in ("57% of CPU", "p99 spikes", hitches) |

Expected signature if the model is faithful: `none` rows healthy (modest wait
share); `1/1024` moves p999 but barely moves throughput (Class 2's "mean looks
fine"); `1/256` drags p99 along and raises wait share toward the §1 numbers;
`2x` rows show whether convoying worsens the tail at flat throughput.

Measured (2026-08-18, 22 hw threads, 1024-item waves; core bound ~2.68 M/s):

| row | M items/s | p50 / p99 / p999 (us) | lock-wait share |
|---|---|---|---|
| 1x none | 0.97 | 8.7 / 12.9 / 20.6 | 2.3% |
| 1x /1024 | 1.11 | 9.5 / 133.5 / 559.5 | 28.2% |
| 1x /256 | 0.71 | 10.3 / 247.3 / 455.5 | 53.7% |
| 2x none | 1.34 | 9.8 / 316.7 / 631.5 | 35.4% |
| 2x /1024 | 0.90 | 9.9 / 643.7 / 1071.9 | 44.8% |
| 2x /256 | 0.57 | 11.4 / 664.3 / 1208.6 | 60.6% |

Every predicted signature reproduced: the healthy baseline (2.3% wait, tight
tail); the Class-2 detachment (`1/1024`: p50 +0.8 us while p999 is 27x and
throughput does not drop - the "mean looks fine" incident shape); the drag
into Class 1 (`1/256`: 54% wait share - the memcached/.NET territory of §1);
and the oversubscription trade in both directions (`2x none` buys +38%
throughput - extra threads absorb OS noise and wave stragglers - and sells a
25x p99; with the slow op it loses on both axes, wait share 60.6%). A model
note: longer waves lengthen sustained bursts and the convoy has more time to
build - wait shares roughly double from 256-item to 1024-item waves at equal
offered load, which is §3(e)'s burst-hysteresis effect made visible.

## 6. Can this library help this case? (honest map)

The striking result of the research: the fixes production teams hand-built
are, nearly one-for-one, this library's primitives.

| Production fix (§4) | Library form |
|---|---|
| move work out of the lock + batch | `Deferred` staging: touches become grant-free stages (~30 ns measured), one serialized apply per batch |
| privatize per thread | `Parallel_recorder` per-worker journal lanes |
| asymmetric read protocol | `Versioned`: readers never wait; the flip is the pointer-swap, packaged |
| async offload | staging + one apply task (the consumer is scheduled work, not a thread parked on a queue lock) |
| admission control | frame/wave structure + bounded in-flight, native to the graph model |

Structural consequences, stated as claims a future companion bench must test:

- **Class 2 is eliminated, not improved**: the slow op becomes the apply body —
  scheduled work no worker queues behind. Its cost converts from stalls into
  *staleness* (next batch visible later). The preempted-holder variant also
  vanishes (no holder mid-touch to preempt).
- **Class 1 becomes amortization**: the singleton's serial lane is paid once
  per batch (apply at 1-3 ns/element, measured) instead of a lock handoff per
  touch.
- **The amplifiers are defused**: workers never park on data access, so
  workers = cores and the oversubscription/convoy question leaves the design
  space; bursts of stages synchronize on nothing.
- **The secretly-writing read** (LRU bump) — the RW-lock defeater — splits
  cleanly: read the `Versioned` snapshot, stage the bump.

Where the library helps less or not at all — stated with equal weight:

1. **Synchronous read-modify-write against current state** (pool borrow, ID
   allocation): staging cannot serve it. Answers: per-worker sharding (user
   side), the id-reservation pattern (`sample/physics.cpp`), or awaited
   `access` as the residual — measured at ~140-240 ns/op with admission
   bounded. This is the weakest mapping; HikariCP-class bespoke designs remain
   superior here.
2. **Locks nobody chose** (allocator/stdlib/third-party): out of reach — and
   today's library itself allocates per task/access, leaning on the allocator
   lock (mitigations: jemalloc-class allocators; the pooling/arena and
   `Access_op` roadmap).
3. **Read-your-writes within a frame**: the staged model's semantic price;
   code needing immediate visibility of its own write needs the grant-holding
   forms.
4. **Re-spelling the lock is not the fix**: mechanically replacing per-touch
   mutexes with per-touch `access` was measured (2026-08-18 exploratory
   benches, since removed; numbers preserved in TODO 1.18/3.9) as a
   regression — a ~1.7x composition-overhead floor and breadth-first latency
   pathologies under unbounded floods. The library helps by *restructuring*
   the touch (stage/snapshot), not by renaming the lock.

## 7. Follow-ups this research feeds

- The companion bench: same workload with touches staged
  (`Parallel_recorder` + wave-end commit) and read-touches on `Versioned` —
  testing §6's claims against the §5 baseline.
- TODO 1.18 (pipe turn tail-chaining) and TODO 3.9 (2x-worker collapse) — both
  born from the exploratory benches this research superseded.
- The `Access_op` redesign (caller-owned operation state) — removes the
  per-touch allocation that dominates the library-side cost in every
  fine-grained comparison.
