# Limits of the Model: What the Harness and Access Declarations Do Not Catch

*Draft, 2026-07. Working document — to be refined and folded into guide.md
(a "known limits" section) later. The honest catalog of where
access-declared concurrency and the runtime harness stop helping, so a user
knows exactly what guarantee they are and are not buying. Sourced from the
cross-system comparison in [research-deepdive.md](research-deepdive.md) §13,
which found every comparable system's real-world failures concentrated in
exactly these gaps. Paragraphs numbered §N.M.*

---

## 1. The one-sentence guarantee, stated precisely

**§1.1 — What the harness actually promises.** When `TS_SAFETY_CHECKS` is
on, an instrumented method of a `Guarded<T>` faults if the running task does
not hold a matching grant for that object. That is: it catches *undeclared
access to an instrumented object through an instrumented method, on a path
that actually executes in a checked build*. Every word is load-bearing, and
every word is also a boundary — the sections below walk each one.

**§1.2 — What it is not.** It is not a proof, not a compile-time guarantee,
and not a total race detector. It is a **sampling oracle**: it fires only
when a violating access happens to run under instrumentation. Rust's borrow
checker is the total, compile-time form of this property; ThreadSanitizer
(no Windows runtime) is the total, runtime form; this harness is the cheap,
always-on-in-dev, incomplete form. The design trade is deliberate
(design.md §7.9): ~1 ns/check and zero constraint on ordinary C++, in
exchange for a weaker guarantee. Knowing where the weakness bites is the
point of this document.

**§1.3 — What is always correct regardless.** The *scheduling* is sound even
where the harness is blind: derived edges and pipe arbitration order
declared conflicts correctly whether or not the harness ever fires. The
harness is a diagnostic for *declaration completeness*, not a load-bearing
part of correctness — an undeclared access is a bug the harness helps you
find, not something the scheduler relied on. This matters: turning
`TS_SAFETY_CHECKS` off does not make correct programs incorrect; it removes
the net that catches *incorrectly-declared* ones.

---

## 2. Coverage gaps — access the harness cannot see

Ordered from most obvious to most subtle.

**§2.1 — Uninstrumented methods.** The harness fires only from methods that
call `TS_CHECK_ACCESS()`. A public method missing the macro is a silent
hole: access through it is invisible. Mitigation on the roadmap: a
clang-tidy check that every public method of a guarded type opens with the
macro (TODO 1.4). Until then, it is convention, and convention is exactly
what the completeness hazard warns about.

**§2.2 — POD and methodless types.** `Guarded<SomePod>` has no methods to
instrument, so direct field access declares nothing and is never checked.
The planned answers are a value-mediating `Guarded_value<T>` or the
sub-object range harness (TODO 1.4/1.6); the shelved `Granted<T>` wrapper
(TODO 1.9) is the other candidate. Today: a POD under `Guarded` is
effectively unchecked.

**§2.3 — Escaped raw references (the sharpest gap).** A task body legally
receives `T&`, hands it to a helper that *stores* the pointer, and later
code accesses the object through that stored pointer — via a free function,
a cached field, another thread — never re-entering an instrumented method.
The harness sees nothing. This is the canonical way to defeat the model
accidentally: pass a guarded resource to a function that no longer knows it
is guarded, and it can innocently stash and use it. Unity partially catches
the analogue only because its container *views carry their safety state* (a
version-stamped handle inside the view struct — a stale escaped view fails
its next check); we do not, today (TODO 1.13 investigates importing that
idea). The fundamental floor: raw-`T&` extraction can always launder the
check away — only a language (Rust) closes this completely.

**§2.4 — Sub-object escapes.** A method legitimately returns a span or
pointer *into* the object (`&vec[0]`, a view); subsequent access through
that goes around the object's methods entirely. The declarable `Region<T>` /
range harness (TODO 1.6) is the designed answer; today the grant covers the
object, not the handed-out interior, so interior access after the method
returns is unchecked.

**§2.5 — Stale inherited grants (CLOSED, 2026-07).** This was a real gap:
`ts::launch` snapshots the launcher's grant set with no validity window, so
a non-nested task could run after its origin scope released the objects,
harness silent. Fixed (TODO 1.11): every context entry declared under a
pipe grant captures the pipe's write-epoch; a snapshot that outlives its
grant window fails the comparison at the next instrumented access and
faults with a stale-grant diagnostic. Residual within this item: the check
fires at *instrumented* access only (§2.1–2.4 still apply), and detection
of a window closing mid-access is advisory (an epoch read races the close
by nature — the harness is diagnostics, not arbitration).

**§2.6 — Shipping builds.** `TS_SAFETY_CHECKS=0` removes the oracle
entirely. Every comparable system shares this (Unity is editor-only, RDG is
Debug/Development-only, Chromium's checks are DCHECK-gated), and the
documented industry consequence is the "works in the editor, corrupts in
shipping" bug report. The harness is cheap enough (~1 ns/check) that leaving
it on in shipping is *possible* for many titles — but that is a per-title,
per-workload measured decision, not a default (a hot trivial getter called
millions of times a frame is a real tax; design.md §8).

---

## 3. Failures no access-declaration system can catch — including this one

These are not gaps in *our* implementation; they are outside what
declared-access concurrency can express at all. Rust's empirical studies are
the evidence: even with a total, compile-time soundness guarantee, these
survive.

**§3.1 — Semantic races: correctly declared, correctly ordered, still
wrong.** The declaration says *which* object and *what mode*; it cannot say
whether your *sequence of accesses* is the one you meant. Classic shapes:

- **Check-then-act across two accesses.** Read a version in one declared
  read, then write in a later declared write assuming the value still holds.
  Both accesses are individually safe and declared; the logic races in the
  gap between them. (Rust's own studies found this class thriving in safe
  code — a two-lock TOCTOU where each lock was correctly taken.)
- **Lost update.** Two writers each read-modify-write the same object in
  correctly-serialized turns, but the design needed one to see the other's
  result and the ordering put them the wrong way round.
- **Order-sensitive gameplay.** `apply_damage` and `regen_health` both
  declare a write to `Guarded<Health>` — every orientation is race-free, but
  they compute different frames. Which one runs first is a *semantic*
  decision the access sets cannot carry (see
  [ordering-ambiguity.md](ordering-ambiguity.md)).

The harness verifies you *declared* the access; it has no model of what the
right *answer* is. The pipe's FIFO at least makes cross-access reasoning
deterministic (submission order is stable), which is the most the mechanism
can offer here.

**§3.2 — Deadlock via misuse.** Blocking `sync()` inside a graph node or
access body, or a wait cycle across `Signal`s (A waits on a signal B will
trigger, B waits on A), is a liveness bug, not a data race — the harness
does not model it. Retraction and caller-participation make the *sanctioned*
patterns deadlock-free (design.md §4.4), and blocking `sync()` under an
active grant is now diagnosed (TODO 1.12, shipped 2026-07: a `TS_ENSURE`
failure — debugger break + report, once per site — with a sharp message for
the certain-deadlock same-object shape), but a determined misuse can still
hang. Rust's studies are again
instructive: its ownership model *introduced* a new deadlock idiom (implicit
scope-end unlock → double-lock), so a safety model removing one class can
add another.

**§3.3 — Incomplete declarations (the completeness hazard itself).** The
whole model rests on declarations being *complete*. An undeclared touch of
an object the graph believes it owns races — and parallelization can
*promote* a latent bug into a live one by manufacturing concurrency around
it. The harness is precisely the runtime oracle for this (it exists to catch
the incomplete declaration), but only within its coverage (§2): the object
must be instrumented and reached through a checked method. The render-graph
world has the exact same hazard one level down — a missed resource
declaration silently reads stale GPU memory instead of erroring.

---

## 4. What to reach for when the harness cannot help

**§4.1 — The complementary oracle: ThreadSanitizer.** TSan catches what
declaration systems structurally cannot — escaped references, uninstrumented
access, context-less threads — because it tracks *actual memory accesses*,
not declarations, at 5–15× slowdown. It has no Windows runtime, so it
verifies the library's own machinery on Linux CI rather than a
Windows-hosted game's code. The healthy posture (Uber runs Go's race
detector this way — periodic sweeps, not production enforcement): the
harness is the always-on first line; TSan is the periodic deep check on any
platform that can run it. Advertise both as the safety story, not the
harness alone.

**§4.2 — Structural avoidance beats detection.** Several gaps are better
designed out than checked: split state along concurrent seams so there is no
conflict to mis-declare (`Versioned<T>`, the sample's double-buffered
transforms); route results through declared `Guarded` state rather than
escaped references; keep guarded objects' interiors behind methods rather
than handing out spans. The model rewards not needing the harness.

**§4.3 — The honest summary for users.** The harness catches the mistake you
are *most* likely to make by accident — forgetting to declare an access, or
declaring the wrong mode — cheaply and in development, on the platform where
no other race oracle runs. It does not catch a reference you smuggled out, a
method you forgot to instrument, or a sequence of correctly-declared
accesses that computes the wrong answer. That is a real and useful guarantee
with a clearly drawn edge — which is more than "you're on your own," and
less than a proof.

---

## 5. Cross-reference

- Full gap catalog with the comparison table:
  [research-deepdive.md](research-deepdive.md) §13.6.
- Ordering/semantic-order specifics:
  [ordering-ambiguity.md](ordering-ambiguity.md).
- The design position (why runtime oracle over compile-time proof):
  design.md §2.2, §7.9.
- Roadmap items that shrink these gaps: TODO 1.4 (completeness clang-tidy),
  1.6 (range harness), 1.11 (grant-generation), 1.12 (blocking-sync check),
  1.13 (escaped-ref hardening).
