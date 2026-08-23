# Coroutine-first leftovers — audit (reconnaissance, not a rewrite)

Audit of the public API surface and the task-layer internals for **leftovers** from the
pre-coroutine "raw dynamic tasks" era, following the coroutine-first transformation
(`docs/coroutine-first.md`). Two questions, per the brief:

- **Part A** — features that may no longer earn their place now that composition is
  `co_await` and raw dynamic tasks (`then`, `when_all`, builders, `.after()`, retraction,
  reuse, `ts::nested`) are gone.
- **Part B** — dead / vestigial *implementation* left over from the wider functionality
  (retraction and reusable tasks especially).

Findings are numbered as stable IDs. Nothing was changed. Each finding is
**keep / cut / reshape** (A) or **live / vestigial / dead** (B) with the evidence.

**Method.** Read `CLAUDE.md`, `docs/coroutine-first.md` (§1/§3/§4/§8), `docs/task-internals.md`,
the public headers under `include/ts/`, and the task/graph/pipe implementation in `src/`.
Grepped the whole tree (`include/`, `src/`, `tests/`, `sample/`, `tsan/`) for every reader
and writer of each suspected-dead symbol. Compile-time reasoning where it was decisive
(constant-folding a value across all its writers).

**Headline.** The API surface is clean of the deleted *verbs* — no `then`, `when_all`,
`Task_builder`, `Task_options`, `add_prerequisite`, or `retract*` survives as callable
surface anywhere in `include/` or `src/`. The leftovers are (1) a handful of **stale doc
comments** naming removed types, and (2) a **generation / reuse substrate inside
`Task_control_block`** that is now threading a compile-time constant, plus **one genuinely
dead helper** (`make_block` / `Result_block<R>`). The author's specific hypothesis — that
`Task_control_block` still carries a `prerequisites` vector — is **not borne out**: it is
already gone (B1). The live vestige the hypothesis was pointing at is the **generation
machinery** (B2), which retraction/reuse needed and nothing live does.

---

## Part A — features vs. the coroutine focus

### A1. `Signal` — KEEP (unique capability intact)
`ts::Signal` (`task.h:1249`) is a bodyless `Task<void>` completed by hand via `trigger()`,
plus `reset()`. `docs/coroutine-first.md` §1 explicitly keeps it, and the code bears that
out: it is the **only** manual/external-completion primitive, and it is load-bearing —
`Frame_gate` (`frame_gate.h`) is built entirely on `Signal` + `Signal::reset()` semantics
(`current_`, `std::exchange(current_, Signal{})`, the per-frame re-arm). `co_await` + a
coroutine cannot replace it: a coroutine completes when its *frame* completes, whereas a
`Signal` is completed by an unrelated thread (an OS callback, a GPU fence, the frame loop).
The three retained capabilities are each exercised: manual completion (`trigger`), phase
gate (`Frame_gate`), reusability (`reset`). No change.

### A2. `ts::launch` — KEEP, but reshaped doc (see A7)
`ts::launch(fn, Launch_options)` (`task.h:1190`) is the eager fire verb for a **functor**
body that touches no guarded object: one heap alloc, no coroutine frame, inherits nothing
(detached, empty access context — `docs/coroutine-first.md` §2). It is still meaningful and
still used internally (`Frame_gate::open` launches the release task so resumptions spread
across workers instead of stalling the frame loop). It is *not* redundant with "just call a
coroutine": a coroutine allocates a frame and carries suspension machinery; `launch` is the
zero-suspension leaf-work path. Keep — the only issue is a stale comment (A7).

### A3. Functor (non-coroutine) task body path — KEEP (not a duplicate path)
The `Executable<Body,R>` path (`task.h:843`) underlies `async`, `launch`, and the
`parallel_for` helpers. It is not a second copy of the coroutine path — it is the **leaf**
path: a body that runs to completion without suspending. Its value over coroutine-only is
exactly what the brief flags: functor bodies are structurally immune to the
suspension/ABBA/guard-across-suspend hazards the whole rules layer (`rules.h`) exists to
catch, and they cost no frame. Keep. (Assessed as requested, not recommended for removal.)

### A4. `parallel_for` / `parallel_for_async` — KEEP, surface is clean
`Parallel_options` (`parallel_for.h:27`) is `{concurrency, balance, priority}` — no
builder-era vestige (no `run_inline`, no retraction hint). The internal
`Parallel_state::inherited_ctx` / `inherited_owner` are the grant-snapshot and trace-owner
inheritance, both current-era. The blocking `parallel_for` is documented as the *one*
sanctioned in-task wait (`parallel_for.h:194`), consistent with §4.1. No leftovers.

### A5. Cancellation surface — KEEP, coherent
`Cancellation_source`/`token`/`Cancel_callback` and the trailing-`Cancellation_token`
body early-out (`takes_token_v`, `Task_result` specialization, `task.h:918-928`) are all
still coherent. The trailing-token early-out applies to **functor** bodies (`async`/`launch`);
a coroutine carries its token on the promise and polls it between awaits — two spellings of
one idea, both live. `try_take()` / `as_optional()` (`task.h:1112`, `1121`) are the
coroutine-era cancellation-tolerant consumes and are tested (`coroutine_tests.cpp`). The
one shrink the transformation implies already happened: `prereq_cancelled` propagation is
now awaiter-local (no successor chain to walk — §4.5), and the flag is still used by graph
successors (`release`, `Executable::run`). Nothing vestigial.

### A6. Options aggregates — KEEP, no vestigial fields
Checked each for fields referencing removed capabilities:
- `Launch_options {token, priority, name}` (`task.h:1029`) — clean; **no** `run_inline`.
- `Access_options {token, priority, name}` (`task.h:971`) — clean; the comment explicitly
  notes it is "deliberately WITHOUT a run-inline knob".
- `Execution_options {token, detach}` (`static_task_graph.h:106`) — `detach` is
  current-era (nested-run opt-out, §4.8). Clean.

There is **no** surviving `Task_options` aggregate anywhere (grep: zero hits outside history
comments) — it went with the builders, correctly. This finding is a negative: the aggregates
are already clean.

### A7. Stale references to removed features in PUBLIC headers — CUT (doc only)
Comment-level surface that should have gone with the feature:
1. `ts.h:11` — `#include "ts/task.h"  // ts::Task<R>, launch/nested, Signal, cancellation`.
   **`nested` is removed** (§4.3); the umbrella header still advertises it.
2. `static_task_graph.h:83` — `Graph_node::set_inline` doc says *"Same trade-offs as
   `Task_builder::set_inline`"*. `Task_builder` no longer exists; the cross-reference
   dangles. (The `set_inline` feature itself is live — see B3.)
3. `scheduler.h:111` — comment says the block's per-dispatch payload is *"(reuse generation,
   owning pipe)"*. Both are stale: there is no reuse, and `dispatch_arg` no longer carries a
   pipe pointer (B2). Minor, but it actively misdescribes live code.

Recommendation: fix the three comments. No code change. (Note: `Graph_node::after`/`before`
are **not** in this list — those are live *graph* edges, unrelated to the removed dynamic
`.after()`.)

### A8. `Task<R>::sync()` / `take()` demoted to the blue boundary — KEEP (as designed)
Not a leftover: `sync()`/`take()` are intentionally boundary-only now, with the in-task
call routed to a fatal (`sync_wait` → `blocking_sync_diagnose`, gated by
`Rule::in_task_sync`). Coherent with §4.1. Listed only to record it was checked.

---

## Part B — implementation leftovers

### B1 (lead). `prerequisites` vector — ALREADY GONE; hypothesis not borne out
**Verdict: the removal the CHANGELOG claims is real and complete.** The author asked to
verify that `Task_control_block` still carries a `prerequisites` collection now needed only
for retraction. It does **not**. I read the full struct (`task.h:318-640`); its members are:

`refcount, num_locks, destroy, result_ptr, execute, on_complete, run_state, dispatch_arg,
pipe_links, token, ready, prereq_cancelled, completed, cancelled, pipe_count, pipes_entered,
flags, mutex, done_cv, nested_parent, continuations` (+ `name` under `TS_DEBUG_NAMES`).

There is **no `prerequisites` member and no `successors` vector.** The size comment at
`task.h:322-325` documents exactly the two shrinks: `320 → 280` when the coroutine-first
deletions dropped `prerequisites`, and `280 → 264` when `successors` collapsed to the single
`nested_parent` `Task_ptr` slot. Grep for `prerequisites` / `add_prerequisite` across
`include/` + `src/` returns only historical-note comments, no field and no code. So B1 is a
clean bill: this particular leftover was already removed. The live vestige the hypothesis was
circling is B2.

### B2. Generation / reuse substrate (`run_state` high bits + `dispatch_arg`) — VESTIGIAL
**This is the main implementation leftover.** The reuse machinery for reusable *executable*
tasks (`Task_builder::reset`, removed §3) left behind a **generation** apparatus that now
threads a compile-time constant.

**Evidence that `generation()` is invariantly `0` for every block that is ever dispatched:**
- Bare tasks (`async`, `launch`): the block is freshly made and **never `reset()`**, so its
  `run_state` stays `0` for life. `build_bare_task` (`task.h:1168`) submits at
  `core->generation()` == `0`.
- Graph nodes: re-armed each run by **directly storing `run_state = 0`**, not via `reset()`
  — `static_task_graph.cpp:601` (`b.run_state.store(0, ...); // nodes aren't reset()`), then
  dispatched at `node.block->generation()` == `0` (`static_task_graph.cpp:352`).
- `Signal`: the only caller of `Task_control_block::reset()` (which *does* bump the
  generation) — but a `Signal` is **bodyless** (`execute == nullptr`), never dispatched, so
  its bumped generation is never read by a `claim`/dispatch. The bump is write-only.

Therefore every live `claim(gen)` sees `gen == 0`: `Executable::run` (`task.h:854`), the
pipe-task trampoline (`guarded.cpp:82-85`), and `run_graph_node` (`static_task_graph.cpp:369`).
The pipe direct-run path even passes a literal (`guarded.cpp:333`: `block->execute(block,
/*gen*/ 0)`). The whole "capture gen before the decrement / thread it through
`release` → `dispatch_ready` → `submit_ready` → `dispatch_arg` → the trampoline → `claim`"
TOCTOU chain (`task.h:467-524`, the comments at `task.h:290`, `353-361`, `472-474`, `502-503`)
guards against a `reset()` racing an in-flight dispatch — a race that **cannot occur** because
no dispatched block is ever `reset()`.

Concrete dead weight and how to read it:
- **`dispatch_arg`** (`task.h:361`) is a full `std::atomic<std::uint64_t>` — 8 bytes on every
  block — whose *only* live traffic is storing `0` (`guarded.cpp:99`) and loading it back
  (`guarded.cpp:82`). Grep confirms no other reader/writer, and `blocking_sync_diagnose` does
  **not** use it — it matches the offending pipe by walking `pipe_links` + `write_epoch`
  (`guarded.cpp:689`, `holds_epoch`), so the CLAUDE.md note about a "`dispatch_arg`-stamped
  `Pipe*`" and a `Flags::pipe_job` bit is itself stale (that bit is gone from `Flags` —
  `task.h:395` has only `priority:2` and `run_inline:1`).
- **`run_state` bits [63:1]** (the generation) are dead; only bit [0] (the one-runner claim)
  is live. With `gen == 0` the CAS in `claim` (`task.h:445`) is `expected 0 → desired 1`,
  i.e. a plain test-and-set — no generation content survives.
- The **`gen` parameter** on `submit_ready`, `dispatch_ready`, `execute`, `release`'s local
  capture, and the `inline_pending` vector's `pair<Task_ptr, uint64_t>` (`task.h:498`) are all
  carrying the constant.

**Classification: vestigial (not dead — it compiles and runs, it just encodes nothing).**
The one-runner **claim bit** should stay (cheap machinery-bug detector across the inline /
queue / node dispatch routes). Recommendation, author's call and best done as one change:
drop the generation dimension — delete `dispatch_arg`, reduce `run_state` to an
`atomic_flag`/`atomic<bool>` claim, and remove the `gen` parameters and the `pair` in
`inline_pending`. Saves 8 bytes/block and removes a subtle TOCTOU comment-load. **Risk:
moderate** — the threading touches the hot dispatch path and three dispatch routes; the claim
bit's acquire/release ordering must be preserved. Do it behind the existing TSan graph/pipe
stress. Not urgent (correct as-is), but it is the single largest "why is this here" in the
block.

### B3. `Flags::run_inline` + `Graph_node::set_inline` — LIVE (graph-internal, as designed)
`docs/coroutine-first.md` §3/§8 keep inline dispatch *graph-internal* while removing it from
the dynamic surface. Confirmed reached and tested, not just present:
- Writer: `static_task_graph.cpp:614` (`b.flags.run_inline = nodes_[i].inline_dispatch`),
  set from `Graph_node::set_inline()` (`static_task_graph.cpp:120`).
- Reader: `dispatch_ready` (`task.h:505`) takes the `inline_pending` trampoline path.
- Exercised: `graph_tests.cpp:353/372/373/389` and `tsan_main.cpp:742-744` call `.set_inline()`
  under async contention.

No public dynamic verb sets `run_inline` (grep: only the graph writes it), which is exactly
the "removed from the dynamic surface only" end-state. **Keep.**

### B4. `inline_pending` trampoline (`dispatch_ready`) — LIVE, but reached only via B3
The per-thread FIFO inline trampoline (`task.h:489-524`) is live, but its `run_inline == true`
branch is reachable **only** through graph inline nodes (B3), since nothing else sets the bit.
The `run_inline == false` branch (plain `submit_ready`) is the universal dispatch path and is
hot. **Keep**; note that if B2 lands, this vector's element type simplifies from
`pair<Task_ptr, uint64_t>` to `Task_ptr`.

### B5. `Task_control_block::reset()` — LIVE for `Signal` only; its generation bump is vestigial
`reset()` (`task.h:625`) has exactly **one** caller: `Signal::reset()` (`task.h:1268`). Graph
re-arm does not use it (it stores fields directly, B2). So `reset()` survives as the
`Signal`-reuse primitive (kept by design). But inside it, the line
`run_state.store((generation() + 1) << 1, ...)` bumps a generation that, for a bodyless
`Signal`, nothing ever reads (B2). **Classification: the function is live; its generation
bump is vestigial.** If B2 lands, `reset()` reduces to clearing the completion scalars +
clearing the claim bit. **Keep the function; simplify with B2.**

### B6. Dead helper: `detail::make_block()` + `Result_block<R>` (non-void) — DEAD
`make_block()` (`task.h:738`) and the `Result_block<R>` struct it instantiates (`task.h:696`,
instantiated only at `task.h:747`) are the "result with no body" detail-producer path — what
`then` / `when_all` / detail-level result producers used. **No live caller:** grep for
`make_block<`, `make_block(`, `make_block` across the whole tree returns only the definition
and doc comments (task.h) — zero call sites. `Result_block<R>` is referenced only inside
`make_block` itself and in two doc comments. Because `make_block` is never called,
`Result_block<R>` for non-void `R` is never instantiated.

Distinguish from live neighbours: `make_bare_block()` (the *void* producer, `task.h:710`) is
live (`Signal`, `settled_void_core`/`cancelled_void_core`, `parallel_for_async` n<=0);
`Result_storage<R>` (`task.h:835`, used by `Executable` and `Task_promise`) is live and
different. Only the standalone `Result_block<R>` + `make_block()` pair is dead.
**Classification: definitely dead. Recommendation: delete both** (a `Task<R>` with a stored
result and no body is now always produced by the fused coroutine promise or a pre-settled
void sentinel). **Risk: low** — nothing references them.

### B7. `current_task`, `current_scope_children`, `detail::add_nested` — LIVE (graph-internal)
Per §4.3, `ts::nested`/`Task_scope`/`join_nested`/`with_inherited_access` are gone from user
code, but `detail::add_nested` survives as graph plumbing. Confirmed live and reached:
- `current_task` (`task.h:790`) — installed by `Executable::run`, the coroutine promise's
  `enter_segment`, and read by `add_nested`, `parallel_for` priority inheritance,
  `reentrant_under_held_grant`, etc. Core-live.
- `add_nested` (`task.h:1206`) — callers are the coroutine node frame gating
  (`coroutine_support.h` final-awaiter regime) and nested graph runs
  (`static_task_graph.cpp`). Tested (`graph_tests.cpp` "nested run *", 9 cases).
- `current_scope_children` (`task.h:796`) — narrow but live: written by `add_nested`, read by
  the graph's non-quiet-scope lending check; installed by the coroutine promise
  (`scope_children_`, `coroutine_support.h:409/441`). It exists solely for that one fatal
  (§4.3 tail); no user-reachable path, which is the intended end-state, not a leftover.

**Keep all three.** None is a retraction/builder remnant.

### B8. `submit_ready` bridge, `num_locks`/`execution_flag`, `on_complete`, `nested_parent`,
`prereq_cancelled`, `pipe_*` — LIVE
Spot-checked each against the brief's list:
- `submit_ready` (`guarded.cpp:96`) — the scheduler-free dispatch seam; universal. Live.
- `num_locks` + `execution_flag` — below-flag now counts **pipe turns only** (the
  prerequisite-counting regime was deleted, `task-internals.md` §4 historical note); above-flag
  counts nested children / the implicit scope. Both halves live (`release`,
  `Executable::run`, the coroutine promise). The three-way branch in `release` (`task.h:477-486`)
  is all reachable for pipe tasks.
- `on_complete` (`task.h:344`) — persistent graph completion hook (`graph_node_completed`).
  Live.
- `nested_parent` (`task.h:413`) — single-slot gating link written only by `add_nested`. Live.
- `prereq_cancelled` (`task.h:386`) — graph successor cancellation propagation. Live.
- `pipe_links` / `pipe_count` / `pipes_entered` — the pipe cascade. Live.

No dead members here beyond B2's `dispatch_arg`.

### B9. Dead includes — one candidate, low-confidence
`task.h` includes `<condition_variable>` (used: `done_cv`), `<vector>` (used:
`continuations`, `inline_pending`), `<functional>` (used: `std::move_only_function`) — all
live. I did **not** find an obviously-dead include in `task.h`. One to eyeball if doing a
cleanup pass: `task.h:3` includes `ts/access.h` "for grant inheritance for launched/nested
sub-work (`snapshot_access`)" — `snapshot_access` is still used by the coroutine promise and
`parallel_for`, so the include is live, but the *comment* rationale ("launched/nested") is
half-stale like A7. Not a code leftover; flagging for the same doc-sweep.

---

## Prioritized cut list — order and risk

1. **B6 — delete `make_block()` + `Result_block<R>`.** Definitely dead, zero callers,
   self-contained. **Risk: low.** Do first; it is free.
2. **A7 + B9 (doc sweep) — fix stale comments** in `ts.h:11` (`launch/nested`),
   `static_task_graph.h:83` (`Task_builder::set_inline`), `scheduler.h:111` (`reuse generation,
   owning pipe`), and the `task.h:3` include rationale. **Risk: none** (comments only).
   Also worth correcting the CLAUDE.md note about `Flags::pipe_job` / `dispatch_arg`-stamped
   `Pipe*`, which no longer matches the code.
3. **B2 (+ B4, B5 fall out of it) — retire the generation dimension.** Delete `dispatch_arg`,
   reduce `run_state` to a claim flag, drop the `gen` parameters, shrink `inline_pending`'s
   element and `reset()`'s bump. Saves 8 bytes/block and removes the last retraction/reuse
   substrate. **Risk: moderate** — touches the hot dispatch path across three dispatch routes;
   preserve the claim bit's acquire/release ordering and re-run the TSan graph/pipe stress.
   Correct as-is, so schedule it, don't rush it.

Everything else audited (Signal, launch, functor bodies, parallel_for, cancellation, the
options aggregates, `set_inline`/`inline_pending`, `add_nested` and the two TLS slots, the
pipe/lock-counter/on_complete machinery) is **live and coherent with the coroutine-first
end-state** — keep as-is.
