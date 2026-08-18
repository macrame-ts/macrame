# `Access_op`: caller-owned operation state for `access`

*Design of record for the zero-allocation `access` redesign (planned 2026-08-18,
from the mutex-comparison bench campaign and the API discussion it drove).
Read together with [coroutine-first.md](coroutine-first.md) (waiting rules) and
[pipe-rebase.md](pipe-rebase.md) §0 (pipe custody). Sections numbered.*

---

## 1. Motivation and evidence

`access` today allocates a `Piped_executable<R,1>` block on every call - before
the inline/reentrant arms even run (guarded.h: `make_access_block` precedes both
checks), because the returned `Task<R>` needs somewhere to hold the result,
completion state, body, and pipe link. Measured cost of that shape (2026-08
exploratory benches, since removed; numbers preserved):

- mechanism floor at W=0: **141.6 ns/op for `access` vs 53.3 ns for an
  uncontended `std::mutex`** - roughly half of the gap is the block
  malloc/free;
- a realistic composition (coroutine + two accesses per ~6.6 us task) carries a
  **~1.7x overhead floor** vs the mutex shape even uncontended.

The theoretical analysis (this session) reduced the problem to one sentence:
**the allocation is the price of detachment, not of `R`.** The block must own
itself only because the caller may drop the handle while the operation is
queued. If the caller keeps the operation state alive - stack, member, array,
coroutine frame - the allocation disappears for *any* `R`, with no SBO tiers.
This supersedes TODO 1.1's void/small-R/large-R tiering entirely: there are no
tiers when the caller owns the storage.

## 2. The model: attended vs detached

- **`access` = attended**: you are staying for the result. Returns the
  operation state itself - caller-owned, zero-alloc, eager.
- **`async` = detached**: the work outlives your interest (fire-and-forget,
  stored handles, dynamic collections, shared completions, cross-frame without
  a stable home). Returns `Task<R>` exactly as today; the heap block is the
  honest price of walking away, ideally paid to a pool/arena (TODO 4.x), not
  the global heap.

Verbs keep their names; the split sharpens from dispatch policy to ownership.
One teaching sentence: *`access` when you stay, `async` when you leave.*

## 3. The type

```cpp
template<typename T, typename Body>          // Body = the USER's decayed functor, verbatim;
class Access_op                              // R and the access mode are deduced from it
{
    detail::Task_control_block core;         // refcounted INTERNALLY (pipe entries), no external handle
    /* result storage for R = Accessor_result_t<Body, T> */
    Body body;                               // no library wrapper closure - see below
    detail::Pipe_link link;                  // embedded, as every pipe entry already is
    T* inst;                                 // flattened context: what today's access wraps in a
    /* epoch / rank capture */               // library-generated closure moves to plain members,
};                                           // and run() installs the Access_context structurally
```

The flattening is load-bearing for member storage (§10): today `access` wraps the
user's functor in a library-generated context-install closure, making the true
body type unnameable *even in principle*. With the wrapper flattened, both
template arguments are user-spellable: `ts::Access_op<World, Snapshot>`.

- **Eager**: the constructor performs today's `access` fast path - reentrant
  arm, `pipe_try_inline`, else `pipe_enter_first`. No `start()`; "launch early,
  await later" is just naming the op early (eagerness is the overlap).
- **Pinned**: non-movable and non-copyable once constructed - the pipe's
  intrusive FIFO holds `&link` (same rationale as `Access_guard`, whose address
  is the installed context; the two converged independently).
- **Destruction contract** (decided, §9.2; refined §10.1): destroying a
  started-but-unsettled op would corrupt the pipe queue. Outside a task the
  dtor waits until settled in every config, checked builds firing a
  `TS_ENSURE` first (the wait is the safety net, the ensure is the bug
  report); inside a task the wait cannot be offered (blue boundary - worker
  exhaustion, or an op on a pipe the task holds never settles), so it is
  fatal via `blocking_sync_diagnose`.

## 4. Custody: why the pipe does not change

Verified against `guarded.cpp` (2026-08-18): a queued entry holds a
**refcount on the owner block** (`pipe_enter_link` takes it; admission adopts
it via `Task_ptr keep(owner, Adopt_ref{})`); `fire_granted` never touches a
pipe after collection; links are embedded in the owner's allocation and never
heap-allocated by the pipe. Consequences:

- The pipe/admission/cascade code paths see a `Task_control_block*` exactly as
  today - **zero pipe-layer changes**. The op keeps `core` refcounted
  *internally*; the invariant "refs drained ⇔ settled" is what the dtor
  contract checks.
- The change is confined to: where the block lives (caller storage vs heap),
  and the fact that no `Task<R>` handle aliases it (the op *is* the handle).

**Implementation correction (2026-08-18, phase 1 as landed)**: the refcounted
custody above did not survive contact. With caller-owned storage the
machinery must hold *no* reference past continuation-fire - a fired
continuation resumes the awaiting coroutine, which may destroy the op (and
the frame containing it) before the settling stack unwinds, so a ref dropped
afterwards is a UAF and a drain-wait on the settler's own stack would
livelock. As landed: a `Flags::caller_owned` bit routes dispatch through the
existing borrowed raw-pointer queue path (two conditionals in `guarded.cpp`:
`pipe_enter_link` skips the entry ref, `fire_task_turn` defuses), and the
op's settle notifies under the lock (the pipe's `idle` teardown pattern) and
touches no member after continuations fire. Consequence: nested
completion-gating inside an op body (e.g. a nested graph `execute()`) is
fatal - that shape needs a self-owning block, use `async`.

## 5. API surface

- `co_await obj.access(fn)` - unchanged spelling. The awaitable temporary is
  materialized in the coroutine frame and the language guarantees it lives
  across the suspension - frame-resident op, zero alloc.
- `obj.access(fn).sync()` - unchanged spelling from blue. **Divergence
  decided here: `Access_op::sync()` returns `R` by value** (the op owns the
  storage and dies at the semicolon in the temporary form; returning `const R&`
  would manufacture a dangling-reference footgun `Task::sync()` does not have).
  `take()` is therefore not needed on the op; `Task<R>` keeps its own
  contract.
- Named form: `auto op = obj.access(fn); ... co_await op;` - eager start at
  the declaration, overlap for free.
- `op.start()` - the one firing verb, legal in exactly two states: never
  started (a dormant/unbound op, §10) or settled; fatal in flight. Firing a
  *settled* op re-enters the pipe from the same storage: zero-alloc steady
  state for a per-frame access from a stable site (the dynamic-site analog of
  the graph's compile()-time node blocks and their allocation-free re-runs).
  There is no separate re-fire verb (`rearm` was rejected: arming suggests
  preparing without firing, but the call fires) - first fire and per-frame
  refire are the same operation. Safe where executable-reuse was not:
  retraction and its generation machinery are gone; single-owner re-fire of a
  settled block is a small contract.
- Cancellation: token stored in `core` as today; awaiting a cancelled value op
  follows `Task`'s rules (check first / `as_optional` analog - §9.3).
- Fire-and-forget on an op is impossible *by construction* - that case is
  `async`, unchanged.

## 6. The scoped guards unify with this

`co_await ts::read_write(obj)` already keeps its awaiter in the coroutine
frame, but acquisition still heap-allocates `pipe_acquire`'s **hold node** (the
null-owner `Pipe_link`). Under this design the awaiter embeds the link
(the guard is a *body-less op* whose settle point is grant-acquired and whose
release is the guard's dtor) - the hold-node allocation disappears, spelling
unchanged. Multi-object guards embed N links, same as multi-object ops.

Deferred to a later phase, catalogued with its hazard: the eager acquire-op
(`request_write()` - turn prefetching, pattern-farming 2.29's shape). Real
capability, but an early-admitted grant held while the requester works
elsewhere stretches the object's exclusion window and needs rank/circular-wait
integration for pending requests. Not in v1.

## 7. What lands where (phases)

0. **Baseline metric** - add an uncontended `access` ns/op row to
   `features_bench` (the old floor number lived in a removed bench); acceptance
   for phase 1 is that row dropping to pipe-claim + context-install cost
   (target: within ~2-3x of a bare mutex, from ~6x).
   *Measured (2026-08-18)*: baseline 125.5 ns/op vs bare mutex ~13 (~9.4x);
   post-phase-1 54.9 ns/op (~4.4x). The malloc was only ~13 ns of the gap
   (LFH); the rest of the win came from cutting settled-path sync-wait
   machinery, the `current_task` refcount pair, and in-ctor-settle
   notification. The residual ~40 ns is the two pipe-mutex passes (admit +
   release), the claim CAS, and the context install - tail-chaining territory
   (TODO 1.18), not this design's.
1. **`Access_op<T, Body>` + `access` returns it** (single object), with the
   §3 flattening from the start (Body = the user's decayed functor; no library
   wrapper closure - retrofitting it later would churn the just-landed type).
   The factory inversion in `guarded.h` (construct in caller storage; run
   reentrant/inline arms through it), the dtor contract, `sync()`-by-value,
   awaiter. Tests:
   settled/unsettled dtor death test, inline/queued/reentrant paths, coroutine
   and blue forms, cancellation. Migration: call sites that stored the old
   `Task<R>` break at compile - each is by definition a *leaver* - switch to
   `async` (census expected small; samples/tests adjusted in the same change).
2. **Guard awaiters embed their links** - hold-node allocation removed;
   `pipe_acquire` reduced to the link-enter protocol.
3. **Multi-object `Access_op`** (N embedded links through the same canonical
   cascade) - closes TODO 1.2 (multi-object inline arm) as a side effect.
4. **The §10 lifecycle** - `start()` refire, default-ctor unbound, `bind()`,
   `ts::dormant`, the §10.1 state machine and its checks; the erased-body
   member spelling rides the `Function<Sig, N>` work (TODO 4.2).
5. **`async` rebased on the same machinery** - a heap-placed op with
   self-ownership; one code path, two ownership modes (pure refactor, no
   behavior change).

## 8. What this closes / touches

- Supersedes TODO 1.1 (zero-alloc tiering - no tiers needed) and delivers
  1.15(c)'s zero-cost-attended-path goal; closes 1.2 at phase 3.
- The `production_contention` lib bench's staged architecture is unaffected
  (staging never allocated per touch beyond the command); the *fine-grained*
  comparisons (composition floor, W=0 row) are where the numbers move.
- Two completion vocabularies (`Task` and `Access_op`) - the doc/guide carry
  the one-sentence rule from §2; `Signal`, `launch`, graph `done` handles,
  `Deferred::commit` all stay `Task`.

## 9. Open questions - all decided 2026-08-18

1. **Decided: `Access_op`.**
2. **Decided: the dtor waits in every config; checked builds additionally
   fire a `TS_ENSURE` before waiting** (an unsettled op at destruction is a
   latent bug worth reporting, but corrupting the pipe or fatalling over it
   buys nothing shipping's wait does not). Refined by §10.1: the wait applies
   outside a task only - inside a task it is fatal (blue boundary). Verify
   the wait against the deadlock net (a blue-thread dtor holding what the op
   needs).
3. **Decided: mirror `Task`** (`is_cancelled()` check, fatal on blind
   consume; `try_take`/`as_optional` analogs if demand appears).
4. **Decided: yes** - `Access_options{.token,.priority}` passes through
   unchanged.
5. **Decided: no per-op naming.** Identity = the `Guarded`'s `debug_name` +
   the `access` call site (the verb's defaulted `source_location`, the
   existing `Named` rule); ops parked on a pipe appear in the tier-3
   suspension registry under that identity.

## 10. Member storage (the never-allocating stored op)

Prerequisite: the §3 flattening (Body = the user's functor type). Three
declaration tiers, in recommended order:

1. **Named functor - the blessed member idiom.** Captures become members, the
   type is directly spellable, mutable cross-frame state has a home:

   ```cpp
   class Hud
   {
   public:
       explicit Hud(ts::Guarded<World>& world)
           : op_(world.access(Snapshot{ this }))   // eager: first run starts here; C++17
       {                                            // guaranteed elision constructs the
       }                                            // non-movable op in place in the member

       void frame()
       {
           hp_shown_ = op_.sync();                  // by value; or co_await from a coroutine
           op_.start();                             // fire next frame's read, same storage, no alloc
       }

   private:
       struct Snapshot
       {
           Hud* self;
           int operator()(const World& w) const { return w.player_hp; }
       };

       int hp_shown_ = 0;
       ts::Access_op<World, Snapshot> op_;          // no decltype anywhere
   };
   ```

2. **Named static lambda** (stateless bodies only): `static constexpr auto
   snapshot = [](const World& w) { ... };` +
   `ts::Access_op<World, decltype(snapshot)> op_;`. Trap to document: every
   lambda *expression* is a distinct type - declaration and initializer must
   reference the same named object; an inline `decltype([]{...})` in the
   member declaration can never be initialized. Captures force tier 1.

3. **Erased fixed-size body** (phase 4, rides TODO 4.2):
   `ts::Access_op<World, ts::Function<int(const World&), 32>>` - any capture
   up to N inline, oversize is a compile error (no silent heap fallback), one
   indirect call per run. For containers of ops and body-polymorphic APIs;
   tier 1 stays the cheaper default.

Lifecycle for members (decided 2026-08-18): three orthogonal steps -
**construct / bind / start** - with constructors as fused conveniences and
`start()` as the only verb that touches the pipe:

- `Access_op()` - default-constructed, **unbound**: no target, no body. For
  members whose target `Guarded` does not exist yet at owner construction.
  Body moves from a plain member into raw storage (placement-new + a bound
  flag - symmetric with the result storage the op already manages; no
  allocation, nothing added to the fire path).
- `op.bind(world, body)` - store the target + construct the body in place;
  does not fire.
- `Access_op(ts::dormant, world, body)` - bound-but-dormant in one expression
  (= default + bind); needed because a member init list cannot call `bind()`.
  (`ts::dormant`, not `defer` - avoids collision with `Deferred`.)
- `Access_op(world.access(body))` - the eager form: bind + `start()` in the
  constructor.
- `op.start()` - fire (§5): never-started or settled only, fatal in flight.

### 10.1 State machine

Four states - **unbound → dormant (bound) → in flight → settled** - plus a
*consumed* sub-flag on settled (`sync()`/`await_resume` move the result out).
Storage: a `bound` bit + a `started` bit; in-flight/settled live in the core.
The full operation matrix:

| op \ state | unbound | dormant | in flight | settled |
|---|---|---|---|---|
| destroy | trivial | destroy body, no check (a) | see dtor contract below | destroy R if unconsumed |
| `start()` | fatal | first fire | fatal | refire; discards unconsumed R (b) |
| `bind()` | ok | rebind ok (destroy old body) | fatal | rebind ok (c) |
| `sync()` / `co_await` | fatal - would hang forever | fatal - would hang forever | wait / suspend | consume; second consume is a checked fatal, mirroring `take()` |

**Dtor contract refined** (supersedes the §9.2 wording): the wait is itself a
blocking sync, so it obeys the blue boundary - **outside a task**: checked
builds fire `TS_ENSURE` then wait, shipping waits silently; **inside a task**:
fatal via the `blocking_sync_diagnose` seam (a worker parking on the wait
risks exhaustion deadlock, and an op targeting a pipe the current task holds
would never settle - the wait cannot be offered). The common coroutine
spellings never hit this: an awaited temporary and an awaited named op are
both settled before their dtor runs; only an *abandoned* in-task op does,
which is exactly a bug worth reporting.

The benign cases, decided as non-issues:

- (a) destroying bound-but-never-started destroys the body with no lost-work
  check - unlike `~Deferred`'s staged-command fatal, a dormant op is a
  declared *capability*, not a pending *effect*; never firing it loses
  nothing.
- (b) refiring over an unconsumed result destroys the old R first - legal and
  documented (`Task` results are droppable too; "skip a stale frame's read"
  is a legitimate steady state). `start()` clears the consumed flag.
- (c) rebind on settled is legal (settled ⇔ pipe refs drained, so
  retargeting to a different `Guarded` is safe) - the pooled/reused-op
  enabler.

Single-owner contract: `start`/`bind`/`sync`/destroy are not thread-safe
against each other - one owner, external synchronization if shared (the
`co_await` completion handshake is of course internally synchronized).

Op size as a member: core + link + body + R ~= 320-400 B - unremarkable, and
it replaces a per-frame heap block of the same magnitude.
