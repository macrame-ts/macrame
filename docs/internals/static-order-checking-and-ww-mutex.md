# Two evaluations against the suspended-ABBA hole

Evaluation note (2026-08). Both items are *evaluations with compiled evidence*, not
implementations. Neither produced a production API change. Context: the residual deadlock
class after the coroutine-first transformation is **suspended ABBA** — task A holds grant G1
and awaits G2 while B holds G2 and awaits G1; no thread parks, both frames simply never
resume. Batch acquisition is already deadlock-free (canonical pipe-address order,
all-or-nothing); the hole is a grant held *then* a later dynamic await
([coroutine-first.md](coroutine-first.md) §2, §10.1).

Three candidate answers exist. One is **already shipped** and must frame both evaluations:

- **The circular-wait detector** (TODO 6.5) is implemented — `circular_wait_record` /
  `circular_wait_clear` in `src/guarded.cpp` (lines 372–492), called from
  `Task_awaiter::await_suspend` and `Access_awaiter::await_suspend` in
  `include/ts/coroutine_support.h`. It records {held pipe → awaited pipe} edges at a genuine
  suspension, DFS-checks for a cycle on insert, and fatals naming both tasks and both
  objects. `TS_SAFETY_CHECKS`-gated. Its weakness is the known one: an edge exists only while
  a task is suspended, so **both halves must be concurrently suspended** for the cycle to
  close — a scheduling coin-flip.
- **TODO 6.14's declared rank** — runtime, deterministic, prevents rather than detects.
- The two evaluated here: **§1 Clang `ACQUIRED_BEFORE`** (static complement to the rank) and
  **§2 the `ww_mutex` pattern** (recovery instead of prevention).

Verdicts up front: §1 **adopt-narrowly** (two macros and a guide recipe; it does work, but it
covers a subset of 6.14 at a higher annotation cost, and it found two defects worth knowing
about). §2 **reject as designed, record a narrower successor with a named trigger** — the
kernel's recipe presupposes revocable grants, which [coroutine-first.md](coroutine-first.md)
§10.4 already evaluated and parked on independent grounds.

---

## 1. Clang thread-safety analysis as a static complement

### 1.1 Toolchain reality (measured)

| toolchain | version | `ACQUIRED_BEFORE` |
|---|---|---|
| `clang-cl.exe`, VS 18 Community (`VC\Tools\Llvm\x64\bin`) — the project's Windows toolchain | **22.1.3** | on with plain `-Wthread-safety`, no beta flag |
| `clang++` in WSL (Ubuntu, `/usr/lib/llvm-21`) — the TSan host | **21.1.8** | requires `-Wthread-safety-beta` |
| Windows PATH `clang-cl` / `clang++` | not installed | — |

Every probe below was run on both and produced identical diagnostics, so the mechanism is
available on the toolchain that matters today (clang-cl 22) without an opt-in flag, and on the
TSan host with one. `-Wthread-safety` over the library's real headers is silent today
(`retry_probe.cpp` in §2.3 compiled clean with it), so turning the warning on costs nothing
before any annotation exists.

The C++ spelling is standard Clang: a `CAPABILITY("...")`-attributed type, `ACQUIRE`/`RELEASE`
on the acquiring/releasing functions, `REQUIRES` on a function that expects a capability held
on entry, `ACQUIRED_BEFORE`/`ACQUIRED_AFTER` on the *object declarations* to state an order.

### 1.2 What it can see (each with the compiler output)

**(a) A class template can be a capability.** `Guarded<T>` marked
`class CAPABILITY("guarded") Guarded` works with no special handling; the diagnostics name the
instance, not the instantiation.

**(b) It relates NAMED GLOBALS, with a forward declaration.** `ACQUIRED_BEFORE(g2)` needs `g2`
in scope, so a global order requires an `extern` declaration first — that is a real ergonomic
cost when the objects are declared far apart.

```
extern Guarded<Nav> g2;
Guarded<Physics> g1 ACQUIRED_BEFORE(g2);
Guarded<Nav> g2;
...
    g2.acquire();
    g1.acquire();   // OUT OF ORDER
```
```
p1.cpp:30:8: warning: guarded 'g1' must be acquired before 'g2' [-Wthread-safety-analysis]
   30 |     g1.acquire();   // OUT OF ORDER
```

**(c) It relates STRUCT MEMBERS, and the check fires through a `World&` reference.** This is
the finding that most changes the picture: the field survey's "named globals only" is too
pessimistic. An order declared on the members of a struct is enforced at every use site,
including uses reached through a reference parameter and through a lambda capture.

```
struct World
{
    Guarded<Nav> nav;
    Guarded<Physics> phys ACQUIRED_BEFORE(nav);
};

void member_order_bad(World& world)
{
    world.nav.acquire();
    world.phys.acquire();      // out of declared member order
```
```
p3.cpp:25:16: warning: guarded 'phys' must be acquired before 'nav' [-Wthread-safety-analysis]
```

The order is per-DECLARATION, not per-instance: `a.nav` then `b.phys` over two different
`World`s also warns. Under a rank discipline that is correct behaviour (a rank is global), and
it is what makes the mechanism a *static rank* rather than a per-object lock order — the same
semantics TODO 6.14 proposes at runtime.

**(d) The order relation is transitive, and holds across a `co_await`.** A declared chain
`a < b < c` diagnoses acquiring `a` while holding `c` (probe 11, `w1_trans`/`w3_trans`). The
analysis runs on the source-level CFG before the coroutine transformation, so a suspension
between the two acquisitions is invisible to it and the check still fires (probe 10,
`coro_nav_then_phys`).

**(e) Acquire and release may live in DIFFERENT functions**, provided both are annotated:
`void take() ACQUIRE(g1)` / `void give() RELEASE(g1)`. The order check sees through the
wrapper (probe 2, `split_order_bad`). An unannotated function that acquires and does not
release warns "still held at the end of function".

**(f) Variadic capability packs work.** `template <class... Gs> void multi_acquire(Gs&... gs)
ACQUIRE(gs...)` compiles, actually grants each element (probe 7, `pack_grants` is clean while
`pack_missing` warns on the ungranted object), and order-checks the pack.

**(g) `GUARDED_BY(this)` inside the capability class catches direct data touches** on a named
instance without a grant (probe 5, line 38).

**(h) The combination that matters — a node body annotated `REQUIRES(<declared set>)` plus a
`SCOPED_CAPABILITY` order token at the dynamic await — produces exactly the ABBA diagnostic
with zero false positives.** Probe 9, in full: two node bodies (as free functions and as
lambdas), a coroutine body, and a runtime-chosen target. Only the two genuine rank violations
fire.

```cpp
template <class T>
class SCOPED_CAPABILITY Awaited { public: explicit Awaited(Guarded<T>& g) ACQUIRE(g) {}
                                         ~Awaited() RELEASE() {} };
#define AWAIT_ACCESS(g) Awaited _acc(g)

void node_A(World& world) REQUIRES(world.phys) { AWAIT_ACCESS(world.nav);  }  // rank-increasing
void node_B(World& world) REQUIRES(world.nav)  { AWAIT_ACCESS(world.phys); }  // the ABBA half
```
```
p9.cpp(47,5): warning: guarded 'phys' must be acquired before 'nav' [-Wthread-safety-analysis]
p9.cpp(54,51): warning: guarded 'phys' must be acquired before 'nav' [-Wthread-safety-analysis]
2 warnings generated.
```
(clang-cl 22.1.3, `-Wthread-safety`, no beta flag. Line 54 is the lambda form; the coroutine
and the runtime-chosen-target cases in the same file are silent — see 1.3.)

### 1.3 What it cannot see

**(i) Aliases are not tracked — confirmed.** A reference bound to a global, a pointer
parameter, an escaped `T*` stored in a global, and a container element are all invisible:

```
void alias_globals()      { Guarded<Physics>& ref1 = g1; g2.acquire(); ref1.acquire(); }
void via_pointer(Guarded<Physics>* p)      { g2.acquire(); p->acquire(); }
void via_container(int i)                  { g2.acquire(); pool[i].acquire(); }
void leak_it()   { g1.acquire(); leaked = &g1.value; g1.release(); }
void use_leak()  { leaked->x = 3; }
```
None of these produced a diagnostic (probe 2, probe 5). Note the asymmetry with 1.2(c): a
*member expression* rooted at a reference (`world.phys`) resolves to a declaration and is
checked; a *reference or pointer to the Guarded itself* does not.

**(j) Function locals cannot carry an order at all** — a hard limitation, diagnosed:
```
p16.cpp:13:21: warning: 'acquired_before' attribute only applies to non-static data members
               and global variables [-Wignored-attributes]
```

**(k) Suspension is not a concept.** Holding a capability across a `co_await` draws no
diagnostic (probe 4, `held_across_suspend` and `scoped_across_suspend` are both silent). The
analysis therefore cannot enforce the library's "no grant across suspension" rule; the
existing `access_guard_depth` fatal remains the only enforcement.

**(l) Body-return is not completion.** A coroutine that acquires and reaches `co_return`
without releasing draws "still held at the end of function" (probe 4, `acquire_and_return`) —
which in this library is the *legal* shape (grants live until frame completion). Any modelling
of real grants has to suppress that.

**(m) The library's own machinery generates guaranteed false positives.** The acquisition loop
over a runtime list of pipes, and acquire-here/release-in-the-completion-callback, both warn:
```
p5.cpp:69:21: warning: expecting pipe 'link.pipe' to be held at start of each loop
p5.cpp:76:20: warning: releasing pipe 'link.pipe' that was not held
p5.cpp:85:1:  warning: pipe 'p' is still held at the end of function
p5.cpp:88:11: warning: releasing pipe 'held' that was not held
```
Standard remedy (what `absl::Mutex` does): `NO_THREAD_SAFETY_ANALYSIS` on the implementation,
checking retained at user call sites. Also note that annotating the *access verb* is
contagious: `void access(Fn fn) { fn(value); }` with `value GUARDED_BY(this)` warns inside the
template body for every instantiation (`-Wthread-safety-reference`), attributed to the user's
call site.

**(n) A batch acquire whose arguments are not in rank order warns** even though batch
acquisition is atomic and order-immune here (probe 7, `pack_batch_unordered`:
`multi_acquire(g3, g1)`). Users would have to write `ts::async(fn, a, b)` arguments in rank
order to stay quiet — a genuine false positive against this design.

### 1.4 Two defects found

**(o) `ACQUIRED_AFTER` is silently order-dependent — do not use it.** Its edge is registered
lazily, only once the annotated capability is itself acquired somewhere earlier in the
translation unit's analysis order. Identical code diagnoses or not depending on source
position:

```cpp
struct World { Guarded<P> phys; Guarded<N> nav ACQUIRED_AFTER(phys); };

void violation_first(World& w)  REQUIRES(w.nav) { Awaited a(w.phys); }   // NO warning
void touches_nav(World& w)                      { Awaited a(w.nav);  }
void violation_second(World& w) REQUIRES(w.nav) { Awaited a(w.phys); }   // same code, warns
```
```
p14.cpp:17:59: warning: guarded 'phys' must be acquired before 'nav' [-Wthread-safety-analysis]
1 warning generated.
```
The mirror test with `ACQUIRED_BEFORE` (probe 15) warns on **both**, so `ACQUIRED_BEFORE` is
the sound spelling. This is unfortunate: `ACQUIRED_AFTER` is the ergonomic one (name the
earlier object, already in scope, no forward declaration), and it silently does nothing.
Reproduced identically on clang 21 (beta) and clang-cl 22 (default-on).

**(p) There is no declaration-level cycle check** — contrary to TODO 6.14's "cycle-checks the
declarations themselves". A declared cycle (`gb ACQUIRED_BEFORE ga; ga ACQUIRED_BEFORE gb`)
compiles silently under `-Weverything`; the diagnostic appears only at a *use* that traverses
it (probe 17). The check is entirely use-driven, so an unexercised rank inversion in the
declarations is not caught. TODO 6.14's line should be corrected.

### 1.5 Coverage against the real codebase (measured)

Object inventory across the repo:

| where | count | annotatable? |
|---|---|---|
| `World` members in `sample/game_frame.cpp` | 38 (35 `Guarded`, 2 `Versioned`, 1 `Deferred`) | **yes** — 1.2(c) |
| function locals in `sample/physics.cpp` | 7 | no — 1.3(j) |
| function locals in `sample/blackboard.cpp` | 6 | no |
| function locals in `tests/` | ~240 | no |
| locals in `benchmarks/` | 15 | no |
| `std::array<Guarded<int>, N>` elements (3 test arrays + 1 bench array) | 104 | no — 1.3(i) |
| namespace-scope / static globals | **0** | — |

So: the survey's "named global `Guarded` objects only" is wrong in letter (0 exist, yet 38
objects are still coverable) and right in spirit (everything else is not). Coverage is
**38/410 ≈ 9% of all objects in the repo**, or **38/51 ≈ 75% of the samples**, which is the
number that represents engine-shaped usage — `game_frame`'s `World` is deliberately the
"real engine state" model, and it is exactly the annotatable shape. Test and benchmark locals
being uncoverable is a style artifact, but it means the mechanism cannot be exercised by the
suite without a fixture-struct refactor of ~240 declarations.

Reachability is not the obstacle it was assumed to be: all 42 `add_node` call sites and all 35
coroutine-variant `ts::async` sites spell their objects `world.<member>` through a `World&`
parameter, and 1.2(c) proves that form is checked. Three system functions take
`Guarded<T>&` parameters (`tick_streaming`, `tick_AI`, `tick_AI_prev`) — those hops are
invisible per 1.3(i).

### 1.6 The annotation burden, honestly

For the ABBA check specifically the analysis needs **both** halves:

1. The awaited side — cheap. The library can ship a `SCOPED_CAPABILITY` order token emitted by
   the same macro that spells the dynamic await.
2. **The held side — expensive and unautomatable.** A node body is a lambda handed to
   `add_node`; the analysis sees it as a standalone function holding nothing. Making it hold
   its declared set requires hand-writing `REQUIRES(world.phys, world.nav)` on each lambda —
   a manual duplicate of the object list `add_node` already receives. Attributes cannot be
   generated from a parameter pack, so this cannot be derived. For `game_frame` that is 42
   lambdas. If the duplicate drifts from the real declaration, the analysis silently checks
   the wrong thing, and nothing detects the drift.

Refactor fragility: moving a `Guarded` out of `World` into a local, into a container, or
behind a `Guarded<T>&` parameter silently removes it from coverage with no diagnostic. The
same move under 6.14's runtime rank keeps working.

### 1.7 Recommendation — **adopt-narrowly**

Ship the enabling annotations and a documented recipe; do not annotate the library's samples
or tests wholesale, and do not gate CI on it.

1. `TS_CAPABILITY` on `Guarded` / `Versioned` (macro; expands to nothing on MSVC, to
   `__attribute__((capability("guarded")))` on clang). Verified zero-cost and zero-noise: with
   `CAPABILITY` alone and no `GUARDED_BY`, no library-internal warnings appear.
2. `TS_ACQUIRED_BEFORE(...)` macro. Document `ACQUIRED_AFTER` as **unusable** (1.4(o)).
3. A guide recipe: the order token + `REQUIRES` on node bodies (1.2(h)), stated as an
   *optional static mirror* of the 6.14 rank for users whose guarded state lives in a struct
   or at namespace scope, with the four limits named (locals, containers, `Guarded&` hops,
   argument order in batch calls).

Do **not** position it as a replacement for 6.14. It covers a strict subset (members and
globals, and only where the held set is hand-declared), it has a silent-false-negative defect
(1.4(o)) and no declaration-level cycle check (1.4(p)), and it costs one hand-maintained
duplicate declaration per node. 6.14's runtime rank is O(1), fires deterministically on the
first offending await, works for locals, containers and reference hops alike, needs no
per-node duplication, and produces a message naming both objects. Order of work is therefore
6.14 first; §1 is the free static echo of the ranks 6.14 makes users write anyway.

Correction to fold into TODO 6.14 when it is implemented: strike "cycle-checks the
declarations themselves" (1.4(p)); replace "can cover named global `Guarded` state and not
much else" with "covers globals and struct members, including through reference parameters and
captures; not locals, containers, or `Guarded&` hops".

---

## 2. The `ww_mutex` pattern (wound-wait with a restartable acquisition phase)

### 2.1 The pattern

Linux `ww_mutex` (Documentation/locking/ww-mutex-design.rst) is the production precedent under
this library's constraints — dynamic, caller-ordered acquisition over non-rollbackable in-place
objects. Recipe: a per-acquire-context ticket (a global timestamp); on detecting a potential
deadlock a lock attempt returns `-EDEADLK`; the caller **unlocks everything and retries the
whole acquisition from scratch with the same ticket**. The retry is cheap because the
acquisition phase is side-effect-free — GEM's prologue locks buffer objects and walks their
relocation lists, mutating nothing. The ticket makes the retry starvation-free: an older
context always eventually wins.

### 2.2 What it would look like here — sketch

The body splits into an **acquisition prologue** (all dynamic awaits, no mutation) and an
**effect body** (mutation, no further acquisition):

```cpp
graph.add_node("resolve_contacts", [](Broadphase& bp, Contact_out& out) -> ts::Task<void>
{
    ts::Acquire_context acq;                      // one ticket for the whole attempt
    std::vector<Body_ref> targets;

    for (;;)                                      // the restartable prologue
    {
        targets.clear();
        auto held = ts::Held_set(acq);            // releases everything on scope exit
        for (auto id : bp.overlapping())          // target set discovered from held data
        {
            auto r = co_await held.acquire(world.bodies[id]);   // may yield -EDEADLK
            if (r == ts::Acquire::wounded)
                goto retry;                       // drop `held`, loop, same ticket
            targets.push_back(r.ref());
        }
        held.commit();                            // promote to the effect body's grants
        break;
    retry:;
    }

    for (auto& t : targets)                       // the effect body: mutation only
        t->integrate();
    co_return;
});
```

Three pieces do not exist: `Acquire_context` (ticket), `Held_set` (a movable, dynamically-sized
collection of grants that can be released as a unit), and a `pipe_acquire` variant that returns
"wounded" instead of queueing.

### 2.3 Blockers, verified against the code

**(a) "No mutation before the prologue ends" — enforceable further than assumed, but not
soundly.** The prompt's premise was that `TS_CHECK_ACCESS` sees a touch, not a write. It is
better than that: `ts::access_check` is *overloaded on `this`-const-ness*
(`include/ts/access.h:159–174`), so a non-const method demands `read_write` and a const method
demands `read_only`. A "prologue mode" TLS flag that fatals on any `access_check(T*)`
(non-const) call would be a real runtime tripwire at **method granularity**. What it still
misses: a non-const method that happens not to mutate (false positive), direct field writes
through the `T&` a node body receives (no check at all — the macro only fires where the user
put it: 9 sites in `game_frame.cpp`, 23 in `physics.cpp`, 16 in `blackboard.cpp`), and
anything reached through an escaped reference. So: documentation plus a partial runtime
tripwire, as suspected, with the tripwire sharper than expected.

**(b) "Coroutine frames are one-shot, so retry cannot re-enter the frame" — this objection is
wrong.** Restarting a prologue does not require re-entering a frame; it requires a *loop*, and
a coroutine body is an ordinary function body. Verified by compiling against the real library
with clang-cl 22:

```cpp
ts::Task<void> retry_loop(ts::Guarded<Cell>& a, ts::Guarded<Cell>& b)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        { auto ga = co_await ts::read_write(a); ga->set(attempt); }
        int seen = co_await b.access([](const Cell& c) { return c.get(); });
        if (seen == 0) co_return;
    }
    co_return;
}
```
compiles clean (`clang-cl /std:c++latest -Wthread-safety -I include`, no diagnostics). The
one-shot property (`initial_suspend` is `suspend_never`, no `reset`) is real but irrelevant to
this shape.

**(c) The real blocker: grants are not revocable, and the guard type cannot be collected.**
`ww_mutex`'s entire recipe is "unlock everything and retry". In this library:
- A node's declared grants are released only at node completion, by `advance_pipe_links`
  (`src/guarded.cpp:308`) from the settle path. There is no mid-body release API — no
  `release_early`, no revoke; `grep` over `include/` and `src/` finds neither.
- `Access_guard` is deliberately **non-copyable and non-movable** (address stability: it installs
  `current_access = &ctx_`, a member — `coroutine_support.h:353–408`). Held grants therefore
  cannot be collected into a `Held_set`. Verified:
  ```
  vector(865,27): error: no matching function for call to 'construct_at'
    ... std::allocator<ts::Access_guard<Cell, ts::Access::read_write>>
  ```
- And revoke is not an open question: [coroutine-first.md](coroutine-first.md) §10.4 already
  evaluated revoke-and-reacquire and **parked** it on four independent costs — the torn node
  view, structurally unrevocable grants (live scope children, nested-run lends), resume
  re-acquisition latency plus priority inversion, and a second "color" of await. So ww_mutex
  is downstream of a decision already taken, and its upside (dissolving suspended ABBA) is the
  same upside §10.4 judged insufficient.

**(d) Batch acquisition has fixed arity.** `async_build_modes` (`include/ts/guarded.h:690–730`)
is a variadic *template*: the pipe array, the mode array, the insertion sort and the link
binding are all `sizeof...(Ts)`-sized. There is no runtime-length batch acquire, so even the
non-ww version of the sketch ("acquire this computed set atomically in canonical order") is not
expressible today.

**(e) Wounding cannot preempt a running task.** Wound-wait requires the *victim* to abort. Our
victim is a task that has been running and mutating in place; there is nothing to roll back.
`ww_mutex` handles this by shrinking the restartable unit to the side-effect-free prologue —
but our held grants at the point of a dynamic await are the node's *declared* set, acquired
before the body started and already mutated through. So the only sound victim is a task still
inside its own prologue, which pushes the design toward (c) again.

**(f) The two guard-form ABBA shapes are already closed, so the hole is narrower than the
sketch assumes.** `Access_awaiter::await_suspend` fatals on `access_guard_depth > 0`
(`coroutine_support.h:454`), and so does `Task_awaiter::await_suspend` (line 175). A body
holding an RAII guard cannot await anything. The surviving hole is exactly: a **graph node's or
pipe job's** declared grants (no `Access_guard` involved, depth 0) plus a `co_await` on a pipe
job — which is what the shipped circular-wait detector watches. Worth recording: because that
fatal lives in `await_suspend`, `hold_then_await` above compiles and *runs fine whenever the
second pipe happens to be free* — the timing-luck coverage TODO 6.11 is about.

### 2.4 Rank vs `ww_mutex` vs the shipped detector

| | declared rank (6.14) | `ww_mutex` | circular-wait detector (shipped) |
|---|---|---|---|
| mode | prevents (rejects the program) | recovers (retries) | detects, then fatals |
| fires | deterministically, first offending await | only under actual contention | only when both halves are concurrently suspended |
| cost | one TLS max vs one field, O(1) | ticket + release-all + re-acquire per conflict | mutex + DFS on the cold suspension path |
| runtime-chosen targets | **cannot express** | handles natively | catches them, but only by luck of timing |
| correct programs rejected | yes (strict order) | no | no |
| needs revocable grants | no | **yes** | no |

The genuinely irreducible case for `ww_mutex` — and the one rank cannot express — is a target
set that is *data-dependent on data you must hold a grant to read*: pick an entity from a
spatial query, then touch that entity. Rank cannot express it because the target is not known
until after the holding grant is taken, and its rank may be anywhere in the order.

Two observations narrow that case sharply here:

1. If the target set can be computed from **snapshot** data (`Versioned`), or the outward
   effect can be **staged** (`Deferred`), the case dissolves — those are tiers 2 and 3 of the
   §2 hierarchy, and they are precisely what Bevy's `Commands` and Unity's `EntityCommandBuffer`
   exist for. The residue is "I must read the target's *current* state back within the same
   node", which staging cannot serve.
2. The pattern needs per-entity `Guarded` granularity to arise at all, and **the repo contains
   zero containers of `Guarded` objects outside four fixed-size test/benchmark arrays whose
   nodes are registered statically**. The library's documented granularity advice runs the
   other way (shard by double-buffering, stage, snapshot; whole-system objects with a compiled
   schedule). So the workload that would justify `ww_mutex` is not merely absent — it is
   currently anti-idiomatic.

If it does arrive, note that a cheaper answer than full `ww_mutex` exists and should be tried
first: **a runtime-length second batch acquisition, taken before any mutation, in canonical
pipe-address order, all-or-nothing** — i.e. lift the fixed-arity limit in (d) and require the
node to declare "I acquire a computed set at the top of my body". That is deadlock-free by the
same argument as `multi_acquire` **provided the node holds nothing else across it**, which is
the same prologue discipline `ww_mutex` demands, without tickets, wounding or retry. Only when
the set must be discovered *incrementally while already holding* — GEM's actual shape — does
the ticket become necessary.

### 2.5 Recommendation — **record for later, with a named trigger**

Do not build it. Record it in TODO 6 alongside 6.14 with this trigger, all three conditions
required:

1. A workload appears with **per-object `Guarded` granularity at entity scale** — a container
   or map of `Guarded` objects whose nodes are registered dynamically, not the fixed arrays
   the tests use.
2. Its target set is **discoverable only under a held grant** and cannot be precomputed from a
   `Versioned` snapshot.
3. The outward effect **cannot be staged through `Deferred`** because the node must read the
   target's current state back within the same body.

Before building `ww_mutex` proper, try the two cheaper steps in order: (i) runtime-length batch
acquisition with a no-mutation-before-it discipline (§2.4, needs only (d) lifted), and (ii) if
that is insufficient, revisit §10.4's revoke decision — `ww_mutex` cannot be built without it,
so the revoke question must be reopened first, on its own merits, with the demonstrating
workload §10.4 asked for in hand.

---

## 3. Actions

1. `[ ]` 6.14 first; §1.7's macros ride along with it (the ranks are written once, the
   attribute mirrors them).
2. `[ ]` Correct TODO 6.14's two factual lines per §1.4(p) and §1.5.
3. `[ ]` Record §2.5's trigger against TODO 6 as a sibling of 6.14; note that it is gated on
   reopening [coroutine-first.md](coroutine-first.md) §10.4.
4. `[ ]` Independent of both: TODO 6.11's hoist of the guard-across-suspension check from
   `await_suspend` to `await_ready` is confirmed load-bearing by §2.3(f) — the current
   placement lets `hold_then_await` pass whenever the target pipe is momentarily free.
