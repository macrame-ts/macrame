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
template<typename R, typename Body>          // spelled via auto at call sites
class Access_op                              // = Piped_executable<R,1> with automatic storage
{
    detail::Task_control_block core;         // refcounted INTERNALLY (pipe entries), no external handle
    /* result storage for R */
    Body body;
    detail::Pipe_link link;                  // embedded, as every pipe entry already is
};
```

- **Eager**: the constructor performs today's `access` fast path - reentrant
  arm, `pipe_try_inline`, else `pipe_enter_first`. No `start()`; "launch early,
  await later" is just naming the op early (eagerness is the overlap).
- **Pinned**: non-movable and non-copyable once constructed - the pipe's
  intrusive FIFO holds `&link` (same rationale as `Access_guard`, whose address
  is the installed context; the two converged independently).
- **Destruction contract**: destroying a started-but-unsettled op corrupts the
  pipe queue. `TS_SAFETY_CHECKS`: fatal with a named message ("await it
  first"). Shipping: follow `~Deferred`'s precedent - a conditional
  `wait`-until-settled safety net rather than UB. (Open question §9.2 on
  whether shipping should wait or also fast-fail.)

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
- `op.rearm()` - restart a *settled* op for the next frame: zero-alloc steady
  state for a per-frame access from a stable site (the dynamic-site analog of
  the graph's compile()-time node blocks and their allocation-free re-runs).
  Safe where executable-reuse was not: retraction and its generation machinery
  are gone; single-owner re-arm of a settled block is a small contract.
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
1. **`Access_op<R, Body>` + `access` returns it** (single object). The factory
   inversion in `guarded.h` (construct in caller storage; run reentrant/inline
   arms through it), the dtor contract, `sync()`-by-value, awaiter. Tests:
   settled/unsettled dtor death test, inline/queued/reentrant paths, coroutine
   and blue forms, cancellation. Migration: call sites that stored the old
   `Task<R>` break at compile - each is by definition a *leaver* - switch to
   `async` (census expected small; samples/tests adjusted in the same change).
2. **Guard awaiters embed their links** - hold-node allocation removed;
   `pipe_acquire` reduced to the link-enter protocol.
3. **Multi-object `Access_op`** (N embedded links through the same canonical
   cascade) - closes TODO 1.2 (multi-object inline arm) as a side effect.
4. **`rearm()` + member placement** - the SBO'd-body spelling for ops as
   members rides the `Function<Sig, N>` work (TODO 4.2); until then, member
   ops are `decltype`-typed or lambda-free.
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

## 9. Open questions (decide before phase 1 code)

1. Type spelling: `Access_op` vs `Pending_access` (users mostly write `auto`;
   pick once, it appears in diagnostics).
2. Shipping-config dtor policy: conditional wait (Deferred precedent) vs
   fast-fail; a waited dtor can deadlock a blue thread holding what the op
   needs - lean wait + document, but verify against the deadlock net.
3. Cancelled-value semantics on the op: mirror `Task` (`is_cancelled()` check,
   fatal on blind consume) or fold `as_optional` into `sync()`'s return -
   lean mirror-`Task` for consistency.
4. Whether `access` keeps accepting `Access_options{.token,.priority}`
   unchanged (lean yes; priority only matters on the queued path).
5. `TS_DEBUG_NAMES`/suspension-registry integration for ops parked on a pipe
   (they should appear in the tier-3 registry like any suspended waiter).
