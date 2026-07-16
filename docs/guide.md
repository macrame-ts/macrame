# User guide

This is the user-facing guide to the library: what it does, how to use each
layer, and how to choose between them. It assumes you are comfortable with
threads, mutexes, and atomics, but not with game engines — engine-flavored
examples are explained as they appear. For the *why* behind the design (and
what was tried and rejected), read [design.md](design.md). For internals,
see the deep docs listed at the [end](#further-reading).

Status note: the library is pre-1.0. APIs are stable in shape but not frozen;
areas marked **WIP** below are actively evolving.

---

## 1. What this library is

A C++23 task system: a work-stealing thread-pool scheduler plus the layers a
real application needs on top of it — dependencies, cancellation, coroutines,
data-parallel loops, and (the distinctive part) **access-declared
concurrency**:

> You declare which shared data each piece of work reads or writes. The
> library schedules around the conflicts, and a runtime harness catches any
> access you did not declare.

That inverts the usual model. Instead of guarding data with locks and hoping
every code path remembers to take them, you attach the data to its guard
(`Guarded<T>`), and the only way to reach the data is by declaring the access
— from which the library derives safe parallel schedules mechanically.

Terminology used throughout, defined once:

- A **task** is a unit of work (usually a lambda) that the scheduler runs on
  a worker thread. Its handle is `ts::Task<R>` where `R` is the result type.
- A **worker** is one of the scheduler's threads. **Work stealing** means an
  idle worker takes queued tasks from a busy worker's queue, keeping all
  cores fed without a central bottleneck.
- A **guarded object** is a `T` wrapped in `ts::Guarded<T>` — the wrapper owns
  the instance, and all concurrent access goes through it.
- An **access mode** is read (`const`) or write (non-`const`). A **grant** is
  the permission a task holds to touch a specific object in a specific mode
  while it runs.
- The **harness** is the runtime checker: instrumented methods verify the
  calling task actually holds a grant for the object, and abort with a
  diagnostic if not.
- The **pipe** is the per-object queue inside `Guarded<T>` that admits readers
  concurrently and writers exclusively, in submission order.

Everything lives in namespace `ts`. Exceptions are disabled by design: all
non-recoverable failures call `ts::fatal`, which prints a message plus a stack
trace and aborts. Most safety checking is gated by `TS_SAFETY_CHECKS`
(default: on).

---

## 2. Quick start

Launch a task, get its result:

```cpp
#include "task.h"

ts::Task<int> t = ts::launch([] { return 6 * 7; });
int v = t.sync();   // block until done; returns const& (see §4.3)
```

Guard a thread-unsafe object and access it from anywhere:

```cpp
#include "guarded.h"

ts::Guarded<std::vector<int>> numbers;

// write: functor takes T& -- exclusive
numbers.access([](std::vector<int>& v) { v.push_back(1); });

// read: functor takes const T& -- concurrent with other reads
ts::Task<size_t> n = numbers.access([](const std::vector<int>& v) { return v.size(); });
size_t count = n.sync();
```

Both accesses run on the object in submission order: the write runs alone, reads
run concurrently with other reads. `access` is *opportunistic* — it runs the
functor immediately on the calling thread when the object is free, otherwise it
queues (see §5); either way you get a `Task<R>` and only `sync()` waits.

The scheduler starts lazily with one worker per hardware thread. To configure
it, construct your own:

```cpp
ts::Scheduler scheduler{ { .num_threads = 4 } };
```

(The library currently routes `ts::launch`/`async` to a process-wide default
scheduler; per-scope scheduler selection is **WIP**.)

---

## 3. The access model

### 3.1 Declaring access

Access is declared by *parameter const-ness*. A functor taking `T&` declares a
write; `const T&` declares a read. You saw it in `access` above; the static
graph (§6) uses the same rule across multiple objects at once:

```cpp
graph.add_node([](Physics& p, const Nav& n) { /* writes p, reads n */ },
               physics, nav);
```

This one convention drives everything: the pipe's reader/writer admission,
the graph's derived ordering edges, and the harness's grants.

### 3.2 The harness

Checking is opt-in per type: you instrument the methods of a guarded type
with `TS_CHECK_ACCESS()`:

```cpp
#include "access.h"

class Nav
{
public:
    Path find_path(Vec3 from, Vec3 to) const
    {
        TS_CHECK_ACCESS();   // verifies the caller holds a read grant on THIS instance
        ...
    }
    void update(float dt)
    {
        TS_CHECK_ACCESS();   // verifies a write grant
        ...
    }
};
```

The macro checks `this` against the grants of the currently running task
(a thread-local set the library installs around every task body). A call with
no grant — a stray pointer, a forgotten declaration, a lambda that captured a
reference it shouldn't have — aborts with the type name, the required mode,
and a stack trace. The check costs about a nanosecond; with
`TS_SAFETY_CHECKS=0` it compiles out entirely.

**Stronger than race detection.** Conventional tools — ThreadSanitizer, or a
game engine's access detector such as Unreal's `FRWAccessDetector` — catch a data
race only when it actually *happens*: two threads touching the data in the same
window. A dormant violation that didn't happen to race on a given run stays
invisible until the timing shifts (a different core count, a production build, a
slower frame). The harness checks something different, and stronger: not "is
another thread touching this right now?" but "did the running task *declare* this
access?". An undeclared access therefore faults the first time its code path runs
— deterministically, whether or not a real race occurred at that moment — so a
latent bug surfaces at the point of the violation instead of waiting for unlucky
timing. You are validating *intent* (the declared grant) rather than observing
*collisions*, which is why a single test run over a code path is enough to catch
what a race detector would only find under the right schedule.

### 3.3 The trust model, honestly

Two limits you should understand:

- The harness only checks **instrumented methods**. Direct member access, or
  methods without the macro, are invisible to it. Instrument every public
  method of types you guard (a clang-tidy check to enforce this is **WIP**).
- Safety is only as complete as the declarations. The graph can only order
  around access it knows about; a task touching an object it never declared
  is exactly what the harness exists to catch at runtime. Treat a harness
  abort as a real bug, never as noise.

Sub-work inherits grants: a task launched from inside a task body (or a
`parallel_for` chunk, or a nested task) carries the parent's grants, so
fan-out over data the parent owns just works.

---

## 4. Tasks

### 4.1 Launching

```cpp
ts::Task<int> a = ts::launch([] { return compute(); });
ts::Task<void> b = ts::launch(io_work, { .priority = ts::Priority::low });
```

`ts::launch(fn, opts)` runs `fn` as soon as a worker is free.
`ts::Launch_options` carries `{ .token, .priority }` — a cancellation token
(§4.5) and a queue priority (`high`, `normal`, `low`; §10.1).

### 4.2 Dependencies

To run a task after others complete, build it with `ts::task(...).after(...)`:

```cpp
ts::Task<void> load  = ts::launch(load_assets);
ts::Task<void> parse = ts::launch(parse_config);

auto ready = ts::task([] { finish_boot(); })
                 .after(load, parse)
                 .launch();
```

`after` accepts any mix of `Task<R>`s; the dependent runs once every
prerequisite has settled. If a prerequisite was *cancelled*, the dependent is
cancelled too (cancellation propagates forward; §4.5).

### 4.3 Getting results: `sync()` and `take()`

`sync()` blocks until the task settles and returns the result **by
`const&`** — it does not consume. Any number of readers may `sync()` the same
task; the result is immutable once set:

```cpp
ts::Task<Mesh> m = ts::launch(build_mesh);
const Mesh& view1 = m.sync();
const Mesh& view2 = m.sync();   // fine
```

To *move* the result out (ownership transfer, or a move-only type), use
`take()` — the one destructive read, which must be last:

```cpp
ts::Task<std::unique_ptr<Level>> t = ts::launch(load_level);
std::unique_ptr<Level> level = t.take();
```

A `sync()` on a task that hasn't started may run it (and its un-started
prerequisites) *inline on the calling thread* instead of waiting — this is
called **retraction** and is what makes blocking on a task safe even when all
workers are busy blocking on other tasks. It applies to plain tasks only, not
to pipe (`async`) work.

`sync()` waits for the task itself — **not** for continuations chained on it
(§4.4). To wait for a continuation, `sync()` the task `then` returned.

### 4.4 Continuations: `then` and `when_all`

```cpp
ts::Task<int> price = fetch_price();
ts::Task<int> doubled = price.then([](int p) { return p * 2; });

ts::Task<int> a = ..., b = ...;
ts::Task<int> sum = ts::when_all(a, b).then([](int x, int y) { return x + y; });
```

`then(fn, opts)` schedules `fn` when the producer completes; for a non-void
producer, `fn` receives the result by reference. `opts` is `ts::Task_options`
`{ .token, .priority, .run_inline }`.

`when_all(tasks...)` joins several tasks into a `Task<std::tuple<...>>`.
`void` prerequisites contribute ordering only and drop out of the tuple
(all-`void` joins produce `Task<void>`); move-only results are moved in. The
consuming `then` may take the tuple by reference, or — as above — take the
elements *unpacked* as separate parameters ("apply-style").

### 4.5 Cancellation

Cooperative, value-based (no exceptions):

```cpp
ts::Cancellation_source src;
ts::Task<int> t = ts::launch(heavy_work, { .token = src.token() });

src.request_cancel();          // not-yet-started work is skipped
...
if (t.is_cancelled()) { ... }  // settled as cancelled
```

Rules:

- A cancelled task **settles cancelled** instead of running; cancellation
  propagates to dependents (`after`) and continuations (`then`).
- `sync()` on a cancelled `Task<void>` simply returns; on a cancelled *value*
  task it is fatal (there is no result) — check `is_cancelled()` first.
- A cancelled `when_all` prerequisite cancels the whole join.

For cancellation arriving *mid-run*, a body may opt in by declaring a
trailing token parameter — it receives the task's token and can poll:

```cpp
ts::launch([](ts::Cancellation_token tok)
{
    while (!done && !tok.is_cancel_requested())
        step();
}, { .token = src.token() });
```

A cooperative early return settles the task **completed** (it ran), not
cancelled. This works in every body position: `launch`/`task`/`nested`
bodies, `async` accessors (`[](T& v, ts::Cancellation_token t)`), and `then`
continuations. For work that blocks rather than polls, register a push
notification: `ts::Cancel_callback cb(token, [] { wake_the_socket(); });` —
`request_cancel()` invokes it synchronously.

### 4.6 Nested tasks

Work launched *inside* a task body can gate the parent's completion:

```cpp
ts::launch([]
{
    for (int i = 0; i < 4; ++i)
        ts::nested([i] { process_chunk(i); });
}).sync();   // returns only after all 4 nested tasks finished
```

`ts::nested(fn)` launches and attaches in one step; `ts::add_nested(task)`
attaches an existing task. Nested work inherits the parent's access grants.

### 4.7 Reusable tasks

`ts::task(fn)` returns a `Task_builder` that can re-run the same task without
reallocating:

```cpp
auto step = ts::task([&sim] { return sim.tick(); });

step.launch();
int r0 = step.sync();

step.reset().launch();       // re-arm and run again
int r1 = step.sync();
```

Constraints: one run in flight at a time; `reset()` only after the previous
run settled and its result was consumed. The cancellation token is **fixed at
creation** (set it once with `.token(t)` before the first launch) and carries
over every rerun — since cancellation is one-way, a reusable task whose token
fired stays cancelled on re-runs; use a fresh task for a fresh cancellation
scope. `ts::Signal` (§10.3) is the reusable phase gate built on the same
mechanism.

---

## 5. `Guarded<T>` — the access-controlled wrapper

`Guarded<T>` owns a `T` (constructed in place; constructor arguments forward)
and is the only sanctioned way to touch it across threads. You never hold a
bare `T&`; you submit accessors with `access` (the default) or `async`:

```cpp
ts::Guarded<World> world{ initial_seed };

world.access([](World& w) { w.step(); });                      // exclusive write
auto pop = world.access([](const World& w) { return w.population(); });  // concurrent read

world.async([](World& w) { w.expensive_rebuild(); });          // heavy: always scheduled
```

`access` and `async` differ only in *where* the functor may run; both declare
the same access (write / read, from const-ness) and both return `Task<R>`:

- **`access`** is **opportunistic**: when the object is free at call time it
  runs the functor immediately on the *calling* thread — no scheduling — and
  otherwise queues it. That fast path suits the many short critical sections
  typical of this API, at the cost of briefly blocking the caller when it takes
  it. This is the default; reach for it unless you have a reason not to.
- **`async`** always schedules the functor onto a worker, never the caller's
  thread. Use it for a heavy functor you don't want running inline (it would
  block the caller and hold the object longer), or when you specifically want
  fire-and-forget submission that never blocks.

(This is distinct from *task* inline dispatch — `set_inline` / `run_inline` on
`launch`/`then`/graph nodes — which is about running a ready task on the
thread that settled its last prerequisite. `access` is about a free object at
call time. Different mechanisms; only the task one is called "inline".)

Semantics of the per-object pipe:

- **FIFO**: accessors run in submission order. A read observes exactly the
  writes submitted before it — this ordering is the correctness contract,
  not an implementation detail.
- **Reader coalescing**: consecutive reads run concurrently; a write runs
  alone.
- **Non-blocking**: submission never blocks the caller; completion drives
  admission.

Options are the same `ts::Task_options` as `then` — `{ .token, .priority }`
apply to `access` and `async` alike (a cancellation token, a scheduling
priority). Two notes:

- Whether a functor may run inline is chosen by the verb (`access` vs `async`),
  not by an option. From inside a graph node, prefer `async` for anything
  non-trivial — an inline `access` blocks the worker for the body's duration.
- The destructor waits until the pipe drains; the object outlives every
  pending accessor.

### 5.1 Multi-object access

To touch several guarded objects in one body, use the free function:

```cpp
ts::Guarded<Physics> physics;
ts::Guarded<Render> render;

ts::access([](const Physics& p, Render& r) { r.mirror(p); }, physics, render);
// options-first form: ts::access({ .priority = ts::Priority::high }, fn, objs...)
```

The free functions `ts::access` / `ts::async` mirror the member verbs;
`ts::async` always schedules. (The opportunistic inline fast path is not yet
implemented across multiple objects, so multi-object `ts::access` currently
schedules like `ts::async` — **WIP**.) Per-argument modes come from const-ness,
as always. The library acquires the pipes in a canonical global order and holds
them for the body — the standard deadlock-free discipline, shared with the
static graph, so dynamic multi-object work and graph nodes can never deadlock
each other.

### 5.2 What `Guarded` is not

It is not a mutex wrapper: you submit a functor rather than lock/unlock around
raw access. With `async` (or a contended `access`) the body runs later on a
worker and the caller keeps going; with an uncontended `access` it runs inline
right away. Either way you get a `Task<R>`; if you need the result *now*, that
is `sync()` and you should be sure you are allowed to block (§11.2).

---

## 6. `Static_task_graph` — build once, run many

For a fixed frame/pipeline structure, declare the whole thing once and run it
every iteration:

```cpp
ts::Guarded<Input>   input;
ts::Guarded<Physics> physics;
ts::Guarded<Anim>    anim;
ts::Guarded<Render>  render;

ts::Static_task_graph frame;

frame.add_node([](Input& in)                          { in.poll(); },            input);
frame.add_node([](const Input& in, Physics& p)        { p.step(in); },           input, physics);
frame.add_node([](const Input& in, Anim& a)           { a.advance(in); },        input, anim);
frame.add_node([](const Physics& p, const Anim& a, Render& r)
                                                      { r.build(p, a); },        physics, anim, render);

frame.compile();

for (;;)
    frame.execute().sync();   // runs the DAG in parallel; physics & anim overlap
```

`compile()` derives edges from access conflicts — two nodes conflict when
they share an object and at least one writes it — plus any explicit
`after`/`before` you add on the returned `Graph_node` handles, then verifies
the result is a DAG (a cycle is fatal, with the nodes named). Conflicting
nodes with no explicit order run in declaration order, deterministically.

`execute()` returns a `Task<void>` completion handle; runs are sequential
(one at a time) and re-runnable, and a run allocates almost nothing (node
state is built at compile time and re-armed per run).

Node capabilities:

- `node.priority(p)` — queue priority per node.
- `node.set_inline()` — run the node on the thread that readied it when its
  objects are immediately available (low-latency chaining for small nodes).
- Node bodies may spawn **nested tasks** (§4.6) — the node's completion, and
  thus its successors, gate on them; nested work inherits the node's grants,
  so dynamic fan-out over the node's data passes the harness.
- `execute(scheduler, token)` accepts a cancellation token: not-yet-started
  nodes are skipped and the run's completion settles cancelled.

Objects are held per node, not per run: a node acquires exactly the objects
it declared, for exactly its duration, so an object is free between its
accessors — concurrent `async` work on the same objects interleaves safely
into the gaps (it queues behind the node that currently holds the object
rather than racing it).

---

## 7. `parallel_for`

Data-parallel loops over an index range:

```cpp
ts::parallel_for(n, [&](int i) { out[i] = f(in[i]); });

ts::Task<void> done = ts::async_parallel_for(n, body);   // non-blocking variant
```

Options — `ts::Parallel_options{ .concurrency, .balance }`:

- `concurrency`: number of parallel executors (0 = scheduler width).
- `balance`:
  - `guided` (default) — chunk size shrinks as work drains: low overhead
    early, fine-grained load balancing at the tail.
  - `balanced` — fixed `n/concurrency` chunks; lowest overhead, assumes
    uniform item cost.
  - `unbalanced` — every item claimed individually; maximum balancing,
    maximum overhead.

The calling thread participates in the loop (it does not just wait), which is
what makes *nested* `parallel_for` — a parallel loop inside a parallel loop's
body — deadlock-free even when every worker is occupied. Chunks inherit the
caller's access grants, so a `parallel_for` inside a graph node may touch the
node's declared objects.

Cross-item mutation (item *i* writing item *j*) is not synchronized by
`parallel_for` itself — see the WIP note in §12 and the staging tools in §9,
which cover the common cases today.

---

## 8. Coroutines

If your toolchain has C++20 coroutines, include `coroutine_support.h`; the
task system becomes awaitable with no other changes.

### 8.1 Awaiting tasks

```cpp
ts::Task<int> pipeline()
{
    int a = co_await ts::launch(step_one);          // suspend until done
    int b = co_await ts::launch([a] { return step_two(a); });
    co_return a + b;
}

int r = pipeline().sync();   // a coroutine returning Task<R> is itself a task
```

`co_await task` suspends the coroutine until the task settles and resumes
with the result (`const R&`, same contract as `sync()`). The win over `then`
chains is ordinary control flow: loops, branches, and early returns across
asynchronous steps read like straight-line code. Deep chains resume
iteratively (bounded stack), and each resumed segment carries the
coroutine's access grants.

Cancellation stays value-based: a cancelled awaited `Task<void>` just
resumes; check `is_cancelled()` or poll a token between awaits — there is no
exception to catch (exceptions are off project-wide).

### 8.2 Awaitable access guards

The pipe doubles as an asynchronous reader/writer lock:

```cpp
ts::Task<void> update(ts::Guarded<World>& world)
{
    {
        auto w = co_await ts::read_write(world);   // suspends until exclusive access granted
        w->step();                            // direct World& access, harness-checked
        w->settle();
    }                                         // guard released at scope exit

    auto r = co_await ts::read_only(world);        // shared read guard
    log(r->population());
}
```

Unlike a callback `access`/`async`, the guard gives you a scope with real control flow over
the object. One hard rule: **never `co_await` anything else while holding a
guard** — that would keep the object locked across a suspension of unknown
duration. The library enforces it: such an await is fatal (the suspension
detector), so the mistake cannot ship silently.

---

## 9. `Deferred<T>` and `Versioned<T>` — staged writes

The pipe serializes readers against every writer. When a target has *many*
producers of small writes and many readers — or when readers must see updates
in atomic batches — staging beats direct writes. Two types share the
machinery (a **journal** of staged commands):

### 9.1 `Deferred<T>`: batch your writes

```cpp
ts::Guarded<Score_board> board;
ts::Deferred<Score_board> staged{ board };

// each producer mints ONE recorder (its identity in the apply order) and reuses it
ts::Recorder<Score_board> rec = staged.recorder();

rec.stage([](Score_board& b) { b.add("alice", 10); });   // no access taken; never blocks
rec.stage([](Score_board& b) { b.add("bob", 5); });

// later, at a point you choose:
ts::Task<void> applied = staged.commit_async();          // ONE write applies everything
```

Contracts, briefly (full statements live in
[deferred-versioned-state.md](deferred-versioned-state.md)):

- **Staging is grant-free**: it touches only the journal — safe from any
  thread, never contends with the target's readers.
- **Snapshot semantics**: readers see none of a batch before the commit, all
  of it after.
- **The cut happens when the commit runs**: commands staged after that ride
  the next commit.
- **Ordering**: FIFO within a recorder (rely on it); across recorders the
  order is fixed and reproducible but *arbitrary* — never encode meaning in
  it. If two producers write the same key, that is a design conflict; give
  the key one producer or make the commands commutative.
- **Lost writes are loud**: destroying a `Deferred` with staged, uncommitted
  commands is fatal (under `TS_SAFETY_CHECKS`); `discard()` is the explicit
  escape.

Inside a graph node that already holds write access to the target, apply
without a second pipe trip: `staged.commit(target_ref)`.

For one logical producer parallelized internally (staging from inside a
`parallel_for`), mint a `Parallel_recorder` instead: per-worker storage, no
contention under any chunking. Its trade: cross-thread apply order becomes
nondeterministic, so use it for commands that are per-key-unique or
commutative.

### 9.2 `Versioned<T>`: stable versions for readers

`Versioned<T>` keeps **two replicas** of `T` behind one guarded front.
Readers always see the last *published* version; producers stage into the
next one; `publish()` flips atomically:

```cpp
ts::Versioned<Poses> poses;                       // T must be default-constructible & swappable
ts::Recorder<Poses> rec = poses.recorder();

// producers, all frame long, grant-free:
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// readers, all frame long, see the LAST published version:
poses.read([](const Poses& p) { draw(p); });

// once per frame:
poses.publish().sync();                           // completes at the version flip
```

Key properties:

- **No read-your-writes**: staged outputs arrive as the *next* version.
  Readers wanting the new version order themselves after the publish (e.g.
  `.after(publish_task)`, or a graph edge).
- The flip holds the write access for nanoseconds (a content swap); applying
  the batch and re-syncing the second replica overlap readers. The front's
  address never changes, so graph declarations and the harness see one
  ordinary object.
- In a static graph, publish is a node:
  `g.add_node(ts::publish_body(poses), poses.state())` — declare **read**
  access on `poses.state()` everywhere else; the publish node is the one
  writer.
- **Resync policy** (constructor argument): `replay` (default — re-applies
  the batch to the second replica; commands must be deterministic, e.g.
  capture random rolls at stage time), `copy`, or `overwrite` (you promise
  each version rewrites everything). `set_divergence_check(hash)` verifies
  replay bitwise and aborts on nondeterministic commands — turn it on in
  development builds.
- One publisher at a time, enforced. A dynamic `publish()` and a graph run
  containing a publish node must not race; `publish().sync()` before the run
  is the sanctioned pattern (and is checked, not just documented).

### 9.3 Choosing between them

| your state | use |
|---|---|
| Per-frame delta is *data* (poses, events, facts) | `Versioned<T>` |
| Producing the next state is heavy *computation* over the current one (a physics world) | one `Guarded<T>` machine + `Deferred` inputs + a `Versioned` *extract* of its outputs |
| The delta is most of the state and rebuilding is cheap | plain snapshot swap (`shared_ptr<const T>`); no journal needed |
| Many small writers into live state, readers can see partial progress | plain `Guarded` `access` writes |

The physics decomposition (sealed simulation machine, staged inputs,
versioned pose extract) is implemented end-to-end in `sample/physics.cpp`;
a blackboard (shared key-value fact board with change notification) recipe is
in `sample/blackboard.cpp`. Both are deterministic and verify themselves.

---

## 10. Utilities

### 10.1 Priorities

`ts::Priority { high, normal, low }`, defaulting to `normal`, accepted by
every route: `launch`/`nested` options, builder `.priority(p)`, `async`
options, `then` options, `Graph_node::priority(p)`. `high` is strict (always
served first); `low` still makes progress under sustained load (an aging
valve prevents starvation).

### 10.2 Scheduler configuration

```cpp
ts::Scheduler s{ {
    .num_threads = 0,                                // 0 = hardware concurrency
    .idle_policy = ts::Idle_policy::spin_then_block, // the default
    .spin_cycles = 64,
} };
```

Idle policies decide what a worker does when it finds no work: `spin` (never
sleeps — lowest latency, burns idle cores), `spin_then_block` (default: spin
briefly, then park), and `handoff` (Go-style: a single spinning worker keeps
watch and wakes a successor when it takes work — moves wake costs off
producers; best when submit latency matters).

### 10.3 `Signal`

A manually-completed `Task<void>` — a phase gate:

```cpp
ts::Signal frame_start;
auto work = ts::task(run_systems).after(frame_start).launch();
...
frame_start.trigger();   // idempotent; releases everything gated on it
```

`Signal::reset()` re-arms it for the next phase (same rules as reusable
tasks).

---

## 11. Patterns and rules of thumb

### 11.1 Granularity decides parallelism

Wrap **subsystems**, not tiny objects: `Guarded<Physics_world>`, not
`Guarded<Rigid_body>` per body. Whole-object access means one writer
serializes all readers of that object — so split state along the lines you
want to parallelize (the sample double-buffers its transforms so early
readers and the writer never touch the same object). Tens to low hundreds of
guarded objects and graph nodes is the intended scale; parallelism *inside* a
system comes from `parallel_for` and nested tasks under the system's grant.

### 11.2 Never block inside a task or node

Blocking a worker starves the pool. Consume another system's result with a
continuation or a graph edge, not a `sync()` inside a body. The exceptions
that are safe by design: `parallel_for` (the caller participates) and
`sync()` from *outside* the scheduler (retraction protects it).

### 11.3 Fire-and-forget is fine

`async` returning a `Task` does not mean you must keep it. Dropping the
handle is safe; the pipe still runs the job. Keep it only if you need the
result, completion, or cancellation.

### 11.4 Choosing the tool

| situation | tool |
|---|---|
| fixed per-frame structure | `Static_task_graph` |
| ad-hoc async work, cross-system calls | `launch` / `async` / `then` |
| complex control flow across async steps | coroutines |
| data-parallel loop | `parallel_for` |
| many writers, batched visibility | `Deferred` |
| stable read view + atomic version flips | `Versioned` |
| ordering gate between phases | `Signal` |

---

## 12. Limitations & WIP

Stated plainly; each is on the roadmap (`docs/TODO.md`):

- **Allocation/performance campaign** — WIP. Hot paths currently allocate
  (one control block per task; journal commands heap past a small-buffer
  threshold). Several planned optimizations (pools, arenas, a typed command
  tier) are designed but not landed.
- **Scheduler selection** — WIP. `launch`/`async` use a process-wide default
  scheduler; scoped/ambient scheduler override is designed, not implemented.
- **Platform breadth** — WIP. Developed on Windows (MSVC/clang-cl); the test
  suite also runs on Linux under Clang/TSan. No macOS/console/mobile support
  claims yet.
- **Benchmarks/CI** — WIP. A benchmark suite exists (`--bench`); regression
  tracking and public CI are being set up.
- **Cross-entity mutation inside `parallel_for`** (item *i* writes item *j*)
  — researched, primitives designed (gather/apply mailboxes, interaction
  coloring), not yet shipped.
- **Generic lambdas** in access-deduced positions (`add_node`, `access`,
  `async`) are not supported — parameter const-ness must be introspectable.

---

## Further reading

- [design.md](design.md) — the design rationale: why these primitives, what
  was tried and rejected, and how it compares to other systems.
- [task-internals.md](task-internals.md) — dynamic-task internals (control
  block, lifecycle, retraction, nested tasks).
- [command-buffer-design.md](command-buffer-design.md) — the design study
  behind `Deferred`/`Versioned`, including the UE research.
- [deferred-versioned-state.md](deferred-versioned-state.md) — the staged-
  write layer's contracts and mechanisms, in full.
- [task-systems-comparison.md](task-systems-comparison.md) — survey of UE
  Tasks, TBB, Taskflow, folly, Rayon/Tokio, and others.
