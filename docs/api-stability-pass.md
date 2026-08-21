<!-- Pre-1.0 API decision record. Working document: the going-public checklist item
     "API-stability pass". Numbers are stable IDs - mark items done, never renumber. -->

# API-stability pass: pre-1.0 decisions

The public surface is cheap to change now and expensive to change after `v0.1.0`
is tagged. This records what an audit of `include/ts/**` found, split into what
can just be done and what needs an author decision.

**The gate:** anything that changes a *field type*, an *overload set*, a
*return type*, or a *symbol's visibility* must land before the tag or not at
all. Additive changes can trail.

Section 2 items are ready to implement as described. Section 3 items each carry
options and a recommendation; reply by number. Section 4 is agreed post-1.0.

---

## 1. The four originally-listed inconsistencies

The list in `TODO.md` "Inconsistencies to resolve" predates the `Access_op` and
`create_scheduler` reworks. Re-checked against the headers:

1. **`run_inline` on `access`** - resolved as written (no such field), but
   returned in new spelling as `Access_options::queued`. See **M2**.
2. **Multi-object `access` semantics** - open, and wider than recorded: since
   the rework the single- and multi-object forms differ in return type and
   ownership, not just dispatch. See **M1**.
3. **Stale `async`/`access` comments** - reopened by both reworks. See **S3**.
4. **`pipe` vs `queue` terminology** - the recorded decision ("queue in the user
   docs, pipe in the deep docs") was never implemented and is now arguably
   backwards. See **S9**.

Two more from that list, for completeness: the reservation path still heap-boxes
one closure per held-grant acquisition (`detail/pipe.h`, `pipe_acquire`'s
`std::move_only_function` parameter) - the graph and multi-object paths use
embedded links, so only `co_await ts::read_write(obj)` pays it; and the
safety-field gating and entity-naming items are resolved.

---

## 2. Recommended, no decision needed

Each is a defect with one sensible fix. Listed briefly; ask if any needs
expanding before implementation.

| # | item | fix |
|---|---|---|
| **M3** | `ts::Recorder<T>` / `ts::Parallel_recorder<T>` are public API living in `include/ts/detail/journal.h` (outside `namespace detail`) | move to a new public `include/ts/recorder.h`, include it from `deferred.h`/`versioned.h` so no user include breaks, add to `ts.h`. Source-compatible. |
| **M4** | `Access_guard`/`Multi_access_guard` have public constructors taking `detail::Pipe&` | make them private, friend the awaiters. `detail::Pipe` then appears in no public signature - which is what the header already does for the awaiter types themselves. |
| **M5** | `set_ensure_handler` / `ensure_failure_count` are inside `#if TS_SAFETY_CHECKS`, so a host that installs an ensure handler compiles in Debug and **fails to compile in Shipping** | declare both unconditionally; compile no-op definitions when checks are off. Public API shape must not depend on build config. |
| **M7** | `Task::sync()` is not ref-qualified, so `const R& r = obj.async(fn).sync();` dangles - while the same spelling on `Access_op` is safe (`sync() &&` returns by value) | add the rvalue overload: `sync() &` keeps returning `const R&`, `sync() &&` returns `R` by value. Mirrors `Access_op` exactly. |
| **M9** | `[[nodiscard]]` is on exactly one function, yet dropping a `Deferred::commit` or `Versioned::publish` handle is a **checked fatal later** in the destructor | mark `commit`, `publish`, `Frame_gate::next`, `async_parallel_for`. Deliberately not `async` (fire-and-forget is sanctioned) nor `access` (discarding an `Access_op` is a defined wait). State that policy once in `task.h`. |
| **S1** | `Versioned` has no `Rank` constructor, so its front can never satisfy `Rule::access_rank` - a user awaiting it while holding anything has no fix available | add `Versioned(Named, Rank, ...)` forwarding the rank to the front `Guarded`. |
| **S3** | stale header comments (item 3 above) | fix the enumerated sites. Highest value: `versioned.h`'s documented `add_node` example **does not compile** (missing the leading `Named`); `guarded.h`'s "`{token, priority}`; deliberately no run-inline knob" is false on both halves; `global_scheduler()` is declared twice with each site pointing at the other. Also refresh the corresponding `CLAUDE.md` bullet. |
| **S4** | `Access_op` has `try_take()` but no `as_optional()`, so the cancellation-tolerant await is missing on the verb most likely to carry a token | add it, completing the `sync`/`take`/`try_take`/`as_optional` vocabulary on both handle types. |
| **S5** | `Event_bus::dispatch_fn()` returns an unspellable lambda while `ts::publish_fn(v)` returns a nameable `Publish_fn<T>` - so a user cannot store a dispatch body as a member | give it a named return type, mirroring `Publish_fn<T>`. |
| **S8** | unqualified `uint32_t` / `size_t` in `scheduler.h`'s public aggregate | qualify with `std::`. |
| **S11** | two code-style violations shipped in headers: padding-space alignment in `parallel_for.h`, and `--` used as a dash in a `static_assert` message in `static_task_graph.h` (the parallel message in `guarded.h` correctly uses `-`) | fix both. |
| **S12** | `Access_op::take()` fatals on a second consume; `Task::take()` silently returns a moved-from object | add the same checked fatal to `Task::take()`. |
| **S13** | `ts.h` claims to be "the whole public API" but omits `frame_gate.h` with no in-file explanation; `version.h` is macro-only | add a one-line comment naming the exclusion and why; add `ts::version_major/minor/patch` constants beside the macros. |

**A1 - decided (author, 2026-08-21): keep the uniform rule.** `Guarded`/`Versioned`
require a spelled `ts::Named`; a bare literal stays a compile error. The audit
found seven non-compiling examples in the public docs caused by this, all now
fixed to `ts::Named{"..."}`. No code change.

---

## 3. Needs your decision

### M1 - the multi-object `access` surface

**Defect.** `g.access(fn)` returns a caller-owned, pinned, allocation-free
`Access_op<T, Body>`. `ts::access(fn, objs...)` forwards verbatim to `ts::async`
and returns a heap-allocated `Task<R>`. The free form accepts one object, so:

```cpp
auto a = numbers.access(fn);          // Access_op<T, Body> - caller-owned, no alloc
auto b = ts::access(fn, numbers);     // Task<R>            - heap block, detached
```

Two spellings of the same single-object operation, with different return type,
ownership model, dispatch, and allocation behaviour. A user who writes
`ts::access(fn, a, b).try_take()` gets an error pointing nowhere useful.

**Options.**

- **(a) Delete the multi-object `ts::access` overloads for 0.1.0** (recommended).
  Packs go through `ts::async`; the *attended* multi-object case already has a
  better spelling in `co_await ts::read_write(a, b)`. Also constrain the free
  `ts::async` pack to two-or-more objects so there is exactly one spelling of
  single-object async. Re-introducing `ts::access` later, returning a genuine
  multi-object `Access_op`, is then purely additive.
- **(b) Implement the multi-object inline path** so `ts::access` returns a real
  `Access_op` over N pipes. This is the principled end state but it is real
  design work (N embedded links in a caller-owned op, plus an inline arm in the
  cascade), not 0.1.0-sized.
- **(c) Ship as is** and document that multi-object `access` is `async`. Locks
  in the divergence.

**Blast radius of (a):** the overloads in `guarded.h`; a small number of call
sites in tests; guide/design/coroutine-first/task-internals mentions; the
`CLAUDE.md` bullet.

---

### M2 - `Access_options::queued` is inert on most surfaces

**Defect.** `queued` is a run-inline knob in negated spelling. It is honoured on
`Guarded::access` only. It is silently ignored by multi-object `ts::access`/
`ts::async`, `Guarded::async`, `Deferred::commit`, and `Versioned::publish`
(which also drops `opts.name`). For `Guarded::async` the ignore is at least
semantically consistent - it always enqueues. For the others it is a lie.

Silently-inert option fields are unfixable after 1.0 without a *behaviour*
change rather than an API change.

**Options.**

- **(a) Document precisely + assert** (recommended, and cheap): state on the
  field which verbs read it, and `TS_ENSURE` when it is set on a surface that
  cannot honour it. If **M1(a)** lands, packs are `async`-only and `queued` is
  vacuously true there, which shrinks the problem to `commit`/`publish`.
- **(b) Honour it everywhere** - meaningful only where an inline arm exists;
  `commit` already auto-dispatches on grant ownership, so `queued` would have to
  interact with that ladder. More design than it looks.
- **(c) Remove `queued`** and let `access` be unconditionally opportunistic,
  reintroducing the knob only if a real use case appears.

Related: **S9** renames the field if you keep it.

---

### M6 - `Access_options::name` / `Launch_options::name` are `const char*`

**Defect.** Every other named entity takes a `ts::Named`. Because these two take
a bare pointer, a multi-object `access`/`async` builds `Named{nullptr}` with no
call-site fallback - so an unnamed multi-object task is **nameless**, while every
other verb falls back to its call site. The cause is that a defaulted
`source_location` cannot follow a parameter pack, which a `Named` field solves.

```cpp
// now
struct Access_options { Cancellation_token token{}; Priority priority = Priority::normal;
                        const char* name = nullptr; bool queued = false; };
// proposed
struct Access_options { Cancellation_token token{}; Priority priority = Priority::normal;
                        Named name = Named(nullptr); bool queued = false; };
```

A literal still works (`Named(const char*)` is implicit), and `{.name = ts::Named{}}`
now captures the **user's** site. Cost: `Named` is three words instead of one
pointer in two by-value option structs.

**Recommendation:** do it. It is the last piece of the "one debug-identity type"
rule and a field-type change, so it is tag-gated.

---

### M8 - `Scheduler`'s public surface is mostly not user API

**Defect.** Publishing 0.1.0 locks all of this as a compatibility promise:

- `submit(Task_func_ptr, void*, Priority)` and the `Task_func_ptr` alias - the
  raw dispatch seam.
- `worker_idle()` / `worker_busy()` - **mutators any user can call**, corrupting
  the quiescence counter the deadlock net reads.
- The entire `TS_PROFILING` block (`arm_busy_tracking`, `add_body_ticks`,
  `add_owner_busy`, `read_bucket_busy`, `util_bucket_count`, …).
- `extern thread_local int ts::current_worker_index` - public mutable TLS.

**Recommendation:** move the profiling block and the worker hooks to private,
friending `detail::Worker_thread` (already a friend) and exposing a
`detail::Scheduler_profiling` accessor for the `tools/` bridges - the same
`detail::Guarded_access` idiom the library already uses. Keep public: `submit`,
`worker_count`, `single_threaded`, `quiescent`. Consider moving
`current_worker_index` to `detail::` behind a public `int ts::worker_index()`.

**Cost:** roughly half a day, mostly mechanical, but it touches
`tools/graph_trace.h`, `trace_stamps.h`, `trace_owner.h`, `src/scheduler.cpp`,
`src/worker_thread.cpp`. Decide whether the churn is worth it before the tag -
it cannot be done cheaply after.

---

### M10 - `Event_bus`'s defaulted `Named` captures the library header

**Defect.** `explicit Event_bus(Named name = {})` (`event_bus.h:261`). A default argument is
evaluated at its *declaration* site, so `Named`'s own defaulted `source_location::current()`
resolves to that line in `event_bus.h` - never to the user's construction site. Every unnamed
bus in a program reports the same identity.

This is precisely the failure mode `named.h` states as the load-bearing rule: "an inner helper
with its own defaulted parameter captures the library header instead, and the name is
worthless". `tests/named_tests.cpp` asserts that captured names point into the test file and
calls itself "the whole guarantee" - and it does not cover `Event_bus`, which is why the one
type that breaks the rule is also the one type the test does not check.

**Options.**

- **(a) Require the name**, as `Guarded` and `Versioned` already do -
  `explicit Event_bus(Named)`. One keystroke per construction site, and the naming rule
  becomes uniform across every named entity kind. Breaking for existing call sites.
- **(b) Keep the default, capture correctly** - take
  `std::source_location site = std::source_location::current()` on the ctor itself and build
  the `Named` from it. Source-compatible; unnamed buses start reporting their real site.

Either way, add an `Event_bus` case to `named_tests.cpp`.

**Recommendation:** (a), for uniformity with the other required-name types - unless you would
rather not touch call sites, in which case (b) fixes the identity without an API change.

### S2 - priority inheritance is inconsistent between the option structs

**Defect.** `Parallel_options::priority` is `std::optional<Priority>` meaning
*inherit the calling task's priority*. `Access_options::priority` and
`Launch_options::priority` are plain, defaulting to `normal`. So inside a
`high`-priority node:

```cpp
ts::parallel_for(...);          // slices run high   - inherited
obj.async(fn);                  // runs normal       - silently demoted
```

That difference is nowhere documented as intentional.

**Options.**
- **(a) Align on inheritance**: make both `std::optional<Priority>` with the same
  resolve-else-`normal` rule. Designated-initializer call sites are unaffected.
- **(b) Keep the difference** (defensible: a detached task is not the caller's
  work) and document it on all three structs.

Either way the field *type* is the breaking part, so decide before the tag.

---

### S6 - three names for worker count

`Scheduler_config::num_threads`, `Scheduler::worker_count()`,
`Parallel_options::concurrency`, plus prose "scheduler width" and "executors".
The docs and the whole trace vocabulary say **workers**.

**Options.** (a) rename both fields to the worker vocabulary; (b) rename only
`num_threads` (a *thread* count that is really a *worker* count is the actively
misleading one); (c) leave it.

Field renames are breaking. Blast radius: every `Scheduler_config{...}` site in
tests, samples, benchmarks, tsan, plus guide and quickstart.

---

### S7 - `async_parallel_for` naming and cancellation

Two smaller calls in one place. It is the only `async_` *prefix* in the library
(elsewhere `async` is a verb), while the sibling suffix convention exists in
`parallel_for_colored`. And `Parallel_options` carries no `token`, making
`async_parallel_for` the only `Task`-returning verb with no cancellation path.
The blocking `parallel_for` not taking one is defensible; the async one is not.

**Recommendation:** add the token; decide the name separately.

---

### S9 - ratify pipe vs queue, then make the API agree

The recorded decision was "queue in README and user guide, pipe in the deep
docs". Reality: the guide *defines* "pipe" as a first-class term and uses it
throughout, and the headers use it heavily. The only place a **user types** the
concept is `Access_options::queued` - the one identifier, in the other
vocabulary.

**Recommendation:** keep **pipe** as the term (it is already load-bearing
everywhere) and rename the field into that vocabulary, e.g.:

```cpp
obj.access(fn, {.enqueue = true});           // "enqueue on the object's pipe"
// or, inverted so the default reads as the behaviour:
obj.access(fn, {.inline_when_free = false});
```

Then update the `TODO.md` entry to record the reversal. Breaking, so tag-gated.
Interacts with **M2** (if `queued` is removed there, this closes with it).

---

### S10 - `Graph_node::priority()` naming

`after`/`before`/`priority(Priority)` are bare while `set_inline()` carries
`set_`, and every other setter in the library (`set_trace`, `set_copy`,
`set_divergence_check`, `set_release_priority`) uses `set_`. `priority()` also
reads like a getter and collides conceptually with the `priority` *field* on
three option structs. Rename to `set_priority()`? Breaking, cheap, tag-gated.

---

### A2 - the exceptions configuration does not reach consumers - DONE (2026-08-21)

**Resolved differently from the options below: the configuration stopped being
something a consumer has to match.** macrame contains no `throw`/`try`/`catch`,
so nothing forced it to be built exceptions-off; what did was the assumption
that a body might throw into library frames. That is now a stated contract with
a checked boundary - `detail::invoke_user_body` and the coroutine promise report
an escaping exception through `escaped_exception_diagnose` and abort - and the
library builds either way, with exceptions ON by default so a consumer's own
translation units are free.

What remains genuinely whole-program is MSVC's `_HAS_EXCEPTIONS=0`, which
rewrites STL declarations. It is now tied to one option,
`MACRAME_NO_EXCEPTIONS` (default OFF), exported as a usage requirement when on
(with `/EHs-c-` / `-fno-exceptions` where a flag can express it), and covered by
a `detect_mismatch` tripwire in `access.h` - verified: a mixed link fails with
`LNK2038: mismatch detected for '_HAS_EXCEPTIONS'`. Option (b), the `#error`
guard, was rejected on the way: it would have made the consumer's exception
setting the library's business, which is the thing this item was complaining
about. Option (a) landed inside the new option; `tests/consumer` dropped its
hand-copied flags, and the consumer CI job now also builds against an
exceptions-off package. Rationale: `docs/design.md` §4.6; user contract:
`docs/guide.md` §10.5.

The original write-up follows.

**Defect.** The library was exceptions-off, and `CMakeLists.txt` stated that
config consistency is load-bearing ("a consumer's own TUs must compile with the
SAME values the library was built with, or the layouts diverge"). But only *half*
the configuration is exported:

- `_HAS_EXCEPTIONS=0` is a **PUBLIC** compile definition - it propagates.
- `/EHs-c-` (clang-cl) and `-fno-exceptions` (Linux) are applied by
  `macrame_private_flags(...)` as **PRIVATE** - they do not.
- The `/EHsc` strip is a `string(REPLACE ...)` on `CMAKE_CXX_FLAGS`, a
  directory-scope variable that no consumer ever sees.

So a downstream user who writes the two obvious lines:

```cmake
find_package(macrame CONFIG REQUIRED)
target_link_libraries(app PRIVATE macrame::macrame)
```

compiles their translation units **with exceptions enabled**, against a library
built with them disabled.

**Why the consumer CI job does not catch this.** `tests/consumer/CMakeLists.txt`
hand-copies the library's private setup - it strips `/EHsc` itself and adds
`/EHs-c-` itself, with the comment "these mirror the library's private build
setup". The job proves `find_package` works only because the test does the work
the package should be doing.

**Why it matters.** This library is header-heavy: `Task<R>`, `Guarded<T>`,
`Access_op`, the awaiters and the whole block layer are templates and inline
functions instantiated in *both* the library's TUs and the consumer's. Under
`-fno-exceptions` versus not, the compiler generates different code for the same
inline function (cleanup paths, implicit terminate calls). Both TUs emit weak
symbols for the same instantiation and the linker keeps one arbitrarily - the
textbook ODR violation, and exactly the class the existing `detect_mismatch`
tripwire was added to prevent for `TS_SAFETY_CHECKS`. On Windows there is a
second wrinkle: the consumer gets `_HAS_EXCEPTIONS=0`, an MSVC-internal macro
that reshapes the STL's throw paths, while still compiling with `/EHsc`.

**Options.**

- **(a) Export the flags as usage requirements.** The natural CMake answer, but
  it does not fully work here: MSVC needs `/EHsc` *removed*, which cannot be
  expressed as a target property, and the code comment records that passing an
  explicit `/EHs-c-` to `cl` makes `<type_traits>` mis-specialize `invoke_result`
  for the sample lambdas. So this covers clang-cl and Linux but not `cl`.
- **(b) A compile-time guard in a public header** (recommended). Turns a silent
  latent ODR hazard into a clear error naming the fix:

  ```cpp
  #if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  #error "macrame is built with exceptions disabled; compile your TUs the same way \
  (-fno-exceptions, or strip /EHsc on MSVC / add /EHs-c- on clang-cl)."
  #endif
  ```

  The library's own build defines neither macro, so it stays silent internally
  and fires exactly on the mismatch. Same philosophy as the existing link
  tripwire, one compile earlier.
- **(c) Ship a helper** from the package config, e.g.
  `macrame_configure_consumer(target)`, doing what the consumer test does by
  hand. Works on all three toolchains but is non-idiomatic and easy to forget.
- **(d) Document only.**

**Recommendation: (b) + (a) + fix the consumer test.** The guard is the airtight
part; exporting the flags where it *is* expressible removes the friction for
clang-cl and Linux users; and once either lands, `tests/consumer` should drop
its hand-copied flags so the job actually tests the package rather than the
workaround.

---

## 4. Deferred to post-1.0

| # | item | why it can wait |
|---|---|---|
| **D1** | multi-object inline fast path (`Access_op` over N pipes) | real design work; **M1(a)** makes deferring it free - the surface simply does not exist in 0.1.0 |
| **D2** | mixed-mode multi-object held guard, `co_await ts::read_write(a, as_read_only(b))` | additive; the callback form covers it today |
| **D3** | enforcing `Versioned::state()`'s read-only contract | `state()` hands out a mutable `Guarded<T>&`, so a direct write compiles and silently breaks the replica invariant. The clean fix is a distinct front handle type exposing only what `add_node`/`read` need - nontrivial graph-declaration plumbing. Document the hazard louder meanwhile |
| **D4** | trimming `Access_op`'s lifecycle surface (`Dormant`/`bind`/`start`/refire) | five states and four verbs for a pooled-op case with no in-tree user, but it is deliberately designed; removing surface later is easier than adding it |
| **D5** | remaining `detail::` types in rvalue-only public signatures | opaque in practice. One cheap exception worth pulling forward: a `ts::Task_awaiter` alias for symmetry with `ts::Access_awaiter` |
| **D6** | `Access_scope` / `Access_context::add` allow forging a grant | needs a documented position ("this is the extension point for instrumenting your own types"), not an API change |

---

## 5. Sequencing

**Tag-gated** (field/overload shape, or visibility): M1, M2, M3, M4, M5, M6, M8,
S2, S6, S9, S10. These land before `v0.1.0` or not at all.

**Additive but grouped with them:** M7, M9, M10, S1, S4, S5, S7, S12. M7 changes
the meaning of existing valid code, so it belongs in the same batch even though
it is technically additive.

**Free, any time:** S3, S8, S11, S13. (A2 is done.)

Two are worth taking regardless of the rest: **M7** (a silent dangling reference
in the most-typed spelling in the library) and **M5** (Shipping does not compile
for a host that uses the ensure hook).
