# Rule policy — escape hatches and shipping defaults for the waiting-rule checks

Design of record for [TODO](TODO.md) 6.15. Implemented in `include/ts/rules.h`.

The waiting rules ([coroutine-first.md](coroutine-first.md) §2) say what a task may suspend
on while it holds a grant. Each rule is enforced by a runtime check that fatals. A user can
uphold a rule by means the library cannot see — an external lock discipline, a phase
invariant, a platform guarantee — in which case our check is a false positive and there must
be a way out. Symmetrically, a shipping build should be able to decide which checks it keeps
paying for. This document settles both, and the obligations a new check inherits.

## 1. Two axes, not one

Every rule check answers two independent questions:

1. **Is it compiled into this build?** — `TS_ENABLED_RULES`, a preprocessor bitmask. When a
   rule's bit is clear, its code *and its state* vanish (the `Pipe_guard` depth counter, the
   awaiters' `recorded_` flags, the circular-wait registry).
2. **Is it enforced at this point?** — `ts::Relaxed_scope`, a scoped RAII opt-out, plus a
   process-wide default for teams that want the rules as advice.

Axis 1 is the answer to "the guard-across-suspension fatal is always-on today with no way to
disable it", which is the observation that opened 6.15. Axis 2 is the answer to "our check is
a false positive". They are genuinely different needs and neither substitutes for the other.

## 2. The rule inventory

| `ts::Rule` | forbids | class | shipping default | `Relaxed_scope` |
|---|---|---|---|---|
| `in_task_sync` | `sync()` / `take()` inside a task | advisory | compiled out | yes |
| `await_under_guard` | `co_await` while a `Pipe_guard` is live | structural | **kept** | no |
| `access_rank` | awaiting an object out of declared rank order (6.14) | advisory | compiled out | yes |
| `circular_wait` | a held-grant → awaited-pipe wait cycle (6.5) | advisory | compiled out | yes |
| `deadlock_net` | quiescence with no possible external wakeup (6.13) | net | compiled out | no (global) |

`TS_ENABLED_RULES` defaults to `TS_RULE_ALL` under `TS_SAFETY_CHECKS=1` and to
`TS_RULE_AWAIT_UNDER_GUARD` under `TS_SAFETY_CHECKS=0` — which is exactly what each
configuration did before the policy existed, so adopting it changes no shipped behavior.

Not every rule can be honoured in every configuration. `circular_wait` reads grant
bookkeeping (`Access_context` entry epochs, `Pipe::write_epoch`) that exists only under
`TS_SAFETY_CHECKS`; asking for it in a shipping build cannot work, so the *effective* mask
(`TS_RULES_EFFECTIVE`) is the requested policy intersected with what the build supports, and
everything downstream reads the effective mask.

## 3. Why "suppressible" is per-rule — the finding that shaped this

6.15 assumed a uniform scoped opt-out. It cannot be uniform, and `await_under_guard` is the
counter-example that shows why.

Relaxing that rule does not merely accept a program the library disapproves of. A
`Pipe_guard` installs `detail::current_access = &ctx_` (a member of the guard) and saves the
previous pointer. If the frame suspends under a live guard and resumes on another worker:

- the resume path installs the promise's creation-time access snapshot over the guard's own
  context, so the guard's grant is no longer the installed one — the object the guard
  nominally protects fails the harness;
- `~Pipe_guard` then restores a `current_access` pointer that was captured on the *original*
  thread, writing a foreign stack address into the resuming thread's thread-local.

So the rule protects an invariant the **implementation** relies on, not a hazard the caller
might know to be absent. There is no claim a user could make that would render the relaxation
sound. It is compile-out-only, and its escape is the sanctioned form — the functor verb
`co_await obj.access(fn)`, or splitting the scope (release, await, re-acquire).

That gives the taxonomy, which is the answer to 6.15's "flat enum, or classes?" question —
**both**: a flat enum of rules (stable bits, individually switchable) plus three classes
expressed as named masks over it.

- **advisory** — the check encodes a hazard the caller may know is absent. Scoped opt-out
  and compile-out.
- **structural** — the check protects an implementation invariant. Compile-out only.
- **net** — a global check with no call site to scope (§7). Compile-out only.

`Relaxed_scope` accepts advisory rules; passing anything else is a diagnosed no-op for those
bits (`TS_ENSURE`, so it reports once per site and the program continues fully checked).
`set_default_relaxed_rules` masks the same way.

## 4. The scoped opt-out and how far it reaches

```cpp
{
    ts::Relaxed_scope relax{ ts::Rule::in_task_sync };
    // ... the claim is documented at the site; the rest of the program stays checked
}
```

`Rule` is a flags enum (`|`, `&`, `~`), so one scope can relax several rules.

**Reach.** The opt-out follows the ambient *task* state, not the thread — the same rule
grants already follow:

- **Across suspensions.** A coroutine's promise carries the frame's relaxation
  (`detail::Relaxed_carrier`) and re-installs it at every `enter_segment`, writing back
  whatever the segment ended with at `exit_segment`. A `Relaxed_scope` opened in a coroutine
  body is therefore still in effect after the body resumes on another worker, and does not
  leak onto that worker. Restoring a saved *value* is thread-agnostic, which is what makes
  this correct where `Pipe_guard`'s saved *pointer* is not (§3).
- **Into launched sub-work.** `with_inherited_access` captures the launcher's relaxation
  alongside its grant snapshot, so `ts::launch` / `ts::nested` children run under it.

The second is wider than the lexical scope suggests, and that is deliberate: a child inherits
the grant, therefore it inherits the hazard the opt-out speaks for. The asymmetry — grants
inherited but the claim about them not — would be the more surprising design.

## 5. The global default

`ts::set_default_relaxed_rules(Rule)` OR-s into every lookup, process-wide. It exists for
teams that want the advisory rules as advice and are not going to annotate call sites; it is
the blunt instrument, and the scoped form is the one to reach for. Read relaxed, so a change
is not ordered against tasks already running — set it once at startup.

## 6. Compile-out policy

`TS_ENABLED_RULES` is an OR of `TS_RULE_*` bits, defined once per binary. Like
`TS_SAFETY_CHECKS` it changes inline function bodies and the coroutine promise's layout, so a
mixed-configuration link is an ODR violation; it carries the same best-effort tripwire
(`#pragma detect_mismatch` on MSVC). Note the tripwire compares token text, so two spellings
of the same value trip it — define the macro in one place, as a build system does.

Because the bits are preprocessor values, a disabled rule leaves nothing behind: no branch,
no thread-local, no per-awaiter field.

## 7. The quiescence net is not locally suppressible

The global deadlock detector (6.13, implemented) fires on a process-wide condition — every
worker idle, queues empty, no registered external wakeup — with no call site to attribute it
to. A scoped opt-out has nothing to scope, and a task that "relaxed" it is not the task the
check is about. It is therefore compile-out-only.

Its escape is a different mechanism and lives at the site of the legitimate wait, not at the
site of the report: a task waiting on something only a non-worker thread can complete
registers that fact for the duration (`ts::External_wait`). A forgotten registration
produces a **false deadlock report**, so the fatal names that escape in its own message.

Shape, in three parts kept apart on purpose:

1. **Quiescence is scheduler-local.** `Scheduler::quiescent()` is "every worker in the idle
   path, every queue empty", read through the plain function seam
   `detail::scheduler_quiescent()` — the same shape as `drain_serial_pending` and
   `blocking_sync_diagnose`, so the task layer keeps not knowing about the scheduler.
   Idleness is an explicit busy↔idle transition count, not "parked": under `Idle_policy::spin`
   a worker never parks, so a park-based test would never report idle. Two atomics per
   crossing, none per task.
2. **The task layer interprets it, from the boundary waiter.** No global per-task counter and
   nothing on the hot path: a blue thread blocked in `Task_control_block::wait()` is already
   stopped, so it costs nothing to have it look around. It polls, and if quiescence holds
   *continuously* for the whole window (default 2 s, `ts::set_deadlock_net_window`) with a
   zero external-wait count, it reports. One sample would be worthless — a worker sits
   briefly between finding work and marking itself busy — which is why the window is
   continuous rather than instantaneous, and long rather than tight: a real deadlock is
   permanent, so detection latency is free, while a short window would fire on a legitimate
   blue-to-blue handoff that happened to be slow.
3. **`ts::External_wait` supplies the predicate Go lacks.** Go's `all goroutines are asleep`
   check has a documented blind spot — any live background thread masks a partial deadlock —
   because it has no way to say "this wait is legitimate". Ours does, and `Frame_gate` holds
   one from the first `next()` of a frame until the matching `open()`, so a frame whose
   workers run dry while a task waits for the boundary is not reported.

### 7.1 The report has three tiers

A deadlock report should say everything that was cheap to collect, and tell you how to get
the rest. So it is layered by what each level costs:

**Tier 1 — always.** The fact of the deadlock, the blocked waiter's identity, how long the
scheduler was quiescent, and the escapes (`ts::External_wait`,
`ts::set_deadlock_net_window`, `TS_ENABLED_RULES`).

**Tier 2 — free wherever `Rule::circular_wait` is compiled in.** The circular-wait registry
already records `{held grant → awaited pipe}` at every pipe suspension and clears at resume,
so at fatal time its live entries *are* the set of tasks suspended while holding a grant. It
is printed, not consulted: an independent mechanism has already concluded the system is
wedged, so this is a post-mortem of those edges rather than a prediction from them. (That
distinction matters. Learned-order detectors that *predict* deadlocks from wait edges have a
bad history — Linux's cross-release was reverted and DEPT never merged, both on false
positives. Printing edges after the fact cannot false-positive.)

**Tier 3 — the suspension registry, `TS_SUSPENSION_REGISTRY`.** Every live suspension:
the suspended task, what it awaits (a task, an object, or its own scope children), and what
it holds. This is what tier 2 structurally cannot see — a task suspended on a plain task
await while holding nothing, which is exactly the two-hop cycle the waits-for graph is blind
to (N holds G1 and awaits foreign task T; T awaits G1).

When tier 3 is compiled out the report says so and names the rebuild flag, so a user hitting
this in a build without it learns the next step from the message rather than from this
document.

### 7.2 What the registry costs, measured

The registry was going to be debug-only on the assumption that per-suspension bookkeeping is
expensive. It was measured instead (numbers and method in [TODO](TODO.md) 6.13):

- **~30 ns per suspension**, uncontended.
- **Zero measurable cost on real frame workloads** — the `game_frame` benchmarks, graph and
  graph-free, at both scales, are inside run-to-run noise.
- **~8% on `coro chn`**, a fixture where one frame ping-pongs a single suspension between
  two threads and does nothing else. That is the structural worst case for any registry, and
  it is a microbenchmark, not a workload.
- **No scaling cliff**: the per-suspension cost *falls* as workers rise, because records
  shard by address and concurrent suspensions land on different shards.

So it is on wherever the safety harness is on, and off in shipping. Two implementation notes
that carry that result. Records live inside the awaiter, which lives inside the coroutine
frame, so nothing is allocated. And the shard key is the **record's address, not the
suspending thread** — a coroutine resumes wherever its awaited work settled, so a record is
linked by one thread and unlinked by another; keying on the thread would put both halves of
every suspension on one mutex and one cache line, which is what the first measurement showed
before the key changed.

## 8. Obligations on a new rule check

6.15's standing requirement, restated as a checklist. A check that lands without these has to
be retrofitted, which is how the always-on `await_under_guard` fatal happened.

1. Take a bit in `TS_RULE_*` and an enumerator in `ts::Rule`, and join exactly one class
   (advisory / structural / net) — with the reasoning stated, not assumed.
2. State its shipping default, and put the check behind `TS_RULE_ON(...)` so the choice is
   real (state included, per the `TS_SAFETY_CHECKS` gating convention in CLAUDE.md).
3. State its escape in prose *and in the fatal message*. For an advisory rule that is
   `Relaxed_scope` plus the sanctioned form; for a structural one, only the sanctioned form;
   for the net, the registration it forgot.
4. Ship the test matrix row: a death test for the violation and a companion test showing the
   sanctioned form — plus, for an advisory rule, a test that the scoped opt-out actually
   suppresses it and is restored on scope exit.
5. Consult `rule_enforced` only *after* the cheap hazard condition is already true (§9).

## 9. Cost discipline

The relaxation lookup is one thread-local load, one relaxed atomic load and a mask. That is
cheap but not free, and it must never sit on the path every call takes. The pattern at every
site is: test the hazard condition first, and consult the policy only on the branch that is
about to fatal.

```cpp
if (pipe_guard_depth > 0 && rule_enforced(Rule::await_under_guard))
    ts::fatal(...);
```

The steady-state cost of an enabled rule is therefore whatever its hazard test costs — a
thread-local compare — and the policy machinery costs nothing until something is already
wrong.

## 10. Deliberately not built

- **Per-object or per-rule-instance suppression** ("relax `access_rank` for *this* object").
  The scope form covers the demonstrated need; a keyed form needs a key design and a lookup
  on a path that currently has none.
- **A tightening scope** (re-enabling a rule the global default relaxed). Symmetric and easy,
  but no use case has appeared; a program that wants the rules on should not relax them
  globally in the first place.
- **Reporting instead of fatalling** (a rule downgraded to a `TS_ENSURE`). Plausible as a
  third state between "enforced" and "off", and a natural extension of the policy mask;
  waiting for a rule whose false-positive rate justifies it.
