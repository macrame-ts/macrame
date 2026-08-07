# Timer / delayed-dispatch primitive: design study

Design-only doc for pattern-farming item **2.2** (the timer / delayed-dispatch
primitive — "foundational, we have none"). No implementation here; this is the
option analysis, the recommended shape, and a rigorous performance-impact
assessment the author asked for. Terminology note: we **launch** tasks, we do not
*spawn* them.

Companion: [pattern-farming.md](pattern-farming.md) §2.2 (motivation, and the
items this unlocks). Grounded in a read of `include/ts/scheduler.h`,
`src/scheduler.cpp` (the `Event_count` park, `signal_submit`, `wait_for_work`,
`run_serial`/`drain_serial_pending`), `include/ts/task.h` (`Signal`,
`External_wait`, `submit_ready`), and `include/ts/frame_gate.h` (the
`open()`-releases-through-the-scheduler pattern this reuses wholesale).

---

## 1. The need and the surface it unlocks

We have **no delayed scheduling at all**: no `sleep`, no `schedule_after`, no
deadline, no timed park. Every wait in the system is either a data/completion
`co_await` or a blue-thread `sync()`. That blocks a whole family of items that a
game frame loop routinely wants:

- **2.3 timeout / deadline** — `within(work, d)` is `when_any(work, sleep(d))`;
  needs `sleep`.
- **2.6 retry / backoff** — `co_await sleep(backoff)` between attempts.
- **2.41 time-budget scheduling** — "run until the frame budget is spent" needs a
  clock and a deadline.
- **cancellation-aware waits** — a `sleep` that resumes promptly when its token
  trips, rather than sitting out the full duration.
- **periodic ticks** — `every(d)` for fixed-rate subsystems.

The primitive is small and self-contained; the whole point of doing it first
(pattern-farming §0 build order) is that it is the enabler under the rest of
Cluster A.

---

## 2. Design options

The firing side is not in question: a fired timer routes through **`Signal`** —
already the sanctioned bridge for "a thread the scheduler does not own completes
something" (`External_wait`'s doc comment names OS I/O, a GPU fence, a `Signal`
from an engine thread). A timer is just an *internal* such thread. `Frame_gate`
already demonstrates the exact shape: something off-schedule becomes ready, and
the release is handed to the scheduler rather than run inline. So the options
below differ only in **who tracks the deadlines and how the fire is delivered**.

### 2.1 Option (a) — dedicated timer thread + min-heap *(recommended)*

One thread owns a binary min-heap of `{deadline, Signal, generation}` entries,
parked on a `std::condition_variable::wait_until(head.deadline)` (or an unbounded
`wait` when the heap is empty). On expiry it pops due entries and delivers each
fire (§2.6). Arming pushes an entry and, if the new deadline beats the current
head, notifies the CV. This is concurrencpp's `timer_queue` shape.

- **+** Zero hot-path cost by construction — the heap and thread are a separate
  object the scheduler's `submit`/`find_work`/`run_task` never touch (§6.1).
- **+** Pure standard library (`std::mutex` + `condition_variable` +
  `steady_clock`); no platform timed-futex, no `#ifdef`.
- **+** Composes with everything already built: delivery is a `Signal`, so the
  resume trampoline, the deadlock net, cancellation, and worker-less mode all see
  a shape they already handle.
- **+** Lazily created on first use (§6.1), so a program that never sleeps pays
  nothing — not even the thread.
- **−** One extra thread when timers are used (a parked thread is ≈free; §6.2).
- **−** A single heap mutex; contention if thousands of threads arm concurrently
  (not our scale — §6.3, with a sharding escape noted).

### 2.2 Option (b) — timer wheel

A hashed/hierarchical wheel gives O(1) arm/expire instead of the heap's O(log n).
Justified only at thousands-to-millions of concurrent timers (network stacks).
Our timer population is tens of frame-scoped deadlines; the constant factors and
the coarse-bucket rounding of a wheel are a net loss. **Rejected for v1**;
revisit only if a real workload shows the heap's `log n` on the timer thread
mattering (it is off every hot path regardless).

### 2.3 Option (c) — fold deadlines into the eventcount park

Tempting: a parked worker is *already* blocked in
`Event_count::commit_wait` — could it wake on the nearest deadline instead of
adding a thread? Two hard problems make this the wrong trade:

1. **`Event_count` has no timed wait, and can't cheaply get one.**
   `commit_wait` is `epoch_.wait(key)` — `std::atomic<T>::wait` (event_count.h:32),
   which has **no timeout overload in standard C++**. A timed park means dropping
   to the platform primitive directly (`WaitOnAddress(..., timeout)` /
   `FUTEX_WAIT` with a `timespec`), reintroducing exactly the per-OS code the
   `Event_count` abstraction removed, and re-validating the carefully-tuned
   prepare/recheck/commit/`advance`/`wake_one` protocol (event_count.h:9–68,
   `signal_submit` scheduler.cpp:240) that the `handoff` and `spin_then_block`
   policies depend on. That protocol is load-bearing and measured; perturbing it
   for a feature that is *off on the hot path* is a bad bargain.
2. **It only covers parking policies.** `Idle_policy::spin` never parks
   (`wait_for_work` → `yield`, scheduler.cpp:341); `handoff` parks only the
   non-spinners. A timer that only fires while a worker happens to be parked is
   not a timer. So this option needs the timer thread *anyway* for the spin and
   all-busy cases — it cannot stand alone.

Even setting those aside, a timed park distributes deadline ownership across N
workers (which worker owns the nearest deadline? what happens when it finds work
and stops parking?), which is far more state than one heap on one thread.
**Rejected.**

### 2.4 Option (d) — OS timer objects (`timerfd` / `CreateTimerQueueTimer`)

Each timer becomes a kernel object with an OS callback. **−** Portability: two
disjoint implementations (`timerfd` is Linux-only, `CreateTimerQueueTimer`
Windows-only, macOS wants `dispatch_source`/`kqueue`), which is the platform-layer
work we have explicitly deferred. **−** The OS callback runs on an OS-chosen
thread, so we still need the "don't run frame bodies on the timer thread" hop
(§2.6) — no simplification bought. **−** At our scale one thread + a heap beats N
kernel timer objects. **Rejected for v1**; a `timerfd`-backed heap is a possible
later optimization behind the same API if it ever pays.

### 2.5 Recommendation

**Option (a): a lazily-created, scheduler-owned timer thread with a
`steady_clock` min-heap, delivering fires as `Signal` triggers.** It is the only
option that is simultaneously zero-cost when unused, portable with no `#ifdef`,
and structurally identical to machinery we already ship and trust
(`Frame_gate`/`Signal`/`External_wait`). The heap's `log n` and the single mutex
are non-issues at our timer scale and never touch a hot path.

### 2.6 Delivery: fire → `Signal` → the *right* thread

The one subtlety worth stating precisely. `Signal::trigger()` → `complete()`
resumes awaiters via the **resume trampoline on the calling thread**
(task.h:1202). If the timer thread called `trigger()` directly, the resumed
coroutine frame would run *on the timer thread* until its next suspension —
exactly the hazard `Frame_gate::open()` documents for the frame-loop thread
(frame_gate.h:34–40: an inline trigger "would run every parked frame … before
`open()` returned"). So the timer thread must **not** run frame bodies. It does
what `Frame_gate::open()` does: hand the trigger to the scheduler.

```cpp
// on expiry, the timer thread does NOT run the continuation inline:
ts::launch([sig]() mutable { sig.trigger(); }, { .priority = Priority::low });
```

The timer thread thus stays lean forever — it only pops the heap and enqueues one
tiny launch per fire; the woken frame runs on a worker. (In worker-less mode this
`launch` runs inline on the timer thread's drain — see §4, which is why
worker-less prefers the virtual clock.)

---

## 3. API surface (in our vocabulary)

### 3.1 `sleep`

```cpp
// Awaitable; resumes ~after `d`. Cancellation-aware: if `token` trips first, it
// resumes PROMPTLY (not at the deadline) and reports cancelled.
Task<void> ts::sleep(std::chrono::steady_clock::duration d,
                     Cancellation_token token = {});

// inside a task:
co_await ts::sleep(16ms);
```

Under the hood `sleep` is a thin composition of pieces we already have: a bare
`Signal`, one heap entry `{now + d, signal}`, and an `External_wait` held for the
duration (§7 / the deadlock-net interaction). Cancellation-awareness is a
`Cancel_callback` on `token` that triggers the signal early and marks the heap
entry dead (lazy removal on pop). Because `Signal::trigger` is idempotent
(task.h:1188), the timer-fire-vs-cancel race needs no lock beyond the heap's.

### 3.2 `launch_after` / `schedule_after`

```cpp
// Detached delayed launch — fire-and-forget after a delay.
void ts::launch_after(Duration d, Fn&& fn, Launch_options opts = {});
```

Implemented as `sleep(d)` chained to the existing `launch` — no new dispatch
path. (`launch`, not "spawn".)

### 3.3 Reschedulable deadline

The modern shape (Python `asyncio.timeout` is a context manager whose expiry can
be *moved*, not a one-shot): a handle whose deadline can be pushed out or pulled
in while it is armed — the natural backing for a frame-budget that is recomputed,
or a watchdog that is fed.

```cpp
ts::Deadline dl{ 2ms };
// ... later, from any thread, before it fires:
dl.reschedule(4ms);        // move the expiry; re-heapifies the entry
co_await dl;               // resumes at the current deadline (or on cancel)
```

`reschedule` needs a stable handle into the heap (an index kept current through
sift operations, or a generation-stamped slot — §7). Everything else is `sleep`.

### 3.4 `every` (periodic) — follow-on

A re-arming source (`co_await tick.next()` in a loop) built on `sleep` + a
re-armable `Signal` (or the auto-reset event, pattern-farming 2.21). Listed for
completeness; not needed for the 2.3/2.6/2.41 unlock.

---

## 4. Worker-less / single-threaded mode

Worker-less mode (`Scheduler_config{.single_threaded = true}`) has **no workers**
and runs every submit inline on the submitting thread via the serial trampoline
(`run_serial`/`drain_serial` scheduler.cpp:201–233). A background timer thread is
the wrong fit here for two reasons: (1) it reintroduces a second thread into a
mode whose entire purpose is single-threaded determinism; (2) a fire delivered
from the timer thread would resume the frame on the *timer* thread, violating the
"everything runs on the submitting thread" invariant.

Recommended behavior in worker-less mode: **no background timer thread; timers are
driven explicitly.** Two spellings, both deterministic:

- **Virtual clock (default for tests, §5):** the timer facility reads time through
  a clock the program advances; `clock.advance(d)` fires every due entry inline on
  the advancing thread, in deadline order, through the serial trampoline. No real
  time, fully reproducible.
- **Explicit pump (for a real-time worker-less program):** a
  `ts::pump_timers()` the frame loop calls once per iteration, firing entries due
  against `steady_clock::now()` onto the calling thread's serial queue. This keeps
  ownership of "when timers run" with the single thread, matching the mode's
  contract.

A worker-less `sleep` awaited with no advance/pump would therefore park forever —
which the deadlock net correctly reports (nothing external outstanding will
progress it), the same as any other unfed external wait.

### 4.1 Relationship to `Frame_gate`

`Frame_gate` and the timer are complementary, not overlapping. `Frame_gate`
realigns an *already-ready* task to the next frame boundary (an event the frame
loop owns); the timer makes a task ready *after a duration* (an event wall-clock
owns). A common composition — "resume this roughly 100 ms from now, but at a frame
boundary" — is `co_await sleep(100ms); co_await gate.next();`. Both deliver their
wake by handing a `Signal` trigger to the scheduler at `Priority::low` (§2.6 is
literally `Frame_gate::open()`'s body), so they share the same off-thread-resume
discipline and the same deadlock-net registration.

---

## 5. Determinism and the mock clock

A real timer is wall-clock nondeterministic, which is at odds with the
worker-less deterministic-test story (CLAUDE.md: worker-less serves "deterministic
tests"). The fix is to read time through a **clock abstraction** rather than
calling `steady_clock::now()` directly:

- `Real_clock` (default) — `steady_clock`; the timer thread waits on it.
- `Virtual_clock` — holds a logical `now`; `advance(d)` moves it forward and fires
  all entries with `deadline <= now`, inline and in order. No thread, no real
  time. A test writes:

```cpp
ts::Virtual_clock clock;
Scheduler_scope s{ { .single_threaded = true } };
auto t = some_task_that_sleeps();     // arms sleep(50ms) internally
clock.advance(49ms);                  // nothing fires
TS_CHECK(!t.is_done());
clock.advance(2ms);                   // the sleep's deadline passes -> fires
TS_CHECK(t.is_done());
```

This makes timeout/retry/time-budget logic (2.3/2.6/2.41) unit-testable without
sleeping in real time and without flakiness. The clock is a construction-time
choice on the timer facility; production uses `Real_clock`, tests inject
`Virtual_clock`. (This mirrors how mature systems test time: Kotlin
`TestCoroutineScheduler`, Tokio `time::pause`/`advance`.)

---

## 6. Performance-impact assessment

The governing requirement: **the timer facility must be exactly zero-cost when no
timers are armed**, which is the overwhelmingly common state for most frames and
all timer-free programs.

### 6.1 Hot path (submit / find_work / run_task) — zero cost, by construction

The timer lives in a **separate object** reached only by `sleep` /
`schedule_after` / `Deadline`. It adds **no field** to `Scheduler`'s hot members,
**no branch** to `submit` (scheduler.cpp:160–175), `find_work`
(scheduler.cpp:275–328), `run_task`, or `signal_submit`. Nothing on the dispatch
path loads a timer field or tests "are timers armed." A program that never calls
`sleep` never even constructs the facility:

> **Lazy creation.** The timer thread and heap are created on the *first* arm and
> torn down with the scheduler. Timer-free programs pay nothing — not a thread,
> not an allocation, not a branch. (Contrast option (c), which would have put a
> timed-wait decision into every worker's park.)

This is the decisive argument for option (a) over (c): (c) cannot be zero-cost on
the park path because the park path would have to consult the nearest deadline.

### 6.2 Idle / wake protocol — untouched; the extra thread is ≈free

Because delivery is a normal `Signal`+`launch`, the eventcount wake protocol
(`signal_submit`, `park`, `handoff_wait`) is **byte-for-byte unchanged** — the
timer's fire is just another producer calling `launch`, indistinguishable from any
other submit. So the block-vs-spin wake-latency/throughput characteristics
(≈194 ns/op spin series; the game_frame handoff/park behavior) are unaffected: no
new path, no altered protocol.

Cost of the extra thread itself: a parked `condition_variable::wait_until` costs a
thread stack (~a few KB) and a kernel thread slot, and **zero CPU** while parked.
Wakeups/sec under a realistic per-frame-timer workload — say one 16 ms `sleep`
armed and fired each frame at 60 fps:

- ~60 arms/sec, ~60 CV notifies/sec, ~60 CV wakeups/sec, ~60 `launch`es/sec.

That is noise next to a frame's thousands of task dispatches. Even a pathological
"a timer per node per frame" (say 150 timers/frame × 60 = 9,000 fires/sec) is one
extra `launch` per fire — the same cost as 9,000 extra `async` submits/sec, still
small against the per-frame task volume (game_frame is ~135–153 tasks/run × 60).

### 6.3 Producer cost (arm / disarm) — off the submit hot path

Arming is paid **only by `sleep` callers**, never by `submit`:

- Arm: lock the heap mutex, `push_heap` (O(log n)), and iff the new deadline beats
  the head, one CV `notify_one`. For n = tens, `log n` is ~5 comparisons.
- Disarm (cancellation / early fire): mark the entry dead (generation bump); it is
  dropped when it reaches the top (lazy removal), so no O(n) search. The
  `Cancel_callback` that does this is the same mechanism cancellation already uses.

The single heap mutex is the only shared point. At our scale (tens of
timers/frame, armed mostly from worker threads mid-frame) it is uncontended. If a
future workload arms from many threads at once, the escape is sharded heaps (K
buckets, K timer threads, or one thread draining K heaps) — but that is a
scalability lever for a problem we do not have, explicitly out of v1.

### 6.4 Memory

- **Per armed timer:** one inline slot in the heap `std::vector` — `{int64_t
  deadline, Signal handle (one word, shared block), uint32 generation}` ≈ 24–32 B,
  amortized-O(1) via vector growth. **No per-timer heap node.**
- **The `Signal` block:** one allocation (`make_bare_block`, as any `Signal`),
  poolable via the same free-list plan as other blocks (CLAUDE.md allocation
  discipline). It can be *the coroutine frame's* member for the `sleep` coroutine —
  the frame already holds the awaited `Signal`, so the heap stores a handle copy
  (shared block) and no separate allocation beyond the frame is required.
- **The facility:** one thread + one vector + one mutex + one CV, created once,
  lazily.

### 6.5 Expected overhead and a benchmark plan

Expected: **0 ns on the hot path**; per-`sleep` cost ≈ one `Signal` block alloc +
an O(log n) heap push + (conditionally) one CV notify; per-fire cost ≈ one heap
pop + one `launch`. To validate:

1. **Zero-hot-path proof.** Run the existing spin-throughput microbench
   (≈194 ns/op empty-task series) with the timer facility compiled in but **no
   timers armed**. Expectation: within noise of baseline. This is the load-bearing
   number — it proves §6.1.
2. **Arm/fire microbench (new).** Arm N `sleep`s, measure (a) arm cost
   (ns/arm vs N, confirming O(log n)) and (b) fire→resume latency (deadline to the
   awaiting frame resuming on a worker). Compare fire→resume against a bare
   `Signal::trigger`+resume to isolate the timer-thread hop overhead.
3. **game_frame makespan.** Run game_frame with one per-frame timer armed (e.g. a
   speculative-work deadline) and confirm makespan and core-utilization are
   unchanged vs the no-timer baseline — the timer thread is off the critical path.
4. **Wakeup accounting.** A debug counter on the timer thread's CV wakeups/sec
   under the game_frame workload, to confirm §6.2's arithmetic empirically.

Regression-gate item 1 alongside the existing spin series; the others are one-off
validation.

---

## 7. Open questions / risks

1. **Deadlock-net interaction (must-fix).** The net fires when the scheduler is
   `quiescent()` (all workers idle + queues empty, scheduler.cpp) and nothing is
   registered via `External_wait` (task.h:922). A pending timer means the scheduler
   *will* make progress, but the timer thread is **not** a worker, so all workers
   could be idle while a `sleep` is outstanding → **false deadlock report** unless
   each armed timer holds an `External_wait` (or a dedicated pending-timer counter
   feeds `scheduler_quiescent`). Recommended: `sleep` holds an `External_wait` for
   the armed duration, exactly as `Frame_gate::next()` arms one (frame_gate.h:64).
   This is the single correctness item that must ship with the primitive.
2. **Reschedule handle representation.** `Deadline::reschedule` needs to find its
   heap entry after arbitrary sift operations. Options: an index kept current
   through the heap's swaps (intrusive), or a generation-stamped slot table the
   entry points back into. The generation approach also cleanly handles
   fire-vs-reschedule and cancel-vs-fire races (a stale generation is ignored on
   pop). Decide before implementing 3.3.
3. **Monotonic vs wall-clock.** `sleep(duration)` is `steady_clock` (monotonic,
   immune to NTP/DST). A `schedule_at(time_point)` against `system_clock` is a
   *different* need (calendar deadlines) and a separate, later API — do not
   conflate; durations cover 2.3/2.6/2.41.
4. **Timer coalescing / tolerance.** Swift's `Clock` carries a `tolerance` so the
   OS can batch wakeups for power. Worth a `sleep(d, tolerance)` overload later
   (mobile/handheld); irrelevant to correctness, omit from v1.
5. **Worker-less real-time story.** §4 recommends virtual-clock or explicit-pump;
   if a real-time worker-less program genuinely wants a background timer thread,
   that reintroduces a second thread into the "single-threaded" mode and needs a
   defined target thread for the fired `launch`. Flag as unsupported in v1 (use the
   pump), revisit on demand.
6. **One global facility vs per-scheduler.** With the single global scheduler
   there is one timer facility, torn down on `configure_scheduler`/`Scheduler_scope`
   exit (must join the timer thread in the same teardown that joins workers). A
   `sleep` outstanding across a reconfigure is a use-during-reconfigure error, same
   contract as any in-flight work during teardown (CLAUDE.md: reconfigure is a
   quiescent-point operation).
7. **Priority of the fire.** §2.6 uses `Priority::low` (matching `Frame_gate`).
   For a *deadline* (2.3) the resumed work may be latency-sensitive; consider
   letting `sleep`/`Deadline` carry the resume priority (default `low`, overridable)
   rather than hard-coding it.
