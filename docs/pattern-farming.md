# Pattern farming: candidates for the suite

Research pass, 2026-08. Goal: mine third-party parallelization
frameworks/libraries/languages — their shipped generic solutions *and* patterns
emerging in their discussions and issue trackers — for **high-level, user-facing
patterns** and **async/await mechanics** we could add to our suite. Scheduler
plumbing (work-stealing, eventcount idle, deques) is out of scope; we have that.

Method: a seven-cluster web sweep (game engines · general C++ ·
`std::execution`/structured-concurrency · other languages · cross-cutting
emerging · language-native async/await · C++ coroutine libraries), then one
consolidation pass — deduped across clusters, filtered against our current suite
(Part 3), ranked.

All code below is an **illustrative sketch in our vocabulary** to show how an
item would *feel* on our primitives — not a claim the API exists. Numbers are
stable IDs; refer to any item as `2.N` / `4.N` / `5.N`. Mark items done/rejected
in place, don't renumber (TODO.md convention).

---

## Decisions log

Live status of items discussed with the author. `APPROVED` items graduate to
`TODO.md` once the review is complete. Each processed item also carries a
**Status:** line inline.

Legend: ✅ approved · ❌ rejected · 🕓 undecided (revisit) · 🔬 design delegated · ⬜ not yet discussed.

| Item | Status | Notes |
|---|---|---|
| 2.1 `when_any` + loser policy | ✅ approved, **mid-pri** | Do with the rest of Cluster A, one by one. Added: loser policy (cancel / detach / let-run), the Cobalt-gap spec, 5.3 rewrite. |
| 2.2 timer primitive | 🔬 design ready | [timer-primitive-design.md](timer-primitive-design.md): timer thread + min-heap, zero-cost when unarmed. Awaiting author decision. |
| 2.3 timeout/deadline | 🕓 undecided | Expanded with the partial-result / anytime-algorithm flavor per author. |
| 2.4 linked / child tokens (+ reason) | 🕓 undecided | Rewritten for clarity per author. |
| 2.5 structured task-group scope | 🕓 undecided | Terminology fixed (launch, not spawn). |
| 2.59 delegate/event recipe | ✅ done | Shipped as `sample/events.cpp` (2026-08): four-tier ladder (intra-system / command / event board / batch+edge), determinism-checked. |

**Undecided items to revisit:** 2.3, 2.4, 2.5. *(kept current as we go — the "remind me later" list)*

**To re-raise when the review resumes** (author, 2026-08-13): (a) the
blocking-rule split — same-object in-task `sync()` stays fatal (provable
deadlock), foreign-object becomes `TS_ENSURE` + oversubscription (the 2.55
promotion) — awaiting verdict; (b) the flip/pump ordering diagnostic (staging
node with no path to the flip/pump node → once-per-site warning) — awaiting
verdict; (c) ~~`Event_bus` catalogue status~~ — RESOLVED 2026-08-13: promoted to the
public library (`include/ts/event_bus.h`, in the umbrella; guide §9.4;
`dispatch_fn()`/`publish_fn()` naming ratified — `_fn` = a functor run later
under the declared grant); follow-ups: dedicated test group, the typed-lane
tier it motivates; (d) the timer design doc
([timer-primitive-design.md](timer-primitive-design.md)) — awaiting verdict.

---

## Part 0 — Synthesis (read this first)

Two findings dominate every cluster.

**(i) Our model is a *data-access dependency* model; the world also has a
*streaming / message-flow* model, and that is where the real gaps cluster.**
Our edges are derived from declared read/write conflicts over a fixed node set,
one run. Every framework surveyed *also* has a streaming vocabulary — bounded
in-flight tokens, backpressure, channels, reorder buffers, keyed joins,
completion-order fan-in — that a DAG + pipe + `Versioned` structurally cannot
express. This is **Cluster B** (2.10–2.19). It is the single biggest *category*
we lack; `Signal` is one-shot and `Task` is single-result, so nothing models a
value *sequence over time* with flow control.

**(ii) We have a bare cancellation flag and no time.** The delivery model
(cooperative flag, cancellation-as-completion-state) is fine and correct given
exceptions are disabled — what's missing is the *composition around it*:
first-to-finish, timeouts, a timer primitive at all, linked/child tokens, a
joinable scope with a completion policy. This is **Cluster A** (2.1–2.9), the
best value/effort ratio in the whole sweep because it rides machinery we already
have (the awaiters' two-state handshake, `Cancel_callback`, the resume
trampoline). The two things a game frame loop most predictably wants — a
**deadline** and a **first-of-N** — are exactly the two our sequential-`co_await`
model cannot spell today.

Suggested build order, by leverage-over-effort:

1. **Timer primitive (2.2)** → unlocks timeouts (2.3), retry/backoff (2.6),
   `sleep`/yield, time-budget (2.41). We have *no delayed dispatch at all*; this
   is foundational and small (a timer heap on one thread firing a `Signal`).
2. **`when_any` + loser-cancel (2.1)** and **linked tokens (2.4)** → together with
   the timer these are the cancellation-composition core; the sample already
   hand-rolls both.
3. **Counting semaphore (2.10)** → the smallest streaming piece, and the
   backpressure knob under 2.11/2.13.
4. **Awaitable sync family (2.20–2.22)** → latch/auto-reset/`Signal<T>` are
   near-free extensions of `Signal`'s intrusive-waiter mechanic.
5. **Change-version skip nodes (2.23)** → the highest-leverage graph-side item;
   the primitive (`write_epoch`) already exists.
6. Then the larger strategic axes as separate efforts: **async streams/channels
   (2.11–2.12)**, **structured task-group scope (2.5)** (gated on a recoverable
   "failure" channel decision), **compile-phase graph analyses (2.24–2.27)**.

A recurring secondary finding worth its own note: **HPX `async_rw_mutex`, Folly
`coro::SharedMutex`, PPL `overwrite_buffer`/`task_completion_event`, Kotlin
`StateFlow`** are independent reinventions of our **pipe / `Versioned` /
`Signal`** — external validation of the core bets (Part 5).

---

## Part 1 — The comparison roster

The systems analysed and compared against (from
[task-systems-comparison.md](task-systems-comparison.md) §0), grouped by milieu.

| # | Milieu | Systems |
|---|---|---|
| 1.1 | Game engines (proprietary) | **Unreal** (Tasks, legacy TaskGraph, Pipes, RDG render-graph), **Unity** (C# Job System, Burst/DOTS/ECS, EntityCommandBuffer) |
| 1.2 | Game engines (open / library) | **enkiTS**, **Google Marl**, fiber systems (**FiberTaskingLib**, Naughty Dog counters) |
| 1.3 | General-purpose C++ | **oneTBB**, **Taskflow**, **HPX**, **PPL / Folly** |
| 1.4 | Platform runtimes | **Apple GCD / libdispatch** |
| 1.5 | Other-language reference points | **Rust Rayon + Tokio**, **Kotlin coroutines**, **Go**, **.NET TPL / Dataflow**, **Java ForkJoinPool / Loom** |
| 1.6 | The coming standard | **C++26 `std::execution`** (senders/receivers), the structured-concurrency movement (Trio nurseries, `StructuredTaskScope`, `async_scope`) |
| 1.7 | C++ coroutine libraries | **cppcoro**, **folly::coro**, **libunifex**, **Boost.Cobalt**, **Boost.Asio**, **concurrencpp**, **Seastar**, **QCoro**, NVIDIA **stdexec** |
| 1.8 | Brief mentions (same family) | Cilk/Cilk Plus, OpenMP tasks, Microsoft PPL, Boost.Asio/Boost.Fiber, .NET TPL, Java `ForkJoinPool` |

---

## Ranking policy (decisions that shape the tiers)

- **Named / pinned / execution-lane threads → LOW priority (Cluster LOW,
  2.56–2.58).** Real and eventually necessary (UE `ENamedThreads`, enkiTS pinned
  tasks, GCD main queue all ship it), but for *this* exercise it is
  infrastructure, not a high-level pattern: a solved executor-affinity mechanism
  with nothing novel to mine, partly in tension with the single-global-scheduler
  decision (2026-07), and whose underlying need is already served by `Frame_gate`
  (hop to a context), `Signal` (OS/GPU-completion bridge), and a dedicated
  1-worker `Scheduler` + `Scheduler_scope`. It becomes load-bearing only once the
  platform-integration layer exists (TODO 6.6 area) — correctly *sequenced after*
  that. One agent called it "the biggest raw gap"; downweighted here on purpose.
- **Anything the pipe / `Versioned` / `Signal` already is** → Part 5 (cite as
  validation, don't rebuild).
- **Anything needing an exception/`Try` channel** → noted but deferred; exceptions
  are disabled project-wide, so a recoverable-error channel is a philosophy fork
  (2.5, 2.9), not a drop-in.

---

## Part 3 — Our current suite (the baseline the sweep measures against)

| # | Primitive / pattern | What it is |
|---|---|---|
| 3.1 | **Static task graph** | Build-once/run-many DAG; edges **derived** from declared read/write conflicts (+ explicit `after`/`before`); parallel via indegree counts. |
| 3.2 | **`Guarded<T>` / access** | Per-object reader/writer *pipe*; `access(fn)` deduces mode from param const-ness; multi-object access in canonical (deadlock-free) order. |
| 3.3 | **Access harness** | Runtime oracle for undeclared access; grant-window validity via `write_epoch`. |
| 3.4 | **`Deferred<T>`** | Command buffer — stage grant-free into per-recorder journals; `commit()` applies under a write grant. Parallel recorder per worker. |
| 3.5 | **`Versioned<T>`** | Double-buffered state; readers see the last published version stably; `publish()` = cut + apply + swap + resync. |
| 3.6 | **Blackboard** | Recipe on `Versioned` (shared KV, staged writes, key-change diff-notify after flip). |
| 3.7 | **`parallel_for`** | Data-parallel fan-out under the caller's grant; priority inheritance; runs on the ambient scheduler. |
| 3.8 | **Coroutines** | `co_await` composition; nested graph runs (lend protocol); coroutine graph nodes; grants held across suspension; resume trampoline. |
| 3.9 | **`Signal`** | Awaitable manual-reset event / barrier / phase gate; the sanctioned OS/GPU-completion bridge. |
| 3.10 | **`Frame_gate`** | Realign cross-frame work to the next frame boundary. |
| 3.11 | **Cancellation** | Cooperative token + `Cancel_callback` + trailing-token mid-body early-out. |
| 3.12 | **Worker-less mode** | Zero-worker deterministic execution. |
| 3.13 | **Machine/extract idiom** | Sealed sim object + `Versioned` output extract (`sample/physics.cpp`). |
| 3.14 | **Graph profiler / trace** | Measured critical path, per-node/edge stats, utilization + dead-time headline, DOT + SVG. |

---

## Part 2 — Candidate patterns

Full treatment (explanation + code sketch) for the top-tier items; compact
treatment for near-duplicates and lower-value items.

### Cluster A — Composition & cancellation combinators

*The best value/effort cluster: rides existing awaiter + token machinery.*

#### 2.1 `when_any` / race + loser policy — **top pick**

**Status: ✅ approved, mid-pri.** Build with the rest of Cluster A, one item at a
time.

Await several tasks, resume on the **first to settle**. The universal spelling of
speculative execution, hedged requests, first-responder, and (against a timer)
timeouts. Cited by *every* cluster as the clearest gap. We have `when_all` for
free (sequential `co_await`), but nothing disjunctive — and `game_frame`'s
"speculative cancellable nav queries" already open-code exactly this.

```cpp
// illustrative sketch
auto [index, result] = co_await ts::when_any(query_a, query_b, query_c);
```

**Loser policy — a knob, not a fixed "cancel the rest" (author question).** Two
genuinely different uses:

- *Redundant / speculative* — you wanted the fastest answer and the others are now
  wasted work (hedged asset source, two nav strategies). Here you want to **cancel
  the losers** to reclaim the cores.
- *First-ready, rest-still-useful* — the first completion only gates *when you
  start consuming*, while the others keep producing results you still want (start
  reacting as soon as the first streamed chunk lands; the remaining loads must
  finish; process results as they arrive is then 2.15). Here you want to **let the
  losers run** (or detach them), not cancel them.

So `when_any` should carry a policy: `On_rest::cancel` (default) / `On_rest::detach`
(let them run free) / `On_rest::keep` (hand back their handles so the caller
awaits them later). The last turns `when_any` into "first now, the rest on demand."

Mechanism: resolve on the first completion via the awaiters' existing two-state
`exchange` handshake; for `cancel`, fire a shared `Cancellation_source` merged
into each child's token (2.4).

**What's missing vs what we already have (Cobalt-`interrupt_await` question).**
We *do* have the cancellation half — `Cancel_callback` fires synchronously on
`request_cancel()`, and a running loser that polls its trailing token can early-out.
What we lack is unwinding a loser that is **parked on a `co_await`**: today a loser
suspended awaiting a still-running inner task stays parked until that inner task
completes, *then* resumes and observes the cancel — the wasted work runs to the
end regardless. Two missing pieces close that:
(1) wire a `Cancel_callback` into the `Task`/`Signal` awaiters so cancelling the
token a parked frame is waiting under **resumes that frame promptly** (cancelled),
instead of only-on-inner-completion; and
(2) Boost.Cobalt's `interrupt_await()` refinement — resume the parked loser
**synchronously, no scheduler hop**, so it unwinds its stack (running its cleanup)
right now. (1) is the functional gap; (2) is the latency refinement. Neither is
preemption — a loser genuinely *running* a body still only stops cooperatively.

Difficulty: low–medium. See 5.3 for the eager-model safety property this gives us
over drop-cancellation systems.

#### 2.2 Timer / delayed-dispatch primitive — **foundational, we have none**

**Status: 🔬 design delegated — doc ready: [timer-primitive-design.md](timer-primitive-design.md).**
Recommendation: a lazily-created, scheduler-owned **timer thread with a
`steady_clock` min-heap**, each fire delivered as a `Signal` trigger handed to the
scheduler via a low-priority `launch` — structurally identical to
`Frame_gate::open()`. Rejected folding deadlines into the eventcount park
(`Event_count`/`atomic::wait` has no timed wait; retrofitting one means per-OS
futex code that perturbs the load-bearing wake protocol). **Perf headline:
zero-cost when no timers are armed, by construction** — the facility is a separate
object; `submit`/`find_work`/`run_task`/`signal_submit` gain no field and no
branch, and the wake protocol is byte-for-byte unchanged (fires are ordinary
`launch`es). Per-`sleep` ≈ one `Signal` block + O(log n) heap push; the parked
timer thread is ≈free (~60 wakeups/s under a per-frame-timer load). Two ship-with-it
notes: armed timers must hold an `External_wait` (else the deadlock net false-fires
while workers idle on a pending sleep); worker-less mode uses a virtual/mock clock
(or explicit `pump_timers()`) — which also gives deterministic timeout/retry tests.

We have **no delayed scheduling at all**: no `sleep`, no `schedule_after`, no
deadline. Every timeout/retry/backoff/time-budget item below is blocked on this.
The primitive is small: a min-heap of deadlines on one dedicated thread (or an
OS timer) that fires a `Signal` — which is already our sanctioned "external
completion → `co_await`" bridge (concurrencpp's `timer_queue` is exactly this: a
background thread that enqueues onto the executor on fire).

```cpp
// illustrative sketch
co_await ts::sleep(16ms);                     // cancellation-aware park
auto tick = ts::every(4ms);                   // periodic Signal source
```

The modern shape (Python `asyncio.timeout`) is a *reschedulable deadline* object,
not a one-shot timer — worth designing in from the start. Difficulty: medium
(the thread + heap is the only genuinely new infrastructure in Cluster A).

#### 2.3 Timeout / deadline combinator

**Status: 🕓 undecided.**

Once 2.1 and 2.2 exist, the base case is `when_any(work, sleep(d))` + cancel the
loser. But there are **two distinct timeout shapes** (author's point), and the
combinator should serve both:

**(a) All-or-nothing** — the operation is only useful if it finishes (an asset is
loaded or it isn't). Timeout discards the in-flight work and yields "nothing":

```cpp
// illustrative sketch
std::optional<Asset> r = co_await ts::within(load_asset(id), 2ms);   // empty on timeout
```

**(b) Partial-result / anytime algorithm** — the operation accrues a *usable
partial result* and, on deadline, should **stop and hand back its best-so-far**,
not throw it away (time-sliced pathfinding that returns the best route found so
far; an iterative solver that returns its current estimate; a culling pass that
returns whatever it has visited). This is exactly our **trailing-`Cancellation_token`
mid-body early-out**: the deadline trips the token, the body polls it at a safe
point, stops, and returns what it has — settling **completed with a partial
result**, not cancelled. So the timeout does not *discard*; it *signals*, and the
body decides how much it produced:

```cpp
// illustrative sketch — the body owns the partial result
Path plan_path(const Nav& nav, Goal g, ts::Cancellation_token deadline)
{
    Path best;
    while (!best.reaches(g)) {
        if (deadline.is_cancel_requested())
            return best;              // best-so-far, settles COMPLETED
        best = expand_one(best, nav);
    }
    return best;
}
// caller:  auto path = co_await ts::before(deadline_ms, plan_path, nav, goal);
//          // always a Path; complete route if it finished, partial if the deadline hit
```

The two shapes differ only in whether the body is deadline-aware: (a) wraps an
opaque task and *cancels* it; (b) hands the deadline token *into* a cooperative
body that returns partial. Both need the timeout to yield a result *distinguishable*
from a caller cancel — see the cancel-reason note in 2.4. Difficulty: low (given
2.1 + 2.2); (b) reuses the existing trailing-token early-out with no new
machinery.

#### 2.4 Linked / child cancellation tokens (+ cancel reason)

**Status: 🕓 undecided.**

Three separate things get lumped under "richer cancellation." You already have the
third clearly; the first two are the additions.

**(A) Token *linking* — one token that trips if *any* of several sources fire.**
The concrete case: a piece of work is running under the frame's cancellation token
(the frame can be abandoned), and it *also* wants its own timeout (2.3). It needs a
single token to poll that becomes cancelled if **either** the frame is cancelled
**or** the timeout fires — otherwise the body has to check two tokens by hand
everywhere. Linking produces that combined token:

```cpp
// illustrative sketch
ts::Cancellation_source timeout;                        // armed by a timer (2.2)
auto tok = ts::link(frame_token, timeout.token());      // trips on EITHER
auto path = co_await ts::before(tok, plan_path, nav, goal);
// inside plan_path, one `tok.is_cancel_requested()` covers frame-abandon AND timeout
```

We have the building block already — `Cancel_callback` lets one source fire a
callback that trips another — so `link` is a thin helper, not new machinery.

**(B) Parent → child token *tree* (the case you already have in mind).** A child
token that trips when its own source *or* its parent trips — cancel the parent and
the whole subtree goes with it, without threading the parent token through every
call by hand. This is what a structured scope (2.5) hands to each child.

```cpp
// illustrative sketch
auto child = frame_source.child_token();   // frame cancel ⇒ this subtree cancels;
                                           // cancelling `child` does NOT touch the frame
```

**(C) Why carry a cancel *reason/source*, not a bare bool.** Once a token can be
tripped by more than one thing (a link of frame + timeout, or a scope cancelling
its children), the code that observes the trip often has to **react differently
depending on who tripped it**:

- tripped by the *timeout* → return the partial/best-so-far result (2.3b), a
  normal completion;
- tripped by the *frame/user* → abort for real;
- tripped by a *sibling failure* inside a task-group (2.5) → propagate that
  failure.

A bare boolean token can't distinguish these — it just says "cancelled." So the
token should carry a small **reason** (`user` / `deadline` / `parent` / `sibling`)
the observer reads. This is also what lets a scope tell its *own* teardown-cancel
apart from an *external* cancel of the scope: when a task-group cancels its
remaining children because one failed, those children's tokens read
`reason == sibling`, and the group's own result should be the *failure*, not
"cancelled." Without a reason, self-inflicted and external cancels are
indistinguishable and the scope reports the wrong outcome.

Difficulty: low–medium. `link` and `child_token` sit on the existing
`Cancel_state` + `Cancel_callback`; the reason is one enum field on the cancel
state, set at `request_cancel(reason)`.

#### 2.5 Structured task-group scope / nursery with a completion policy

**Status: 🕓 undecided.** (Terminology: we **launch** tasks, we don't *spawn*
them — "spawn" is overloaded in gamedev. Applied throughout the doc.)

A joinable scope you **launch** a *dynamic, heterogeneous* set of children into;
the scope cannot exit until all finish, governed by a policy:
**shutdown-on-first-failure** (cancel siblings on first error), **-on-first-success**
(race — 2.1 generalized), or **supervise** (a sibling failure doesn't tear down
the cohort). This is the safe half of what we removed: `ts::nested`/`Task_scope`
were deleted as unsafe *grant-inheriting* concurrent children
([coroutine-first.md](coroutine-first.md) §4.3) — a group of **detached**
(`ts::launch`, no grant inheritance) children joined at scope end is a different,
safe animal, and fills the gap `parallel_for` (homogeneous, known count) doesn't.

```cpp
// illustrative sketch
ts::Task_group g{ ts::On_failure::cancel_siblings };
g.launch([]{ return load_mesh(); });
g.launch([]{ return load_textures(); });
co_await g.join();     // all done, or first failure cancelled the rest
```

The blocker is the *policy*: exceptions are disabled, so "failure" must first
become a recoverable channel (an `expected`-like payload on `Task<R>`, or a
`TS_ENSURE`-style signal) before fail-fast has anything to key on. WG21 removing
`ensure_started`/`start_detached` (in favor of scope-joined launch) is direct
evidence our detached `ts::launch` should perhaps *default* to scope-joined, with
truly-detached as the explicit escape — the mirror of `execute({.detach=true})`.
Difficulty: medium; join + source + first-failure latch exist in pieces, the
error channel is the real design work. Consider a combinator-internal *lazy* task
(4.1) here — "build then collectively start" is safer than racing eager children.

#### 2.6 `retry` / `repeat` / backoff

`retry` re-runs on failure (optionally with a predicate/backoff); `repeat_n`
re-runs on success N times. `repeat_n(graph.execute())` is a clean spelling of
"run this frame graph N times"; `retry` + `sleep` (2.2) gives resilient async
loads (the streaming sample's four loads want per-load retry). Difficulty: low as
named combinators over a `co_await` loop; `retry` needs a failure to key on
(2.5).

#### 2.7 `shield` / non-cancellable section

Run an inner op that must complete even if the surrounding work is cancelled — a
side-effectful commit, a graceful close. Directly relevant to `Deferred`/`Versioned`
commits that "must finish" (our dtor already waits out an in-flight commit). A
`shield`-marked child opts its token out of the parent's cancel.

```cpp
// illustrative sketch
co_await ts::shield(recorder.commit());   // completes even if the frame is cancelled
```

Difficulty: low — mainly a policy flag on the child token (2.4).

#### 2.8 Priority escalation on await (anti-inversion)

When a high-priority frame `co_await`s a lower-priority producer, transiently
raise the producer to the waiter's priority — Swift's escalation, transitive
along the dependency/pipe cascade. We have fixed `Priority` on the block with no
inheritance-on-await. Game-relevant (a critical node waiting on a normal producer
should bump it). Difficulty: medium — re-filing a still-queued block is cheap;
one already mid-steal on a Chase-Lev deque can't be cheaply re-prioritized. Pairs
with the compile-phase HEFT priority (2.26).

#### 2.9 Cancellation-as-value adapters (`stopped_as_optional` / `let_stopped`)

Awaiting a *cancelled* value-task is fatal in our model (must check
`is_cancelled()` first). A `stopped_as_optional`-style adapter — `co_await
ts::as_optional(t)` yielding `optional<R>` — turns the cancelled path into a plain
value the caller branches on, removing a class of fatal footguns. `let_stopped`
(recover from cancellation by substituting a task) maps onto our model today.
Difficulty: low; a thin awaiter over existing cancellation state.

### Cluster B — The streaming axis (the biggest *category* we lack)

*Single-value (`Task`, `Signal`) and latest-value (`Versioned`) we have; a value
**sequence over time** with flow control we do not. Build order: 2.10 → 2.11 →
2.13/2.14, with 2.12 the larger strategic piece.*

#### 2.10 Counting semaphore / in-flight limiter — **smallest streaming piece**

Cap at most N operations in flight through a region, *independent of data
dependencies* — "≤ 2 asset loads at once", "≤ N MB decode buffers". Distinct from
"how many workers exist": each item may `co_await` on I/O, so worker count does
not bound concurrent *async* ops. Nothing expresses this today (the pipe limits
per-object; `parallel_for` bounds by workers).

```cpp
// illustrative sketch
ts::Async_semaphore sem{ 2 };
for (auto id : ids)
    ts::launch([&, id]() -> ts::Task<void> {
        auto permit = co_await sem.acquire();   // suspends the frame if full, frees the worker
        co_await load(id);
    });
```

A `co_await`-native semaphore *suspends the frame* (freeing the worker), which
structurally sidesteps the blocking-semaphore + nested-task deadlock Taskflow
documents. Difficulty: modest, self-contained — a counter + FIFO of parked
waiters on the resume trampoline. Strong standalone recommendation.

#### 2.11 Bounded async channel + backpressure

An MPMC handoff queue where both ends are awaitable and *capacity is the
backpressure knob*: `co_await ch.send(v)` suspends the producer when full,
`co_await ch.recv()` suspends the consumer when empty; drop policies
(`drop_oldest`/`drop_newest`) trade loss for never blocking. The imperative
substrate under 2.12/2.13/2.16/2.19. We own the hard part already (lock-free MPMC
+ SPSC queues live in the scheduler).

```cpp
// illustrative sketch
ts::Channel<Chunk> ch{ .capacity = 8, .full = ts::On_full::suspend };
// producer:  co_await ch.send(chunk);
// consumer:  while (auto c = co_await ch.recv()) process(*c);
```

Design question unique to us: a channel is shared mutable state — should its
access participate in the grant/declaration system (be a `Guarded` type), or sit
beside it like `Signal`? Difficulty: medium (builds on 2.10). Note the name
collision with our per-object *pipe*.

#### 2.12 Async generators / streams

Model a value sequence produced asynchronously — streamed asset chunks, socket
reads, per-frame events — as a first-class pull sequence (`while (auto x =
co_await gen.next())`), backpressure implicit in the consumer's pacing. Our
streaming sample *fakes* this with sequential `co_await`s over four `async`
loads; an async generator is the honest primitive.

```cpp
// illustrative sketch
ts::Async_generator<Chunk> stream_asset(Id id)
{
    while (auto block = co_await read_block(id))
        co_yield decode(*block);
}
```

Tension with eagerness: a *cold* restart-per-collect `Flow` fights our model; a
*hot* push-into-buffer `AsyncStream`/channel bridge fits `Signal`/our queues
naturally — so 2.11 (channel) and 2.17 (reactive `Versioned`) are the cheaper
entry points, full generators the bigger lift. A *synchronous* `generator<T>`
(C++23 `std::generator` shape) is self-contained, touches nothing concurrent, and
is trivially addable. End-of-stream/error must use our optional/fatal convention,
not the throwing one. Difficulty: high for `async_generator` (lifecycle +
early-destruction cleanup, 2.54); low for sync `generator`.

#### 2.13 Bounded-in-flight pipeline (assembly line)

A chain of stages over a *stream* of items, overlapping like an assembly line
(stage 2 works item *n* while stage 1 pulls *n+1*); each stage declares its
concurrency mode (`parallel` / `serial_in_order` / `serial_out_of_order`); a
global token cap bounds in-flight items (both the backpressure knob and the
parallelism ceiling). TBB `parallel_pipeline` / Taskflow `Pipeline`. The
`parallel` + `serial_out_of_order` modes fall out of a coroutine-per-token chain
of `co_await`s over per-stage `Guarded<StageState>` (the pipe's FIFO gives
out-of-order-serial for free; cross-token overlap is automatic from independent
frames). The genuinely new pieces are the token-admission gate (2.10) and the
in-order sink (2.14). Difficulty: medium–high; the strategic streaming
capability.

#### 2.14 Sequence-gated reorder buffer (`serial_in_order` sink)

Work runs in parallel (arbitrary completion order) but is emitted downstream in
user-specified sequence with a no-gap gate — token *t* releases only once *t-1*
has passed. TBB `sequencer_node`. This is precisely the documented "pipe FIFO
does NOT stand in for conflict edges" trap from the graph-free experiment
([coroutine-first.md](coroutine-first.md) §10.5): a system blocked on its first
object hasn't taken its slot on later ones, so a later launch overtakes it —
losing a frame of draw commands *silently*. A `Guarded` sink stashing early
tokens in an indexed buffer, firing the awaiting frame only when `next_expected`
matches, is the fix and the reusable in-order stage for 2.13. Difficulty:
moderate.

#### 2.15 Completion-order fan-in (`JoinSet` / `as_completed` / `FuturesUnordered`)

A growable set of heterogeneous launched tasks consumed **in completion order**
(`co_await set.next()`), dropping the set cancels all members — react to each
result as it lands rather than waiting on the slowest (progressive asset loads,
first-done-first-served). Distinct from 2.5 (that's the *scope*; this is the
*consume-as-they-finish* mechanism) and from 2.1 (all, not first).

```cpp
// illustrative sketch
ts::Join_set<Result> set;
for (auto& job : jobs) set.launch(job);
while (auto r = co_await set.next()) consume(*r);   // completion order
```

Difficulty: medium — an N-way awaiter (2.1 generalized to yield *each*
completion) + a shared source for group-cancel.

#### 2.16 Broadcast event bus (lag-as-data)

Fan every event to *all* subscribers, each with an independent cursor over a
bounded ring; a slow subscriber is told *how many it missed* (Tokio
`broadcast`'s `Lagged`) or a drop policy applies. Distinct from `Versioned`
(latest *state*) and `Signal` (one-shot): this is the *stream of transitions* —
an engine event bus. Lag-as-data fits our "surface hazards, don't hide them"
style. Difficulty: medium–high (per-consumer cursor + wake `Signal`); default to
a drop policy given the frame loop. Builds on 2.11. The frame-coherent baseline
it upgrades is the event-board tier of the delegate/event recipe (2.59,
`sample/events.cpp`).

#### 2.17 Reactive latest-value: `Versioned::changed()` + `collectLatest`

`Versioned<T>` is already a richer `StateFlow`/`watch` (deterministic
double-buffer, stable front address, replay/resync — Kotlin/Tokio are just
`replay=1 + drop_oldest`). The one missing edge is the **subscription**: a
`co_await v.changed()` that parks a reader until the next `publish()` instead of
polling `read()` per frame, plus a `collectLatest`-style consumer that **cancels
its in-progress handler when a fresher value lands** (via a linked token, 2.4).

```cpp
// illustrative sketch
while (true) {
    co_await board.changed();          // resumes exactly on publish
    auto snap = board.read().sync();   // (outside a task) or co_await inside one
}
```

Difficulty: low–medium — a per-`Versioned` `Signal` triggered/reset by `publish()`
(the `Frame_gate` pattern) + a version counter to close the missed-wakeup window.
The blackboard sample's per-subscriber diff is the manual version of this; the
delegate/event recipe's board consumers (2.59) are the other client.

#### 2.18 Content-addressed join by key

Correlate messages across streams by a *runtime key*, emit only when all inputs
for that key are present (reunite items processed out of order, join by entity
ID). TBB `join_node` `key_matching`. Orthogonal to our conflict-derived edges;
the hardest to express. Difficulty: hard — a `Guarded<hash_map<K, partial_row>>`
launching a body when a key's row completes. Catalogue; low priority until a
concrete use appears.

#### 2.19 Actor / mailbox recipe

A single task drains a bounded inbox, serializing all mutations of some state
behind message passing. `Guarded<T>`'s pipe is *already* an actor-without-an-inbox
(serialized access to one object); `Deferred` stages commands to one owner. The
missing piece is the *live, ordered, backpressured inbox* a producer sends to
asynchronously while the actor runs — a thin recipe once 2.11 exists (channel + a
task draining it under a grant). Difficulty: low given 2.11. Mostly a documented
pattern, not a new type. The frame-batched sibling (deferred inbox, applied at
the flip) is the event board of 2.59.

### Cluster C — Awaitable sync primitives beyond `Signal`

*`Signal` = `async_manual_reset_event` (broadcast). The variants below are the
same intrusive-frame-borrowed-waiter mechanic with different set/reset rules —
zero-alloc, no new machinery.*

#### 2.20 Countdown latch

An atomic countdown that broadcasts at zero — the fork-join "wait for N children"
barrier as a primitive, cleaner than reusing `Signal` for counting joins (the
Naughty Dog counter / cppcoro `async_latch`).

```cpp
// illustrative sketch
ts::Latch l{ n };
for (auto& job : jobs) ts::launch([&]{ do_work(); l.count_down(); });
co_await l;    // resumes when the count hits zero
```

Difficulty: low.

#### 2.21 Auto-reset event / single-consumer event

`async_auto_reset_event` releases *exactly one* waiter and re-arms atomically
(lost-wakeup-free one-to-one handoff); `single_consumer_event` is the cheapest
producer/consumer signal (at most one waiter, just a flag). Both are set/reset-rule
variants of `Signal`. Difficulty: low.

#### 2.22 Value-carrying settable `Signal<T>` (`task_completion_event`)

Our `Signal` is the *void* form (bodyless `Task<void>` completed via `trigger()`).
The value-carrying variant — a handle a non-task thread later `set(value)`s, that
waiting tasks observe — is the natural bridge for OS/GPU/RPC completions that
*carry data*. Difficulty: small — a `Signal` guarding a result slot, or a settable
`Task<T>` whose result block is filled by hand.

### Cluster D — Static-graph compile-phase analyses

*What render graphs run at compile that we don't. The pipe already **is** the
automatic barrier and `Versioned`'s resync already **is** an auto-inserted
transition — so the genuinely new capabilities are the graph-wide analyses.*

#### 2.23 Change-version skip / memoized nodes — **top graph-side pick**

A node caches the `write_epoch`s of its read-set at completion and, next run,
**skips its whole body if none advanced** — whole systems no-op when their
declared inputs are unchanged (Unity DOTS change filters). Highest-leverage here
because **the primitive already exists**: `Pipe::write_epoch` is a seqlock parity
bumped on every write — an exact change-version source.

```cpp
// illustrative sketch
g.add_node("ai", ai_body, world, nav).skip_if_unchanged();
// next run: if world's and nav's write_epochs are unchanged since last run, skip
```

Difficulty: low–medium, additive — a per-node epoch cache + a skip predicate in
`dispatch_ready`; reuses `write_epoch`; no pipe/coroutine change. Subtlety: a
skipped *writer* must still release successors and not falsely advance a version.

#### 2.24 Automatic pass culling / dead-node elimination

At execute-entry, reverse-reach from nodes marked as frame *outputs*; drop every
node whose writes feed nothing surviving (fixpoint DCE). Lets systems declare
optional/debug/editor passes unconditionally and pay only for what feeds output
(UE RDG `CullPasses`). Needs one new surface — a `Graph_node::output()` /
"escapes the frame" marker — else everything looks dead.

```cpp
// illustrative sketch
g.add_node("submit", submit_body, cmd).output();   // a sink
g.compile();   // nodes not reaching a sink are culled per run
```

Difficulty: low — we already Kahn-check at `compile()`; add a reverse BFS + a
live-bit, skip culled dispatch. Compile-time cost, amortized by run-many.

#### 2.25 Conditional / branch nodes + cyclic (loop) subgraphs

A node picks at runtime which successor(s) fire (branch/switch), or a back-edge
forms a loop inside a single graph (fixed-point iteration, converge-until-stable).
Our graph is strictly acyclic/build-once. The imperative forms
(branch/loop/switch around `co_await`s, `co_await inner.execute()` in a `while`)
are *already native* to coroutine nodes — and dodge Taskflow's documented
weak/strong-dependency deadlock/race pitfalls entirely. The one true gap is the
**static-graph-with-a-branch/loop-node the tracer and DOT still see as
structure** (the coroutine form is opaque to the profiler). Difficulty: forward
branch-skip is low–medium (don't dispatch culled successors, reuse indegree);
true back-edges break Kahn + the `Run_state` re-arm and are a deliberate non-goal
of the derived-edge model. Deferred-conditional per-run enable predicate (a node
inert this run, successors auto-handled) is the cheap 80% (TODO 2.6).

#### 2.26 Critical-path-derived node priority (HEFT) — close the trace→dispatch loop

Assign per-node priority from *derived* structure — longest remaining path to a
sink (upward rank) — so the critical spine never waits behind off-path work. We
already **measure** critical-path frequency in `--trace` but don't **feed it
back** into dispatch; the game_frame optimisation does "parallelise the fattest
critical bar" by hand. New sub-part: auto-derive the priority hint at `compile()`
from the measured/structural criticality. Difficulty: medium — a compile-phase
upward-rank pass writing into `Graph_node::priority`; the trace already computes
the input. Pairs with 2.8 (runtime escalation) and TODO 2.5.

#### 2.27 Transient graph-scoped resources + lifetime aliasing

A resource that lives only within one run; the graph computes each object's
`[first-write, last-read]` interval and lets disjoint-interval objects share
backing memory (Frostbite/Granite frame-graph aliasing). Two sub-ideas: (a)
run-scoped *scratch* objects as a new user concept (a system produces intermediate
data a later system consumes, then it's gone); (b) a **transient-vs-imported**
classification — graph-managed scratch (poolable/aliasable/cullable) vs imported
persistent state the graph only coordinates (our `Guarded`/`Versioned`), plus an
*export* verb to promote a transient result to persistent after the run (our
nested-run lend is the import half; there's no export half). This *automates* the
CLAUDE.md "arena allocation for scope-bounded lifetimes" note graph-wide and feeds
the arena TODO. Difficulty: medium — intervals fall out of edge derivation; the
typed scratch pool + interval-coloring is the work; the harness is the runtime
oracle for a mis-aliased overlap. The transient/imported flag (low difficulty) is
the foundational enabler that makes 2.23/2.24 *safe*.

#### 2.28 Automatic node fusion of serial chains

Detect a linear single-in/single-out chain over a common object and fuse into one
dispatch — data stays cache-hot, one scheduling hop. Generalizes the manual
`Graph_node::set_inline()` to a compile-time pass. Our own graph-free measurement
found the graph's perf edge *is* resume locality, which fusion attacks — but
`set_inline` already captures most of it, so this is lower priority. Difficulty:
low–medium.

#### 2.29 Split-barrier / two-phase prep edge

Instead of a point-event edge, start expensive readiness work (prefetch,
decompress, a `Versioned` resync) as a *begin* right after the producer and a
light *end/join* right before the consumer, filling the slack the graph can see
between both. `Versioned`'s three-phase publish is already a special case.
Difficulty: medium — an auto-inserted helper-node pair bracketing an edge; the
open question is what the prep is generically. Most compelling with 2.27.

#### 2.30 Dynamic work expansion / amplification node (recipe)

A node computes a runtime count (cull → N clusters, broadphase → M pairs) and
expands into that many downstream tasks, joined in its grant window (D3D12 work
graphs; the CPU analog). `parallel_for` + `co_await inner.execute()` + `add_nested`
*already realize this* — so it's a **recipe + a thin helper** (launch N from a
runtime count, join in-grant) plus tracer support so the expansion shows in the
SVG. The most natural fit to existing primitives of anything in Cluster D.
Difficulty: low.

#### 2.31 `try_put_and_wait` — inject one item, await its subtree

Feed a single item into a live/reusable graph and block only until *that item's*
work finishes, not the whole run (per-request latency in a persistent processing
graph). oneTBB preview feature. Partial gap — our `execute()` runs the whole DAG.
Difficulty: moderate; depends on per-item token identity threaded through the run,
and interacts with the one-run-per-graph rule (TODO 2.3). Lower priority.

#### 2.32 Over-declaration diagnostic + clobber/poison debug mode

Two harness extensions. (a) Report objects a node *declared* but never accessed
(silent lost parallelism; RDG's unused-declaration warnings — TODO 2.9). (b)
**Poison-fill** an object before its first declared writer runs, so a node reading
an object a producer never actually wrote (an incomplete-declaration / missing-edge
bug — our named *completeness hazard*) yields visibly garbage instead of
silently-stale data (RDG `ClobberResources`). We catch *undeclared* access; we
don't catch *declared-but-never-produced* reads. Difficulty: low; opt-in per
poisonable object. Aggregate over runs before warning (conditional early-outs
legitimately skip declared access).

#### 2.33 SSA / N-version resource handles (finer edge derivation)

Each write *renames* a virtual handle to a new version; readers bind a specific
version, so RAW/WAW edges derive from version dataflow and multiple historical
versions coexist — "reader A wants version N, writer B produces N+1" avoids
serializing. Finer than our coarse "shared instance + a writer ⇒ edge", and
generalizes `Versioned` from 2 to N versions. Difficulty: hard — touches
`access.h` mode deduction and `compile()`'s edge builder, needs a version-handle
type distinct from `Guarded<T>&`. High parallelism payoff, hits the core
declaration model. Catalogue as the ceiling of the granularity axis.

#### 2.34 Run-scoped typed blackboard / DAG payload

Two related conveniences: (a) a per-run type-keyed store (`FRDGBlackboard`) so a
distant producer/consumer pair needn't thread state through intermediate
signatures — *ephemeral, single-run*, distinct from our persistent double-buffered
blackboard (3.6); (b) `marl::DAG<T>::run(data)` — a fresh per-run payload threaded
to every node body, so one compiled graph processes many inputs (our nodes bind
object identities at `compile()`; no per-run context). Difficulty: low (a) /
moderate (b); (a) bypasses access-derivation, so restrict to already-ordered data
or pair with explicit `after`.

### Cluster E — Data-parallel algorithms & cross-entity

#### 2.35 `parallel_reduce` (deterministic-by-default)

Per-worker identity-seeded partials, combined at the end — sum/min/histogram-merge
that *returns a value* (vs side-effecting `parallel_for`). A differentiator:
combining partials in **index order** gives bit-reproducible FP for free, so the
*deterministic* variant is our default (inverting TBB, which needs a partitioner
restriction) — aligns with our determinism guarantees.

```cpp
// illustrative sketch
int total = ts::parallel_reduce(bodies, 0,
    [](const Body& b){ return b.contacts; },   // map
    std::plus<>{});                             // associative combine
```

Difficulty: low — `parallel_for` over chunks writing per-worker slots (the
`parallel_recorder` substrate keyed on `current_worker_index`) + index-ordered
combine.

#### 2.36 `parallel_scan` (prefix sum)

Two-pass prefix sum for any associative op — cumulative sums, stream-compaction
offsets, allocation-offset packing of variable-sized items (the naive form is
serial). Difficulty: easy–medium, no new primitive — two `parallel_for`s with a
small serial fold of partials between them; the DAG expresses the
phase-A → fixup → phase-B skeleton.

#### 2.37 `parallel_sort`

Parallel merge/quicksort — spatial sort, draw-order, radix buckets. Missing
entirely. Difficulty: medium — a recursive split + parallel merge on
`parallel_for`/nested launches.

#### 2.38 Partitioner / grain-size knob for `parallel_for`

Pluggable work-splitting: static contiguous *range* (cache-friendly, uniform
cost) vs dynamic *guided* chunking (load-balances a variable-cost body — common in
games with variable-cost entities), plus TBB `auto_partitioner`-style recursive
split-on-steal and optional cross-frame affinity replay of split points. Also a
`fn(begin,end)` range-body signature to amortize per-chunk setup (Unity
`IJobParallelForBatch`). Difficulty: low–medium; additive to `parallel_for` via
`Parallel_options{.partitioner, .grain}`.

#### 2.39 Islands + graph coloring for conflict-free parallel loops

For an interacting loop (physics constraints, particle collisions, n-body): color
the interaction graph so constraints sharing a body get different colors — all
constraints of one color run in parallel with no write conflicts; independent
islands run fully parallel (Jolt `LargeIslandSplitter`, Avian solver).
`parallel_for` fans out *independent* iterations and has nothing for iterations
that write shared neighbors. `Deferred` with commutative commands already covers
the *scatter-then-merge* variant; the **colored-phases** variant (a barrier
between colors) is new.

```cpp
// illustrative sketch
ts::parallel_for_colored(constraints, color_of,   // barrier between color bands
    [](Constraint& c){ c.solve(); });
```

Difficulty: medium (coloring is user code; the library piece is the phased
`parallel_for`).

#### 2.40 Sort-key deterministic playback for `parallel_recorder`

Each staged command carries a caller sort-key; the batch is sorted by key at the
cut, so parallel recording under *any* chunking (guided/stealing/long tail) yields
deterministic apply order — closing the documented "commutative-only" limitation
of `parallel_recorder` without forcing index-keyed lanes (Unity ECB `sortKey`).
Difficulty: medium — a key field + a sort at the cut in `journal.h`; touches
`Versioned` replay determinism, not the pipe.

### Cluster F — Scheduling policies

#### 2.41 Time-budget / amortized-across-frames scheduling

Give a work category (streaming, GC, navmesh rebuild) a per-frame *time budget*;
run as much as fits, defer the rest to next frame — spreading a large job across N
frames to hold a stable frame time. `Frame_gate` re-aligns to a boundary but
doesn't cap work per frame. The 80% is a coroutine that `co_await gate.next()`
when its elapsed budget is spent; the missing piece is scheduler-level budget
accounting + a low-priority pull that stops at the cap. Difficulty: medium.

#### 2.42 Latency-hiding topological reorder (trace-fed)

Among valid orderings, prefer the one that spaces a producer far from its consumer
(and clusters aliasing candidates), hiding latency instead of merely satisfying
dependencies (Granite). We already measure edge meet-points and criticality — feed
them back as a dispatch bias rather than only reporting. Difficulty: medium; a
self-contained heuristic, low correctness risk. Related to 2.26.

#### 2.43 Live stackable global concurrency throttle

Cheaply clamp active worker count (down to serial) for a scoped region *without*
tearing down the pool, nested scopes composing by min (TBB `global_control`). Our
`single_threaded=true` is the static `==1` case; reconfiguration is coarse
teardown+recreate. Difficulty: moderate — park/unpark a subset live. Useful for
debugging, contention control, power.

#### 2.44 Scheduler-aware blocking-call offload

A verb to call a genuinely blocking third-party function (sync file read, vendor
SDK) from a node without occupying a worker: shunt onto a scratch thread, suspend
the task, resume via a `Signal`. In-task blocking is fatal for us and the only
escape is `co_await` over *our* primitives — this is the safe bridge for *foreign*
blocking APIs (Marl `blocking_call`).

```cpp
// illustrative sketch
auto bytes = co_await ts::blocking(  []{ return fread_whole(path); }  );
```

Difficulty: low — essentially `ts::launch` onto a non-worker thread + a `co_await`ed
`Signal`; the value is naming/blessing it.

#### 2.45 Cooperative yield / reschedule checkpoint

`co_await ts::yield()` re-submits the resume through `submit_ready` at the current
priority — fairness in a long CPU-bound node with no natural suspension point (a
ready higher-priority node can slot in). Our eager model has no voluntary-yield
today. Difficulty: low. Note: a yield while holding a pipe grant keeps the grant
held across the yield (correct, the reentrant-safe case) but lets other work run
while the object stays locked; a no-op in worker-less mode. Pairs with 2.8/2.26
and the yield-points-in-nodes TODO (2.11 there).

### Cluster G — Smaller recipes & ergonomics

#### 2.46 Multi-producer flush node

Make the *apply point* a first-class, orderable graph citizen many producers
target — separating *when you record* (grant-free, many recorders) from *when the
world quiesces to apply* (one declared node, frame-ordered), collapsing many
mid-frame stalls into one predictable point (Unity `EntityCommandBufferSystem`). A
convention/helper committing a *set* of `Deferred`s at one declared node — no core
change. Difficulty: low.

#### 2.47 Ordered ticket / turnstile

A free-standing "commit in arrival order" gate (`marl::Ticket`) for in-order
submission where the ordered resource isn't a single `Guarded` — the standalone
version of the pipe's FIFO. Difficulty: low (same intrusive-FIFO-of-`Signal`s +
cascade as the pipe, minus reader-group logic). Overlaps 2.14.

#### 2.48 Targeted `drain(obj)` boundary verb

Block (from outside a task) until one *object's* pipe is quiescent, leaving
unrelated in-flight work running — vs `sync()` on a named `Task` or a world
barrier (Unity `CompleteDependencyBeforeRW`). The pipe already has an idle condvar
for teardown; expose a boundary-only `drain(obj)`. Difficulty: low–medium; a
coroutine form `co_await drain(obj)` is a thin awaiter.

#### 2.49 Async-IO completion bridge (recipe + minimal reactor)

Submit OS async IO (io_uring / IOCP), suspend the coroutine, resume from the
completion — overlapping streaming/decompression with compute and folding loaded
data into the graph. The bridge is trivial (`Signal::trigger` from the completion
callback, TODO 6.6); the platform reactor + "who owns the WAITING" is the real
work. Difficulty: medium — a documented recipe + a minimal reactor, not a new core
primitive. `callbackFlow`'s lesson (Kotlin): pair it with RAII teardown that
unregisters on consumer cancel (2.54), and force a buffer-overflow policy since
the callback thread can outrun the consumer.

#### 2.50 Speculative execution recipe

Start the likely (or both) paths early, cancel the wrong/slow one when the truth
is known — trade wasted work for latency (speculative nav/AI queries, straggler
backups). `game_frame` already does this by hand. Package as a recipe on 2.1 +
cancellation, with the harness verifying the loser's writes never landed.
Difficulty: low (on 2.1).

#### 2.51 Recursive fork-join divide-and-conquer (recipe)

Launch-half/launch-half/join for recursively-subdivided algorithms (BVH build,
tree traversal) — Cilk `spawn`/`sync`, continuation-stealing. `co_await`ing launched
children + the resume trampoline is *already* our eager equivalent of symmetric
transfer (steal-the-continuation), so this is largely a **recipe** + verifying
deep recursion stays O(1) stack via the trampoline. Difficulty: low–medium.

#### 2.52 Per-worker sharded object pool

A per-worker-sharded free-list of reusable scratch objects (per-frame temporaries,
decode buffers), lock-free on the owning worker (Go `sync.Pool`). CLAUDE.md's
allocation discipline already prescribes per-type recycling *internally*; the
novelty is a *user-facing* sharded pool for application scratch, keyed on
`current_worker_index` (the `parallel_recorder` overflow-lane already handles
non-worker threads). Difficulty: low.

#### 2.53 General task-local / context propagation

An implicit typed value carried across `co_await` boundaries down the task tree
(inherit-down, no-leak-up, explicit opt-out) — tracing spans, frame index, frame
budget, profile zone — without threading it through every signature (C#
`AsyncLocal`, Python `contextvars`, Swift `@TaskLocal`). We already own the
propagation machinery (segment-swapped TLS + child snapshots); today it has one
hard-coded customer (`Access_context` + `trace_owner`). A general task-local
*generalizes* what `trace_owner` does by hand. Difficulty: low–moderate. Worth it
if per-frame ambients are expected (they are, for a game engine); skip if the
access context stays the only customer.

#### 2.54 Async cleanup / `finally` (async RAII)

A scope-exit hook that runs an *async* cleanup on every exit path including
cancellation — release a resource whose release is itself async (flush a GPU
upload, commit-or-discard a `Deferred`, unregister an IO callback). Our
`Access_guard` is synchronous RAII; a suspended coroutine node can't guarantee async
cleanup on the cancel path today. Difficulty: medium — expressible as a guard
whose teardown schedules a task; `final_suspend` gating on a cleanup child is
exactly what `add_nested` already does. Tension: a teardown that awaits must run
under the holder's grant (the same `Access_context` snapshot as the body), else
it's the "suspend on a foreign object" hazard. Becomes load-bearing the moment we
add streams (2.12) or timeouts (2.3) that abandon a suspended body — Rust's
unsolved `AsyncDrop` is the cautionary tale: *design cancellation so cleanup is
possible*, since retrofitting it is the hardest problem in the space.

#### 2.55 Retraction / oversubscription on a boundary wait

A boundary `sync()` on a not-yet-started target runs *that exact task inline* on
the waiter instead of parking a core (UE `Wait()` retraction); oversubscription
wakes a standby worker when a worker unavoidably stalls (e.g. the one
`parallel_for`-join exemption). Difficulty: retraction low (the inline trampoline
exists — run the block on the waiter if `num_locks==0`); oversubscription medium
(a standby reserve, touches the parking model). Note: we *deleted* general
retraction with the coroutine-first transformation; this is the narrow
boundary-only form, worth weighing against that decision.

#### 2.59 Delegate / event recipe (UE-delegate decomposition) — **shipped as a sample**

**Status: ✅ done — executable in `sample/events.cpp` (2026-08).** Numbered out of
cluster order (added after 2.58; IDs are stable).

How UE-style delegates map onto the suite. A delegate bundles **notification**
("X happened") with **reaction** ("do work in response"); the reaction runs at a
sanctioned point — a graph edge, a dispatch node, an enqueued task — never
inline under the publisher's grant (a synchronous broadcast into foreign systems
is the completeness hazard by construction). Four tiers, cheapest ceremony
first:

| Tier | Use | Firing cost / ceremony |
|---|---|---|
| **Intra-system** | parts of one system, under one grant | a plain synchronous delegate — zero; there is no concurrency inside a grant window. One rule (harness-checked): handlers don't touch other guarded state inline — outbound effects go through `async`/`stage` |
| **Command** (one known target) | "play this sound" | `target.async(fn)` — one line, exists today |
| **Lightweight notifications** (many types, sparse) | most of UE's delegate zoo | one `Versioned<Event_board>` per *domain* with a typed lane per event kind; **staging is grant-free**, so firing is one line from anywhere — no declaration, no edge, no board contention; flip + readers are fixed per-domain cost, and readers overlap as readers |
| **Structural / heavyweight flows** | dependencies that should shape the schedule | a dedicated batch object + derived edge — O(events), not O(population); visible in the DOT dump and the trace |

Load-bearing ordering facts the recipe rests on: journal apply order is
recorder-creation order (a clear-recorder minted first applies before any
producer's events regardless of when the clear was staged), and grant-free
staging leaves no derived edges — same-frame delivery needs an explicit
`flip.after(producers...)` intent edge. Honest costs: board delivery is deferred
(events land at the flip — immediate reaction is a command or intra-system by
definition); cross-recorder order is arbitrary (one producer per lane, or sort —
2.40); closure-tier staging allocs past SBO make this a concrete motivating
fixture for the typed-POD command tier (`command-buffer-design.md` §7.3).
Upgrades: 2.16 (event bus with subscriber cursors), 2.17
(`Versioned::changed()`), 2.19 (mailbox), 2.23/2.40 (skip-on-quiet-frame,
deterministic playback — this recipe is a motivating fixture for both).

### Cluster LOW — Named threads / affinity *(low priority — see ranking policy)*

#### 2.56 Pinned / named-thread routing

A task that must run on a specific thread (OS message pump, render/RHI thread,
audio thread, a vendor SDK's owning thread), plus external-thread registration to
receive routed work and park for it (enkiTS `IPinnedTask`/`RegisterExternalTaskThread`,
UE `ENamedThreads`). The underlying *need* is real and is the packaging answer to
TODO 6.6 — but as ranked, it is platform-layer infrastructure, not a suite
pattern, and its need is met today by a dedicated 1-worker `Scheduler` +
`Scheduler_scope` / `Frame_gate` / `Signal`. Sketch, when built: a per-thread
inbox drained preferentially + a non-stealable affinity tag on the block + a
`co_await` that resumes on the named thread. Difficulty: moderate.

#### 2.57 Per-node execution-lane affinity / second pool (async-compute)

`Graph_node::on_scheduler(pool)` — run a subgraph on a second pool (streaming,
decode, low-priority sim) with the graph auto-inserting the cross-lane fence at the
feedback edge (RDG async-compute). Collides head-on with the single-global-scheduler
decision — a design call, not just code. Low / design-tension.

#### 2.58 `task_arena` / NUMA / core-type affinity

Bind a workload to a concurrency context; pin to a NUMA node or P/E core type
(TBB `task_arena` + `constraints`). The *opposite* of the single-process-wide
bet — a large new subsystem, deliberate non-goal. Note only: TBB's `isolate` (a
suspended task not stealing unrelated work) is achieved *by construction* for us —
the never-block-in-a-task rule is a stronger static form.

---

## Part 4 — Coroutine-machinery notes (about our eager-`Task` itself)

Lower-level than patterns: observations about our coroutine mechanics from the
C++ coroutine-library sweep. Mostly "already have / design note", not new user
features.

#### 4.1 Lazy-task duality — a deliberate road not taken, worth a written rationale

Every surveyed library uses *lazy* tasks (`initial_suspend = suspend_always`); we
are eager. Laziness buys: (a) a constructed-but-unawaited task never runs, so it
can't outlive its captures (structured-scoping safety for free); (b) "build N then
start collectively" (safe `when_all`/`when_any`); (c) no inter-thread race to
publish a result to a not-yet-attached awaiter; (d) HALO frame-elision fires more
readily. We chose eager for the fast path (settled-await never suspends,
settled-consume) and pay for the missing free synchronization with the resume
trampoline + grant/context snapshotting. **Action: none** except documenting *why
eager* — and considering a **combinator-internal `Lazy_task`** (never
user-facing) *only if* 2.1/2.5 prove awkward on eager children (where
"build-then-start" is the safe idiom and eager children force the
cancel-already-running compromise).

#### 4.2 `await_transform` as an enforcement/injection lever

If a promise defines `await_transform`, the compiler routes *every* `co_await` in
that body through it — a hook to (a) enforce the blue boundary structurally
(`=delete` an await of a `sync()`/`take()`-shaped thing), (b) install the
"suspension under guard is fatal" check (`access_guard_depth`) uniformly instead of
per-awaiter, or (c) inject the cancellation token (2.4) at every await without a
trailing param. **Load-bearing caveat:** `await_transform` sees only `co_await`
operands — it *cannot* police the synchronous prefix of an eager coroutine (that
already ran at construction). This *reinforces* our snapshot-at-frame-creation
design and explains why we can't lean on an await-funnel alone. Design note.

#### 4.3 `ValueTask` ready-path — mostly have; consider generalizing the sentinel

.NET's `ValueTask` retrofits a zero-alloc synchronous path onto a `Task`-first
world (with a nasty await-once constraint). Our eager `Task` already banks the
"settled-await never suspends" half, and `Deferred::commit`'s inline arm already
returns a **shared static pre-settled block, no allocation** — precisely
ValueTask's sentinel pattern. Possible refinement: generalize that sentinel to any
"produce an already-ready `Task<R>`" call (e.g. `access` when `pipe_try_inline`
succeeds) so we never allocate a block for a synchronously-known value — and make
the single-consume constraint *structural* (our `take()`) rather than
by-convention. Low-medium priority; the single-allocation discipline already
captures most of the win. Ties to TODO 1.1.

#### 4.4 Symmetric transfer — already the strongest eager variant

Our thread-local resume trampoline is the hand-rolled iterative equivalent of
handle-returning `await_suspend` tail-calls, and is *stronger* than QCoro's eager
`Task` (which loops `.resume()` over a waiter vector, risking stack growth).
Validation, not a gap. One micro-refinement: where a resume is same-thread and the
target is immediately runnable, a handle-returning awaiter could bypass even the
trampoline enqueue for a true tail-jump — worth checking our awaiters already do
this for the settled case.

#### 4.5 Ad-hoc `async_mutex` — don't build; the pipe is stronger

cppcoro/folly/concurrencpp ship `async_mutex`/`coro::Mutex`/`SharedMutexFair`.
Our pipe/`Guarded` *is* an async reader/writer lock with signature-deduced mode
and canonical-address deadlock-freedom, which none of them have (they rely on the
user's lock ordering). A free-standing `async_mutex` re-opens exactly the manual
lock-ordering deadlock risk our canonical ordering eliminates — keep resisting it.
The only piece that might justify itself is the `async_condition_variable`
wait-on-predicate shape, and even that maps onto `Signal` + re-check. (Contrast for
the record: concurrencpp's `async_lock` is deliberately non-recursive — the
opposite of our `writer_owner == current_task` reentrant arm.)

---

## Part 5 — Convergent validation & conscious non-gaps (cite, don't build)

External evidence for the core design bets, and things consciously excluded.

#### 5.1 The pipe / `Versioned` / `Signal` were independently reinvented

**HPX `async_rw_mutex`**, **Folly `coro::SharedMutex`/`SerialExecutor`**, **PPL
`overwrite_buffer`/`single_assignment`/`task_completion_event`**, **Kotlin
`StateFlow`** are independent reinventions of our **pipe** (async reader/writer
serializer), **`Versioned`** (latest-value / overwrite / conflated state), and
**`Signal`** (settable completion / one-shot). Cite in `design.md` as
convergence — the parts other systems bolt on separately, we derived from one
access model.

#### 5.2 The scoped-task trilemma validates deleting `Task_scope`

The structured-concurrency literature (Smith "Go statement considered harmful";
withoutboats "The Scoped Task Trilemma") proves {concurrency, parallelizability,
borrowing} can't all coexist for scoped tasks. Our removed `ts::nested`/`Task_scope`
was exactly a borrowing-concurrent-child nursery; we kept parallelizability +
borrowing via *synchronous* joins (`parallel_for`) and cut the concurrent-borrowing
corner. `ts::launch` is the one intentional leak escape. Cite in
`coroutine-first.md` §4.3. Optional low-cost add: a `goleak`-style "no detached
launch alive at shutdown" assert under `TS_SAFETY_CHECKS`.

#### 5.3 Our cancellation model avoids the `select!` data-loss footgun

**Setup.** In Rust/Tokio (and the poll-based async model generally), a task is a
*state machine that makes progress only when polled*, and **cancelling it means
destroying it on the spot** — at whatever `.await` point it happens to be parked
on. `select!` does exactly this: the moment one branch completes, the other
branches are dropped mid-flight.

**The hazard, concretely.** Say a losing branch is running this:

```rust
// Rust — a select! branch
let item = queue.pop().await;   // (1) item removed from the queue, now a local
process(item).await;            // (2) parked here when select! cancels this branch
```

If `select!` cancels this branch while it is parked at (2), the whole state machine
is destroyed — and `item` is destroyed with it. But `item` was already **removed
from the queue** at (1) and never processed and never put back. It is silently
gone: no error, no log, the queue is just missing an element. This is what the Rust
community calls "cancellation safety" — the property that a future can be dropped
at an await point *without* losing data — and the notorious part is that most code
is *not* cancellation-safe and the compiler doesn't warn you.

**Why we don't have this.** Our cancellation is a **cooperative flag**, not a
drop. A cancelled task is never destroyed out from under itself at an arbitrary
suspension point — it keeps running its current body (or reaches its next explicit
poll of the trailing token) and *then* settles cancelled. There is no moment where
a half-finished body with a live local (the `item` above) is torn down mid-step. So
the "pulled it out of the queue but got destroyed before storing it" loss cannot
happen: whatever the body took, it either finishes handling or reaches a poll point
it chose, with its locals intact.

This is a real **differentiator** — the eager + cooperative-flag model makes
`when_any` (2.1) safe by construction where the ecosystem's default makes it a
footgun — and worth stating plainly in `design.md` when 2.1 lands. (The cost side:
a cooperative loser can't be stopped *instantly* the way a drop can — see the
parked-loser unwind gap in 2.1.)

#### 5.4 Task isolation is achieved by construction

TBB's `this_task_arena::isolate` prevents a thread suspended on nested parallel
work from stealing *unrelated* tasks (a subtle TLS/locality bug). Our
never-block-in-a-task rule (`co_await` suspends and frees the worker rather than
stealing) is a stronger *static* version — nothing to add.

#### 5.5 `co_awaitTry` / `Try<T>` result channel — N/A (exceptions disabled)

folly `co_awaitTry`, Cobalt `gather` capture completion as a value-or-error object
instead of throwing. Exceptions are disabled project-wide; our error model is
`ts::fatal` + cancellation-as-completion-state, so the throwing/`Try` machinery has
no analogue. The *pattern* that maps is a fan-out returning each child's
completion state (completed vs cancelled) without one child's cancellation forcing
the others — relevant to a `collectAllTry`-equivalent if 2.15 is built. Documented
so the record shows it was considered and consciously excluded.

---

## Sources

Primary citations are inline per item. The seven raw research reports (with full
link sets) are the sweep's backing evidence; this document is the consolidated,
deduped, ranked synthesis. The framework roster (Part 1) and per-system detail
live in [task-systems-comparison.md](task-systems-comparison.md); roadmap
cross-references are to [TODO.md](TODO.md).
