# Retraction vs. pool-exhaustion deadlocks — prior-art survey

Evidence that the deadlock class our **task retraction** (`docs/internals/task-internals.md` §6)
is designed to prevent is a recurring, well-documented failure in every comparable
work-stealing/thread-pool system. Retraction lets a thread that blocks on a task run
that task's un-started dependency subtree *inline on the waiting thread* (claimed via
the §4 CAS), so a waiter under worker exhaustion makes progress instead of parking a
worker that the awaited work needs. This doc collects the reported problems, the
vendors' own hazard warnings, and the mitigations each ships — and states precisely
which of those cases retraction removes and which it does not. It also catalogs the
hazard family that the peer *mitigation* — general cooperative busy-wait — brings with
it (§3): the reason retraction runs only the waiter's own dependency subtree and nothing
else. The two failure modes are the two horns of the same dilemma: **park** and you risk
pool-exhaustion deadlock (§1–2); **busy-wait on arbitrary work** and you inherit the
reentrancy/TLS/stack/priority/latency family (§3). Retraction takes neither horn for
retractable task work, and converts the non-retractable residue into a diagnosed misuse.

Compiled 2026-07. Sources at the end; issue numbers are stable, quoted text is from
the linked pages.

## 1. The hazard class: thread-starvation (pool-exhaustion) deadlock

All the failures below are one bug: a task running on a bounded pool **blocks waiting
for other tasks that need a thread from the same pool to run**. If enough workers do
this at once, every worker is parked waiting for work that can only run on a parked
worker. The canonical statement is SEI CERT **TPS01-J, "Do not execute interdependent
tasks in a bounded thread pool"**:

> Thread-starvation deadlock occurs when currently executing tasks submit other tasks
> to a thread pool and wait for them to complete and the thread pool lacks the capacity
> to accommodate all the tasks at once.

Fork-join, nested parallelism, "submit N and join," `sync()`/`get()`/`wait()` inside a
task, and "hold a lock, then call a parallel algorithm" are all instances. Retraction
targets exactly the sub-case where the awaited work is **un-started task work in the
same scheduler**: the waiter runs it rather than depending on a free worker existing.

## 2. Per-library evidence

### 2.1 Taskflow (C++, work-stealing)

- **Reported:** [#506 "Deadlock when using nested tasks + semaphore"](https://github.com/taskflow/taskflow/issues/506)
  — the executor "forgets" to run the innermost task while the other workers sit
  waiting; [#402 "Deadlock when running subflows in a pipeline"](https://github.com/taskflow/taskflow/issues/402)
  — workers stuck in futex wait.
- **Their own docs warn** ([Executor cookbook](https://taskflow.github.io/taskflow/ExecuteTaskflow.html)):
  > Blocking a worker can cause deadlock if all workers are waiting for their taskflows
  > to complete without making any progress.
- **Mitigation shipped:** `tf::Executor::corun` / `corun_until` — the worker
  "is cooperatively blocked and continues to execute the taskflow alongside other tasks
  in the executor's work-stealing loop." This is *general* cooperative busy-wait
  (runs arbitrary in-executor tasks), requires the user to rewrite `wait()` → `corun`,
  and is callable only from a worker thread.
- **vs. retraction:** retraction fires automatically on `sync()`/`take()`, needs no API
  change, and runs *only the awaited subtree* (no arbitrary work → no priority
  inversion / stack growth). It also works from a non-worker caller (the retractor is
  whatever thread blocked).

### 2.2 oneTBB (C++, steal-oldest work-stealing)

- **Reported:** [#1316 "Facing Deadlock issue with nested TBB"](https://github.com/uxlfoundation/oneTBB/issues/1316)
  (nested parallel regions with thread constraints); [#353 "Deadlock between std::thread,
  TBB TaskGroup, and OpenMP"](https://github.com/uxlfoundation/oneTBB/issues/353);
  [#1903 "Integrating TBB into application with existing thread-pool"](https://github.com/uxlfoundation/oneTBB/issues/1903).
- **Their own docs warn** ([Work Isolation](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/work_isolation.html)):
  a thread blocked at a nested `parallel_for` may **steal an unrelated outer task** and
  then not return to release the inner wait — a "moonlighting" deadlock. `this_task_arena::isolate`
  restricts a blocked thread to steal only within its isolation region.
- **Mitigation shipped:** work isolation (`isolate`) + arenas. Opt-in, and it is the
  *general busy-wait* discipline (still executes arbitrary tasks, just fewer of them) —
  the user must know to wrap the blocking construct.
- **vs. retraction:** TBB's hazard is a *direct consequence* of general busy-wait (a
  blocked thread executes unrelated work and never comes back). Retraction never
  executes unrelated work, so the moonlighting class does not arise; no arena/isolation
  ceremony is needed.

### 2.3 Rayon (Rust, Cilk-lineage work-stealing)

- **Reported:** [#592 "Using rayon under a Mutex can lead to deadlocks"](https://github.com/rayon-rs/rayon/issues/592)
  — a lock held across `par_iter().collect()` recurses under work-stealing;
  [#1174 "Thread pool without work stealing"](https://github.com/rayon-rs/rayon/issues/1174)
  — users request a non-stealing pool specifically to avoid this deadlock;
  [#751 "stack overflow in nested ThreadPool.scope"](https://github.com/rayon-rs/rayon/issues/751);
  a real downstream outage ([Lorikeet #1](https://github.com/rhysnewell/Lorikeet/issues/1)).
- **Their own docs / ecosystem warn:** nested `par_iter` is flagged "death by deadlock";
  the guidance is "don't call Rayon under a mutex / use separate pools"
  ([Avoiding Rayon Thread Pool Exhaustion](https://imfeld.dev/writing/avoiding_rayon_thread_pool_exhaustion)).
  Rayon added a recursion guard in `par_bridge` to blunt one instance.
- **vs. retraction:** the *pure* fork-join exhaustion case (nested `join`/`scope`) is what
  retraction removes. The **lock-across-parallel-region** case (#592) is a different
  animal — we design it out at a different layer: `Guarded` replaces ad-hoc mutexes, the
  access harness catches undeclared access, and blocking under an access scope is
  diagnosed (§3). Retraction alone would not fix an ad-hoc `std::mutex` held across a
  parallel call; our answer to that is the access model, not retraction.

### 2.4 Java `ForkJoinPool` (the closest peer — see also `docs/internals/task-internals.md` §6 discussion)

- **Reported pattern:** blocking inside the common pool (parallel streams, nested
  `CompletableFuture.join()`) exhausts it — "all worker threads switched to WAIT after
  `join()`, no worker left to run the work." This is the single most common
  production incident for the framework.
- **Their own docs / rules warn:** SEI CERT **TPS01-J** (above); the
  [`ManagedBlocker`](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/concurrent/ForkJoinPool.ManagedBlocker.html)
  Javadoc exists precisely to keep a blocking join from starving the pool.
- **Mitigation shipped:** two of them. (a) **Helping** — `ForkJoinTask.join()` first tries
  `tryRemoveAndExec` (pop and run the joined task itself; this *is* single-task retraction)
  then helps the stealer; (b) **`ManagedBlocker`** — *compensate* by spawning/activating a
  spare thread so the pool keeps its target parallelism while a worker blocks.
- **vs. retraction:** FJP is the one system whose join already retracts the specific
  awaited task, and even walks a subtree for `CountedCompleter`. Differences: FJP reaches
  the subtree by helping the *stealer's* deque rather than walking declared
  `prerequisites`; its fallback (`ManagedBlocker`) is oversubscription (spare threads),
  which retraction avoids; and it relies on the GC for the reclamation-safety our
  single-refcounted-block layout provides structurally.

### 2.5 .NET TPL (managed, ThreadPool + work-stealing local queues)

- **Reported:** [dotnet/roslyn #13611 "Task.Result and Task.Wait design flaw leads to
  deadlock in high loaded applications"](https://github.com/dotnet/roslyn/issues/13611)
  — sync-over-async on a pool thread deadlocks under load.
- **Their own docs warn:** [Microsoft Learn — Debug ThreadPool Starvation](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/debug-threadpool-starvation);
  [VS-threading — Investigating thread starvation](https://microsoft.github.io/vs-threading/docs/threadpool_starvation.html).
  The blocking thread waits for work that needs a thread; the pool grows only ~1
  thread/second, so a burst starves it.
- **Nuance on inlining:** .NET *has* `TaskScheduler.TryExecuteTaskInline`, and `Task.Wait`
  will inline the awaited task **only if it sits on the current thread's local queue** —
  a much narrower condition than retraction's "walk the whole un-started prerequisite
  subtree and claim each." So the mechanism that is closest to retraction as an *API*
  still does not prevent the common starvation deadlock, because it does not reach
  across the dependency graph.
- **Mitigation shipped:** "async all the way down" (never block on async); `JoinableTaskFactory`
  for the specific UI-thread reentrancy case (a structured inline/reentrancy pump).
- **vs. retraction:** our stance is the opposite of "never block" — a `sync()` on
  retractable task work is *made safe* by running it inline, rather than forbidden.

### 2.6 Grand Central Dispatch (macOS/iOS, lazy global pool)

- **Reported/known:** thread explosion on concurrent queues (the pool caps ~64–65
  threads; a `dispatch_sync` onto a saturated concurrent queue while the main thread is
  blocked deadlocks — "both waiting for each other").
- **Their own docs / guidance warn:** Apple forums
  [Avoid Dispatch Global Concurrent Queues](https://developer.apple.com/forums/thread/711736)
  and [".sync in concurrent queue"](https://developer.apple.com/forums/thread/106319);
  Apple's rule: never `dispatch_sync` to the queue you are running on.
- **Mitigation shipped:** serial queues + target queues, bounded concurrency, no
  `dispatch_sync` on same queue — pure programmer discipline plus oversubscription
  (spawn more threads), which is the *cause* of thread explosion.
- **vs. retraction:** GCD has no work-stealing DAG and no notion of running the awaited
  block on the waiter; the entire mitigation is "don't block." Retraction is the
  structural alternative.

## 3. The other horn: the busy-wait family of hazards

The peer mitigation for §1 is *general cooperative busy-wait* — the blocked thread
rejoins the steal loop and runs **arbitrary** queued work (TBB `isolate`-scoped stealing,
Taskflow `corun`, Rayon's steal-while-waiting, .NET inlining, FJP helping). That trades
the deadlock for a distinct hazard family, because the waiter now runs work *unrelated to
what it is waiting for*, on a thread that was mid-operation. The categories, with evidence:

### 3.1 Re-entrancy / "moonlighting" correctness bugs

TBB's own term: a thread "has unfinished child tasks in flight and steals unrelated tasks
to keep itself busy." Because it can steal an **outer** task while waiting on an inner one,
it may run outer work before the inner completes — violating an invariant the waiter was
mid-way through. TBB docs are explicit that "developers often incorrectly assume ... the
same thread could never execute an inner task and then start executing an outer task before
completing the inner task." Reported instance: [oneTBB #942 "task_group::wait() waits for
tasks in unrelated task groups"](https://github.com/uxlfoundation/oneTBB/issues/942).
.NET hit the identical class: the [Task.Wait and "Inlining"](https://devblogs.microsoft.com/dotnet/task-wait-and-inlining/)
post documents that inlining an unexpected task caused reentrancy bugs, and the fix was to
**restrict** inlining to tasks already on the waiter's local queue — which *reopens* the
starvation deadlock of §1. Restricting inlining and keeping the deadlock, or inlining
freely and risking reentrancy, is a genuine lose-lose the targeted model avoids.

### 3.2 TLS / thread-identity corruption

Running an unrelated task on a borrowed thread breaks thread-local invariants. The
canonical case is the [Rayon+Tokio trap](https://lobste.rs/s/mebxps/mixing_rayon_tokio_for_fun_hair_loss):
a rayon task yields mid-`block_on` with its scoped TLS still active; a child rayon task
stolen onto the same thread then runs "indistinguishable from calling `block_on` from an
asynchronous execution context," corrupting the scoped-TLS contract. Allocator caches,
RNG state, and "this runs on thread X" affinity contracts break the same way; Marl
documents the sibling footgun for fibers (bound TLS, debugger confusion).

### 3.3 Unbounded stack growth / overflow

Each busy-wait nests another task's execution on the waiter's stack; recursive waits blow
it. Direct instance: [Rayon #751 "stack overflow in nested ThreadPool.scope"](https://github.com/rayon-rs/rayon/issues/751).
The Cilk "work-first" bound on stack depth holds only for the structured spawn/sync tree —
arbitrary steal-while-waiting has no such bound.

### 3.4 Priority inversion

A high-priority thread that busy-waits picks up low-priority unrelated work (or a
lock-holder runs a long unrelated task), delaying the high-priority path behind work it
never should have run. Studied directly in the work-stealing literature
([Load Balancing Prioritized Tasks via Work-Stealing](https://link.springer.com/chapter/10.1007/978-3-662-48096-0_18)),
and observed as "detrimental task execution patterns" in mainstream OpenMP runtimes
([arXiv:2406.03077](https://arxiv.org/pdf/2406.03077)).

### 3.5 Latency / convoying (wandering off)

The waiter can grab a long unrelated task and not return to observe its awaited result for
that task's whole duration — unbounded added latency even though the result was ready. The
Rayon+Tokio case again: a CPU-bound stolen task stalls a thread that should be servicing
I/O, and "thousands of unrelated network requests spike in latency."

### 3.6 The mitigation's own burden

The guards are opt-in and easy to forget. TBB requires wrapping the blocking construct in
`this_task_arena::isolate` — and the hazard is general enough that Taskflow users request
the same feature ([Taskflow #522 "Task Isolation like TBB"](https://github.com/taskflow/taskflow/issues/522)).
`corun` requires rewriting `wait()`→`corun`, only works from a worker, and its exception
propagation is a known gap ([Taskflow #519](https://github.com/taskflow/taskflow/issues/519)).
Every one of these puts the burden on the user to know the trap exists.

### 3.7 Why retraction sidesteps the family

Retraction is *targeted*: it runs only the waiter's own un-started dependency subtree, in
dependency order, under the waiter's inherited access grant.

- **No unrelated task ever runs on the waiter** → 3.1 / 3.2 / 3.4 / 3.5 do not arise
  (nothing to moonlight, no foreign TLS, no unrelated low-priority work, no wandering off —
  the waiter runs exactly what it blocks on, nothing else).
- Retractable tasks are **bare** (no pipe/access binding, `Flags::retractable`), so an
  inlined body cannot re-enter a resource the waiter holds; the non-retractable residue
  (pipe/`async` under an access scope) is *diagnosed*, not silently run (§4.2).
- **Stack (3.3) is the one shared cost, but bounded**: depth is the program's actual
  dependency depth — work the waiter already transitively awaits — and the per-thread FIFO
  trampoline (`inline_pending`) caps inline chains instead of recursing. Bounded by
  construction, not by arbitrary steal depth.
- Retraction does bypass the retracted tasks' queue *priority* (a documented caveat), but
  it never *inverts* priority: it runs only the work the (possibly high-priority) waiter
  needs, never unrelated lower-priority work ahead of it.

## 4. What retraction does and does not cover (honest scope)

Retraction is not a universal deadlock cure; it removes a specific, high-frequency
class. Precise mapping:

1. **Removed:** bare-task fork-join / nested-task exhaustion — `sync()`/`take()` on a
   `ts::task`/`launch`/`nested` (or a `then`/`when_all` built on them) whose subtree is
   un-started. The waiter runs the subtree inline; no free worker is required. This is
   the TPS01-J class, the FJP-join class, the Taskflow-nested-wait class, the
   Rayon-nested-scope class.
2. **Handled by another mechanism, not retraction:** blocking under an access scope on
   **non-retractable** work — pipe/`async` jobs (why: `docs/internals/task-internals.md` §6 and the
   pipe-rebase note in `docs/TODO.md` §1.14 — pipe ordering is not a `prerequisite`
   chain, so `retract` cannot see it). Here the **blocking-sync diagnostic**
   (`blocking_sync_diagnose`, `TS_SAFETY_CHECKS`) fires a `TS_ENSURE` at the call site —
   with the sharp same-object message when the target is a pipe job on a pipe the waiter
   already holds (certain deadlock). It does not make the block safe; it converts a hang
   into a diagnosed misuse and points at continuations (`then`/`when_all`) or nested tasks.
3. **Handled by the access model, not retraction:** the "lock held across a parallel
   region" recursion deadlock (Rayon #592 shape). `Guarded` replaces ad-hoc mutexes and
   the harness is the runtime oracle for undeclared access; retraction is orthogonal.
4. **Not addressed (same discipline as everyone):** blocking on external I/O or an OS
   primitive that is not task work — nothing to retract. The never-block rule and
   continuation-passing apply, as in every system here.

## 5. Summary

| System | Reported deadlock | Their mitigation | Opt-in? | Needs spare threads? | Executes unrelated work? |
|---|---|---|---|---|---|
| **Taskflow** | #506, #402 (nested wait) | `corun`/`corun_until` | yes (rewrite `wait`) | no | yes (whole steal loop) |
| **oneTBB** | #1316, #353, #1903 | `this_task_arena::isolate` | yes | no | yes (isolated subset) |
| **Rayon** | #592, #1174, #751 | separate pools / don't lock; `par_bridge` guard | yes | no | yes |
| **Java FJP** | parallel-stream / `join` starvation | helping + `ManagedBlocker` | partial | **yes** (ManagedBlocker) | yes (helps stealer) |
| **.NET TPL** | roslyn #13611 | "async all the way"; narrow inlining | yes | grows pool | limited |
| **GCD** | thread explosion / `dispatch_sync` | serial queues; don't block | yes | **yes** (explosion) | n/a |
| **this project** | — (retraction) | automatic on `sync()` + blocking-sync diagnostic | **no** | **no** | **no** (targeted subtree only) |

The differentiators: retraction is **automatic** (no `corun`/`isolate`/`ManagedBlocker`
opt-in), **targeted** (only the waiter's own un-started dependency subtree — never the
priority-inverting general steal loop), and **allocation/thread-free** (no spare/spawned
worker, no fiber stack). The residual non-retractable case (pipe/`async`) is *diagnosed*
at the call site rather than left to hang. The last column is the §3 discriminator: every
"yes" is a system exposed to the moonlighting/TLS/stack/priority/latency family; retraction
is the only "no."

## 6. Sources

Pool-exhaustion (§1–2) — issue trackers:
- Taskflow [#506](https://github.com/taskflow/taskflow/issues/506), [#402](https://github.com/taskflow/taskflow/issues/402)
- oneTBB [#1316](https://github.com/uxlfoundation/oneTBB/issues/1316), [#353](https://github.com/oneapi-src/oneTBB/issues/353), [#1903](https://github.com/uxlfoundation/oneTBB/issues/1903)
- Rayon [#592](https://github.com/rayon-rs/rayon/issues/592), [#1174](https://github.com/rayon-rs/rayon/issues/1174), [#751](https://github.com/rayon-rs/rayon/issues/751); downstream [Lorikeet #1](https://github.com/rhysnewell/Lorikeet/issues/1)
- .NET [dotnet/roslyn #13611](https://github.com/dotnet/roslyn/issues/13611)

Pool-exhaustion (§1–2) — vendor docs / hazard statements:
- SEI CERT [TPS01-J — Do not execute interdependent tasks in a bounded thread pool](https://wiki.sei.cmu.edu/confluence/spaces/java/pages/88487727/TPS01-J.+Do+not+execute+interdependent+tasks+in+a+bounded+thread+pool)
- Taskflow [Executor cookbook (corun / deadlock warning)](https://taskflow.github.io/taskflow/ExecuteTaskflow.html)
- oneTBB [Work Isolation](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/work_isolation.html)
- Java [`ForkJoinPool.ManagedBlocker`](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/concurrent/ForkJoinPool.ManagedBlocker.html)
- .NET [Debug ThreadPool Starvation](https://learn.microsoft.com/en-us/dotnet/core/diagnostics/debug-threadpool-starvation), [VS-threading — thread starvation](https://microsoft.github.io/vs-threading/docs/threadpool_starvation.html)
- GCD — Apple forums [Avoid Dispatch Global Concurrent Queues](https://developer.apple.com/forums/thread/711736), [.sync in concurrent queue](https://developer.apple.com/forums/thread/106319)

Busy-wait hazard family (§3):
- **Re-entrancy / moonlighting:** .NET [Task.Wait and "Inlining"](https://devblogs.microsoft.com/dotnet/task-wait-and-inlining/) (why inlining was restricted to the local queue); oneTBB [#942 "task_group::wait() waits for tasks in unrelated task groups"](https://github.com/uxlfoundation/oneTBB/issues/942); Pro TBB / oneTBB [Work Isolation](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/work_isolation.html) (the "moonlighting" definition)
- **TLS / thread-identity:** [Mixing rayon and tokio for fun and (hair) loss](https://lobste.rs/s/mebxps/mixing_rayon_tokio_for_fun_hair_loss); [The Tokio/Rayon Trap](https://pmbanugo.me/blog/why-async-await-complect-concurrency)
- **Stack growth:** Rayon [#751](https://github.com/rayon-rs/rayon/issues/751)
- **Priority inversion:** [Load Balancing Prioritized Tasks via Work-Stealing](https://link.springer.com/chapter/10.1007/978-3-662-48096-0_18); [Detrimental task execution patterns in mainstream OpenMP runtimes (arXiv:2406.03077)](https://arxiv.org/pdf/2406.03077)
- **Mitigation burden:** Taskflow [#522 "Task Isolation like TBB"](https://github.com/taskflow/taskflow/issues/522), [#519 (exception support for corun)](https://github.com/taskflow/taskflow/issues/519)

Ecosystem write-ups:
- [Avoiding Deadlock from Rayon Thread Pool Exhaustion](https://imfeld.dev/writing/avoiding_rayon_thread_pool_exhaustion)
- [.NET ThreadPool starvation, and how queuing makes it worse (Criteo)](https://medium.com/criteo-engineering/net-threadpool-starvation-and-how-queuing-makes-it-worse-512c8d570527)

## 7. Outcome (2026-08): retraction retired by the coroutine-first decision

The assessment arc, for the record. First pass proposed banning in-task blocking and
deleting retraction; the author's pushback established three corrections: (a) retraction
is an optimization, not only a correctness net (executing an un-started dependency inline
on the waiting core is never slower than a park + worker round-trip — retraction is to
`sync()` what symmetric transfer is to `co_await`); (b) a graph node with pending nested
tasks is already a suspension point holding grants, so the real rule is *what* you await
under a grant, not *whether* (the waiting rules (suspension under access),
[coroutine-first.md](coroutine-first.md) §2); (c) partial retraction coverage plus
coloring made "coroutines replace it" incomplete while both colors existed.

The coroutine-first decision resolves the trilemma by removing the blue color from task
interiors: dynamic waits are suspensions (no park, no pool exhaustion — §1's hazard is
structurally gone), the inline-execution optimization survives natively (eager start +
symmetric transfer), and in-task `sync()` becomes a fatal instead of a mitigated hazard.
Retraction and its supporting machinery (claim/generation, `retractable`, hints) are
removed in that plan's stage 4. This survey stays as the analysis that priced all three
options — both horns, retraction, and the ban — and as the record that the ban only
became the right answer once coroutines were made mandatory rather than optional.
