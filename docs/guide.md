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

The scheduler is a single process-wide instance, started lazily with one worker
per hardware thread. There are no ad-hoc scheduler objects to construct;
reconfigure the one global — a coarse teardown+recreate for startup or between
phases (not while work is in flight):

```cpp
ts::configure_scheduler({ .num_threads = 4 });
```

or run a block on a specific pool and restore the previous config on exit:

```cpp
{
    ts::Scheduler_scope pool{ { .num_threads = 4 } };
    // graph.execute(), ts::launch(...), etc. run on the 4-worker pool here
}   // previous config restored
```

`graph.execute()` and `ts::launch`/`async` all use this one global scheduler;
nothing takes a scheduler argument.

### 2.1 Single-threaded (worker-less) mode

```cpp
ts::Scheduler_scope serial{ { .single_threaded = true } };
graph.execute().sync();   // the whole run happens on this thread, deterministically
```

With `single_threaded = true` the scheduler has **no worker threads at all**:
every task executes inline, at the point it is submitted, on the submitting
thread (a chain of tasks drains iteratively — no stack growth). Everything
works — graphs, `async`, nested tasks, `parallel_for`, the harness — and runs
in a deterministic order. Use it for:

- **Debugging and bisection**: breaks parallel but works single-threaded →
  suspect an ordering/declaration bug; breaks in both → plain logic bug.
  Breakpoints and stack traces stay on one thread.
- **Deterministic tests**: same inputs, same execution order, every run.
- **Platforms or builds without threads**, and very small workloads where
  worker wake-up costs more than the parallelism returns.

Semantics to be aware of: a task body runs *before* `launch`/`async` returns
(so launching while holding your own lock runs the body under that lock);
priorities are inert; work triggered from another thread (an external
callback completing a `Signal`) runs on *that* thread — "single-threaded"
means no workers, not one blessed thread. A body that blocks waiting for
something only another thread could produce will deadlock, exactly as in any
single-threaded program.

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

The same rule extends to **generic lambdas**: write `const auto&` for a read
and `auto&` for a write, and the mode is deduced from the spelling — no
annotation needed:

```cpp
graph.add_node([](const auto& p, auto& a) { a.pose(p); },   // p: read, a: write
               physics, anim);
```

(The library cannot see *inside* a generic lambda — its `operator()` is a
template — so it classifies each parameter by whether it can bind an rvalue:
`const auto&` can, plain `auto&` cannot. The effect is exactly "const means
read", same as the non-generic form.)

The full spelling table, for every access-deduced position (`access`/`async`,
multi-object `ts::access`/`ts::async`, and `add_node`):

| parameter spelling | classified as | if the body mutates |
|---|---|---|
| `T&` / `auto&` | write (exclusive) | fine |
| `const T&` / `const auto&` | read (concurrent) | compile error |
| `T` by value or `T&&` | **rejected** (`static_assert`) | — |
| `auto&&` | read | compile error (read bodies receive `const T&`) |
| `auto` by value | read | silent copy — avoid this spelling |

Two of those rows deserve a word. A **by-value** resource parameter is rejected
outright because it would copy the resource and silently discard writes — the
one spelling the library cannot police is the *generic* by-value `auto`, which
is indistinguishable from `const auto&` at the declaration level; don't write
it. And every **read** position hands the body `const T&`, so mutating under a
read classification is a compile error, not a runtime surprise.

If you prefer declaring access explicitly at the call site instead of relying
on parameter spelling, tag every object with `ts::as_read_only` /
`ts::as_read_write` (the tag then wins; don't mix tagged and bare arguments in
one call — that's a compile error):

```cpp
graph.add_node([](auto& p, auto& n) { n.query(p); },
               ts::as_read_write(physics), ts::as_read_only(nav));
```

The tags are also the escape hatch for an `auto&&` parameter that must write
(it classifies as a read by default), and a write tag over a `const T&`
parameter is legal — a deliberate, conservative over-declaration that
serializes where deduction would have allowed concurrency.

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

An inherited grant is only valid while the access scope it came from is
still open. Nested sub-work (`ts::nested`) is always safe — the parent's
completion (and so its grant release) waits for it. A plain `ts::launch`
that is *not* gated as nested can outlive the parent's access scope; if it
then touches the parent's guarded data, the harness aborts with a
stale-grant diagnostic instead of letting the access race whoever holds
the object next. The fix is always the same: gate the sub-work with
`ts::nested`.

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

### 4.2 Composing with `co_await`

Composition — sequencing, joining, transforming results — is written as
coroutines. A function whose return type is `ts::Task<R>` and whose body uses
`co_await`/`co_return` *is* a task; it starts eagerly when called, suspends at
each await that isn't ready yet, and frees its worker while suspended:

```cpp
ts::Task<int> pipeline()
{
    int a = co_await ts::launch(step_one);              // sequence: run after step_one
    int b = co_await ts::launch([a] { return step_two(a); });
    co_return a + b;
}
```

A join is just several awaits — launch everything first (the tasks run
concurrently from the moment they are launched), then await in any order.
Awaiting an already-settled task costs no suspension, so the order of the
awaits does not serialize the work:

```cpp
ts::Task<Scene> load_scene()
{
    ts::Task<Meshes>    m = ts::launch(load_meshes);     // all three in flight
    ts::Task<Textures>  t = ts::launch(load_textures);
    ts::Task<Animation> a = ts::launch(load_animations);
    co_return Scene(co_await m, co_await t, co_await a); // join
}
```

`co_await task` resumes with the result by `const&` (non-consuming, same
contract as `sync()`; §4.3). Loops, branches, and early returns across
asynchronous steps read as straight-line code — there is no callback
vocabulary to learn, and no callback-flavored types to thread results through.
If a prerequisite was *cancelled*, awaiting it: a `Task<void>` simply resumes,
a value task is fatal — check `is_cancelled()` first (§4.4).

One structural rule (the coroutine-lambda trap): a coroutine lambda's captures
live in the lambda *object*, which usually dies at the end of the statement —
while the coroutine's frame lives on. For any coroutine that outlives its
defining statement, use a free (or member) coroutine function and pass state
as parameters; parameters are copied into the frame and live as long as it
does.

### 4.3 The blue boundary: `sync()` and `take()`

Threads split into two kinds: **task threads** (workers running task bodies)
and **blue threads** — `main`, dedicated engine threads, anything outside the
scheduler. Inside a task you *await*; a blue thread cannot await, so it
*blocks*:

```cpp
ts::Task<Mesh> m = ts::launch(build_mesh);
const Mesh& view = m.sync();    // blue thread: block until settled, read by const&
```

`sync()` returns the result **by `const&`** — it does not consume; any number
of readers may `sync()` the same task. To *move* the result out (ownership
transfer, or a move-only type), use `take()` — the one destructive read,
which must be last:

```cpp
ts::Task<std::unique_ptr<Level>> t = ts::launch(load_level);
std::unique_ptr<Level> level = t.take();
```

`sync()`/`take()` are **blue-thread verbs**. Calling them inside a task parks
a worker on work that may need that worker — the pool-exhaustion deadlock —
so a `sync()` that would genuinely block inside a task is **fatal** under
safety checks (§5.0.1). The sanctioned in-task waits are `co_await`, the
`parallel_for` join (it runs chunks on the caller), and gating completion on
children via `ts::nested` (§4.5).

### 4.4 Cancellation

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
  propagates to graph successors as a completion state.
- `sync()` on (or `co_await` of) a cancelled `Task<void>` simply
  returns/resumes; on a cancelled *value* task it is fatal (there is no
  result) — check `is_cancelled()` first and branch.
- A coroutine polls between awaits: check `is_cancelled()` on what it awaited,
  or its own token, and `co_return` early.

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
cancelled. This works in `launch`/`nested` bodies and `async` accessors
(`[](T& v, ts::Cancellation_token t)`). For work that blocks rather than
polls, register a push notification:
`ts::Cancel_callback cb(token, [] { wake_the_socket(); });` —
`request_cancel()` invokes it synchronously.

### 4.5 Nested tasks and scopes

Work launched *inside* a task body can gate the parent's completion:

```cpp
ts::launch([]
{
    for (int i = 0; i < 4; ++i)
        ts::nested([i] { process_chunk(i); });
}).sync();   // returns only after all 4 nested tasks finished
```

`ts::nested(fn)` launches and attaches in one step; nested work inherits the
parent's access grants. In a coroutine body the children also join the
frame's implicit scope, so you can await them mid-body:

```cpp
ts::Task<void> frame_section()
{
    ts::nested(build_shadow_list);
    ts::nested(build_visible_list);
    co_await ts::join_nested();     // both lists done here
    merge_lists();                  // safe to consume their output
}
```

For several independent lifetimes — or a scope handed to helpers — use the
explicit `ts::Task_scope`: `scope.launch(fn)` records a child,
`co_await scope.join()` awaits all recorded children. A scope destroyed with
unjoined children is fatal (lost children), the structured-concurrency
analogue of the journal's lost-writes fatal.

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
  it. This is the default; reach for it unless you have a reason not to. It is
  also **reentrant**: if the calling task already holds this object's write
  access (a graph node's declared write, an enclosing write body), the functor
  runs under that access rather than queueing behind it — so a helper that
  takes a `Guarded<T>&` and calls `access` works whether or not its caller
  happens to hold the object.
- **`async`** always schedules the functor onto a worker, never the caller's
  thread. Use it for a heavy functor you don't want running inline (it would
  block the caller and hold the object longer), or when you specifically want
  fire-and-forget submission that never blocks.

(This is distinct from *graph-node* inline dispatch — `Graph_node::set_inline`
— which is about running a ready node on the thread that settled its last
prerequisite. `access` is about a free object at call time. Different
mechanisms; only the node one is called "inline".)

Semantics of the per-object pipe:

- **FIFO**: accessors run in submission order. A read observes exactly the
  writes submitted before it — this ordering is the correctness contract,
  not an implementation detail.
- **Reader coalescing**: consecutive reads run concurrently; a write runs
  alone.
- **Non-blocking**: submission never blocks the caller; completion drives
  admission.

Options are `ts::Access_options` — `{ .token, .priority }` apply to `access`
and `async` alike (a cancellation token, a scheduling priority). There is no
`run_inline` option: the verb chooses inline-vs-scheduled. Two notes:

- Whether a functor may run inline is chosen by the verb (`access` vs `async`),
  not by an option. From inside a graph node, prefer `async` for anything
  non-trivial — an inline `access` blocks the worker for the body's duration.
- The destructor waits until the pipe drains; the object outlives every
  pending accessor. Two lifetime mistakes are caught with fatals (in
  `TS_SAFETY_CHECKS` builds): destroying a `Guarded` while a compiled
  `Static_task_graph` still references it, and destroying (or move-assigning
  over) a graph while a run is in flight — both would otherwise dangle and
  crash far from the cause.

### 5.0.1 The never-block rule is enforced

Blocking inside a task or node body ties up a worker and risks
pool-exhaustion deadlock; the rule is "await results with `co_await`, or
gate sub-work with `ts::nested` — never `sync()` inside a body". In
`TS_SAFETY_CHECKS` builds a violation is **fatal** at the call: a `sync()`
that is genuinely about to park inside a task aborts with a stack, with two
messages:

- *"sync() inside a task on an access to an object this task already
  holds"* — the certain-deadlock shape: the awaited access is queued behind
  the very grant you are waiting inside.
- *"blocking sync() inside a task"* — the general hazard; `co_await` it
  instead.

What does **not** fire: `parallel_for` inside a node (its join runs chunks
on the caller and waits only on provably running helpers), and any `sync()`
from a blue thread (no task context — blocking is what blue threads do).

The suspended twin of the blocked-thread deadlock is also detected: two
coroutines that each *hold* an object and `co_await` the other's object
deadlock with **no thread parked** — both frames are suspended, every
worker is free, and the frames simply never resume. The safety harness
records waits-for edges at every suspension on a pipe and fatals the moment
an edge closes a cycle, naming both tasks and both objects (§8.2 has the
rule that avoids the shape in the first place).

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

Generic lambdas follow the same spelling rule as everywhere else (§3.1):
`const auto&` positions are reads, `auto&` positions are writes — or tag every
object with `ts::as_read_only`/`ts::as_read_write` to declare modes explicitly:

```cpp
ts::access([](const auto& p, auto& r) { r.mirror(p); }, physics, render);

ts::access([](auto& p, auto& r) { r.mirror(p); },   // same, spelled with tags
           ts::as_read_only(physics), ts::as_read_write(render));
```

### 5.2 What `Guarded` is not

It is not a mutex wrapper: you submit a functor rather than lock/unlock around
raw access. With `async` (or a contended `access`) the body runs later on a
worker and the caller keeps going; with an uncontended `access` it runs inline
right away. Either way you get a `Task<R>`; inside a task, consume it with
`co_await`; on a blue thread, `sync()` (§4.3, §11.2).

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

- `node.after(a, b, …)` / `node.before(…)` — declare explicit ordering edges to
  other nodes (on top of the ones `compile` derives from access). The variadic
  form reads as intent: `submit.after(cmd, particles, ui)` makes `submit` depend
  on all three — the same as `.after(cmd).after(particles).after(ui)`, but
  without reading like a sequence among them.
- `node.priority(p)` — queue priority per node.
- `node.set_inline()` — run the node on the thread that readied it when its
  objects are immediately available (low-latency chaining for small nodes).
- Node bodies may spawn **nested tasks** (§4.5) — the node's completion, and
  thus its successors, gate on them; nested work inherits the node's grants,
  so dynamic fan-out over the node's data passes the harness.
- A node body may be a **coroutine**: return `ts::Task<void>` from the body
  and it may `co_await` mid-node — the node completes (releasing its grants
  and successors) when the *frame* completes, not at the first suspension,
  and its declared grants are held for the frame's whole life. While the
  frame is suspended its worker is free.
- `execute({ .token = t })` accepts a cancellation token: not-yet-started
  nodes are skipped and the run's completion settles cancelled.

Objects are held per node, not per run: a node acquires exactly the objects
it declared, for exactly its duration, so an object is free between its
accessors — concurrent `async` work on the same objects interleaves safely
into the gaps (it queues behind the node that currently holds the object
rather than racing it).

### 6.1 Node names and the structure dump

`add_node` takes an optional leading name — a static string literal, referenced
rather than copied — and `compile` takes an optional file path that writes the
compiled structure as a Graphviz DOT file:

```cpp
auto sim = frame.add_node("physics", [](const Input& in, Physics& p) { p.step(in); },
                          input, physics);
frame.add_node({}, [](Anim& a) { a.advance(); }, anim);   // unnamed: labels as file:line

frame.compile("frame.dot");
```

An unnamed node with a `{}` placeholder labels as the `add_node` call site
(`file:line`); a fully bare `add_node` labels as `node<N>`. Objects are named
too — an optional leading `ts::Named` on the `Guarded` (or `Versioned`)
constructor, a distinct wrapper so it can never be mistaken for `T`'s own
first constructor argument:

```cpp
ts::Guarded<Physics> physics{ ts::Named{"physics"}, world_size };
ts::Versioned<Poses>  poses{ ts::Named{"poses"} };
```

Render the dump with Graphviz (`dot -Tsvg frame.dot -o frame.svg`, or the
repo's `show_graph.bat`) or paste it into an online viewer (e.g. edotor.net).
The output is dark-themed; edges are green with the line style carrying their
origin: solid edges are your explicit `after`/`before` orderings, dashed edges
were derived from declared access — hovering
one (in SVG) shows which object and modes produced it (`physics: W->R`;
unnamed objects fall back to an `objN` ordinal). The picture answers
"why does this edge exist", which is exactly what you need when re-shaping a
graph: a `W->R` edge is real dataflow, while a `R->W` or `W->W` edge is an
ordering artifact you may be able to remove by double-buffering (`Versioned`)
or deferral (`Deferred`).

The dump (and all profiling instrumentation) compiles out with
`TS_PROFILING=0`; the `compile(DOT_path)` parameter remains and becomes a
no-op. Node names are kept in all builds — they are one pointer per node and
pay for themselves in debugging.

### 6.2 Runtime traces: the average run

The structure dump shows what `compile()` derived; a runtime trace shows how it
actually executes. Attach a `ts::tools::Graph_trace` (from `tools/graph_trace.h`)
to a compiled graph, run it any number of times, and render the aggregate:

```cpp
ts::tools::Graph_trace trace;
graph.set_trace(&trace);            // requires compile(); pass nullptr to detach
for (int f = 0; f < 1000; ++f)
    graph.execute().sync();
trace.write_SVG("my_frame_avg.svg");
```

The SVG is an **average run**: bars at the median start with median duration,
packed into anonymous **concurrency rows** — workers are interchangeable and
the node→worker assignment reshuffles every run, so rows carry no worker
identity; time-overlapping bars simply occupy different rows, and row
occupancy over time reads directly as the average frame's concurrency (free
vertical space at some time = capacity genuinely unused there). Dependency
edges connect the bars in the structure dump's styling (solid = explicit,
dashed = derived). Edges are uniformly faint by default so the picture reads as
bars first (critical edges keep their pink colour but no extra prominence —
critical structure at rest is carried by the orange node borders and labels);
hovering a node brightens its incident edges to full so you can trace its exact
dependencies. Hovering a bar raises a formatted tooltip — the node name
coloured by its queue priority (red = high, green = normal, grey = low; the
same colouring as the name on the bar), with its priority tag right-aligned on
that line, then the stats: mean / P95 / σ / CV / min/max execution time; its
**true busy** (body + `parallel_for` slices + async fan-out — for a parallel
node this exceeds the bar, which is wall time, because the bar shows elapsed
while true busy is core time summed across the fan-out); the declared accesses
with each mode colour-coded (**RO** green read-only, **RW** red read-write,
e.g. `transforms: RO`); and the node's incoming (`->|`) and
outgoing (`|->`) edges, each neighbour name coloured by that edge's share of
binding chains. The tooltips are scripted into the SVG itself (they work with
the file open in a browser; not when embedded via `<img>`). A coloured
headline carries three frame classifiers: **core utilization** — the share
of the run window the scheduler's workers spent executing tasks (green ≥ 75%,
red < 50%; work run inline on non-worker threads is not counted) —
**critical path dead time** (green < 5% of frame time, red > 10%), and
**task-system overhead** — the share of the frame's *compute* (body + machinery,
excluding idle) spent in the scheduler's own machinery rather than your functors
(green ≤ 5%, red > 15%). Utilization says how much of the machine the frame used;
dead time says whether the critical chain itself had to wait; overhead says
whether your tasks are coarse enough that scheduling them is cheap relative to
running them (a high figure means the graph is too fine-grained — the tasks cost
less than the machinery that dispatches them). Overhead is an upper bound: it is
measured with tracing on, which adds a per-task clock bracket, so cross-check
against the untraced task throughput if it matters. Each significant chain wait is also drawn in place:
a hatched pink band spanning the picture's full height, occupying the visible
gap between the binding predecessor and the waiting critical node (screen-space
gaps, unioned, so a band is always a real break between bars) — the wait
belongs to the chain, not to any row. Behind the bars, a faint full-height
**core-utilization wash** colours each time slice by how busy the machine was
there (green all cores busy → yellow half → red idle) — sampled from
per-worker busy time bucketed over the run, so every task counts at its real
time (a `parallel_for` node's fan-out across cores registers fully). The green
stretches saturate the cores; the red valleys are idle capacity. A panel below
shows the global numbers: run count, frame time mean/min/max (ms),
**critical path** — the CPM dependency lower bound on frame time (median
durations, no scheduling waits) — worker count, **tasks** (total across
the trace and mean per run: every task the scheduler ran — nodes plus
`parallel_for` slices, async jobs, and continuations — so it far exceeds the
node count and shows the real fan-out volume), and the **body / machinery** split
in µs per run that backs the overhead headline (summed across workers). An edge's tooltip names the ordering it enforces: for a derived edge,
the conflicting resource and the two nodes' modes (`physics RW -> propagation
RO`); for an explicit one, "explicit ordering" (plus any coinciding conflict).

The **critical path** is picked out by colour: each run, the trace walks back
from the last-finishing node through the predecessor whose completion released
each node — the chain that actually bound that run's makespan. Across runs
this yields a per-node *share* (different chains bind different runs). A
node's border and label blend cyan → orange with that share — a solid orange
bar was critical in essentially every run, plain cyan never — and an edge's
colour blends green → pink the same way, keeping its solid/dashed origin
marking. The tooltip adds three numbers:
"critical in N% of runs" (measured), "slack" (structural — how far the node
can slip in the average frame before the dependency chain lengthens; omitted
when ~0), and "dispatch wait" (mean ready-to-start latency: queue + object
acquisition). Measured and structural disagree exactly where scheduling —
not dependencies — binds the frame, which is what you tune next.

No samples are stored: statistics stream into fixed-size state (mean/variance,
P50/P95 quantile markers, min/max, a per-worker histogram), so a million runs
cost the same memory as ten. Two consequences of drawing medians are handled
for you: an edge's bars can overlap in the aggregate even though no real run
overlapped them (medians aren't additive) — such bars are clamped to the
edge's average hand-off point; and any two bars that overlap in time land on
different rows by construction of the packing.

Tracing costs two clock reads per node per run when attached, one branch when
not, and nothing at all with `TS_PROFILING=0`. Cancelled runs are not folded;
a recompile re-pushes the structure and resets the aggregates.

The sample wires this up as `task_system --trace [frames]`, tracing two
variants of the same ~30-system frame on a 6-worker scheduler — a `baseline`
and an `optimised` version tuned by reading the baseline's own trace —
writing `sample_game_frame_avg_baseline.svg` and
`sample_game_frame_avg_optimised.svg` plus `sample_game_frame.dot`; a
no-argument `show_graph.bat` renders the structure dump and opens both.
[example-frame-optimization.md](example-frame-optimization.md) walks the two
side by side: which optimisations the trace says are worth trying, and which
it says not to bother with.

---

## 7. `parallel_for`

Data-parallel loops over an index range:

```cpp
ts::parallel_for(n, [&](int i) { out[i] = f(in[i]); });

ts::Task<void> done = ts::async_parallel_for(n, body);   // non-blocking variant
```

Options — `ts::Parallel_options{ .concurrency, .balance, .priority }`:

- `concurrency`: number of parallel executors (0 = scheduler width).
- `balance`:
  - `guided` (default) — chunk size shrinks as work drains: low overhead
    early, fine-grained load balancing at the tail.
  - `balanced` — fixed `n/concurrency` chunks; lowest overhead, assumes
    uniform item cost.
  - `unbalanced` — every item claimed individually; maximum balancing,
    maximum overhead.
- `priority`: queue priority for the helper tasks. Unset (the default)
  inherits the calling task's priority — a `parallel_for` inside a
  high-priority task or graph node dispatches its helpers at `high`; outside
  a running task it is `normal`. Set it (`{.priority = ts::Priority::low}`)
  to override. The calling thread's own share is unaffected either way — it
  runs inline, not through the queue.

The calling thread participates in the loop (it does not just wait), which is
what makes *nested* `parallel_for` — a parallel loop inside a parallel loop's
body — deadlock-free even when every worker is occupied. Chunks inherit the
caller's access grants, so a `parallel_for` inside a graph node may touch the
node's declared objects.

Cross-item mutation (item *i* writing item *j*) is not synchronized by
`parallel_for` itself — see the WIP note in §12 and the staging tools in §9,
which cover the common cases today.

---

## 8. Coroutines in depth

Composition is coroutines (§4.2); this chapter is the deeper contract. The
support is part of the core (`ts.h` includes it; coroutines are required).

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
with the result (`const R&`, same contract as `sync()`). A coroutine task
starts **eagerly** — the body runs to its first genuine suspension on the
calling thread — and while suspended holds no worker. Deep chains resume
iteratively (bounded stack), and each resumed segment carries the
coroutine's access grants and task identity, whatever thread it resumes on.

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
ts::Task<void> applied = staged.commit();                // ONE write applies everything
```

`commit()` auto-dispatches on grant ownership. Called from the task that holds
the target's write grant — a graph node that declared the write, an
`async`/`access` write body — it applies **inline under that grant**, no second
access acquisition, and returns an already-settled task. Called from anywhere
else, it enqueues one ordinary async write on the target and returns that
write's completion. One verb, both worlds; the old `commit_async` is gone.

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
- **Sync before destroying**: destroying a `Deferred` while an enqueued
  `commit()` is still in flight is fatal (under `TS_SAFETY_CHECKS`) — sync the
  task it returned first. The pending job uses the `Deferred`, and a destructor
  that silently blocked on it would hide a bug. With the last commit settled
  the destructor is non-blocking (inline commits finish in-call).
- **The inline path's task carries no ordering**: when `commit()` applies
  inline (you held the grant), the returned task settled *before* the apply —
  it answers `is_done()` truthfully but provides no happens-before edge.
  Observers of the data order through the object's pipe, which orders.
- **Commit from the grant holder, not nested sub-work**: inside a node/body
  that holds the write grant, call `commit()` there. Calling it from a nested
  task running under the *inherited* grant is a misuse (the nested task is not
  the holder; the enqueued write would queue behind the very grant it waits
  out) — fatal under `TS_SAFETY_CHECKS`.

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
every route: `launch`/`nested` options, `async` options,
`Graph_node::priority(p)`. `high` is strict (always served first); `low`
still makes progress under sustained load (an aging valve prevents
starvation).

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
ts::Task<void> work = [](ts::Signal gate) -> ts::Task<void>
{
    co_await gate;       // suspends until triggered
    run_systems();
}(frame_start);
...
frame_start.trigger();   // idempotent; releases everything awaiting it
```

`Signal::reset()` re-arms it for the next phase — the one sanctioned re-arm
(one use in flight; reset only after it settled and every awaiter resumed;
re-arming an un-triggered signal is fatal).

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

Blocking a worker starves the pool. Consume another system's result with
`co_await` or a graph edge, never a `sync()` inside a body — the harness
makes the violation fatal (§5.0.1). The exceptions that are safe by design:
`parallel_for` (the caller participates, and waits only on running helpers)
and `sync()` from a blue thread (blocking is what blue threads do).

### 11.3 Fire-and-forget is fine

`async` returning a `Task` does not mean you must keep it. Dropping the
handle is safe; the pipe still runs the job. Keep it only if you need the
result, completion, or cancellation.

### 11.4 Choosing the tool

| situation | tool |
|---|---|
| fixed per-frame structure | `Static_task_graph` |
| ad-hoc async work, cross-system calls | `launch` / `async` |
| sequencing / joining / transforming results | coroutines (`co_await`) |
| data-parallel loop | `parallel_for` |
| many writers, batched visibility | `Deferred` |
| stable read view + atomic version flips | `Versioned` |
| ordering gate between phases | `Signal` |

---

## 12. Migrating from the callback vocabulary

Earlier revisions composed with callbacks and builders. That entire surface
is deleted — composition is coroutines — and every removal is a compile
error, not a behavior change. The replacements:

| deleted | replacement |
|---|---|
| `t.then(fn)` | `co_await t`, then just call `fn` — `co_return fn(co_await t);` |
| `ts::when_all(a, b, c)` | launch all, then await all: `co_await a; co_await b; co_await c;` (awaiting settled tasks is free; results come back typed, no tuple) |
| `ts::task(fn).after(x, y).launch()` | a coroutine body: `co_await x; co_await y; fn();` — prerequisites are awaits at the top of the body |
| `Task_builder::priority/token` | `ts::launch(fn, { .priority, .token })` at the launch site |
| `Task_builder::set_inline` / `Task_options::run_inline` | deleted for dynamic tasks (graph nodes keep `Graph_node::set_inline`) |
| `Task_builder::reset()` (reusable tasks) | call the coroutine again — one frame per run is the model; `Signal::reset()` remains the re-armable phase gate |
| `ts::add_nested(task)` | `ts::nested(fn)` (launch-and-attach), or record into a `ts::Task_scope` |
| `Task_options` | `Launch_options` (`launch`/`nested`) or `Access_options` (`access`/`async`) |
| retraction (blocking `sync()` running work inline) | deleted — a blue thread parks (that is fine); an in-task `sync()` is fatal, `co_await` instead |

---

## 13. Limitations & WIP

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
- **Generic by-value parameters** (`[](auto v)`) in access-deduced positions
  classify as reads and copy the resource — writes hit the copy, silently.
  Undetectable at the declaration level (§3.1); use references.

---

## Further reading

- [design.md](design.md) — the design rationale: why these primitives, what
  was tried and rejected, and how it compares to other systems.
- [task-internals.md](task-internals.md) — dynamic-task internals (control
  block, lifecycle, coroutine frames, nested tasks).
- [command-buffer-design.md](command-buffer-design.md) — the design study
  behind `Deferred`/`Versioned`, including the UE research.
- [deferred-versioned-state.md](deferred-versioned-state.md) — the staged-
  write layer's contracts and mechanisms, in full.
- [task-systems-comparison.md](task-systems-comparison.md) — survey of UE
  Tasks, TBB, Taskflow, folly, Rayon/Tokio, and others.
