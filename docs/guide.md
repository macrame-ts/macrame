# User guide

> For the reasoning behind the design, and what was tried and rejected, read
> [design.md](design.md). For internals, see the deeper documents listed at
> the [end](#further-reading).
>
> Samples are gamedev-based for illustration, you don't need to know anything
> about gamedev.
>
> The library is pre-1.0. APIs are stable in shape but not frozen; areas
> marked WIP below are actively evolving.

---

## 1. What this library is

A C++23 task system. It provides a work-stealing thread-pool scheduler plus
the layers a real application needs on top of it: tasks, dependencies,
cancellation, coroutines, data-parallel loops, and access-declared concurrency.

> You declare which shared data each piece of work reads or writes. The
> library schedules around the conflicts, and a runtime harness catches
> violations.

That inverts the usual model. Instead of guarding data with locks and hoping
every code path remembers to take them, you attach the data to its guard
(`Guarded<T>`), and the only way to reach the data is by declaring the access.
The library derives safe parallel schedules mechanically from these declarations.

Terminology:

- A **task** is a unit of work (usually a lambda) that the scheduler runs on
  a worker thread. Its handle is `ts::Task<R>` where `R` is the result type.
- A **worker** is one of the scheduler's threads. **Work stealing** means an
  idle worker takes queued tasks from a busy worker's queue, keeping all
  cores fed without a central bottleneck.
- A **guarded object** is a `T` wrapped in `ts::Guarded<T>`. The wrapper owns
  the instance, and all concurrent access goes through it.
- An **access mode** is read (`const`) or write (non-`const`). A **grant** is
  the permission a task holds to touch a specific object in a specific mode
  while it runs.
- The **harness** is the runtime checker. Instrumented methods verify that the
  calling task actually holds a grant for the object, and abort with a
  diagnostic if not.
- Every `Guarded<T>` has its own **queue** of pending accessors. An accessor
  waits for its **turn**; admission is concurrent for readers and exclusive for
  writers, in submission order.

Everything lives in namespace `ts` ("Task System"). The library itself uses no exceptions. All
non-recoverable failures call `ts::fatal`, which prints a message plus a stack
trace and aborts. Most safety checking is gated by `TS_SAFETY_CHECKS`, which
is on by default.

---

## 2. Quick start

Launch a task, get its result:

```cpp
#include "ts/task.h"

ts::Task<int> t = ts::launch([] { return 6 * 7; });
int v = t.sync();   // Blocks until done; returns a const reference (see §4.3).
```

Guard a thread-unsafe object and access it from anywhere:

```cpp
#include "ts/guarded.h"

ts::Guarded<std::vector<int>> numbers{ ts::Named{"numbers"} };   // The leading name is used in diagnostics.

// A write: the functor takes T&, so access is exclusive.
numbers.access([](std::vector<int>& v) { v.push_back(1); }).sync();

// A read: the functor takes const T&, so it runs concurrently with other reads.
auto n = numbers.access([](const std::vector<int>& v) { return v.size(); });
size_t count = n.sync();
```

Both accesses run on the object in submission order. The write runs alone, and
reads run concurrently with other reads. `access` is opportunistic: it runs the
functor immediately on the calling thread when the object is free, and
otherwise it queues (see §5). It returns a caller-owned `Access_op` rather than
a `Task`, so it allocates nothing. Consume the result once, with `.sync()` or
with `co_await` from a coroutine. Do not discard the handle for a write you
want to happen; use `async` for fire-and-forget (see §5).

The scheduler is a singleton, and you bring it up
explicitly. A scheduler is heavy, so it never starts lazily. Create it once at
startup, before any scheduled work; creating a second one is fatal, because
there is exactly one:

```cpp
ts::create_scheduler({ .num_workers = 4 });   // Called once, at startup.
// ... graph.execute(), ts::launch(...), etc. all run on it ...
ts::destroy_scheduler();                        // Optional; it is torn down at program exit.
```

There are no ad-hoc scheduler objects to construct, and you never hold an
instance, because nothing needs one. To run a region on a different
configuration, tear the scheduler down and create a new one:

```cpp
ts::destroy_scheduler();
ts::create_scheduler({ .num_workers = 4 });
// graph.execute(), ts::launch(...), etc. run on the 4-worker scheduler here
```

`graph.execute()` and `ts::launch`/`async` all use this one scheduler; nothing
takes a scheduler argument. Calling any of them with no scheduler running is fatal.

### 2.1 Single-threaded (worker-less) mode

```cpp
ts::create_scheduler({ .single_threaded = true });
graph.execute().sync();   // The whole run happens on this thread, deterministically.
```

With `single_threaded = true` the scheduler has no worker threads at all.
Every task executes inline, at the point it is submitted, on the submitting
thread; a chain of tasks drains iteratively, so the stack does not grow.
Everything works, including graphs, `async`, coroutine tasks, `parallel_for`,
and the harness, and everything runs in a deterministic order. Use it for:

- Debugging and bisection. Something that breaks in parallel but works
  single-threaded suggests an ordering or declaration bug; something that
  breaks in both is a plain logic bug. Breakpoints and stack traces stay on
  one thread.
- Deterministic tests. The same inputs give the same execution order, every
  run.

A few semantics are worth knowing. A task body runs before `launch`/`async`
returns, so launching while holding your own lock runs the body under that
lock. Priorities are inert. Work triggered from another thread, such as an
external callback completing a `Signal`, runs on that thread; "single-threaded"
means no workers, not one special thread. A body that blocks waiting for
something only another thread could produce will deadlock, exactly as in any
single-threaded program.

---

## 3. The access model

### 3.1 Declaring access

Access mode is defined by parameter const-ness. A functor taking `T&` declares a
write, and one taking `const T&` declares a read. The static graph (§6) uses the same rule:

```cpp
graph.add_node("nav_step", [](Physics& p, const Nav& n) { /* writes p, reads n */ },
               physics, nav);
```

This one convention drives everything: each object's reader/writer admission,
the graph's derived ordering edges, and the harness's grants.

The same rule extends to generic lambdas. Write `const auto&` for a read and
`auto&` for a write, and the mode is deduced from the spelling, with no
annotation needed:

```cpp
graph.add_node("pose", [](const auto& p, auto& a) { a.pose(p); },   // p is a read, a is a write.
               physics, anim);
```

The library cannot see inside a generic lambda, whose `operator()` is a
template, so it classifies each parameter by whether it can bind an rvalue. A
`const auto&` parameter can, and a plain `auto&` cannot. The effect is exactly
"const means read", the same as the non-generic form.

The full spelling table, for every access-deduced position (`access`/`async`,
multi-object `ts::access`/`ts::async`, and `add_node`):

| parameter spelling | classified as | if the body mutates |
|---|---|---|
| `T&` / `auto&` | write (exclusive) | fine |
| `const T&` / `const auto&` | read (concurrent) | compile error |
| `T` by value or `T&&` | rejected (`static_assert`) | — |
| `auto&&` | read | compile error (read bodies receive `const T&`) |
| `auto` by value | rejected (`static_assert`) | — |

Two rows need explanation. A by-value resource parameter is rejected, generic
or not, because it would copy the resource and silently discard writes. For
the generic spelling the rejection comes from a second probe that offers the
position a non-copyable stand-in, which a reference parameter binds and a
by-value parameter cannot copy. Two consequences follow. If the body
intentionally copies the object, for example by returning it by value, the
probe cannot compile it, so tag the argument with `ts::as_read_only` instead.
And for a final or non-class type no stand-in can be derived, so the check is
skipped there and a generic by-value parameter remains a silent copy; avoid
the spelling for those types. Every read position hands the body a `const
T&`, which makes mutating under a read classification a compile error rather
than a runtime surprise.

If you prefer declaring access explicitly at the call site instead of relying
on parameter spelling, tag every object with `ts::as_read_only` /
`ts::as_read_write`. The tag then wins. Do not mix tagged and bare arguments
in one call; that is a compile error.

```cpp
graph.add_node("query", [](auto& p, auto& n) { n.query(p); },
               ts::as_read_write(physics), ts::as_read_only(nav));
```

The tags are also the escape hatch for an `auto&&` parameter that must write,
since it classifies as a read by default. A write tag over a `const T&`
parameter is legal as well; it is a deliberate, conservative over-declaration
that serializes where deduction would have allowed concurrency.

### 3.2 The harness

Checking is opt-in per type. You instrument the methods of a guarded type
with `TS_CHECK_ACCESS()`:

```cpp
#include "ts/access.h"

class Nav
{
public:
    Path find_path(Vec3 from, Vec3 to) const
    {
        TS_CHECK_ACCESS();   // Verifies the caller holds a read grant on this instance.
        ...
    }
    void update(float dt)
    {
        TS_CHECK_ACCESS();   // Verifies a write grant.
        ...
    }
};
```

The macro checks `this` against the grants of the currently running task, a
thread-local set the library installs around every task body. A call with no
grant, whether from a stray pointer, a forgotten declaration, or a lambda that
captured a reference it should not have, aborts with the type name, the
required mode, and a stack trace. The check costs about a nanosecond, and with
`TS_SAFETY_CHECKS=0` it compiles out entirely.

The harness validates intent rather than observing collisions. Conventional
tools, such as ThreadSanitizer or a runtime race detector, catch a data race only when it actually happens,
meaning two threads touch the data in the same window. A dormant violation
that did not happen to race on a given run stays invisible until the timing
shifts with a different core count or a production build. The
harness checks something different. The question is not "is another thread
touching this right now?" but "did the running task declare this access?". An
undeclared access therefore faults the first time its code path runs,
deterministically, whether or not a real race occurred at that moment, so a
latent bug surfaces at the point of the violation instead of waiting for
unlucky timing. You are validating intent, the declared grant, rather than
observing collisions. That is why a single test run over a code path is enough
to catch what a race detector would only find under the right schedule.

### 3.3 The trust model

Two limits you should understand:

- The harness only checks instrumented methods. Direct member access, or
  methods without the macro, are invisible to it. Instrument every public
  method of the types you guard; a clang-tidy check to enforce this is WIP.
- Safety is only as complete as the declarations. The graph can only order
  around access it knows about; a task touching an object it never declared
  is exactly what the harness exists to catch at runtime. Treat a harness
  abort as a real bug, not as noise.

Structurally gated sub-work inherits grants. A `parallel_for` chunk, or a
coroutine segment after a suspension, carries the parent's grants, so fanning
out over data the parent owns just works. The gating is what makes this sound:
the parent's completion, and with it the grant release, waits for the child,
so the grant provably outlives it.

A detached `ts::launch` inherits nothing. Its handle may be dropped, so it can
outlive the parent's access scope. An inherited grant would then race whoever
holds the object next, and the harness would catch that only on a late touch,
or not at all in a shipping build (with the harness compiled out). Running the child under an empty context
instead makes any touch of the parent's guarded data fault deterministically,
on the first access, in every checked run. To fan out over the parent's data,
use `ts::parallel_for`, whose chunks inherit the parent's grant and join
synchronously, staying inside the grant window. Alternatively, acquire fresh
access with `obj.async(...)` or `co_await obj.access(...)`.

---

## 4. Tasks

### 4.1 Launching

```cpp
ts::Task<int> a = ts::launch([] { return compute(); });
ts::Task<void> b = ts::launch(io_work, { .priority = ts::Priority::low });
```

`ts::launch(fn, opts)` runs `fn` as soon as a worker is free.
`ts::Dispatch_options` carries `{ .token, .priority, .name }`: a cancellation
token (§4.4), a queue priority (`high`, `normal`, `low`; §10.1), and a debug
label. Every verb that always schedules its body takes it.

### 4.2 Composing with `co_await`

Composition, meaning sequencing, joining, and transforming results, is written
as coroutines. A function whose return type is `ts::Task<R>` and whose body
uses `co_await`/`co_return` is itself a task. It starts eagerly when called,
suspends at each await that is not ready yet, and frees its worker while
suspended:

```cpp
ts::Task<int> pipeline()
{
    int a = co_await ts::launch(step_one);
    // Sequencing: this runs after step_one.
    int b = co_await ts::launch([a] { return step_two(a); });
    co_return a + b;
}
```

A join is just several awaits. Launch everything first, since the tasks run
concurrently from the moment they are launched, then await them in any order.
Awaiting an already settled task costs no suspension, so the order of the
awaits does not serialize the work:

```cpp
ts::Task<Scene> load_scene()
{
    ts::Task<Meshes>    m = ts::launch(load_meshes);     // All three tasks are in flight.
    ts::Task<Textures>  t = ts::launch(load_textures);
    ts::Task<Animation> a = ts::launch(load_animations);
    co_return Scene(co_await m, co_await t, co_await a); // The join.
}
```

`co_await task` on an lvalue handle resumes with the result by `const&`. It
is non-consuming, with the same contract as `sync()` (§4.3). Awaiting an
rvalue task, as in `co_await ts::launch(...)` or the joins above, is a
consuming await: the temporary handle dies with the statement, so the result
is moved out and returned by value, mirroring `take()`. Loops, branches, and early returns
across asynchronous steps read as straight-line code. There is no callback
vocabulary to learn, and no callback-flavored types to thread results through.
If a prerequisite was cancelled, awaiting a `Task<void>` simply resumes, while
awaiting a cancelled value task (a task with a non-void result) is fatal, so check `is_cancelled()` first
(§4.4).

One structural rule is worth knowing, the coroutine-lambda trap. A coroutine
lambda's captures live in the lambda object, which usually dies at the end of
the statement, while the coroutine's frame lives on. For any coroutine that
outlives its defining statement, use a free or member coroutine function and
pass state as parameters. Parameters are copied into the frame and live as
long as it does.

### 4.3 The blue boundary: `sync()` and `take()`

Threads split into two kinds. Task threads are workers running task bodies.
Blue threads are `main`, dedicated engine threads, and anything else outside
the scheduler. Inside a task you await; a blue thread cannot await, so it
blocks:

```cpp
ts::Task<Mesh> m = ts::launch(build_mesh);
const Mesh& view = m.sync();    // On a blue thread: block until settled, then read by const reference.
```

`sync()` returns the result by `const&`. It does not consume, and any number
of readers may `sync()` the same task. To move the result out, for an
ownership transfer or a move-only type, use `take()`. It is the one
destructive read, and it must come last:

```cpp
ts::Task<std::unique_ptr<Level>> t = ts::launch(load_level);
std::unique_ptr<Level> level = t.take();
```

Because `sync()` hands back a reference into storage the task's block owns,
binding it to the result of a temporary handle dangles. The last handle dies
at the end of the full expression:

```cpp
const Report& bad = world.async(build_report).sync();   // Dangles.
auto ok = world.async(build_report).sync();             // Safe; the value is copied inside the expression.
```

`Access_op::sync()` on an rvalue does return by value (§5), because an
`Access_op` is single-owner and provably the last one. A `Task` is a
refcounted handle that may have live copies, so it has no such overload.

`sync()` and `take()` are blue-thread verbs. Calling them inside a task parks
a worker on work that may need that worker, which is the pool-exhaustion
deadlock, so an in-task `sync()` is fatal under safety checks (§5.0.1). The
sanctioned in-task waits are `co_await` and the `parallel_for` join, which
runs chunks on the caller and is also how you fan out over the parent's data
(§4.5).

Both verbs assert that the task cannot be cancelled and abort if it was. That
is right when no token is in play but wrong when one is, because you cannot
check and then read without a race. Two accessors branch instead:

```cpp
if (std::optional<Mesh> m = t.try_take())   // Never blocks; empty if unsettled
    use(*m);                                // or if cancelled.

std::optional<Mesh> m = co_await t.as_optional();   // Waits; empty if cancelled.
```

`try_take()` never blocks, so it is legal inside a task too. It is the
non-blocking spelling of "consume it if it is ready". Both move the result out
like `take()`, so either must be the last consume. That is checked rather than
just documented. A `take()` after any consume is fatal in checked builds, and
a second `try_take()` reads empty rather than handing back the moved-from
object. Neither accessor exists for `ts::Task<void>`: there is no result to be
missing, `is_done()` answers the first question, and awaiting a cancelled void
task already resumes normally.

### 4.4 Cancellation

Cooperative, value-based (no exceptions):

```cpp
ts::Cancellation_source src;
ts::Task<int> t = ts::launch(heavy_work, { .token = src.token() });

src.request_cancel();          // Work that has not started yet is skipped.
...
if (t.is_cancelled()) { ... }  // The task settled as cancelled.
```

Rules:

- A cancelled task settles cancelled instead of running; cancellation
  propagates to graph successors as a completion state.
- `sync()` on, or `co_await` of, a cancelled `Task<void>` simply returns or
  resumes. On a cancelled value task it is fatal, since there is no result;
  check `is_cancelled()` first and branch.
- A coroutine polls between awaits. Check `is_cancelled()` on what it awaited,
  or its own token, and `co_return` early.

For cancellation arriving mid-run, a body may opt in by declaring a trailing
token parameter. It receives the task's token and can poll:

```cpp
ts::launch([](ts::Cancellation_token tok)
{
    while (!done && !tok.is_cancel_requested())
        step();
}, { .token = src.token() });
```

A cooperative early return settles the task completed, because it ran, not
cancelled. This works in `launch` bodies, in `async` accessors
(`[](T& v, ts::Cancellation_token t)`), and in graph node bodies (§6, where
the token delivered is the run's). For work that blocks rather than polls,
register a push notification such as
`ts::Cancel_callback cb(token, [] { wake_the_socket(); });`. A
`request_cancel()` invokes it synchronously.

### 4.5 Fanning out over the parent's data

To parallelise over data a task already holds a grant on, use
`ts::parallel_for`. Its chunks inherit the parent's access grants through a
by-value `Access_context` snapshot, and its join is synchronous, meaning the
caller participates and does not return until every chunk has run. The
sub-work therefore stays strictly inside the parent's grant window, and the
harness accepts each chunk's touch of the parent's guarded data:

```cpp
ts::launch([&mesh]
{
    ts::parallel_for(4, [&mesh](int i) { process_chunk(mesh, i); });
}).sync();   // Returns only after all four chunks have finished.
```

Inside a coroutine body the same call gates a mid-body consume. The loop has
finished when `parallel_for` returns, so its output is ready on the next line:

```cpp
ts::Task<void> frame_section()
{
    ts::parallel_for(2, [&](int i) { i == 0 ? build_shadow_list() : build_visible_list(); });
    merge_lists();   // Both lists are done here, so it is safe to consume their output.
}
```

There is no verb for a concurrent child that inherits the parent's grant and
outlives the fan-out point. An earlier design offered one, `ts::nested` plus
an explicit `ts::Task_scope` nursery, and it was removed. A child that runs
concurrently with its parent while sharing the parent's access grant can race
the parent on the same mutable guarded state, and because both sides
"declared" the access the harness cannot see it. The synchronous join of
`parallel_for` closes that window, since the child never outlives the parent's
use of the object. When you need genuinely independent work, launch it
detached with `ts::launch`, which inherits nothing, and have it take its own
turn on the object via `obj.async(...)` or `co_await obj.access(...)`. When it
needs last-frame data, read a `Versioned` snapshot. When it produces outward
writes, stage them with `Deferred`. See docs/coroutine-first.md §4.3 for the
full rationale and the field survey behind the removal.

---

## 5. `Guarded<T>` — the access-controlled wrapper

`Guarded<T>` owns a `T`, constructed in place with forwarded constructor
arguments, and is the only sanctioned way to touch it across threads. You
never hold a bare `T&`. You submit accessors with `access`, the default, or
with `async`:

```cpp
ts::Guarded<World> world{ ts::Named{"world"}, initial_seed };   // The leading name, then T's constructor arguments.

world.access([](World& w) { w.step(); }).sync();               // An exclusive write.
auto pop = world.access([](const World& w) { return w.population(); });  // A concurrent read.
int n = pop.sync();

world.async([](World& w) { w.expensive_rebuild(); });          // A heavy functor; always scheduled.
```

`access` and `async` differ in where the functor may run and in what they
return. Both declare the same access, write or read, from const-ness. `async`
returns a free-standing `Task<R>`, which you can dispatch, await, `sync()`, or
drop for fire-and-forget. `access` returns a caller-owned `Access_op<T, Body>`.
The whole operation, including the result storage, the body, and the queue
entry, lives in that handle, so an `access` allocates nothing. It is attended,
meaning you consume the result exactly once:

- Use `co_await op` from a coroutine, or `op.sync()` from outside a task. On
  an lvalue, `op.sync()` is a non-consuming `const R&` peek; on an rvalue
  (`world.access(fn).sync()`) it returns `R` by value, so the temporary stays
  dangle-free. `op.take()` is the explicit consuming move. `op.try_take()`
  never blocks, reading empty until settled, and is also legal inside a task.
  `co_await op.as_optional()` waits but yields empty on cancellation.
- The handle is pinned, meaning non-copyable and non-movable, because the
  object's queue holds the address of its embedded entry. Store a `Task<R>`
  from `async` if you need a movable handle, and keep the `Access_op` local
  otherwise.
- Destroying an `Access_op` whose access has not settled is a bug, which the
  destructor reports through `TS_ENSURE` and then blocks on. A discarded
  `access` for a write is therefore only safe when it ran inline. If it may
  enqueue, either `.sync()` it or use `async`.

For a member that must outlive one call, an `Access_op` also has a deferred
form. Default-construct it unbound and then call `bind(target, body)` followed
by `start()`, or construct it as `Access_op(ts::dormant, target, body)` to
store it without firing. A `start()` refires a settled op with the same
storage, allocation-free, can be called multiple times.

Where the functor may run:

- `access` is opportunistic. When the object is free at call time it runs the
  functor immediately on the calling thread, with no scheduling, and otherwise
  it queues the functor. That fast path suits the many short critical sections
  typical of this API, at the cost of briefly blocking the caller when it
  takes it. Use it by default unless you have a reason not to.
  An object the calling task already holds is lent rather than acquired. If
  the caller has the access this functor needs, such as a graph node's
  declared write or an enclosing write body, the functor runs under that
  access instead of queueing behind it. A helper that takes a `Guarded<T>&`
  and calls `access` therefore works whether or not its caller happens to hold
  the object.
- `async` always schedules the functor onto a worker, never inline. Use it for a heavy functor you do not want running inline, since it
  would block the caller and hold the object longer, or when you specifically
  want fire-and-forget submission that never blocks.

This is distinct from graph-node inline dispatch (`Graph_node::set_inline`),
which is about running a ready node on the thread that settled its last
prerequisite. `access` is about a free object at call time. They are different
mechanisms, and only the node one is called "inline".

Semantics of the per-object queue:

- FIFO. Accessors run in submission order. A read observes exactly the writes
  submitted before it; this ordering is the correctness contract, not an
  implementation detail.
- Reader coalescing. Consecutive reads run concurrently, and a write runs
  alone.
- Non-blocking. Submission never blocks the caller; completion drives
  admission.

There are two option structs, split so that no verb takes a field it cannot
honour. `ts::Dispatch_options` carries `{ .token, .priority, .name }`, a
cancellation token, a scheduling priority, and a debug label, and goes to
every verb that always schedules its body: `async`, `ts::launch`,
`Deferred::commit`, and `Versioned::publish`. `ts::Access_options` adds
`.queued` and goes to the opportunistic verbs, `access` and `Versioned::read`,
which are the only ones with an inline arm to skip. There is no `run_inline`
option; the verb chooses between inline and scheduled. Two notes:

- Whether a functor may run inline is chosen by the verb, `access` versus
  `async`, not by an option. From inside a graph node, prefer `async` for
  anything non-trivial, because an inline `access` blocks the worker for the
  body's duration. If you want the attended `Access_op` result but never the
  inline arm, for a heavy body whose result you still stay for, pass
  `.queued = true`. It skips only the inline-when-free arm. An object the
  calling task already holds is still lent and still runs inline; that part is
  correctness, not opportunism.
- The destructor waits until the queue drains, so the object outlives every
  pending accessor, including the one you just `sync()`ed. A task wakes its
  waiters before it releases the objects it held, which means a returned
  `sync()` says "this task settled", not "the object is free". The
  destructor's drain is what makes `t.sync(); /* destroy the object */` safe.
  Two lifetime mistakes are caught with fatals in `TS_SAFETY_CHECKS` builds:
  destroying a `Guarded` while a compiled `Static_task_graph` still references
  it, and destroying or move-assigning over a graph while a run is in flight.
  Both would otherwise dangle and crash far from the cause.

### 5.0.1 The never-block rule is enforced

Blocking inside a task or node body ties up a worker and risks pool-exhaustion
deadlock. The rule is to await results with `co_await` or fan out with
`ts::parallel_for`, and never `sync()` inside a body. In `TS_SAFETY_CHECKS`
builds a violation is fatal at the call, with two messages:

- *"sync()/take() inside task X on an access to Y, which this task already
  holds"*. This is the certain-deadlock shape, where the awaited access is
  queued behind the very grant you are waiting inside.
- *"sync()/take() inside task X"*. This is the general hazard. Use `co_await`,
  or read it with `try_take()` (§4.3).

The check fires on the call, not on whether that particular run would have
blocked, so `sync()` inside a task is illegal even when the target is already
settled. That is deliberate. A check that triggers only when the wait
genuinely parks inherits the hazard's own timing. It stays quiet through
development, where targets are usually settled, and then parks a worker on the
one frame where a prerequisite runs long. Checking the rule instead means the
first execution of a bad path fails, every time.

If you know the wait is bounded by something the library cannot see, say so at
the site and keep the rest of the program checked:

```cpp
ts::Relaxed_scope relax{ ts::Rule::in_task_sync };   // "this wait cannot deadlock, and here is why"
```

Two things do not fire the check: `parallel_for` inside a node, whose join
runs chunks on the caller and waits only on provably running helpers, and any
`sync()` from a blue thread, which has no task context, since blocking is what
blue threads do.

The suspended twin of the blocked-thread deadlock is also detected. Two
coroutines that each hold an object and `co_await` the other's object deadlock
with no thread parked. Both frames are suspended, every worker is free, and
the frames simply never resume. The safety harness records wait edges at every
suspension on an object's queue and fatals the moment an edge closes a cycle,
naming both tasks and both objects. §8.2 has the rule that avoids this shape
in the first place.

### 5.0.2 Lock ranks for dynamically-awaited objects

Everything the library acquires in a batch, whether a node's declared set or a
multi-object `ts::access`, is taken in one canonical order, all or nothing, so
it cannot deadlock however many objects it names. What nothing orders is a
grant a task already holds against an object it `co_await`s later. That
missing constraint is the suspended-deadlock hole, and a lock rank closes it.
If every dynamic await must strictly climb, a wait cycle cannot be written.

```cpp
ts::Guarded<Physics> physics{ ts::Named{"physics"}, ts::Rank{ 10 } };
ts::Guarded<Audio>   audio  { ts::Named{"audio"},   ts::Rank{ 20 } };

// A node holding physics may await audio (20 > 10), never the reverse.
float mix = co_await audio.access([](const Audio& a) { return a.mix_level(); });
```

Two things are deliberate. Ranks are not defaulted, neither to address order
nor to declaration order, because a default would make rejection depend on the
ABI, and a program that built today could then be rejected tomorrow with no
source change. Unranked objects are treated strictly: a task holding an object
with no `ts::Rank` may not dynamically await at all. Only objects actually
involved in a dynamic await need a rank, and a graph that never awaits outside
its declared sets never sees this rule.

The rejection is deterministic. It fires on the first offending await, not
when two halves of a cycle happen to interleave, which is the difference
between this and the circular-wait detector (§5.0.1), which needs the race to
actually happen. The cost is the standard one for a lock hierarchy: a strict
order rejects some correct programs. The escapes, in preference order, are to
restructure the code, by declaring the object on the node, reading a
`Versioned` snapshot, or staging through `Deferred`, and then
`ts::Relaxed_scope{ts::Rule::access_rank}` for a claim the library cannot
verify.

### 5.0.3 The deadlock net

The cycle detector above sees the shapes it models. Behind it sits a net that
misses no shape at all, because it does not model anything. If every worker is
idle, every queue is empty, and nothing is registered as completable from
outside the pool, then no thread and no queue can ever settle what you are
waiting for. Progress is impossible, whatever the cause. A blue thread blocked
in `sync()` notices this itself and aborts with a stack trace instead of
hanging.

The catch is the third clause, and it is your responsibility. Plenty of
correct programs wait on something the scheduler cannot complete, such as an
OS I/O completion, a GPU fence, or a `Signal` triggered from a dedicated
engine thread. Declare those:

```cpp
ts::External_wait declared;      // "a thread we do not own will complete this"
io_done.sync();
```

`Frame_gate` does this for you, since a gate handed out and not yet opened is
an outstanding external wakeup. A forgotten declaration is the net's failure
mode. It reports a correct program as deadlocked, which is why the message
names `ts::External_wait` explicitly.

The condition must hold continuously for a window, two seconds by default,
since a real deadlock is permanent and a short window would fire on a slow but
legitimate handoff between two of your own threads. Tune or disable it with
`ts::set_deadlock_net_window(std::chrono::milliseconds{...})`, where 0 means
off; `TS_ENABLED_RULES` drops it from the build. It is compiled out of
shipping builds by default.

The report tells you as much as it cheaply can, then how to get more:

```
deadlock: waiting on task 'frame.cpp:212', but every worker has been idle …
  suspended while holding a grant:
    task 'physics' holds 'bodies', awaits 'nav'
    task 'ai' awaits 'bodies', holds 'nav'
    task 'stuck' awaits 'never_triggered', holding 'held_object'
  (3 suspended task(s) listed above)
```

The middle block comes free from the circular-wait machinery and covers tasks
suspended while holding something. The full list, including tasks suspended on
a plain `co_await` while holding nothing, which the first block cannot see, is
the suspension registry, `TS_SUSPENSION_REGISTRY`. It defaults on only in
debug builds, meaning checked builds with `NDEBUG` undefined, and off
everywhere else; define `TS_SUSPENSION_REGISTRY=1` to turn it on in any
configuration. It costs about 30 ns per suspension and nothing measurable on
real frames. If you are looking at a report from a build without it, the
message says so and names the rebuild flag.

### 5.1 Multi-object access

To touch several guarded objects in one body, use the free function:

```cpp
ts::Guarded<Physics> physics{ ts::Named{"physics"} };
ts::Guarded<Render> render{ ts::Named{"render"} };

ts::access([](const Physics& p, Render& r) { r.mirror(p); }, physics, render);
// options-first form: ts::access({ .priority = ts::Priority::high }, fn, objs...)
```

The free functions mirror the member verbs at every arity. `ts::async` returns
a `Task<R>` and always schedules. `ts::access` returns the caller-owned
`ts::Access_op<Objects..., Body>`, the same operation handle `obj.access(fn)`
returns, so a multi-object access allocates nothing either, and it is consumed
the same way, with `co_await`, `.sync()` from outside a task, or `try_take()`.
Per-argument modes come from const-ness, as always. The library takes the
objects' turns in one canonical global order and holds them for the body. This
is the standard deadlock-free discipline, shared with the static graph, so
dynamic multi-object work and graph nodes can never deadlock each other.

`access` is opportunistic at every arity, in two steps:

- Objects the calling task already holds are lent. No turn is taken on them,
  because the access runs inside the caller's own grant window, which is
  already the exclusion those objects need. This is the same protocol a nested
  `graph.execute()` uses, and at one object it is what "a reentrant access
  runs inline under the held grant" means. If every object is lent, the body
  runs inline without queueing on anything at all.
- The rest are probed all or nothing. If every one of them is free right now,
  with nothing queued on it and the reader/writer rules allowing, the whole
  set is admitted in one pass and the body runs on the calling thread. If any
  one of them is busy, nothing is admitted and the operation enqueues through
  the canonical cascade instead. As with a single object, a queued entry is
  never jumped.

One case is rejected rather than served. If the calling task holds only a read
grant on an object the body writes, that is fatal in checked builds. A read
grant cannot be lent to a writer, and enqueueing would put the access behind
the caller's own hold, which becomes a deadlock the moment it is awaited.
Declare the write on the calling task, or hand it to `ts::async` and do not
wait for it.

Generic lambdas follow the same spelling rule as everywhere else (§3.1).
`const auto&` positions are reads and `auto&` positions are writes, or you can
tag every object with `ts::as_read_only`/`ts::as_read_write` to declare modes
explicitly:

```cpp
ts::access([](const auto& p, auto& r) { r.mirror(p); }, physics, render);

ts::access([](auto& p, auto& r) { r.mirror(p); },   // The same access, spelled with tags.
           ts::as_read_only(physics), ts::as_read_write(render));
```

### 5.2 What `Guarded` is not

It is not a mutex wrapper. You submit a functor rather than locking and
unlocking around raw access. With `async`, or a contended `access`, the body
runs later on a worker and the caller keeps going; with an uncontended
`access` it runs inline right away. `async` hands you a `Task<R>` and
single-object `access` a caller-owned `Access_op` (§5). Inside a task, consume
either with `co_await`; on a blue thread, use `sync()` (§4.3, §11.2).

---

## 6. `Static_task_graph` — build once, run many

For a fixed frame/pipeline structure, declare the whole thing once and run it
every iteration:

```cpp
ts::Guarded<Input>   input{ ts::Named{"input"} };       // Every Guarded and every node takes a
ts::Guarded<Physics> physics{ ts::Named{"physics"} };   // leading name (§6.1). It is what the DOT
ts::Guarded<Anim>    anim{ ts::Named{"anim"} };         // dump, trace, and diagnostics print.
ts::Guarded<Render>  render{ ts::Named{"render"} };

ts::Static_task_graph frame;

frame.add_node("poll",    [](Input& in)                   { in.poll(); },     input);
frame.add_node("physics", [](const Input& in, Physics& p) { p.step(in); },    input, physics);
frame.add_node("anim",    [](const Input& in, Anim& a)    { a.advance(in); }, input, anim);
frame.add_node("render",  [](const Physics& p, const Anim& a, Render& r)
                                                          { r.build(p, a); }, physics, anim, render);

frame.compile();

for (;;)
    frame.execute().sync();   // Runs the DAG in parallel; physics and anim overlap.
```

`compile()` derives edges from access conflicts, where two nodes conflict when
they share an object and at least one writes it, plus any explicit
`after`/`before` you add on the returned `Graph_node` handles. It then
verifies the result is a DAG; a cycle is fatal, with the nodes named.
Conflicting nodes with no explicit order run in declaration order,
deterministically.

`execute()` returns a `Task<void>` completion handle. Runs are sequential, one
at a time, and re-runnable, and a run allocates almost nothing, because node
state is built at compile time and re-armed per run. The graph is literally
build-once. Add nodes and edges, call `compile()` exactly once, then
`execute()` as many times as you like. Adding a node or edge after
`compile()`, or compiling twice, is fatal. For a different structure, build a
new graph; your graph-building function makes that a one-liner, and
pre-compiled variants cover discrete mode switches. The handle is
`[[nodiscard]]` because it is the run's only completion signal, and the
one-run rule means the next `execute()` is legal only after the previous run
was awaited or `sync()`ed. A second call while a run is in flight is fatal in
checked builds. A deliberate fire-and-forget spells its intent with `(void)`.

Node capabilities:

- `node.after(a, b, …)` / `node.before(…)` declare explicit ordering edges to
  other nodes, on top of the ones `compile` derives from access. The variadic
  form reads as intent: `submit.after(cmd, particles, ui)` makes `submit`
  depend on all three, the same as `.after(cmd).after(particles).after(ui)`
  but without reading like a sequence among them.
- `node.set_priority(p)` sets the node's queue priority.
- `node.set_inline()` runs the node on the thread that readied it when its
  objects are immediately available, which gives low-latency chaining for
  small nodes.
- Node bodies may fan out with `ts::parallel_for` (§4.5). The chunks inherit
  the node's grants, and the synchronous join gates the node's completion, so
  dynamic parallelism over the node's data passes the harness.
- A node body may be a coroutine. Return `ts::Task<void>` from the body and it
  may `co_await` mid-node. The node completes, releasing its grants and
  successors, when the frame completes, not at the first suspension, and its
  declared grants are held for the frame's whole life. While the frame is
  suspended its worker is free.
- `execute({ .token = t })` accepts a cancellation token. Nodes that have not
  started are skipped and the run's completion settles cancelled. A node body
  may opt into a mid-body early-out by declaring a trailing
  `ts::Cancellation_token` after its resource parameters, the same spelling as
  `launch`/`async` bodies (§4.4), and receives the current run's token. A
  cooperative return settles the node completed, not cancelled.

Objects are held per node, not per run. A node acquires exactly the objects it
declared, for exactly its duration, so an object is free between its
accessors. Concurrent `async` work on the same objects interleaves safely into
the gaps, queueing behind the node that currently holds the object rather than
racing it.

### 6.1 Names and the structure dump

Every node and every guarded object carries a `ts::Named`, a required leading
argument, because the name is what the DOT dump, the trace, and every
diagnostic print. It is either a string literal, referenced rather than
copied, or the call site that created the entity, spelled `ts::Named{}`:

```cpp
auto sim = frame.add_node("physics", [](const Input& in, Physics& p) { p.step(in); },
                          input, physics);
frame.add_node({}, [](Anim& a) { a.advance(); }, anim);   // Identified as file:line.

ts::Guarded<Physics> physics{ ts::Named{"physics"}, world_size };
ts::Versioned<Poses> poses{ ts::Named{"poses"} };
ts::Guarded<Anim> anim{ ts::Named{} };                    // Identified as file:line.

frame.compile("frame.dot");
```

`ts::Named` is a distinct wrapper rather than a bare leading `const char*` so
it can never be mistaken for `T`'s own first constructor argument.

Tasks carry one too, but there it is optional. `ts::launch` and the access
verbs capture their own call site by default, so an unnamed task is still
identified in a diagnostic. Pass a literal when the site is not the useful
name:

```cpp
ts::launch(stream_textures, { .name = "stream_textures" });
```

The one exception is the multi-object `ts::access` / `ts::async`. They end in
an object pack, so no defaulted `source_location` can follow and the verb has
no site to capture. `{.name = ...}` is their whole identity, either a literal
or `{.name = ts::Named{} }` to capture the call site by hand.

Render the dump with Graphviz (`dot -Tsvg frame.dot -o frame.svg`, or the
repo's `show_graph.bat`) or paste it into an online viewer such as edotor.net.
The output is dark-themed. Edges are green, with the line style carrying their
origin: solid edges are your explicit `after`/`before` orderings, and dashed
edges were derived from declared access. Hovering a dashed edge in the SVG
shows which object and modes produced it (`physics: W->R`). The picture
answers the question of why an edge exists, which is exactly what you need
when reshaping a graph. A `W->R` edge is real dataflow, while a `R->W` or
`W->W` edge is an ordering artifact you may be able to remove by double
buffering (`Versioned`) or deferral (`Deferred`).

The dump, along with all profiling instrumentation, compiles out with
`TS_PROFILING=0`; the `compile(DOT_path)` parameter remains and becomes a
no-op. Node names are kept in all builds. They are three words per node and
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

The SVG shows an average run. Bars are drawn at the median start with the
median duration and packed into anonymous concurrency rows. Workers are
interchangeable and the node-to-worker assignment reshuffles every run, so
rows carry no worker identity. Time-overlapping bars simply occupy different
rows, and row occupancy over time reads directly as the average frame's
concurrency; free vertical space at some time means capacity was genuinely
unused there. Dependency edges connect the bars in the structure dump's
styling, solid for explicit and dashed for derived. Edges are uniformly faint
by default so the picture reads as bars first. Critical edges keep their pink
colour but no extra prominence, since critical structure at rest is carried by
the orange node borders and labels. Hovering a node brightens its incident
edges to full so you can trace its exact dependencies.

Hovering a bar raises a formatted tooltip. It shows the node name coloured by
its queue priority (red = high, green = normal, grey = low, the same colouring
as the name on the bar), with its priority tag right-aligned on that line.
Then come the stats: mean / P95 / σ / CV / min/max execution time; the node's
true busy time, meaning the body plus `parallel_for` slices plus async
fan-out, which for a parallel node exceeds the bar, because the bar shows wall
time while true busy is core time summed across the fan-out; the declared
accesses with each mode colour-coded (RO green read-only, RW red read-write,
e.g. `transforms: RO`); and the node's incoming (`->|`) and outgoing (`|->`)
edges, each neighbour name coloured by that edge's share of binding chains.
The tooltips are scripted into the SVG itself, so they work with the file open
in a browser but not when the file is embedded via `<img>`.

A coloured headline carries three frame classifiers.

- Core utilization is the share of the run window the scheduler's workers
  spent executing tasks (green ≥ 75%, red < 50%; work run inline on
  non-worker threads is not counted). It says how much of the machine the
  frame used.
- Critical path dead time (green < 5% of frame time, red > 10%) says whether
  the critical chain itself had to wait.
- Task-system overhead is the share of the frame's compute, meaning the body
  plus framework overhead and excluding idle, that was spent in the
  scheduler's own framework overhead rather than in your functors (green ≤ 5%,
  red > 15%). It says whether your tasks are coarse enough that scheduling
  them is cheap relative to running them; a high figure means the graph is too
  fine-grained, with tasks that cost less than the framework overhead of
  dispatching them.

Framework overhead is derived by pure subtraction, `busy − body`. Every
on-worker moment that is not user-functor time, including task setup and
completion and successful work-finding scans, is framework overhead, with no
separate accumulator to keep in step. The per-run graph setup, meaning link
binding, node re-arm, indegree init, and root dispatch, runs on the calling
thread outside any task and scales with node count. It forms a distinct
fourth bucket, orchestration, shown alongside, so a large, cheap-bodied
graph's setup cost is visible without inflating the framework-overhead
figure. The headline also prints a serial floor: the same frame traced once
worker-less (single-threaded), where the whole frame runs serially with no
idle to confound it, so `(total − body) / total` is the complete framework
cost by pure subtraction. The gap between the multi-worker overhead and the
serial floor is the framework overhead only workers pay, that is cross-thread
dispatch, object hand-off, and park/wake. It is the cost of the parallelism,
not a measurement error. Overhead is an upper bound; it is measured with
tracing on, which adds a per-task clock bracket, so cross-check against the
untraced task throughput if it matters.

Each significant chain wait is also drawn in place, as a hatched pink band
spanning the picture's full height and occupying the visible gap between the
binding predecessor and the waiting critical node. The gaps are screen-space
and unioned, so a band is always a real break between bars, and the wait
belongs to the chain, not to any row. Behind the bars, a faint full-height
core-utilization wash colours each time slice by how busy the machine was
there, from green with all cores busy through yellow at half to red when
idle. It is sampled from per-worker busy time bucketed over the run, so every
task counts at its real time, and a `parallel_for` node's fan-out across
cores registers fully. Green stretches are saturated cores; red gaps are idle
capacity.

A panel below shows the global numbers: the run count; frame time
mean/min/max (ms); the critical path, which is the CPM dependency lower bound
on frame time, computed from median durations with no scheduling waits; the
worker count; tasks, both the total across the trace and the mean per run,
counting every task the scheduler ran (nodes plus `parallel_for` slices,
async jobs, and continuations), so it far exceeds the node count and shows
the real fan-out volume; and the body / framework overhead split in µs per
run that backs the overhead headline, summed across workers. An edge's
tooltip names the ordering it enforces: for a derived edge, the conflicting
resource and the two nodes' modes (`physics RW -> propagation RO`); for an
explicit one, "explicit ordering", plus any coinciding conflict.

The critical path is picked out by colour. Each run, the trace walks back
from the last-finishing node through the predecessor whose completion
released each node, giving the chain that actually bound that run's makespan.
Across runs this yields a per-node share, since different chains bind
different runs. A node's border and label blend from cyan to orange with that
share: a solid orange bar was critical in essentially every run, and a plain
cyan one never was. An edge's colour blends from green to pink the same way,
keeping its solid or dashed origin marking. The tooltip adds three numbers:
"critical in N% of runs" (measured), "slack" (structural: how far the node
can slip in the average frame before the dependency chain lengthens, omitted
when close to zero), and "dispatch wait" (the mean ready-to-start latency,
queue plus object acquisition). The measured and structural views disagree
exactly where scheduling rather than dependencies binds the frame, which is
what you tune next.

No samples are stored. Statistics stream into fixed-size state (mean/variance,
P50/P95 quantile markers, min/max, a per-worker histogram), so a million runs
cost the same memory as ten. Two consequences of drawing medians are handled
for you. An edge's bars can overlap in the aggregate even though no real run
overlapped them, because medians are not additive; such bars are clamped to
the edge's average hand-off point. And any two bars that overlap in time land
on different rows by construction of the packing.

Tracing costs two clock reads per node per run when attached, one branch when
not, and nothing at all with `TS_PROFILING=0`. Cancelled runs are not folded.

The sample wires this up as `macrame_playground --trace [frames]`. It traces
two variants of the same ~30-system frame on an 8-worker scheduler, a
`baseline` and an `optimised` version tuned by reading the baseline's own
trace, and writes `sample_game_frame_avg_baseline.svg` and
`sample_game_frame_avg_optimised.svg` plus `sample_game_frame.dot`. A
no-argument `show_graph.bat` renders the structure dump and opens both.
[example-frame-optimization.md](example-frame-optimization.md) walks the two
side by side, showing which optimisations the trace says are worth trying and
which it says not to bother with.

### 6.3 Running a graph inside a graph

A node body may run another compiled graph and await it. This is how you build
a large frame out of reusable sub-graphs: compile the sub-graph once, execute
it from whichever node needs it.

```cpp
ts::Static_task_graph inner;                     // Compiled once, elsewhere.
inner.add_node("solve", [](Physics& p) { p.solve(); }, physics);
inner.compile();

outer.add_node("step", [&inner](Physics& p) -> ts::Task<void>
{
    p.begin_step();
    co_await inner.execute();                    // Runs under this node's grant.
    p.end_step();
}, physics);
```

Note that both graphs declare `physics`, and the outer node is holding it when
the inner run starts. Two things make that work, and both are automatic:

- Lending. Objects the calling task already holds a covering grant on (a
  write grant covers reads and writes, a read grant covers reads) are lent to
  the inner run, whose nodes skip taking their own turn on them. They do not
  need one. The caller's grant is what already excludes everyone else, and it
  stays held for the whole nested run. Without this the inner node would
  queue behind the grant its own caller is holding while that caller waits
  for it, which is a deadlock. Ordering within the inner graph is unaffected,
  since its compiled conflict edges still sequence its own nodes on a lent
  object. Code outside sees nothing different either, because the object's
  queue never learns about the lend; an unrelated `async` still queues behind
  the outer node's hold. Nesting composes to any depth.
- Scope join. The inner run joins the calling task's scope, so you may fire
  it without awaiting and still be sure it finished before the caller
  completes. Pass `{.detach = true}` for a run that should genuinely outlive
  its launcher. A detached run also forgoes lending, since it is no longer
  contained in the caller's grant window, so it simply queues like any
  external work.

Three mistakes are caught with a fatal in checked builds:

- The calling node declares read on an object the inner graph writes. A read
  grant cannot be lent to a writer. Declare the write on the calling node, or
  move the writing node out of the sub-graph.
- Lending while an earlier un-awaited nested run of the calling task is still
  in flight. That run holds the same grant, so it could touch the lent object
  concurrently with the inner graph, each of them "validly". `co_await` the
  previous nested run first.
- Calling `execute()` on a graph whose previous run is still in flight. Runs
  happen one at a time; give each concurrent caller its own instance, or
  order the callers with an edge. This also catches the plain single-threaded
  mistake of starting a second run without awaiting the first.

### 6.4 Do I need the static graph?

Nothing forces you to build one. The access verbs plus `co_await` compose the
same work. Launch each system with `ts::async(fn, objs…)`, hold the returned
`Task<void>` handles, and `co_await` a system's producers before launching it.
The sample carries both spellings of one ~30-system frame, `build_frame_graph`
and `run_frame_graph_free` in `sample/game_frame.cpp`, over the same `World`
and the same system bodies, so the difference is only in how the schedule is
produced. The comparison shows three things.

Safety belongs to the object, not the graph. A hand-composed system still
takes a mode-aware turn on every object it declares, so two conflicting
systems never overlap and the harness still fatals on an undeclared touch.
Dropping the graph costs you nothing here.

Order belongs to the graph. `compile()` derives every conflict edge from the
declarations the nodes already carry. Written by hand, each of those edges is
an explicit `co_await` somewhere in the chain coroutines the frame is cut
into, plus the joins that fold them back together. Do not expect the objects'
FIFO order to stand in for the conflict edges. A multi-object access enters
its links one at a time in canonical order, so a system blocked on its first
object has not yet taken its slot on the later ones, and a system launched
after it walks straight past. Launching the sample's node list in declaration
order with no explicit awaits runs `frustum_cull` before `camera` and lets
`submit` clear the draw queue before `cmd_record` reaches it. That loses a
frame of draw commands, with every declaration correct, the harness silent,
and the frame 7.6% faster for it. The harness is an oracle for undeclared
access, not for mis-ordered declared access; a missing edge is a silent race.

The cost is a scheduler round trip per edge, not the allocations. Measured on
that frame (22 hardware threads; `--bench` reports both):

| | µs/frame | allocs/frame |
| --- | --- | --- |
| graph, heavy systems (~4.1 ms frame) | 4102–4110 | 38 |
| graph-free, same frame | +1.4–1.6% (+56–64 µs) | +95 |
| graph, light systems (~0.46 ms frame) | 457–470 | 38 |
| graph-free, same frame | +19–29% (+89–131 µs) | +95 |

The graph-free composition costs ~95 extra allocations per frame, but at
~17 ns each that is under 2 µs, a rounding error against the 56–131 µs it
actually costs. The real difference is the resume. The graph dispatches a
successor directly on the thread that settled the last predecessor, while an
awaited handle suspends a coroutine and resumes it, and ~50 of those round
trips at ~1.8 µs each (`coro chn` in `--bench`) is the whole gap.

On a frame whose systems take milliseconds, hand composition is within noise
and the graph is a maintainability tool. The schedule is derived from
declarations that already exist, it is checked (cycles are fatal), and it is
visible (`--dot`, the trace SVG). On fine-grained work the round trips start
to matter. Reach for the graph when the structure is fixed and you want it
derived and inspectable. Compose by hand when the structure is dynamic,
data-dependent, or small enough that writing the edges out is honest
documentation.

### 6.5 Declaration order is not a specification

`compile()` derives an edge for every access conflict and directs it by the
order the two nodes were added. That direction is real, and the schedule
honours it, but it is not something to build on.

A derived edge exists for safety, meaning two nodes that conflict must not
overlap. Either direction satisfies that, so which one you get is an artifact
of how the building code happens to be written, not a statement about your
frame. The rule:

> If node A must logically precede node B, that is intent; say it with an
> explicit `after`/`before` edge. Rely on the determinism of a compiled
> graph, never on the order nodes were declared in.

```cpp
auto record = graph.add_node("cmd_record", …, draw_lists);
auto submit = graph.add_node("submit",     …, draw_lists);
submit.after(record);   // Intent: submit consumes what record emits.
```

The explicit edge costs nothing when it agrees with the derived one;
`compile()` dedups them into a single edge, drawn solid with the conflict
still in its tooltip. What it buys is that the ordering survives changes to
the code that builds the graph, and survives changes to the declarations too.
In the optimised variant of `sample/game_frame.cpp` the producers stage
through `Deferred` and no longer touch the draw queue at all, so the conflict,
and with it the derived edge, disappears entirely. The intent edge is what
keeps the frame correct across that refactor.

This also reserves room. Because direction carries no meaning, a future
`compile()` is free to reorder independent conflicting nodes to shorten the
critical path. Programs that wrote their intent down keep working; programs that
leaned on declaration order would silently change behaviour.

---

## 7. `parallel_for`

Data-parallel loops over an index range:

```cpp
ts::parallel_for(n, [&](int i) { out[i] = f(in[i]); });

ts::Task<void> done = ts::parallel_for_async(n, body);   // The non-blocking variant.
```

The options struct is
`ts::Parallel_options{ .max_workers, .balance, .priority, .token }`:

- `max_workers` is the upper bound on the executors that may participate, the
  calling thread included; 0 means the scheduler's worker count. Helpers are
  fanned out up to it, and one that never gets a worker finds nothing left to
  claim.
- `balance`:
  - `guided` (the default): the chunk size shrinks as work drains, giving low
    overhead early and fine-grained load balancing at the tail.
  - `balanced`: fixed `n/max_workers` chunks, the lowest overhead, assuming
    uniform item cost.
  - `unbalanced`: every item is claimed individually, giving maximum
    balancing at maximum overhead.
- `priority` is the queue priority for the helper tasks. Unset, the default,
  inherits the calling task's priority, which is the same rule every option
  struct follows (§10.1). A `parallel_for` inside a high-priority task or
  graph node dispatches its helpers at `high`, and outside a running task the
  priority is `normal`. Set it (`{.priority = ts::Priority::low}`) to
  override. The calling thread's own share is unaffected either way, because
  it runs inline, not through the queue.
- `token` provides cancellation for `parallel_for_async` only. Once
  requested, chunks not yet claimed are skipped and the returned task settles
  cancelled (`is_cancelled()`); chunks already running finish. The blocking
  forms ignore it, since their caller participates and joins, so stopping
  early is the body's own early-out.

The calling thread participates in the loop rather than just waiting, which
is what makes nested `parallel_for`, a parallel loop inside a parallel loop's
body, deadlock-free even when every worker is occupied. Chunks inherit the
caller's access grants, so a `parallel_for` inside a graph node may touch the
node's declared objects.

Cross-item mutation, where item *i* writes item *j*, is not synchronized by
`parallel_for` itself; see the WIP note in §13 and the staging tools in §9,
which cover the common cases today. For the case staging cannot serve,
iterative solvers whose cross-item effects must land within the pass (physics
constraints, relaxation), use interaction coloring with
`ts::parallel_for_colored(bands, rounds, body)`. Partition items into bands
whose members touch disjoint state, coloring the interaction graph once at
setup, and the driver runs `rounds` passes with bands sequential and each
band's items in parallel. Helpers fan out once for the whole run, because
band transitions are atomic phase advances rather than per-band fork/join.
The caller participates and can drain every band alone, which is the same
deadlock freedom as `parallel_for`. If the bands uphold the coloring
invariant, the result is bit-deterministic under any chunking, stealing, or
worker count. A worked example, including the greedy coloring, is
`sample/coloring.cpp`.

---

## 8. Coroutines in depth

Composition is coroutines (§4.2); this chapter is the deeper contract. The
support is part of the core: `ts.h` includes it, and coroutines are required.

### 8.1 Awaiting tasks

```cpp
ts::Task<int> pipeline()
{
    int a = co_await ts::launch(step_one);          // Suspend until done.
    int b = co_await ts::launch([a] { return step_two(a); });
    co_return a + b;
}

int r = pipeline().sync();   // A coroutine returning Task<R> is itself a task.
```

`co_await task` suspends the coroutine until the task settles and resumes
with the result. On an lvalue handle that is a `const R&` with the same
contract as `sync()`, non-consuming; on an rvalue, as both launches above,
the result is moved out and yielded by value, mirroring `take()` - the
temporary handle dies with the statement, so a reference into it would
dangle. A coroutine
task starts eagerly, meaning the body runs to its first genuine suspension on
the calling thread, and while suspended it holds no worker. Deep chains
resume iteratively with a bounded stack, and each resumed segment carries the
coroutine's access grants and task identity, whatever thread it resumes on.

Cancellation stays value-based. A cancelled awaited `Task<void>` just
resumes; check `is_cancelled()` or poll a token between awaits. There is no
exception to catch (§10.5).

### 8.2 Awaitable access guards

An object's queue doubles as an asynchronous reader/writer lock:

```cpp
ts::Task<void> update(ts::Guarded<World>& world)
{
    {
        auto w = co_await ts::read_write(world);   // Suspends until exclusive access is granted.
        w->step();                            // Direct World& access, checked by the harness.
        w->settle();
    }                                         // The guard is released at scope exit.

    auto r = co_await ts::read_only(world);        // A shared read guard.
    log(r->population());
}
```

Unlike a callback `access`/`async`, the guard gives you a scope with real
control flow over the object. One hard rule applies: never `co_await` anything
else while holding a guard. That would keep the object locked across a
suspension of unknown duration, and the guard's own access context cannot
survive a resume on a different thread. The library enforces the rule at the
`co_await`, whether or not that particular await would have suspended. An
await that happens to complete synchronously is just as illegal, and gating on
whether it actually suspended would let the mistake ship on every run where
timing was friendly.

The sanctioned forms are the functor verb, `co_await obj.access(fn)`, where
the grant lives only for `fn`, and splitting the scope: release the guard,
await, re-acquire. For two objects at once, take them together, either with
`co_await ts::read_write(a, b)` for a held scope guard over both (giving
`a`/`b` by reference via structured bindings) or with the callback
`co_await ts::access(fn, a, b)`. Each acquires in one canonically ordered step
rather than nesting guards.

There is exactly one exemption. An access whose objects this task already
holds is lent every one of them, so it takes no turn at all, runs inline under
those grants (waiting rule (b)), and is settled before the `co_await` is
evaluated. It cannot suspend by construction. An access that still has to
acquire something, even one object of several, is not exempt.

```cpp
auto g = co_await ts::read_write(world);
int n = co_await world.access([](const World& w) { return w.size(); });   // Lent, so this is fine.
```

This rule has no runtime opt-out, because it protects an invariant the
implementation relies on, not a hazard you might know is absent (§8.3). A
build can drop the check entirely with `TS_ENABLED_RULES`.

A worked example is `sample/scope_access.cpp`, a small bank showing all three
shapes: single-object read-modify-write under a held grant, interest across
several accounts one by one, and a two-account transfer taken together.

### 8.3 Rule policy: turning a check off

The waiting rules are enforced by runtime checks that abort. Sometimes you
uphold a rule by means the library cannot see, such as an external lock
discipline, a phase invariant, or a platform guarantee, and the check is a
false positive. There are two levers, from narrow to blunt:

```cpp
{
    ts::Relaxed_scope relax{ ts::Rule::in_task_sync };   // "I know this wait is bounded"
    // ... Only this scope is affected; the rest of the program stays checked.
}

ts::set_default_relaxed_rules(ts::Rule::in_task_sync);   // Process-wide, for treating the rules as advice.
```

A relaxation follows the ambient task state rather than the thread. It
survives a coroutine's suspensions, so a `Relaxed_scope` opened in a coroutine
body is still in effect when the body resumes on another worker. It is
therefore a little wider than the lexical scope suggests, deliberately so,
since a resumed segment inherits the grant and with it the hazard. A detached
`ts::launch` inherits neither the grant nor the relaxation; it is a fresh
context, so an opt-out does not follow it. `parallel_for` helpers inherit the
grant but not the relaxation, so a helper that needs an opt-out states it
itself.

Not every rule can be relaxed. `Rule::await_under_guard` (§8.2) protects an
invariant the implementation relies on, not just a hazard you might know is
absent, so it has no runtime opt-out. Its escape is the sanctioned form: use
the functor verb `co_await obj.access(fn)`, or split the scope (release,
await, re-acquire). The global deadlock net has no call site to scope at all.

To drop a check from a build entirely, including its state, define
`TS_ENABLED_RULES` to an OR of `TS_RULE_*` bits, one value per binary, like
`TS_SAFETY_CHECKS`. The defaults are everything in checked builds and
`TS_RULE_AWAIT_UNDER_GUARD` in shipping builds. The full table and rationale
are in [waiting-rule-policy.md](waiting-rule-policy.md).

### 8.4 Thread-local state

A task does not own a thread. A coroutine resumes on whichever worker settled
the task it awaited, so a frame runs its segments on whatever workers happen to
be free, and a `thread_local` written before a `co_await` and read after it
belongs to two different threads:

```cpp
thread_local Draw_scratch scratch;                  // This is not per-task state.

ts::Task<void> record(ts::Guarded<Scene>& scene)
{
    scratch.clear();
    co_await scene.async([](const Scene& s) { prepare(s); });
    scratch.emit();                                 // This is another worker's scratch.
}
```

No check catches this and no compiler is at fault. Both accesses are legal;
they simply name different objects. It is the natural mistake to arrive with
from a thread-pool library, where a callback runs to completion on the thread
that picked it up.

A coroutine's own locals are the task-local storage. They live in the frame,
travel with the task across every suspension, and are correct by
construction:

```cpp
ts::Task<void> record(ts::Guarded<Scene>& scene)
{
    Draw_scratch scratch;                           // Lives in the frame and follows the task.
    co_await scene.async([](const Scene& s) { prepare(s); });
    scratch.emit();
}
```

That replaces most of what `thread_local` gets used for. What stays
legitimate is a genuinely per-thread resource, such as a per-worker scratch
arena, an allocator pool, or a random-number stream, where thread ownership
is the point. The rule for those is to never carry a value or a reference
derived from a thread-local across a suspension. Re-read it on the other
side.

```cpp
ts::Task<void> upload(ts::Guarded<Scene>& scene)
{
    Staging_buffer& buffer = worker_staging();      // This worker's buffer.
    fill(buffer);
    flush(buffer);

    co_await scene.async([](const Scene& s) { prepare(s); });

    Staging_buffer& resumed = worker_staging();     // Re-read it: buffer belongs to
    fill(resumed);                                  // the thread we suspended on.
}
```

The same applies to anything that identifies a thread rather than being
stored in one. `ts::current_worker_index()` is a reading, not a property of
the task. After a suspension it describes the worker you resumed on, and a
value saved before the suspension describes the one you left.

### 8.4.1 When the compiler caches the address

Discipline is not always enough. A compiler may resolve a thread-local's
block address once and reuse it, notably by hoisting it out of a loop that
contains a suspension, where the address is loop-invariant. Code that
carefully re-reads the variable after a resume then still reads the
suspending thread's slot. This defeats correct code specifically, because the
cached address is one the programmer never wrote.

Two of the bugs it produced in this library were misreads of safety state,
and both aborted programs that were doing nothing wrong. One was a false
"accessed without declared access" from the harness, in any coroutine
acquiring a guard inside a loop. The other was a false `await_under_guard`
fatal, the one rule a shipping build keeps, where a misread counter fails
both ways: a false positive kills a correct program, and a false negative
lets a genuinely unsafe suspension through unreported.

This is a known cross-compiler class with an incomplete upstream fix. LLVM
#47179 (2020), "[coroutines] Compiler incorrectly caches thread_local address
across suspend-points", was fixed and refiled as #63022 (2023) because the fix
did not cover every scenario; D92661 ("[RFC] Fix TLS and Coroutine") is the
standing proposal. MSVC has its own Developer Community report ("Incorrect
optimization: thread-local variables cached across coroutine ..."). LLVM
#72006 is the general form, a coroutine frame reused as scratch across a
suspension, and a sibling clang issue caches `pthread_self()`, so anything
that identifies a thread is exposed, not only `thread_local`.

Choosing a particular compiler is not a mitigation. clang recomputes the
address in the scenarios this library's regression tests exercise, but that
is one shape measured on one version, not immunity. The codegen that triggers
the hoist is not stable across compiler versions or build settings either.
One of this library's cases reproduced only in an exceptions-enabled build,
where the codegen differs enough to spill the address; the same source
compiled with exceptions off did not.

What does work is putting a barrier underneath the variable. Reach it through
out-of-line accessors, at both ends, so the block address is resolved where
the access happens and can never be kept in a caller's frame.

```cpp
#if defined(_MSC_VER) && !defined(__clang__)
#define APP_NO_INLINE __declspec(noinline)
#else
#define APP_NO_INLINE [[gnu::noinline]]
#endif

namespace app::detail
{
    inline thread_local int trace_depth = 0;        // Named only in this namespace.

    APP_NO_INLINE inline int load_trace_depth() { return trace_depth; }
    APP_NO_INLINE inline void store_trace_depth(int depth) { trace_depth = depth; }
}
```

Two constraints make the barrier hold. The accessors must pass by value,
since an `int&` accessor hands the caching hazard straight back to the caller
one level up. State that cannot be passed by value, such as a container or a
queue, exposes whole operations out of line instead, so the address never
leaves the accessor's frame at all. Making the variable private to the
accessors, rather than merely agreeing to go through them, is what stops the
next person who touches it from bypassing the rule without noticing. This is
the shape the library uses for its own thread-locals
(`include/ts/detail/thread_local.h`), and the reason
`ts::current_worker_index()` is a function rather than a variable.

Consider a canary test for your build. Whether you are exposed is decided by
your toolchain, optimization settings, and code shape, not by ours, so this
belongs in your suite rather than only in ours. The shape that caught both of
the bugs above:

1. a coroutine that acquires an access guard inside a loop whose body
   contains a suspension, since the loop invites the hoist and the guard
   keeps the address live across the suspension point;
2. enough concurrent copies of that coroutine that resumes genuinely cross
   workers, because an uncontended run tends to resume on the thread it left
   and proves nothing;
3. inside the loop, read the thread-local twice, once directly (inlinable)
   and once through a `noinline` accessor, and assert the two readings agree.

Disagreement is the signature: one reading came from a cached block address,
and the other resolved a fresh one. The worked example is the test
`"co thread-local state survives resumption on another worker"` in
`tests/coroutine_tests.cpp`; copy its structure and substitute your own
thread-local. Failures are intermittent, and ours reproduced between one run
in four and one run in twenty-five, so run the canary in a loop rather than
once.

---

## 9. `Deferred<T>` and `Versioned<T>` — staged writes

An object's queue serializes readers against every writer. When a target has
many producers of small writes and many readers, or when readers must see
updates in atomic batches, staging beats direct writes. Two types share the
machinery, a journal of staged commands.

### 9.1 `Deferred<T>`: batch your writes

```cpp
ts::Guarded<Score_board> board{ ts::Named{"board"} };
ts::Deferred<Score_board> staged{ board };

// Each producer mints one recorder, its identity in the apply order, and reuses it.
ts::Recorder<Score_board> rec = staged.recorder();

rec.stage([](Score_board& b) { b.add("alice", 10); });   // No access is taken; this never blocks.
rec.stage([](Score_board& b) { b.add("bob", 5); });

// Later, at a point you choose:
ts::Task<void> applied = staged.commit();                // One write applies everything.
```

`commit()` auto-dispatches on grant ownership. Called from the task that
holds the target's write grant, such as a graph node that declared the write
or an `async`/`access` write body, it applies inline under that grant, with
no second access acquisition, and returns an already settled task. Called
from anywhere else, it enqueues one ordinary async write on the target and
returns that write's completion.

The contracts, briefly; the full statements live in
[deferred-versioned-state.md](deferred-versioned-state.md):

- Staging is grant-free. It touches only the journal, is safe from any
  thread, and never contends with the target's readers.
- Snapshot semantics. Readers see none of a batch before the commit and all
  of it after.
- The cut happens when the commit runs. Commands staged after that ride the
  next commit.
- Ordering is FIFO within a recorder, and you can rely on that. Across
  recorders the order is fixed and reproducible but arbitrary, so never
  encode meaning in it. If two producers write the same key, that is a design
  conflict; give the key one producer or make the commands commutative.
- Lost writes are caught. Destroying a `Deferred` with staged, uncommitted
  commands is fatal under `TS_SAFETY_CHECKS`; `discard()` is the explicit
  escape.
- Sync before destroying. Destroying a `Deferred` while an enqueued
  `commit()` is still in flight is fatal under `TS_SAFETY_CHECKS`; sync the
  task it returned first. The pending job uses the `Deferred`, and a
  destructor that silently blocked on it would hide a bug. With the last
  commit settled the destructor is non-blocking, and inline commits finish
  within the call.
- The inline path's task carries no ordering. When `commit()` applies inline,
  because you held the grant, the returned task settled before the apply. It
  answers `is_done()` truthfully but provides no happens-before edge.
  Observers of the data order through the object's queue, which does order.
- Commit from the grant holder, not from grant-inheriting sub-work. Inside a
  node or body that holds the write grant, call `commit()` there. Calling it
  from sub-work running under the inherited grant, such as a `parallel_for`
  helper or a coroutine frame launched from the node, is a misuse, because
  the sub-work is not the holder and the enqueued write would queue behind
  the very grant it waits out. It is fatal under `TS_SAFETY_CHECKS`.

For one logical producer parallelized internally, staging from inside a
`parallel_for`, mint a `Parallel_recorder` instead. It uses per-worker
storage, with no contention under any chunking. The trade is that
cross-thread apply order becomes nondeterministic, so use it for commands
that are per-key-unique or commutative.

### 9.2 `Versioned<T>`: stable versions for readers

`Versioned<T>` keeps two replicas of `T` behind one guarded front. Readers
always see the last published version, producers stage into the next one, and
`publish()` flips atomically:

```cpp
ts::Versioned<Poses> poses{ ts::Named{"poses"} };  // T must be default-constructible and swappable.
ts::Recorder<Poses> rec = poses.recorder();

// Producers, all frame long, grant-free:
rec.stage([id, xf](Poses& p) { p.set(id, xf); });

// Readers, all frame long, see the last published version. read returns an Access_op:
poses.read([](const Poses& p) { draw(p); }).sync();

// Once per frame:
poses.publish().sync();                           // Completes at the version flip.
```

Key properties:

- There is no read-your-writes. Staged outputs arrive as the next version.
  Readers wanting the new version order themselves after the publish, either
  by `co_await`ing the task `publish()` returns or through a graph edge from
  the publish node.
- The flip holds the write access for nanoseconds, since it is a content
  swap. Applying the batch and re-syncing the second replica overlap readers.
  The front's address never changes, so graph declarations and the harness
  see one ordinary object.
- In a static graph, publish is a node:
  `g.add_node("publish", ts::publish_fn(poses), poses.state())`. Declare read
  access on `poses.state()` everywhere else; the publish node is the one
  writer.
- The resync policy is a constructor argument: `replay` (the default, which
  re-applies the batch to the second replica; commands must be deterministic,
  so for example capture random rolls at stage time), `copy`, or `overwrite`
  (you promise each version rewrites everything). `set_divergence_check(hash)`
  verifies replay bitwise and aborts on nondeterministic commands; turn it on
  in development builds.
- One publisher at a time, enforced. A dynamic `publish()` and a graph run
  containing a publish node must not race. A `publish().sync()` before the
  run is the sanctioned pattern, and it is checked rather than just
  documented.

### 9.3 Choosing between them

| your state | use |
|---|---|
| Per-frame delta is *data* (poses, events, facts) | `Versioned<T>` |
| Producing the next state is heavy *computation* over the current one (a physics world) | one `Guarded<T>` machine + `Deferred` inputs + a `Versioned` *extract* of its outputs |
| The delta is most of the state and rebuilding is cheap | plain snapshot swap (`shared_ptr<const T>`); no journal needed |
| Many small writers into live state, readers can see partial progress | plain `Guarded` `access` writes |

The physics decomposition, a sealed simulation machine with staged inputs and
a versioned pose extract, is implemented end to end in `sample/physics.cpp`.
A blackboard recipe, a shared key-value fact board with change notification,
is in `sample/blackboard.cpp`. Both are deterministic and verify themselves.

### 9.4 `Event_bus` — pub/sub on staged writes

`ts::Event_bus` (`ts/event_bus.h`) packages the staged-write machinery into
pub/sub. The event struct is the topic, firing is one grant-free line from
anywhere, and delivery is a batch at a per-domain dispatch point.

```cpp
struct Damage { int npc; int amount; };          // The type is the channel.

ts::Event_bus bus{ ts::Named{"gameplay"} };
bus.publish(Damage{ npc, 12 });                  // Fire from any thread or task.

auto c = bus.subscribe(hud, &Hud::on_damage);    // Pinned: hud is kept alive per call.
auto c2 = bus.subscribe([&](const Damage& d) { audio.async(...); });  // Unpinned.

// The delivery point, once per domain: a graph node...
g.add_node("events", bus.dispatch_fn(), bus.state()).after(producers...);
// ...or one write access per frame, graph-free:
bus.state().access(bus.dispatch_fn()).sync();
```

The event type is deduced from the handler's parameter. Pinned subscriptions
(`subscribe(owner, ...)` with a `shared_ptr` owner) weak-lock the owner
around each invocation. A dying owner is skipped and reaped, so owner
destruction needs no unsubscribe call, and the handler receives the locked
owner as `self`. Delivery is deferred by design, with handlers seeing events
at the dispatch node; something that must happen immediately is a command
(`target.async`) or an intra-system observer, not a bus event. Subscribing is
staged like a publish, so subscribe-then-publish from one thread is always
delivered. `Connection`'s explicit removal runs under the board's grant, so
call it from outside tasks. Handlers run under the bus's grant and must not
touch other guarded state directly; route through `async`/`publish`/`stage`,
and checked builds fault a handler that strays. Heavy flows whose consumers
deserve their own nodes belong to a dedicated batch object and a derived edge
(§6), not the bus. A full demonstration, including minimal setup floors, is
`sample/events.cpp`.

---

## 10. Utilities

### 10.1 Priorities

`ts::Priority { high, normal, low }` is accepted by every route: `launch`
options, `async`/`access` options, `parallel_for` options, and
`Graph_node::set_priority(p)`. On the option structs the field is
`std::optional<Priority>`. Unset inherits the calling task's priority, and
outside a task it is `normal`. An `async` issued from a `high` node therefore
runs `high` unless you say otherwise, and `{.priority = ts::Priority::low}`
always wins. `high` is strict, always served first; `low` still makes
progress under sustained load, because an aging valve prevents starvation.

A coroutine has no priority option of its own, because its frame is never
queued; it starts on the caller's thread and resumes on the thread that
settled what it awaited. It inherits the priority of the task that created
it, which matters only for what the body launches. A `parallel_for` inside a
coroutine called from a `high` node dispatches its helpers at `high`.

### 10.2 Scheduler configuration

There is one process-wide scheduler, brought up explicitly with
`create_scheduler` and configured there. You never construct or hold an
instance. It is heavy, so it never starts lazily. Create it once at startup
and it services every task until `destroy_scheduler` or program exit:

```cpp
ts::create_scheduler({
    .num_workers = 0,                                // 0 means hardware concurrency.
    .idle_policy = ts::Idle_policy::spin_then_block, // The default.
    .spin_cycles = 64,
});
```

Creating a second scheduler while one is running is fatal, since there is
exactly one per process, and using the scheduler with none running is fatal
too, since there is no lazy spawn. Reconfiguring means tearing the scheduler
down and creating a new one, done at a quiescent point. Restoring the previous
configuration afterwards works the same way, with `current_scheduler_config()`
providing the config to restore.

Idle policies decide what a worker does when it finds no work. `spin` never
sleeps, giving the lowest latency while burning idle cores. `spin_then_block`,
the default, spins briefly and then parks. `handoff` is Go-style: a single
spinning worker keeps watch and wakes a successor when it takes work, which
moves wake costs off producers and is best when submit latency matters.

### 10.3 `Signal`

A `Signal` is a manually completed `Task<void>`, a phase gate:

```cpp
ts::Signal frame_start;
ts::Task<void> work = [](ts::Signal gate) -> ts::Task<void>
{
    co_await gate;       // Suspends until triggered.
    run_systems();
}(frame_start);
...
frame_start.trigger();   // Idempotent; releases everything awaiting it.
```

`Signal::reset()` re-arms it for the next phase and is the one sanctioned
re-arm. Keep one use in flight, reset only after the signal settled and every
awaiter resumed, and note that re-arming an un-triggered signal is fatal.

A `Signal` is also the bridge for completions that come from outside the task
system entirely, such as an overlapped I/O callback, an `io_uring`
completion, or a GPU fence. The handle is refcounted, so the callback
captures one by value and triggers it. One caveat: triggering releases the
awaiting coroutines on the triggering thread. In an OS callback context,
whether an APC, an IOCP worker, or a driver callback, that is the last place
you want arbitrary user code to run, so hop first:

```cpp
// In the OS completion callback:
ts::launch([done]() mutable { done.trigger(); });   // Release on a worker and return now.
```

The library deliberately provides no I/O reactor (see §13); this idiom is the
integration path, and it is the same reason `Frame_gate::open()` below
releases through the scheduler rather than inline.

### 10.4 `Frame_gate` — realigning cross-frame work

Work that waits on something outside the schedule resumes at an arbitrary
moment, quite possibly mid-frame, when the systems it wants to touch are
half-updated. `ts::Frame_gate` (in `ts/frame_gate.h`, and in the umbrella header)
parks a task until the next frame boundary:

```cpp
ts::Frame_gate gate;                 // Owned by the frame loop.

// Somewhere in a long-running task:
co_await io_done;                    // An external completion with arbitrary timing.
co_await gate.next();                // Realign: resume at the next frame start.
co_await world.access([](World& w) { w.apply(result); });

// In the frame loop:
for (;;)
{
    gate.open();                     // Release everyone waiting and re-arm for the next frame.
    frame_graph.execute().sync();
}
```

`next()` hands out the current frame's gate, so a handle taken just before a
boundary is released by that boundary rather than missing it. That is the
race a hand-rolled `trigger()`/`reset()` pair has, along with `reset()`'s
precondition that every awaiter has already resumed. The cost is one small
allocation per frame, which is nothing at frame scale; `Signal::reset()`
remains available when you can guarantee the precondition and want zero.

`open()` returns immediately and releases the waiters through the scheduler,
at `Priority::low` by default; `set_release_priority` changes it. Releasing
them inline would run every parked task, however many accumulated, on the
frame loop's own thread before `open()` returned. The flip side is that a
returned `open()` does not mean the waiters ran, which is the right semantics
for a phase signal.

### 10.5 Exceptions

Macrame never throws, but it is otherwise exception-neutral. Failures are
`ts::fatal`, which prints a message and a stack trace and aborts. Cancellation
is value-based. Results come back through `sync()` / `co_await` rather than out
of a throw. Whether your code uses exceptions is your choice. The library
builds and links either way, and nothing in its interface asks you to turn
them off.

One rule makes that work: an exception must not leave a body.

```cpp
world.async([](World& w)
{
    try { load_from_disk(w); }
    catch (const std::exception& e) { log(e.what()); }   // Handled here, not thrown out.
});
```

A body returns into the library's own frames, such as a worker's dispatch
loop, an object release, or a coroutine resume, which hold grants, lock
counts, and refcounts, and which may be compiled with no exception support at
all. A body that lets an exception escape is therefore fatal, on every path
that runs one: `ts::launch`, `access` / `async`, a graph node, a
`parallel_for` chunk, and a coroutine body, which reports through its
promise. The report names the running task and, for anything deriving from
`std::exception`, its `what()`:

```
FATAL: task 'physics' let an exception escape its body: asset missing - macrame
does not carry an exception across a task boundary: ...
```

Those two are what locate the throw, because a handler runs after unwinding,
so the stack trace below the message starts at the library's seam, not at the
`throw`. Give a task a name (`add_node("physics", ...)`,
`ts::launch(fn, {.name = "stream"})`) and the report points straight at it;
an unnamed one falls back to the call site that created it. Use exceptions
freely inside a body, and handle them before returning.

To build the library itself with exceptions disabled, for a program that is
exceptions-off throughout, configure `-DMACRAME_NO_EXCEPTIONS=ON`. That is a
whole-program setting, not a private one. MSVC's `_HAS_EXCEPTIONS=0` rewrites
standard-library declarations, so the option exports it, along with the
compiler flag where a flag can express it, as a usage requirement, and a link
that mixes the two settings fails with a `_HAS_EXCEPTIONS` mismatch instead
of corrupting quietly.

---

## 11. Patterns and rules of thumb

### 11.1 Granularity decides parallelism

Wrap subsystems, not tiny objects: `Guarded<Physics_world>`, not a
`Guarded<Rigid_body>` per body. Whole-object access means one writer
serializes all readers of that object, so split state along the lines you
want to parallelize; the sample double-buffers its transforms so early
readers and the writer never touch the same object. Tens to low hundreds of
guarded objects and graph nodes is the intended scale, and parallelism inside
a system comes from `parallel_for` running under the system's grant.

### 11.2 Never block inside a task or node

Blocking a worker starves the pool. Consume another system's result with
`co_await` or a graph edge, never with a `sync()` inside a body; the harness
makes the violation fatal (§5.0.1). Two waits are safe by design:
`parallel_for`, whose caller participates and waits only on running helpers,
and `sync()` from a blue thread, since blocking is what blue threads do.

### 11.3 Fire-and-forget is fine

`async` returning a `Task` does not mean you must keep it. Dropping the
handle is safe; the object's queue still runs the job. Keep it only if you need the
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
| lightweight inter-system notifications, many event types | `Event_bus` (§9.4) |
| ordering gate between phases | `Signal` |
| reusing a sub-graph inside a frame | `co_await inner.execute()` (§6.3) |
| realigning cross-frame work to a frame start | `Frame_gate` (§10.4) |

---

## 12. Migrating from the callback vocabulary

Earlier revisions composed with callbacks and builders. That entire surface
is deleted, composition is now coroutines, and every removal is a compile
error, not a behavior change. The replacements:

| deleted | replacement |
|---|---|
| `t.then(fn)` | `co_await t`, then just call `fn`: `co_return fn(co_await t);` |
| `ts::when_all(a, b, c)` | launch all, then await all: `co_await a; co_await b; co_await c;` (awaiting settled tasks is free; results come back typed, no tuple) |
| `ts::task(fn).after(x, y).launch()` | a coroutine body: `co_await x; co_await y; fn();` (prerequisites are awaits at the top of the body) |
| `Task_builder::priority/token` | `ts::launch(fn, { .priority, .token })` at the launch site |
| `Task_builder::set_inline` / `Task_options::run_inline` | deleted for dynamic tasks (graph nodes keep `Graph_node::set_inline`) |
| `Task_builder::reset()` (reusable tasks) | call the coroutine again (one frame per run is the model); `Signal::reset()` remains the re-armable phase gate |
| `ts::add_nested(task)` / `ts::nested(fn)` / `ts::Task_scope` | `ts::parallel_for` for grant-inheriting fan-out over the parent's data; `co_await` to compose; `ts::launch` for detached work (removed; see §4.5) |
| `Task_options` | `Dispatch_options` (`launch`, `async`) or `Access_options` (`access`) |
| retraction (blocking `sync()` running work inline) | deleted; a blue thread parks (that is fine), an in-task `sync()` is fatal, use `co_await` instead |

---

## 13. Limitations & WIP

Stated plainly; each is on the roadmap (`docs/TODO.md`):

- Allocation and performance campaign, WIP. Hot paths currently allocate:
  one control block per task, and journal commands heap past a small-buffer
  threshold. Several planned optimizations (pools, arenas, a typed command
  tier) are designed but not landed.
- Scheduler selection, partial. There is one process-wide scheduler, created
  explicitly with `create_scheduler` and reconfigured by teardown and
  recreate. What remains WIP is ambient per-call selection, meaning two live schedulers
  running at once with individual `launch`/`async`/`execute` calls routed
  between them.
- Platform breadth, WIP. Developed on Windows (MSVC/clang-cl); the test suite
  also runs on Linux under Clang/TSan. No macOS, console, or mobile support
  claims yet.
- Benchmarks and CI. CI runs the suite (MSVC, clang-cl, Shipping,
  Linux/TSan); a benchmark suite exists (`--bench`), and benchmark regression
  tracking is WIP.
- Cross-entity mutation inside `parallel_for`, where item *i* writes item
  *j*. The gather/apply mailbox primitives are researched and designed but
  not yet shipped. Interaction coloring, the other half of this space, is
  shipped (`ts::parallel_for_colored`, §7).
- Generic by-value parameters (`[](auto v)`) in access-deduced positions
  classify as reads and copy the resource, so writes hit the copy silently.
  This is undetectable at the declaration level (§3.1); use references.
- One run per graph instance. A compiled graph runs one execution at a time
  (checked, §6.3), so a sub-graph shared by two concurrently running parents
  needs one instance per caller. Queued and pipelined runs are on the
  roadmap.

---

## Further reading

- [design.md](design.md) — the design rationale: why these primitives, what
  was tried and rejected, and how it compares to other systems.
- [task-internals.md](task-internals.md) — dynamic-task internals (control
  block, lifecycle, coroutine frames, completion gating).
- [command-buffer-design.md](command-buffer-design.md) — the design study
  behind `Deferred`/`Versioned`, including the UE research.
- [deferred-versioned-state.md](deferred-versioned-state.md) — the staged-
  write layer's contracts and mechanisms, in full.
- [task-systems-comparison.md](task-systems-comparison.md) — survey of UE
  Tasks, TBB, Taskflow, folly, Rayon/Tokio, and others.
