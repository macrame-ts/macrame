# Handoff — verify (and if needed fix) the current pipe drain race

For a fresh session working on **`master`** (the current mutex-guarded pipe), spun off from
the pipe-rebase design review. Self-contained. Prepared 2026-07-31.

## 0. TL;DR

I (the pipe-rebase session) flagged that the current `Pipe::wait_until_idle` drain *might*
have the same use-after-free race UE fixed in `FPipe` with `TaskCount` + a shared
`FEventCount`. On a closer read it looks like **our version is probably already safe**
(the notify is done *under* the mutex, which is the textbook-safe pattern, unlike UE's
lock-free event). But "probably safe" on a destruction race deserves a rigorous second look
+ a targeted TSan/ASan stress, and we should recover UE's exact rationale to be sure our
shape is immune. That is this task. Outcome is either a real fix or a documented "why we're
safe" note — both valuable before public release.

This is orthogonal to the pipe rebase (branch `pipe-rebase`), which replaces the pipe
entirely. But the rebase won't land for a while, and `master` ships first, so any real bug
here must be fixed on `master` now. If you conclude it's safe, drop a one-line note at the
drain site so the next reader doesn't re-flag it.

## 1. The code

- **Waiter:** `Pipe::wait_until_idle()` — `include/ts/guarded.h` (~line 82). A
  `std::condition_variable idle` on `std::mutex mutex`, predicate
  `jobs.empty() && active_readers == 0 && !writer_active`. The sole caller is `~Guarded`
  (`guarded.h`, ~line 401), i.e. object teardown.
- **Signaler:** `release_and_redispatch(scheduler, pipe, mode)` —
  `src/guarded.cpp` (~line 131). Under `pipe.mutex`: updates the reader/writer counts,
  `dispatch(pipe, admitted)`, then `if (idle-predicate) pipe.idle.notify_all();`. The scope
  ends (mutex unlocks), then — OUTSIDE the lock — `submit_admitted(scheduler, pipe, admitted)`.

```cpp
void release_and_redispatch(Scheduler& scheduler, Pipe& pipe, Access mode)
{
    Admitted admitted;
    {
        std::scoped_lock lock(pipe.mutex);
        // ... --active_readers / writer_active = false ...
        dispatch(pipe, admitted);
        if (pipe.jobs.empty() && pipe.active_readers == 0 && !pipe.writer_active)
            pipe.idle.notify_all();                       // <-- under the lock
    }                                                     // <-- mutex unlocks here
    submit_admitted(scheduler, pipe, admitted);           // <-- pipe& used after unlock
}
```

## 2. Why it looks safe (verify this reasoning, don't trust it)

The classic CV destruction UB is: a signaler calls `notify_*` *without* holding the lock,
the waiter wakes, its predicate is true, it returns and destroys the CV — all before the
signaler's `notify_*` finishes touching the CV internals → UAF. The standard fix is either
to notify under the lock, or to keep the CV alive via a shared ref (UE's approach).

Our signaler notifies **under the lock**. So the waiter cannot make progress past
`cv.wait(lock, pred)` until it re-acquires `mutex`, which it can only do after the signaler
releases it at the end of the scope — by which point `notify_all()` has already returned.
So the signaler is not inside `notify_all` when the waiter could destroy the CV. That
appears to close the UE-class window.

The one remaining post-unlock touch is `submit_admitted(scheduler, pipe, admitted)` at the
end (binds `pipe&` after the mutex is released, so after the waiter could have destroyed the
pipe). BUT: the idle notify only fires when nothing was admitted (admitting sets
`writer_active`/`active_readers`, which would make the idle predicate false), so in the
notified/idle case `admitted` is **empty** and `submit_admitted`'s loop body never runs — it
dereferences nothing in `pipe`. So there's no actual member access on a destroyed pipe,
though binding a reference to an out-of-lifetime object is technically dicey.

## 3. What to actually check (rigorously)

1. **Confirm the notify-under-lock argument** against the current `libstdc++`/`MSVC STL`
   `condition_variable` semantics and `[thread.condition.condvar]` (the `~condition_variable`
   precondition: "no thread blocked on `*this`"). Is a *notified-but-not-yet-rewoken* waiter
   "blocked on `*this`" at destruction? Walk the exact happens-before.
2. **The `submit_admitted(pipe&)` post-unlock binding** — prove `admitted` is always empty
   when the idle notify fired (I believe it is; confirm from `dispatch`), or make it
   unambiguously safe (e.g. skip the call when `admitted.empty()`, or move the idle check
   after `submit_admitted`).
3. **Any other thread still touching the pipe after the last notify** — e.g. `run_pipe_job`
   epilogue (`src/guarded.cpp` ~line 163), `pipe_try_inline`'s tail, a second reader
   finishing concurrently. Enumerate every code path that touches `pipe` and confirm none
   runs after the drain predicate can go true for the destroying thread.
4. **Targeted stress** (this is the real oracle — Windows has no TSan, use WSL):
   - A tight loop: create a `Guarded`, fire N async jobs (mix reader/writer), drop all task
     handles, destroy the `Guarded` immediately so `~Guarded` races the last job's
     completion/notify. Thousands of iterations, several threads. Run under **WSL TSan**
     (`bash tsan/run.sh` — add a `stress_pipe_drain` stage) and under Windows **ASan**
     (`/p:EnableASAN=true`) + `--stress`. Note: the pipe-rebase branch already added a
     `stress_pipe_lifetime` stage (create/destroy with in-flight work) that is TSan-clean on
     the current pipe — start from that shape; it lives in `tsan/tsan_main.cpp` on
     `pipe-rebase`. On `master` you'll add an equivalent.

## 4. UE reference — recover the rationale (a required subtask)

UE's `FPipe` (the prior art the rebase generalizes) hit and fixed this exact class of race
*after the current author left Epic*, so the rationale isn't in their head. The design:

- `FPipe::TaskCount` (`std::atomic<uint64>`) — emptiness counter, separate from the tail
  pointer `LastTask`. `Public/Tasks/Pipe.h` ~lines 136–138.
- `FPipe::EmptyEventRef` (`TSharedRef<UE::FEventCount>`) — a **heap, shared** drain event.
  `WaitUntilEmpty` polls `TaskCount == 0` armed on it. `Private/Tasks/Pipe.cpp`
  ~lines 33–94 (`ClearTask` / `WaitUntilEmpty`).
- The fix's fingerprint, in `ClearTask`: the last decrementer takes a **local copy of the
  shared event ref BEFORE** `TaskCount.fetch_sub`, with the comment `// use-after-free
  territory!` — because a waiter in `WaitUntilEmpty` may destroy the whole `FPipe` the
  instant the count hits zero. The shared ref keeps the event alive past the pipe's death.

**The local `ue6-main` clone at `c:\src\ue6-main` is squashed to a single commit** ("Fix
Horde static analysis") — no history, no messages. So:

- **Task:** on the **public UE GitHub** (`github.com/EpicGames/UnrealEngine`, requires an
  Epic-linked account), get the git blame / PR history for
  `Engine/Source/Runtime/Core/Private/Tasks/Pipe.cpp` and `.../Public/Tasks/Pipe.h`, focused
  on the introduction of `TaskCount`, `EmptyEventRef`, and the `// use-after-free territory!`
  comment. Pull the commit message(s) / PR description / linked bug. That tells us EXACTLY
  what race the lock-free-event version had — which confirms whether our mutex-CV version is
  structurally immune (my hypothesis) or shares the window.
- If you lack Epic GitHub access, note it and fall back to the code-level analysis in §2–3;
  the mechanism (`ClearTask` + `WaitUntilEmpty` in the local squashed clone) is fully
  readable even without history.

## 5. If a real race exists — fix options

- **Simplest for us (recommended if a fix is needed):** drop the CV; make `~Guarded`
  **spin-yield on the atomic drain predicate**. Our only waiter is teardown, not a hot API,
  so a bounded spin is fine, and there is no event/CV object to free out from under the
  signaler — the last job's final atomic write *is* its last pipe touch, and the waiter frees
  the pipe only after observing idle. This is strictly simpler than UE's shared-event dance
  (UE needs the event because `WaitUntilEmpty` is a general, core-not-to-burn API).
- **UE's pattern:** a refcounted (shared) drain event + local-ref-before-the-last-decrement.
  More machinery; only warranted if we later expose a public non-teardown `wait_until_idle`.
- Either way: make the signaler touch **nothing** in the pipe after the write that can
  unblock the waiter.

Note the pipe-rebase (branch `pipe-rebase`, `docs/pipe-rebase.md` §7) already plans the
spin-yield-on-`TaskCount` drain for the rewritten pipe, so a `master` fix in that shape
ports forward cleanly.

## 6. Coordination

- Work on `master`. The pipe-rebase branch is a parallel rewrite; don't fix there (it
  replaces the drain anyway).
- If you add a stress stage, mirror the pipe-rebase `stress_pipe_lifetime` shape so the two
  branches converge.
- Report back: (a) is there a real race? (b) the UE rationale you recovered; (c) the fix or
  the "safe because …" note added at the drain site.

## 7. Resolution (2026-07-31)

**(a) No real race of the UE class.** The hypothesis in §2 holds: our notify is done
*under* `pipe.mutex`, so a `wait_until_idle` waiter cannot return from `idle.wait` (and thus
cannot destroy the pipe) until it re-acquires `mutex`, which the signaler only hands off
*after* `notify_all()` has returned. This is the pattern [thread.condition.condvar]
explicitly blesses ("only the notification to unblock the wait needs to happen before
destruction"). Verified TSan-clean: added a `stress_pipe_lifetime` stage
(`tsan/tsan_main.cpp`, mirrors the pipe-rebase shape — 4 threads × 4000 iters, create a
`Guarded`, fire a reader/writer mix, drop all handles, `~Guarded` races the last
completion/notify) — `bash tsan/run.sh` reports "no races" through that stage, both before
and after the §(c) fix. MSVC Debug x64 builds clean.

**(b) UE rationale — verified from `ue5-main` git history via `gh` (the two commits the
handoff named, both authored by danny couture, both *after* the Nov-2023 departure).** UE's
`FPipe` drain is *lock-free*: `WaitUntilEmpty` parks on a `UE::FEventCount` and early-outs on
`TaskCount == 0`; `ClearTask` (`Private/Tasks/Pipe.cpp`) does the unlocked `Notify()`. The
history shows the race was *introduced by the fix to a different bug*, then fixed in turn:

- **`ca98382` (2024-04-17, UE-204563)** — replaced the old `WaitUntilEmpty` (which pushed a
  dummy `FPlaceholderTask` into the pipe and waited on it — correct but it *leaked a task on
  repeated timeout calls* and didn't count prerequisite-pending tasks) with the lock-free
  `TaskCount` (atomic counter) + `EmptyEvent` (a **by-value** `FEventCount`) drain. Also
  fixed a UAF where a task launched with prerequisites wasn't registered on the pipe until
  its prereqs fulfilled, so `HasWork()`/destruction couldn't see it — fixed by incrementing
  `TaskCount` at `Launch`, before prereqs (visible in `Pipe.h`). This new lock-free drain
  used `std::memory_order_relaxed` throughout and an unlocked `EmptyEvent.Notify()`.
- **`95e9c0d` (2025-01-28)** — fixed the teardown UAF that `ca98382`'s new drain introduced:
  "*Fix potential use-after-free when destroying an FPipe just after a call to
  WaitUntilEmpty*" + "*Fix potential race between ClearTask and WaitUntilEmpty because of
  missing barriers on TaskCount.*" The diff (verified):
  ```cpp
  // Pipe.h:  UE::FEventCount EmptyEvent;  ->  TSharedRef<UE::FEventCount> EmptyEventRef;
  // Pipe.cpp (ClearTask):
  -  if (TaskCount.fetch_sub(1, std::memory_order_relaxed) == 1)
  -      EmptyEvent.Notify();
  +  // Avoid use-after-free by taking a ref on the event before decrementing the value.
  +  TSharedRef<UE::FEventCount> LocalEmptyEvent = EmptyEventRef;
  +  if (TaskCount.fetch_sub(1, std::memory_order_release) == 1)   // release, was relaxed
  +      // use-after-free territory!
  +      LocalEmptyEvent->Notify();
  // WaitUntilEmpty: every TaskCount.load  relaxed -> acquire  (the "missing barriers")
  ```

The instant `TaskCount.fetch_sub` hits 0, a waiter (or `~FPipe` right after it) frees the
whole pipe — including its `EmptyEvent` member — *while the last decrementer is still inside
`Notify()`*. The fix is the exact fingerprint the handoff predicted: a **refcounted
`TSharedRef` event + a local copy taken before the decrement** so the `FEventCount` outlives
the pipe, plus the `release`/`acquire` pairing so the `TaskCount == 0` observation
synchronizes. So UE needs the keep-alive *because their notify is unlocked*. Ours is under
the mutex, so we need nothing equivalent. The same "touch no members after the linearization
point" discipline appears in `TClosableMpscQueue::Close` (the container the pipe used to be
built on) — comment: "the queue is closed at this point, and the user is free to destroy it
— no members should be accessed."

The two-commit arc is itself the lesson: a lock-free drain counter is the natural
performance choice for a general core API, but it *re-created a teardown UAF* that took a
second fix (a refcounted event + barriers) to close. Our mutex-CV drain trades a
per-completion lock for structural immunity to that entire class — the right trade because
our only waiter is teardown (cold), not a hot public `WaitUntilEmpty`.

**(c) Fix applied** (`src/guarded.cpp`, `release_and_redispatch`): the one post-unlock pipe
touch, `submit_admitted(scheduler, pipe, admitted)`, is now guarded by `if
(!admitted.empty())`. `dispatch` admits nothing exactly when the idle predicate holds (any
admission sets `writer_active`/`active_readers`), so a notify implies `admitted` is empty —
and only a notify can release the destroying waiter. The guard therefore means we never even
*bind* `pipe&` after the waiter could have freed it (removing the one residual
[basic.life] concern from §2, which was benign — the loop body never ran — but is now
impossible). "Safe because" notes added at both `release_and_redispatch` and
`Pipe::wait_until_idle`.

**What we can borrow from UE (for the pipe-rebase, not needed on `master`).** UE's pipe is
an *intrusive* task chain — an atomic `LastTask` pointer threaded through the task objects
via `AddSubsequent`, plus a separate atomic `TaskCount` for emptiness — so a launch
allocates zero container nodes and the drain reads emptiness with one atomic load, no lock.
That is what enables their lock-free `WaitUntilEmpty` (and the spin-yield-on-`TaskCount`
drain the pipe-rebase already plans). Our `master` pipe is a mutex-guarded `std::deque<Job>`
and reads emptiness under the lock; correct and teardown-immune, but it pays a mutex per
completion and allocates deque chunks under load. The migration UE itself made —
`TClosableMpscQueue` (a lock-free consume-once MPSC, one heap `FNode` per enqueue, with a
documented consume-side livelock hazard when a producer is preempted mid-link) → intrusive
`LastTask` chain — is the same allocation/lock-free lesson the pipe-rebase is chasing. When
we adopt an atomic `TaskCount`-style counter there, port the refcount discipline only if we
also drop to a lock-free/unlocked drain notify; with a mutex-held notify it stays
unnecessary.
