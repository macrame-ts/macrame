# Escaped-Reference Hardening (T21 / TODO 1.13): Decision Document

*2026-07. Analysis only — no code changes ride this document. Companion to
[limits.md](limits.md) (the gap catalog this targets),
[research-deepdive.md](research-deepdive.md) §13 (the cross-system safety
comparison), and TODO 1.9 (the shelved `Granted<T>` design re-examined here).
Paragraphs numbered §N.M for discussion. Two author directives drive the
structure: (1/a) find real systems using a checked-handle approach and assess
how successful they actually were, against the stated doubt that "very little
prevents passing a `T&` extracted from `Granted<T>` to an existing method
taking `T&`, so `Granted<T>` becomes annoying noise"; (1/b) how a range
harness can concretely be done.*

---

## 1. The gap, restated

**§1.1 — What remains open.** After T1 (stale inherited grants fault) and T4
(blocking-sync diagnosed), the harness's remaining coverage gaps are
limits.md §2.1–2.4: uninstrumented methods, POD/methodless types, escaped raw
references (a body hands `T&` to a helper that stores it; later access never
re-enters an instrumented method), and sub-object escapes (interior
pointers/spans handed out by legitimate methods). All four share one shape:
**the access exists, but no instrumented checkpoint sits on its path.** Any
hardening therefore either adds checkpoints (wrappers, deeper
instrumentation) or removes the need for them (memory-level oracles).

---

## 2. Precedent research (directive 1/a): checked handles in the wild

**§2.1 — Chromium `raw_ptr<T>` / MiraclePtr (BackupRefPtr).** The largest
deployment of a validating pointer wrapper in existence: class/struct fields
across Chromium hold `raw_ptr<T>` instead of `T*`. Success is real and
measured: Google projects it protects roughly half of exploitable
use-after-free bugs in covered processes, at 4.5–6.5% browser-process memory
overhead on Windows and no measurable performance impact on 64-bit
(SecurityWeek/Google figures; the allocator reserves a hidden ref-count per
allocation). Two facts matter for us more than the numbers:

1. **Adoption was tool-enforced, not disciplinary.** A Clang plugin rejects
   raw `T*` fields; the "Big Rewrite" converted the codebase mechanically.
   Nobody was asked to remember to use the wrapper.
2. **The design concedes the extraction hole rather than fighting it.**
   `.get()` and implicit conversion hand out an unprotected `T*`, and local
   variables/parameters are deliberately *excluded* from the rewrite. The
   protection survives laundering because it is not a deref-time check at
   all: the allocation stays quarantined and poisoned *as long as any
   `raw_ptr<T>` to it exists*. The safety property is attached to the
   wrapper's existence, not to routing every access through it.

Lesson: the one wrapper deployment that worked at scale worked by (a)
compiler-enforcing adoption and (b) defining the guarantee so extraction
does not defeat it. A deref-checked wrapper whose guarantee dies on
extraction is precisely what Chromium chose *not* to build.

**§2.2 — Unity `NativeArray`/`NativeSlice` views.** The view struct embeds
the `AtomicSafetyHandle`; every indexer call checks it; version bumps
invalidate stale aliased views (research-deepdive.md §13.2). It is the
best-regarded safety feature in the engine space — and its structural
precondition is the one we lack: **the view is the only handle the API
produces.** Job code receives the `NativeArray` view by value; there is no
sanctioned `T&` to the storage at all, and getting a raw pointer requires an
explicit attribute (`[NativeDisableUnsafePtrRestriction]`) that turns
tracking off visibly. Even so, the escape hatches see routine use (the
whole-container conservatism pushes disjoint-range writers to them), and
Unity's own framing of the escapes is telling — "at least all unsafe usage
is declared, which means once you do get race conditions at least you can
easily search" (Joachim Ante). Enforcement is editor-only; player builds
carry none of it.

**§2.3 — `folly::Synchronized` / `LockedPtr` — the closest shape to
`Granted<T>`.** Access to the guarded object goes through a `LockedPtr`
proxy returned by `wlock()`/`rlock()`. Laundering is exactly as possible as
the author's doubt describes, and the documentation says so plainly: the
design "makes it difficult to access the data without holding the lock, but
not impossible — in particular you should never store a raw pointer or
reference to the internal data for longer than the lifetime of the
`LockedPtr`". It is nonetheless a success by broad adoption at Meta and in
open source — because its goals are the modest ones: bind data to its mutex
so *forgetting the lock entirely* is impossible, and make every escape
syntactically visible in review. Crucially, for `folly::Synchronized` the
proxy is the **only** sanctioned access path — there is no stronger
instrumented-method alternative it competes with.

**§2.4 — MSVC checked iterators / `_GLIBCXX_DEBUG`: validating handles at
STL scale.** The history is a controlled experiment in where validating
handles belong. VS2005 shipped checked iterators *on by default in release
builds* (`_SECURE_SCL=1`); the performance complaints were persistent
enough that VS2010 turned them off in release (`_ITERATOR_DEBUG_LEVEL=0`
default) while keeping full iterator debugging as the *debug* default
(`_ITERATOR_DEBUG_LEVEL=2`), where it has caught invalidation and
out-of-bounds bugs for two decades. Mixing levels across TUs is a hard
linker error — the ABI cost of a validating handle is real and permanent.
Lesson: the industry's settled position for validating handles is
**dev-build oracle, ship-build absent** — the same posture as
`TS_SAFETY_CHECKS`, and evidence that a checked handle earns its keep as a
diagnostic, not as shipped protection.

**§2.5 — Rust `Ref`/`RefMut` and lock guards: the enforced endpoint.** The
contrast case: a `Ref<T>`'s borrow cannot outlive the guard in safe code —
the lifetime system makes laundering a compile error, not a convention. That
is what full success looks like, and it requires the language. Two honest
footnotes from the empirical studies already in
[research-deepdive.md](research-deepdive.md) §13.3: the guard model
*created* a new failure class (implicit scope-end unlock → 30 of 59 studied
deadlocks were double-locks), and `unsafe` escapes at the ecosystem level
are where the real-world failures concentrated. Even the enforced endpoint
is not free.

**§2.6 — Adjacent: UE `TWeakObjectPtr`.** A validity-checked handle whose
deref path (`IsValid()`/`Get()`) hands back a raw `UObject*` — fully
launderable, yet successful for its modest goal: detecting staleness *at
the point you re-enter through the handle*. It never claims to police what
you do with the extracted pointer. Same law as folly: modest goal, only
idiomatic path, visible escapes.

**§2.7 — The success/noise law, formalized.** Across the five systems,
checked-handle approaches succeed exactly when at least one of these holds:

- **(a) Only-handle**: the wrapper is the sole sanctioned access path and
  extraction is unidiomatic/visible (Unity, folly, Rust, `TWeakObjectPtr`).
- **(b) Tool-enforced adoption**: a compiler/plugin makes non-use an error
  (Chromium, Rust).
- **(c) The guarantee survives extraction**: the property attaches to the
  wrapper's existence or the allocation, not the deref (Chromium).

And they become noise when none holds: opt-in adoption, idiomatic
laundering, and a stronger alternative path already present.

**§2.8 — Verdict on the author's doubt: validated, for our situation
specifically.** `Granted<T>` in this library sits in the noise quadrant on
all three axes: adoption would be opt-in (functors keep receiving `T&`
unless rewritten); laundering is one implicit conversion away *and
idiomatic* (every existing method takes `T&`); and instrumented methods —
which we already have — are strictly stronger for owned types (TODO 1.9's
own record says so). folly's identical shape succeeds only because it lacks
that competing stronger path and polices nothing beyond lock-binding.
The one thing T1 changed since the 1.9 "maybe never" verdict: a `Granted`
deref could now be epoch-validated, catching post-release use of a stashed
*handle*. That is a real but narrow gain — it still misses the stashed raw
`T&`, which is the actual gap under discussion — and §3's Option B catches
the stashed-handle case *and* the stashed-reference case without any new
user-facing type. Recommendation: **`Granted<T>` stays shelved**; the 1.9
entry gains a pointer here.

---

## 3. Range harness design (directive 1/b): how it can concretely be done

**§3.1 — Option A: range-widened grants (interval containment).**
Mechanism: an `Access_context` entry becomes an interval — `add(instance,
size, mode, epoch)` with `size = sizeof(T)` supplied by `Guarded<T>` at
every existing declaration site; `check(p)` replaces pointer equality with
containment (`base <= p < base + size`), still a linear scan over ≤8
entries, still nanoseconds (two comparisons instead of one). The epoch
staleness check rides unchanged on the interval entry.

What it buys — and this is larger than it first looks: **instrumenting
member types becomes possible.** Today, `TS_CHECK_ACCESS()` inside
`Bone::set()` *false-positives* under a `Guarded<Skeleton>` grant (the
`Bone*` is not the declared `Skeleton*`), so interior types simply cannot
carry the macro. With containment, a `Bone` method checks its `this`
against the skeleton's granted range and passes — so instrumentation can
go as deep as the user cares to push it, and an *escaped interior pointer*
that later re-enters any instrumented member method gets the full
treatment, including the T1 staleness fault if the grant window closed.
No new user-facing type; the one spelling rule is untouched.

Honest limits, in order of importance:

1. **Indirect storage is outside the interval.** A `std::vector` member's
   elements live on the heap, not in `[&obj, &obj + sizeof(T))`. Interval
   containment covers the object's direct bytes only. A declarable extent
   escape (`declare_extent(ptr, bytes)` under a write grant, registering
   additional intervals on the pipe, copied into contexts at grant capture)
   is *mechanically* feasible but honestly assessed poor: extents churn
   with container reallocation (stale registration = false negatives or
   worse), the 8-entry context cap and per-grant copy grow with extent
   count, and the bookkeeping lands on the user — exactly the kind of
   manual declaration the library exists to avoid. Defer unless a workload
   with a *stable* owned arena (fixed block, no reallocation) actually
   asks; that narrow case works fine.
2. **Nested `Guarded` members.** A `Guarded<Inner>` field inside a
   `Guarded<Outer>` sits inside Outer's interval, so an Outer grant would
   bless access to Inner's payload without Inner's pipe. Nesting guards is
   already architecturally incoherent (double arbitration); make it a
   documented anti-pattern, and note a cheap belt is possible later
   (`Guarded`'s constructor could assert no enclosing granted interval in
   debug — not v1).
3. False-match risk otherwise: none — intervals are exact object extents;
   unrelated allocations cannot overlap a live object's bytes.

Cost: one `std::size_t` per entry (dev-build-relevant fields, same gating
posture as T3's `graph_refs`), two-comparison check, `sizeof(T)` threading
through the existing builders. Small, contained, and it is the natural
implementation of TODO 1.6.

**§3.2 — Option B: grant-scoped ASan poisoning (the novel oracle).**
Mechanism: in ASan builds (`__SANITIZE_ADDRESS__` / MSVC
`/fsanitize=address`, which supports the manual-poisoning interface),
`Guarded<T>` poisons the object's bytes whenever **no grant is active** and
unpoisons them for the duration of grant windows. The hooks already exist
with exactly the right edges — the T1 epoch work identified every
acquire/release transition under the pipe mutex: unpoison on idle→held
(first reader in, or writer acquire), poison on held→idle (last reader
out, writer release, non-handoff node release); a graph handoff keeps the
object held, so nothing toggles. Construction order: poison at the tail of
`Guarded`'s constructor (after `T` is built), unpoison in the destructor
after `wait_until_idle()` and before `~T`.

What it catches that nothing else can: **any instruction touching the
bytes without a grant** — the stashed raw `T&` used from a free function,
POD field access, an uninstrumented method, a stale coroutine frame —
reported by ASan with the access stack and the poison stack, at
memory-level fidelity. This closes limits.md §2.1, §2.2, and §2.3 for the
object's direct bytes in ASan runs — and, decisively for this project, it
does so **on Windows**, where TSan does not exist and the harness is
otherwise the only oracle (design.md §8's platform-reality point). The
same indirect-storage limitation as Option A applies (heap members are
separate allocations; ASan's ordinary UAF checks still cover their
lifetimes, just not our grant discipline).

Costs and caveats, assessed:

- Poison/unpoison writes shadow memory at size/8 bytes per transition — a
  1 MB subsystem costs ~128 KB of shadow memset per grant edge. At
  per-node-per-frame grant frequency this is real but lives only in ASan
  builds (already ~2× slowdown); small objects are noise.
- `Versioned<T>` audit: the shadow replica is *not* a `Guarded`, so it is
  never poisoned (correct — it is grant-free by design); the front is
  touched only under pipe grants (`swap_replicas` under the write grant,
  resync as a pipe read job). Expected to need **no exemption** — to be
  verified first thing in the prototype.
- Legal non-grant windows: none exist by the model's own contract — that
  is precisely why the scheme is sound, and every violation it reports is
  a real one (no false-positive class identified beyond debugger/tooling
  memory inspection, which ASan already tolerates).
- Fully absent outside ASan builds (macros compile to nothing); gate
  additionally on `TS_SAFETY_CHECKS` for symmetry.

**§3.3 — Option C: page protection — rejected on numbers.** Toggling
`VirtualProtect`/`mprotect` per grant window costs a syscall plus a TLB
shootdown (IPIs to every core) — microseconds — against grant windows whose
management currently costs tens of nanoseconds, and it forces page-aligned
(4 KB) allocation per object. That is the design point of external
full-page-heap debugging tools, not of an always-on-in-dev harness. No
further consideration.

---

## 4. Catch matrix

Rows = limits.md gap classes; columns = status quo vs candidates. "inst."
= only when the access path re-enters an instrumented method.

| Gap | Status quo (harness+T1+T4, TSan on Linux) | A: intervals | B: ASan poisoning | `Granted<T>` |
|---|---|---|---|---|
| §2.1 uninstrumented methods | TSan only | no change | **yes** (direct bytes) | no |
| §2.2 POD / methodless types | TSan only | no change | **yes** | partial (handle deref only) |
| §2.3 escaped raw `T&`, used later | TSan only | inst. only (then epoch-checked) | **yes** | no — launderable |
| §2.4 interior pointers / sub-objects | none | **yes** via member instrumentation; indirect storage excluded | yes (direct bytes); same exclusion | no |
| Stashed *handle* reused post-release | T1 (contexts) | extended to interior entries | yes | yes (epoch-validated deref) |
| §3.1 semantic races | none — out of scope for any of these | — | — | — |
| Platform reality | TSan absent on Windows | Windows-native | **Windows-native** | Windows-native |

---

## 5. Recommendation and sequencing

**§5.1 — Do:** (1) **Option A as the implementation of TODO 1.6** — small,
no new user-facing type, unlocks member-type instrumentation that today
false-positives, and extends T1 staleness to interior re-entry. (2)
**Prototype Option B behind the existing ASan config** (TODO 10.2's proper
ASan build story is its natural home) — it is the only candidate that
catches the stashed-raw-reference class at all, and it does it on the
platform where no other race-adjacent oracle exists. Validate the
`Versioned` no-exemption expectation and measure the shadow-write cost on
`game_frame` before promoting it.

**§5.2 — Don't:** `Granted<T>` remains shelved (§2.8 — the noise quadrant
on all three axes of §2.7's law; Option B dominates its epoch-deref gain).
Option C rejected. The declarable-extent escape for indirect storage stays
unbuilt until a stable-arena workload asks.

**§5.3 — What stays TSan's job** (and should keep being advertised as
such, per limits.md §4.1): cross-object semantic races and anything
touching indirect storage under our grant discipline on Linux CI.

---

## 6. Open questions for the author

**Resolution (author, 2026-07): everything shelved.** ASan poisoning (Option
B) judged interesting but too limited for now — maybe in the future; Option A
shelved with it; `Granted<T>` stays declined. The questions below are moot
until a revisit; kept for the record.

1. Option A entry layout: grow the existing `Entry` (one `size_t`,
   simplest) or a parallel gated array (keeps checked/unchecked layouts
   identical — same trade as T3's fully-gated `graph_refs` vs T1's
   unconditional epoch field; pick one convention for all three)?
2. Option B gating: ASan-build-only, or also a `Scheduler_config`/compile
   flag so ASan users can opt out of grant-poisoning independently?
3. Nested-`Guarded`-member rule (§3.1.2): documented anti-pattern only, or
   also the debug-time constructor assert?
4. Sequencing: A before B (small win first) or B-prototype first (bigger
   unknown, informs whether A's instrumented-only coverage still matters)?

---

## Sources (precedent research)

- Chromium: [Use-after-freedom: MiraclePtr](https://security.googleblog.com/2022/09/use-after-freedom-miracleptr.html) ·
  [raw_ptr.md](https://chromium.googlesource.com/chromium/src/+/HEAD/base/memory/raw_ptr.md)
  (clang-plugin enforcement; extraction concession; exclusion lists) ·
  [SecurityWeek coverage](https://www.securityweek.com/google-improves-chrome-protections-against-use-after-free-bug-exploitation/)
  (~50% of exploitable UAFs; 4.5–6.5%/3.5–5% memory overhead).
- folly: [Synchronized.md](https://github.com/facebook/folly/blob/main/folly/docs/Synchronized.md)
  ("difficult ... but not impossible; never store a raw pointer or
  reference ... longer than the lifetime of the LockedPtr").
- MSVC: [_ITERATOR_DEBUG_LEVEL](https://learn.microsoft.com/en-us/cpp/standard-library/iterator-debug-level) ·
  [_SECURE_SCL](https://learn.microsoft.com/en-us/cpp/standard-library/secure-scl)
  (VS2005 checked-in-release default reversed in VS2010; debug default
  IDL=2; ABI mismatch is a linker error).
- Unity: research-deepdive.md §13.2's primary sources (AtomicSafetyHandle
  docs, JobSystemTroubleshooting, the Ante "Jobs system safety" thread).
- Rust: research-deepdive.md §13.3's studies (PLDI 2020; RustSec).
