# Ordering Ambiguity: Bug Class, Optimization Freedom, or Both

*2026-07. Written to resolve the standing disagreement over TODO 2.2
("ambiguity detection"). Inputs: the original evidence review
([research-static-vs-dynamic.md](research-static-vs-dynamic.md) §3.6), the
deep dive ([research-deepdive.md](research-deepdive.md) §6, §4.5), and the
author's position: "ambiguity feels good because it provides optimisation
opportunity and flexibility." Paragraphs numbered §N.M. The conclusion (§5)
proposes a rescope, not a defense of the original framing — the author's
challenge is partly upheld.*

---

## 1. Definitions, and one formal frame

**§1.1 — A conflicting pair** is two nodes touching the same `Guarded`
instance where at least one writes. Safety requires they not run
concurrently; that much every layer of this library already guarantees
(pipes, derived edges). The question is only *which order* they run in when
nothing semantic pins it.

**§1.2 — "Ambiguity" in Bevy's sense** is a conflicting pair with *no
ordering constraint at all*: the executor may run A→B this frame and B→A the
next, decided by worker timing. The observed values differ per run — genuine
nondeterminism.

**§1.3 — Our graph never has that.** `compile()` derives an edge for every
conflicting pair, oriented by declaration index. There are no unordered
conflicts; order is total and reproducible. What we have instead is
**accidental orientation**: an edge whose *direction* nobody chose — it fell
out of the order in which two `add_node` lines happen to appear.

**§1.4 — The useful formal frame: disjunctive arcs.** Job-shop scheduling
models exactly this: operations sharing a machine get a *disjunctive arc
pair* — they must be ordered, either direction is feasible, and choosing
orientations is the scheduling decision. Mapped here: object = machine,
conflicting pair = disjunctive pair. The three regimes on the table are then:

- **Bevy**: orientation decided *at run time by arrival* — nondeterministic.
- **This library today**: orientation *fixed at compile by a tiebreak* —
  deterministic, but arbitrary.
- **The optimizer we want (TODO 2.4/2.5, discussion item 18)**: orientation
  as a *degree of freedom*, chosen to shorten the critical path — but only
  where legal.

The whole disagreement compresses into one question: **when is flipping an
orientation legal?** (§3.)

---

## 2. What concretely goes wrong (the missing failure detail)

**§2.1 — Bevy's bug class, for the record.** With runtime-decided
orientation, the same build produces different frames: transform jitter
(propagation vs. camera read racing in order), one-frame input lag appearing
intermittently, physics results differing per run, replay/lockstep
divergence, flaky tests. Bevy 0.10's release notes state it plainly ("If
your App has ambiguities, this can cause bugs"); their own engine carried
**370+ ambiguities** (296 in `ExtractSchedule` alone) until a CI gate froze
the count — evidence both that the bug class is real and that an optional
diagnostic rots. Unity's `[UpdateBefore]/[UpdateAfter]` constraint webs
across third-party packages are the same disease in another engine.

**§2.2 — Our failure mode is different: temporal, not per-run.** Because
our orientation is deterministic, none of §2.1 applies to a fixed binary —
replays, lockstep, and test stability hold. The author's instinct is
**upheld against the imported Bevy framing**. What remains is a
*maintainability* hazard: the orientation is load-bearing but invisible and
unratified. Concrete scenarios:

1. A refactor reorders two `add_node` calls — say `apply_damage` and
   `regen_health`, both writing `Guarded<Health>`. The diff shows moved
   lines, nothing semantic. The derived edge flips; entities that survived
   a frame now die (regen no longer lands before damage). No diagnostic
   exists to say "this order was carrying weight."
2. Two teams add writer nodes to the same store from different files or
   registration functions; orientation is decided by whichever registration
   runs first — effectively link/init order. It is stable until a build
   change reorders registration.
3. A middleware package that registers graph nodes is upgraded; its nodes'
   declaration positions shift relative to yours; observed values change
   with no change in your code.

In each case the system did nothing wrong by its contract — but the contract
("declaration order is the order") is implicit, undocumented, and nothing
marks which pairs depend on it.

**§2.3 — The third stake: optimizer legality.** Item 18's critical-path
optimizer wants to choose orientations (§1.4). If it flips
`apply_damage`/`regen_health` to shorten the frame, it silently changes
gameplay. If it refuses to flip anything, it loses the main lever a static
graph has over runtime arbitration. Without knowing *which pairs are
order-free*, the optimizer is either unsound or neutered.

---

## 3. The "ambiguity is opportunity" claim, examined

**§3.1 — Where the freedom is real.** Commutative pairs: two nodes appending
to different keys of a store; two accumulator updates (`+=` into
independent tallies); event emitters whose consumers sort by key; any pair
whose combined effect is order-independent by construction. For these,
orientation is pure scheduling slack — run whichever is ready, or whichever
the critical path prefers. The author's intuition describes exactly this
set, and the optimizer should own it.

**§3.2 — Where the freedom is illusory.** Non-commutative pairs
(damage/heal; impulse/teleport; write-config/read-config-and-act): both
orientations are *safe* — no race either way — but they compute different
frames. "Flexibility" here means letting a tiebreak (today) or a profiler
(tomorrow) choose your gameplay semantics. That is not an optimization
dimension; it is an unratified semantic decision.

**§3.3 — The system cannot tell §3.1 from §3.2.** Both look identical in
the access sets: same object, both write. Commutativity is semantic
knowledge only the author has. This is the crux the original research
stated abstractly and this document now states plainly: *detection exists
to route each pair to a human once*, because no analysis can classify them.

---

## 4. Synthesis: what 2.2 actually needs to be

**§4.1 — First, adopt the contract we already implement.** "Graph
declaration order is program order" — the STF stance (StarPU/OpenMP infer
edges from sequential submission order), and the same philosophy as the
pipe's FIFO contract (design.md §3: reordering a submission-ordered queue
changes values, so order is semantics). Under this contract there is no
"ambiguity" in our graph at all — every orientation is defined. It should
be written into guide.md as a contract, not left as an implementation
detail. This is the part of the original "ambiguity detection" framing that
dies: we are not detecting missing order; order is never missing.

**§4.2 — What remains is a three-part feature, rescoped:**

1. **Edge provenance** (already needed by the DOT dump and the optimizer):
   for every edge, *why* it exists — explicit / conflict-derived, object,
   modes, and whether its orientation came from user intent or the
   declaration tiebreak.
2. **A fragile-orientation lint**: at `compile()`, list conflict pairs
   whose orientation rests *only* on the tiebreak — no explicit edge, no
   annotation. Each is a one-time triage question to the author: this pair
   runs A→B because A is declared first; is that meaningful?
3. **A commutativity annotation** (`ambiguous_with` in Bevy's vocabulary;
   ours might be `commutes_with(a, b)` or object-level): the author's
   declaration that a pair is §3.1-class. It silences the lint *and* hands
   the optimizer a legal reorder lever (TODO 2.4c). This is the honest,
   durable form of "ambiguity is opportunity": freedom that is *declared*
   is usable; freedom that is accidental is a latent bug.

Triage outcomes per pair: add an explicit edge (order matters — now
ratified and refactor-proof), annotate commutative (order free — optimizer
may flip), or restructure (split the object / use `Versioned` — delete the
conflict entirely; the provenance names the object and modes, so the fix is
guided).

**§4.3 — Noise budget: why this won't be Bevy's 370.** Bevy's count came
from type-level access sets over thousands of systems. Our unit is the
object instance and our graphs are O(100) coarse nodes; the tiebreak-only
pair count in a real frame should be single digits (worth measuring on
`game_frame` as the first validation step — if it's 40, this design needs a
rethink). Triage is one-shot per pair and survives refactors (the
annotation names the pair, not positions).

**§4.4 — Default.** Lint on by default at Warn (info-level listing, never
error) — Bevy's rot lesson says opt-in diagnostics get ignored until the
codebase is saturated; our small expected counts make on-by-default cheap.
An `Error` mode can exist for teams that want ratification enforced
(CI-gating new unratified pairs, which is what Bevy ultimately built for
itself).

---

## 5. Recommendation

**§5.0 — Author decision (2026-07): PARKED.** The key point landed — hidden,
non-obvious dependencies between nodes are the real hazard, and yes it is
worse when the order can also change per frame (Bevy) than when it is merely
arbitrary-but-fixed (us). How much our determinism actually helps is still
unclear. The sticking point is the annotation cost: explicitly marking every
commutative *pair* (Bevy's `ambiguous_with`) looks like combinatorial
explosion. The mitigation is object-level annotation ("all writers of this
store commute" — §5.3.1), which is O(objects) not O(pairs), but it is not
obviously right either. Decision: **do nothing for now** — gather real usage
data (the tiebreak-only pair count on `game_frame`, §4.3, is still the cheap
first measurement) and revisit after more thought. Everything below is the
design-on-file, not scheduled work.

**§5.1 — Rescope TODO 2.2** from "ambiguity detection (Bevy-style)" to
**"conflict provenance + fragile-orientation lint + commutativity
annotation"**, and re-anchor it as a *prerequisite of the optimizer track*
(2.4/2.5, item 18) rather than a standalone safety feature:

- Provenance: P2, with 2.4 (the optimizer cannot classify edges without it;
  the DOT dump wants it anyway).
- Commutativity annotation: P2, designed together with 2.4c (it is the
  optimizer's legality input; useless alone).
- Lint: P3 (maintainability value only — real but not urgent; our
  determinism means nothing is broken today).
- Contract sentence ("declaration order is program order") into guide.md:
  P1-cheap, one paragraph, do with the next docs pass.

**§5.2 — What this concedes and what it keeps.** Conceded to the author:
the Bevy-derived "top user-facing failure mode" framing does not transfer —
our determinism removes the bug class the original research led with, and
unordered-conflict *freedom* is genuinely valuable once declared. Kept from
the research: the freedom must be *declared* to be usable (by the optimizer)
or *ratified* to be safe (against refactors); a pair-classification
mechanism is therefore still needed — but as optimization infrastructure
with a maintainability side benefit, not as a safety feature.

**§5.3 — Open questions for the author.**
1. Annotation granularity: per node pair, per object ("all writers of this
   store commute"), or per (object, mode) — object-level is terser, pair
   level is precise.
2. Should the lint live in `compile()` output or in the DOT/tooling layer
   (keeps the core silent; risks nobody seeing it)?
3. Ratify the §4.1 contract sentence for guide.md?
4. Validation: measure the tiebreak-only pair count on `game_frame` before
   committing to defaults (§4.3).
