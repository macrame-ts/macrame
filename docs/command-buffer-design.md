# Command buffers: design study

Status: proposal / design study (2026-07). Follows up the `docs/TODO.md` item
"Deferred command-buffer writes" and its design points (the recording-front-end
framing, the derived-strata phases discussion). Question under study: **do
command buffers need something new, or can we bend what exists?**

Prior conclusions this builds on (from `docs/TODO.md`):

- **Framing already decided:** a command buffer is *a recording front-end
  terminating in an ordinary `Guarded` write* — not a separate synchronization
  concept, not an "enhanced `Guarded`". `record()` needs no grant on the
  target; `flush()` IS a normal write access (an `async` or a graph node). The
  pipe stays the single arbitration mechanism; the harness is untouched.
- **Pipe FIFO is the serialization contract**, not a scheduling choice:
  reordering a reader past an earlier writer changes observed values.
  Relaxations are explicit *staleness opt-ins* asserted by the user about
  specific reads — never a throughput knob. (This interacts directly with the
  lazy-`Guarded` candidate below.)
- **Phases dissolve into derived strata:** a flush is just a node; edges derive
  from declared access; the pathological mutual-deferral case surfaces as a
  `compile()` cycle naming the exact nodes.
- Allocation discipline: scope-bounded bulk-freed storage is the opt-in bump
  arena's natural first customer (alloc-audit 3.1 #7).

---

## 1. The problem

The common engine shape: many systems *produce* mutations of one logical target
(a render queue, an event bus, an ECS world) while many others *read* it. Under
plain `Guarded`, each producing write serializes against every reader on the
target's pipe — the DAG collapses toward sequential exactly where the frame is
widest. The fix family is "don't mutate live, publish later": producers record
into private storage with **no grant on the target**, and one well-defined
write applies everything. What's missing from the library is the generic layer:
parallel producers into one logical buffer, graph materialization of the flush,
a deterministic apply order, merge/dedup hooks, and an allocation story.

Rendering is the proving use case — it is exactly UE's game-thread →
render-thread pipeline, which is why §2 reads the UE source first.

---

## 2. UE research (read from `c:\src\UnrealEngine-release`)

### 2.1 `ENQUEUE_RENDER_COMMAND` — the coarse cross-thread queue

`Engine/Source/Runtime/RenderCore/Public/RenderingThread.h` (+ `.cpp`).

- **Command = a lambda**: `TUniqueFunction<void(FRHICommandListImmediate&)>`
  (a variant also admitting `void()` and `void(FRHICommandList&)`), recorded by
  the game thread, executed on the render thread. Granularity is coarse —
  "update this proxy", "init this resource" — tens-to-hundreds per frame, not
  tens of thousands.
- **Storage**: `UE::RenderCommandPipe::FCommandList` — an intrusive linked list
  of command records placement-allocated from an `FMemStackBase` (page-based
  **linear allocator**). Enqueue is mutex-guarded on the pipe.
- **Batch-amortized launch**: `FRenderThreadCommandPipe::EnqueueAndLaunch`
  launches a consumer task only on the empty→non-empty transition; that task
  swaps out the whole context and replays every command in order
  (`ConsumeCommands`). One task per burst, not per command.
- **Ordering**: FIFO in submission order per pipe. Named
  `FRenderCommandPipe`s (`DEFINE_RENDER_COMMAND_PIPE`) split streams
  per subsystem so independent streams replay in parallel — structurally our
  per-object `Guarded` pipes.
- **Immediate escape hatch**: if already on the render thread, `Enqueue`
  executes the lambda *right now* instead of recording.
- **`FRenderCommandList`** (the newer layer): a TLS-bound `FRecordScope`
  diverts `ENQUEUE_RENDER_COMMAND` into a thread-local list — "the cost of
  queuing commands into command lists is very light when recording into command
  lists as there are no locks, at the cost of deferring submission" (comment at
  `RenderingThread.h:814`). Lists nest (parent/child splice), and
  `FParallelForContext` hands **one command list per task thread**, whose
  contents are *patched into the parent stream at the submit point* — commands
  recorded in parallel land, in order, between the commands surrounding the
  `Submit()` call.

### 2.2 `FRHICommandList` — the fine per-list recorder

`Engine/Source/Runtime/RHI/Public/RHICommandList.h`.

- **Command = a data struct**, not a lambda: `FRHICommandBase` is an intrusive
  singly-linked node (`Next` + one virtual `ExecuteAndDestruct`), and every
  typed command (`FRHICommandSetShaderParameters`, draw commands, …) is
  placement-new'd out of the list's own `FMemStackBase MemManager` via
  `ALLOC_COMMAND` (`RHICommandList.h:414`, `AllocCommand` at `:567`). Per
  command: a linear-allocator bump + a link store. No per-command heap, no
  refcount, destruction fused into execution.
- **Lambda escape hatch exists here too** (`EnqueueLambda` →
  `TRHILambdaCommand`), used for the irregular cases; the hot commands are
  structs.
- **Parallel recording, ordered replay**: many `FRHICommandList`s are recorded
  concurrently on task threads, then
  `QueueAsyncCommandListSubmit(TArrayView<FQueuedCommandList>, ETranslatePriority)`
  chains them into the immediate list — "allowing in-order submission of
  parallel rendering work" (`RHICommandList.h:4683`), with optional parallel
  translate of each chunk.
- **Bypass mode**: `Bypass()` short-circuits recording — commands execute
  directly against the RHI context (`IsBottomOfPipe()`); recording asserts
  against it (`checkfSlow(!Bypass(), "Invalid attempt to record commands in
  bypass mode")`). Note it is a **debug/profiling toggle** (compile-time in
  shipping, `CAN_TOGGLE_COMMAND_LIST_BYPASS` otherwise), *not* a per-instance
  semantic mode application code branches on — and even so it leaks assertion
  complexity into every call site.
- **Immediate-vs-deferred split** is two types (`FRHICommandListImmediate` vs
  the deferred lists) — the semantic difference is *in the type*, not a flag on
  a shared type.

### 2.3 RDG — access-declared passes, derived order

`RenderGraphBuilder.h`, `RenderGraphPass.h`, `RenderGraphAllocator.h`.

- `FRDGBuilder::AddPass(name, parameter_struct, flags, execute_lambda)`:
  passes declare resource access via the parameter struct; "resource barriers
  and lifetimes are **derived** from RDG parameters … the resulting graph is
  compiled, culled, and executed in `Execute()`" (`RenderGraphBuilder.h:42`).
  Explicit extra edges via `AddPassDependency(producer, consumer)` — our
  `after`/`before`.
- Setup lambdas record; nothing executes until `Execute()` — deferred
  execution at a defined moment, exactly our `compile()`/`execute()` split,
  except RDG **rebuilds per frame** (they pay for it with culling, parallel
  setup, and a dedicated `FRDGAllocator` — a TLS `MemStack` linear allocator
  that owns passes, parameter structs, and the pass lambdas themselves).
- Structurally this is `Static_task_graph` for GPU resources, shipped in
  production at enormous scale: **the access-derived-ordering thesis of this
  library is UE's production answer for the hardest scheduling domain they
  have.** Their per-frame rebuild also validates our build-once/run-many
  choice by contrast — RDG's setup cost is a constant fight (culling exists
  partly to claw it back).

### 2.4 Transferable lessons

1. **Two-level structure.** A coarse cross-thread queue (lambda commands,
   mutex enqueue, batch replay) *and* a fine per-list recorder (POD commands,
   lock-free thread-local recording, linear allocator) — with an order of
   magnitude between their command granularities and correspondingly different
   storage. Our `Guarded::async` already *is* the coarse level (lambda job,
   per-object pipe, batch admission). What we lack is the fine level. Don't
   bend the coarse mechanism to be the fine one — UE keeps them separate types.
2. **Linear allocators for command storage, everywhere.** `FMemStackBase` /
   `FRDGAllocator`: placement-new, intrusive links, destroy-on-execute, bulk
   page reset. Never one heap alloc per command.
3. **Parallel recording with ordered replay.** Per-thread/per-task sub-lists
   spliced at a defined point (`FParallelForContext`,
   `QueueAsyncCommandListSubmit`). Determinism comes from *where the splice
   is*, not from when threads ran.
4. **The immediate escape hatch is real but type- or debug-gated.**
   `FRHICommandListImmediate` is a distinct type; `Bypass()` is a build-config
   toggle. UE deliberately avoids "same instance, different semantics decided
   at runtime by construction mode" for application-facing code — the closest
   thing they have (`Bypass`) is fenced with asserts and disabled in shipping.
5. **Commands as data when hot, closures when irregular.** Both tiers exist in
   UE; the hot tier is structs.

---

## 3. Candidate designs

### 3.1 Design 1 — lazy `Guarded`

A `Guarded` flavor (construction-time mode) whose *write* jobs do not dispatch
when admitted: they accumulate in the pipe, and a dedicated graph node
("acquire write + flush queued closures") drains them at a defined moment.

**What it gets right.** Zero new types; the flush is conceptually trivial (the
pipe drains under one write hold); every recorded write is a normal block, so a
per-command `Task<R>` (result, token, priority) falls out for free; and the
harness story is automatic (each queued body runs under an ordinary write grant
at flush). It is maximally "just the pipe".

**(a) Mode-dependent semantics.** The same `x.async(fn)` call now means "runs
ASAP under pipe rules" on one instance and "parks until some node flushes" on
another, with nothing at the call site to distinguish them. This is the
author's own stated worry, and the UE precedent cuts against it: UE's only
comparable switch (`Bypass()`) is a debug toggle fenced by asserts, while the
*semantic* immediate/deferred split is carried by distinct types
(`FRHICommandListImmediate` vs deferred lists). A latency property this large —
"your write may not happen this frame unless a flush node exists" — wants to be
visible in the type, not in a construction flag three files away. It also
poisons the composed surfaces: what does `sync()` on the returned `Task<R>`
mean before anyone flushes (deadlock unless retracted — and pipe blocks are
deliberately *not* retractable); what does `wait_until_idle` in the destructor
mean; does `run_inline` bypass laziness?

**(b) FIFO blocks readers — the fork that breaks it.** The pipe is FIFO
(`dispatch()` in `guarded.cpp` stops at the first inadmissible front job; a
front writer holds back everything behind it). Park an unflushed write at the
front and **every later reader queues behind it until the flush**. Two ways
out, both bad:

- *Readers wait (pure lazy).* Value semantics are perfectly preserved — each
  read still observes exactly the writes submitted before it, just later in
  wall-clock time. But then recording gives **zero reader parallelism**: the
  entire motivation (producers running concurrently with readers of the
  pre-flush state) evaporates. This variant is semantically clean and
  practically pointless.
- *Readers pass queued writes.* This is precisely the "staleness opt-in"
  relaxation already analyzed in the TODO's pipe-FIFO item: a read reordered
  past an earlier write **observes different values**, which that analysis
  concluded is a per-read user assertion, never a mode default. To make lazy
  `Guarded` useful you must flip *both* knobs at once (lazy writes + reads pass
  writes), at which point the instance's pipe is no longer a pipe — it is a
  command buffer wearing the pipe's API, with the pipe's ordering guarantees
  silently revoked for that instance. Two coupled semantic switches on one
  type, each individually incoherent, is the confusability worry squared.

Note the connection runs the other way too: the CB design (3.2) delivers
exactly the semantics the second variant gropes for — readers see none of the
recorded writes until the flush, all of them after — but gets them *honestly*,
by construction (separate storage), instead of by relaxing an
order-preserving structure.

**(c) Per-command cost and the closure ceiling.** A recorded write is a full
pipe job: one `Task_control_block` (288 B, refcounted, mutex+CV inside) + a
mutex-guarded `deque` node + an indirect dispatch at flush — per command. The
commands are opaque closures, so there is no merge/sort/dedup surface (Flecs
merges per-entity commands; Unity sorts by `sortKey` — impossible over
type-erased bodies), and no arena story (blocks are individually heap-allocated
and refcounted; the alloc-audit free-list helps but a bump arena cannot own
them). Recording also contends: every `record` is `pipe_enqueue` under the
single pipe mutex, so N producer nodes serialize on the very object they were
supposed to stop contending on — where per-producer sub-buffers record with no
synchronization at all. UE's fine tier exists precisely because the
lambda-queue tier tops out around here. Finally, apply order = cross-thread
mutex-acquisition order — *racy*, not deterministic run to run.

**Verdict:** decline as a `Guarded` mode. The one variant that preserves pipe
semantics buys nothing; the useful variant is a different concept that should
carry a different name — at which point it is design 3.2.

### 3.2 Design 2 — `Command_buffer<T>` bound to a `Guarded<T>`

The TODO's framing, made concrete. One new type; `Guarded` untouched; the pipe
stays the only arbitration mechanism.

- `Command_buffer<T> cb{target}` binds to a `Guarded<T>` for its lifetime.
- **`record(cmd)` needs no grant** on the target and touches only the buffer.
  Storage is per-producer sub-buffers (chunked, arena-backed — the bump-arena
  item's first customer): recording is contention-free and the harness is
  untouched (nothing reads or writes `T`).
- **`flush()` is an ordinary write access**: `cb.flush_async(opts)` is
  sugar for `target.async([cb](T& t){ cb.apply_all(t); }, opts)` — one pipe
  acquisition amortized over N commands, applied in a cache-warm sweep; or a
  graph node with declared write access (§3.4). Consistency is the stable
  snapshot the TODO already identified: readers between record and flush see
  none of the writes, after it all of them.
- **Apply order is fixed at flush**, not at record: sub-buffers apply in
  producer order (a stable key — graph node index or recorder creation index),
  intra-buffer FIFO. Deterministic across runs regardless of thread timing —
  *better* than the pipe's cross-thread submission order, and the natural seam
  for merge/sort/dedup hooks (sort by material, dedup per-entity writes).
- **Commands**: two tiers, mirroring UE. The typed tier
  (`Command_buffer<T, Cmd>` with `apply(T&, const Cmd&)`, or `Cmd::apply(T&)`)
  stores POD commands contiguously — bump-alloc per record, sortable,
  mergeable. The closure tier (`record([](T& t){ … })`) stores the callable in
  the same arena behind a fn-ptr thunk — still no task block, no refcount, no
  per-command heap; just no sort/merge. Offer both; the typed tier is the
  point.
- **Not supported (honestly):** per-command results — the flush returns one
  `Task<void>`; a result-bearing mutation stays a normal `async` (that is what
  the pipe is for). No read-your-writes before the flush — for produce-
  downstream, not in-place RMW (same contract as Bevy `Commands` / Unity
  `EntityCommandBuffer`).

Cost picture per command: bump-pointer alloc + a POD store (typed tier) vs
design 1's 288 B block + refcount + deque node + indirect call — roughly two
orders of magnitude, before counting the eliminated pipe-mutex contention.

### 3.3 Design 3 — status quo / hand-rolled

Producers fill local containers; one `async` or node applies them. Zero new
API, and the sample's double-buffered transforms show the family already works
by hand. What each user re-invents, badly or not at all:

- **Parallel producers into one logical buffer** — hand-rolled TLS or
  per-node vectors plus a gather step, per use site.
- **Graph materialization** — the flush node and its edges wired by hand with
  explicit `after`; forget one producer and the flush silently races it
  (completeness hazard, and the graph *manufactures* parallelism around it).
- **Determinism** — apply order is whatever the gather found; nobody sorts.
- **Merge/dedup hooks, arena backing** — absent.

This is the null hypothesis, and it is respectable — but the TODO item exists
because the missing layer is exactly the part that is generic.

### 3.4 Design 4 — hybrid: design 2 + graph sugar (recommended)

`Command_buffer<T>` as the **only** new public type, `Guarded` untouched, plus
first-class graph integration:

- `add_node` accepts a `Command_buffer<T>&` alongside `Guarded<>`s; the
  functor takes a `Recorder<T>&` for it. Declaring a CB means **append
  access**: a new `Access::append` mode that is self-concurrent (recorders
  don't conflict with each other) and conflicts with the flush. In the
  conflict lattice it behaves like `read_only` (concurrent with itself,
  ordered against the exclusive accessor); a distinct enumerator keeps it
  honest in diagnostics. Each recording node gets its **own sub-buffer, keyed
  by node index** — contention-free *and* deterministic.
- `Static_task_graph::add_flush_node(cb)` materializes the flush: a node with
  `Access::read_write` on the *bound target*, plus derived edges after every
  node that appends to `cb` (append→flush conflict) and, via the existing
  conflict derivation, ordered against every other accessor of the target.
  Returns a normal `Graph_node` (`after`/`before`/`priority`/`set_inline` all
  apply).
- The dynamic path stays: `cb.recorder()` mints a producer handle;
  `cb.flush_async(opts)` for non-graph frames.
- The TODO's derived-strata story falls out: `A_record` (reads A, appends CB)
  → `F_B` (writes B) → `A_continue` (reads B) — all edges derived; mutual
  deferral surfaces as a `compile()` cycle naming the nodes.

Harness impact: none. Recording touches no guarded state; the flush node's
body runs under an ordinary write grant like any node. The pipe remains the
single arbitration mechanism — a concurrent `async` on the target simply
queues around the flush like around any writer.

---

## 4. Comparison

| | 1 lazy `Guarded` (readers wait) | 1' lazy + reads-pass-writes | 2 `Command_buffer<T>` | 3 hand-rolled | 4 = 2 + graph sugar |
|---|---|---|---|---|---|
| **New API surface** | none (a ctor mode) | ctor mode + per-instance read relaxation | 1 type (`Command_buffer`, `Recorder`) | none | 1 type + `Access::append` + `add_flush_node` |
| **Semantic clarity** | poor: same `async`, different latency per instance; `sync()`/dtor/inline muddied | worst: two coupled mode switches; pipe ordering silently revoked | good: recording vs access split by type; flush = normal write | good (nothing to confuse) | best: append access is declared and visible in the graph |
| **Reader parallelism while recording** | **none** (FIFO blocks readers behind parked writes) | yes, by weakening pipe semantics | yes, by construction (no grant taken) | yes | yes |
| **Per-command cost** | task block (288 B) + refcount + deque node + pipe mutex + indirect call | same | bump alloc + POD store (typed); arena'd closure (untyped) | vector push (good), gather copy (extra) | same as 2 |
| **Merge/sort/dedup** | no (opaque closures) | no | yes (typed tier; flush-time hooks) | by hand | yes |
| **Deterministic apply order** | no — cross-thread mutex-acquisition order | no | yes — producer-keyed sub-buffers, splice order fixed at flush | by hand | yes, keyed by node index |
| **Graph integration** | flush node by hand; no derived record→flush edges (recording isn't declared) | same | flush = `async` or hand-wired node | all by hand (completeness hazard) | flush = derived node; append→flush + write→read edges all derived |
| **Harness impact** | none (queued bodies run under normal grants) | read-relaxation must be taught to the oracle | none | none | none |
| **Arena/allocation story** | none (refcounted blocks; free-list at best) | none | natural fit — the bump-arena item's first customer | by hand | same as 2 |
| **Result-returning commands** | **yes** — each write is a `Task<R>` | yes | no — flush returns one `Task<void>`; results stay on `async` | no | same as 2 |

---

## 5. The rendering example (design 4)

A small frame renderer: game-side systems record draw/update commands during
their graph nodes — parallel, **no grant on the renderer** — a flush node
applies them, render-side nodes consume.

```cpp
// The render queue is an ordinary guarded system; commands mutate it only at the flush.
struct Draw_cmd
{
    Mesh_id mesh;
    Transform xf;
    Material_id material;

    void apply(Render_queue& q) const { q.submit(mesh, xf, material); }
};

Guarded<Render_queue> render_queue;
Command_buffer<Render_queue, Draw_cmd> render_cb{render_queue};

Static_task_graph g;

// Producers: append-only access to the buffer -- parallel with each other AND with
// any reader of render_queue (they hold no grant on it). Each node records into its
// own sub-buffer (keyed by node index -> deterministic apply order).
auto animate = g.add_node(
    [](Anim_system& anim, Recorder<Render_queue, Draw_cmd>& rq)
    {
        for (const Skeletal_mesh& m : anim.visible())
            rq.record(Draw_cmd{ .mesh = m.mesh, .xf = m.world_xf, .material = m.skin_material });
    },
    anim_system, render_cb);

auto scatter = g.add_node(
    [](const Particle_system& fx, Recorder<Render_queue, Draw_cmd>& rq)
    {
        for (const Emitter& e : fx.emitters())
            rq.record(Draw_cmd{ .mesh = e.billboard, .xf = e.xf, .material = e.material });
    },
    particle_system, render_cb);

// The flush: write access on render_queue, materialized as a node. Derived edges:
// AFTER animate/scatter (append -> flush on render_cb), BEFORE draw (write -> read
// on render_queue). Optional flush-time hook sorts by material for the sweep.
auto flush = g.add_flush_node(render_cb,
    { .sort = [](const Draw_cmd& a, const Draw_cmd& b){ return a.material < b.material; } });

// Consumer: ordinary read access; runs after the flush by conflict derivation.
auto draw = g.add_node(
    [](const Render_queue& q, Rhi_context& rhi)
    {
        for (const Draw_cmd& cmd : q.draws())
            rhi.draw(cmd);
    },
    render_queue, rhi_context);

g.compile();
g.execute().sync();

// Dynamic (non-graph) path: mint a recorder, flush as a plain async.
auto rec = render_cb.recorder();
rec.record(Draw_cmd{ .mesh = debug_mesh, .xf = identity, .material = wireframe });
Task<void> applied = render_cb.flush_async({ .priority = Priority::high });
```

Every edge above is derived: `animate ∥ scatter` (append is self-concurrent),
`animate/scatter → flush` (append vs flush), `flush → draw` (write vs read on
`render_queue`). A debug-HUD node reading `render_queue` *before* the flush
would be ordered before it (read vs write) and would observe the pre-frame
state — the stable snapshot, by construction.

**The same frame on design 1**, for contrast: `animate` would issue one
`render_queue.async([cmd](Render_queue& q){ q.submit(cmd); })` **per draw** —
one 288 B task block and one pipe-mutex hit each, ~thousands per frame; the
producers all contend on the one pipe mutex they were meant to escape; the HUD
reader either parks behind every queued draw until the flush (pure lazy) or
must opt into reads-passing-writes; and the flush node has no derived edges
from the producers, because recording was never declared — you wire
`flush.after(animate).after(scatter)` by hand and hope nobody adds a producer.

---

## 6. Recommendation

Ranked:

1. **Design 4** — `Command_buffer<T>` as the only new public type, `Guarded`
   untouched, `Access::append` + `add_flush_node` for the graph. Staged:
   (a) core `Command_buffer`/`Recorder` + `flush_async` (dynamic path only,
   vectors before the arena lands); (b) graph integration (`Access::append`,
   `add_flush_node`, node-index-keyed sub-buffers); (c) the typed-command
   sort/merge hooks; (d) rebase storage onto the bump arena when alloc-audit #7
   ships. Validate shape by porting one sample system per stage.
2. **Design 3** — legitimate as the interim position; the sample should
   hand-roll the render-queue example *first* anyway, as the fixture design 4
   is validated against.
3. **Design 1** — declined as a `Guarded` mode. Its one semantically clean
   variant (readers wait) delivers no reader parallelism — the motivating
   property; the useful variant requires the reads-pass-writes relaxation,
   which the pipe-FIFO analysis already classified as a value-changing per-read
   opt-in, not an instance mode. What survives of the idea is real, though:
   the *flush terminating in the pipe* is exactly how design 4's flush works,
   and the staleness-opt-in read relaxation remains separately tracked on its
   own merits.

Where lazy `Guarded` honestly wins — zero new types, per-command `Task<R>`
results, harness-for-free — and where it loses — mode confusion on the
library's central type, FIFO-blocked readers, closure-only commands at task
prices, racy apply order, no arena — the losses land precisely on the axes the
use case exists for. The author's instinct to minimize API surface is better
served by one honest new type than by one old type with two personalities: UE,
given decades and the same pressure, kept the coarse lambda queue, the fine POD
recorder, and the access-declared graph as *three* named things.

### Open decision points

1. **Command representation default**: typed-POD tier and closure tier both, or
   typed-only to start? (Recommendation: both from the start; the closure tier
   is cheap once the arena thunk exists, and it is the migration path from
   hand-rolled code.)
2. **Sub-buffer keying off-graph**: recorder handles are explicit
   (`cb.recorder()`) — is a TLS implicit recorder (`cb.record(cmd)` from
   anywhere) worth the nondeterminism it reintroduces, or is per-producer
   explicitness the contract? (Lean: explicit; graph nodes get theirs
   implicitly by node index.)
3. **`Access::append` vs reusing `read_only`** in the conflict lattice: same
   derived edges either way; a distinct enumerator costs a case in
   `conflicts()` and buys honest diagnostics + a place to hang append-specific
   rules later. (Lean: distinct.)
4. **Flush-node coalescing**: one flush per CB per graph (current sketch) vs
   auto-materialized per-wave flushes from `defers_write` declarations (the
   TODO's more ambitious derived-strata sketch). (Lean: explicit
   `add_flush_node` first; auto-materialization is sugar that can come later
   without breaking anything.)
5. **Unflushed-at-destruction / unflushed-at-run-end policy**: fatal, warn, or
   silently drop? (Lean: `ts::fatal` under `TS_SAFETY_CHECKS` — an unflushed
   buffer is a lost write, the same severity as an undeclared access.)
6. **Re-run semantics**: the CB is re-armed per graph run like node blocks
   (flush leaves it empty, arena bulk-reset) — does a *skipped* flush
   (cancelled run) carry commands into the next run or drop them? (Interacts
   with 5; lean: carry, with a diagnostic counter.)
7. **Cross-target commands**: a command touching two `Guarded`s doesn't fit a
   single-bound CB. Defer — record into two CBs, or fall back to multi-object
   `ts::async`; a multi-target CB is a real design if a use case shows up.
8. **`parallel_gather_apply` convergence**: the intra-node sibling (per-entity
   inboxes) shares the record→bin→apply skeleton; design the arena chunk +
   recorder machinery so both sit on it (already flagged in TODO 7.x — keep
   the storage layer common).
