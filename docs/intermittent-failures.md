<!-- Process record, written 2026-08-23 after a week in which five timing-dependent defects
     surfaced, none through the normal test run. Author asked: how were they missed, and what
     prevents the next one. -->

# Intermittent failures: how they were missed, and what catches them

## 1. The record

Five timing-dependent defects in one week, all in shipped code, none found by the suite
passing or failing once:

| defect | rate | how it actually surfaced |
|---|---|---|
| scheduler teardown race (`2079096`) | once, ever | a single slow CI runner; 27 minutes of silence after the fatal |
| relaxation read from the wrong thread (`cd4c2a8`) | build-dependent | appeared only when a different configuration changed the codegen |
| `current_access` misread across resume (`9ea9d0e`) | ~1 in 5 | a repeat loop run for an unrelated reason; the isolated negative check then gave 10/20 |
| `access_guard_depth` misread (`9ea9d0e`) | ~1 in 25 | the residual after the previous fix, caught only because the loop was repeated |
| discarded op's ENSURE (`4f1c805`) | rare locally, ~1 in 4 on CI | CI; the same commit had passed as a PR |

And one pattern repeated within the week: a clean batch of 20 declared the job done, and the
next batch found a different bug at the same rate. Twice.

## 2. Why the suite could not have found them

The suite runs **once**, in CI and locally, at whatever worker count the machine has. Every
defect above is a race, and a race at 1-in-N is invisible to a single run with probability
(N-1)/N. At 1-in-25 a single run misses it 96% of the time; across the seven CI configurations
that is still a 75% chance of all-green. The suite is not weak - it caught every one of these
*the moment it was run enough times*. It simply was not being asked the question.

Three things compound it:

- **Contention differs by machine.** This machine has 22 hardware threads; a CI runner has 2.
  A race that needs a particular interleaving shows on one and not the other, so "passes
  locally" and "passes on CI" are two different measurements, not one.
- **No per-test timeout.** A hang is reported as nothing at all - the suite sits until the job's
  wall-clock limit. The teardown race cost 27 minutes of silence; the first Debug CI job
  surfaced it only because a 30-minute `timeout-minutes` had been added that morning.
- **The stress mode is not a repeat.** `--stress` runs the *sample* at a fast scale for
  sanitizers. Nothing in the tree repeats the suite.

## 3. What was reasoned instead of measured

Each defect was also preceded by a correct-sounding argument that it could not exist. Every
one of those arguments reasoned about the *source*:

- "The other thread-locals do not straddle a suspension" - the optimizer does not read source;
  it hoists an address out of a loop the argument never looked at.
- "A probe that found no disagreement" - a probe run once, at one arity, on one compiler.
- "Discarding an `Access_op` is a defined wait" - true, and the destructor's ENSURE fires
  anyway whenever the wait is real, which is timing.
- "Probe-first is equivalent to lend-first" - true for *what is acquired*, false for *where
  the body runs*.

None of these was careless. They were the kind of argument that is usually right, made
about the kind of code where usually right is not enough. The remedy is not better
arguments; it is a measurement that does not depend on the argument.

## 4. What catches them

### 4.1 Repeat the suite, as a first-class mode

`macrame_playground --tests --repeat N`: run the suite N times in one process, stop at the
first failure, print the run number and the full output of that run. This is the single
highest-value change - it turns "passes" into "passes N times", and N is a dial.

It also makes the failure *capturable*. The repeat loops this week were ad hoc shell loops,
and the first of them recorded only the run number, which meant re-running a 30-minute hunt to
read the failure text. The mode prints it.

### 4.2 A repeat job in CI

One job, `windows-msvc-repeat`, running `--tests --repeat 20` on Release. Twenty runs take
about two minutes and catch a 1-in-25 race with 55% probability per push - not certainty, but
every push rolls again, and the defect has to survive every one. Keep it as a separate job so
a flake there does not block the others and so its failure reads as "intermittent" rather than
"broken".

### 4.3 Vary the worker count

The suite should run under more than one scheduler width. Thirty tests already use a
`Scheduler_scope` for a specific shape, and ten run worker-less; the remaining hundreds run at
the machine's width only. A 2-worker pass and a 1-worker pass exercise interleavings a 22-thread
machine rarely produces - and 2 workers is what the CI runner has, so the local run would
finally measure the same thing CI does. Cheapest form: `--workers N` on the driver, and the
repeat job runs it at 2.

### 4.4 A per-test timeout in the harness

`run()` should fail a test that exceeds a deadline, naming it, rather than letting the process
hang. A hang is the most expensive failure there is - it produces no output and burns the
job's whole budget - and every deadlock-class defect presents as one. The harness already
spawns subprocesses for death tests; the same mechanism bounds a hang.

### 4.5 Negative checks are part of the fix, not an extra

Every fix this week that held up was proven by removing it and watching the test go red.
Every claim that did not hold up was missing that step. It should be the stated requirement
for any change to concurrent code: the commit message names the negative result, with its
count. A fix that cannot be shown to fail when reverted has not been shown to fix anything.

### 4.6 A test must not be able to pass vacuously

Found by 4.1 and 4.3 together, at run 107 of a loaded loop: `graph trace task count` asserts
that a `parallel_for(64)` inside a node fans out into separate counted tasks. The first
diagnosis - that the participating caller drains the range before any helper wakes, so no
helper task runs - was wrong, and reading the dispatch shows why: `parallel_for` submits its
`conc - 1` helpers unconditionally, so the task count is fixed regardless of the balancing
policy. What actually happens is narrower. The trace counts only while the run is *armed*, and
the caller's `parallel_for` returns once the items are done, not once the helpers have exited.
A helper that got its worker late claims nothing and passes `run_task` after the node has
completed and the run has disarmed - uncounted. Under load most helpers are late, and the count
drops toward the run count. The assertion was about the OS scheduler's timing relative to the
trace window, not about the counter.

The first fix attempted was wrong, and the way it was wrong is the lesson. It pinned the
`parallel_for` to `Balance::balanced` at a fixed concurrency, reasoning that the caller could
then claim at most one chunk and helpers had to run the rest. It passed - but for a reason the
reasoning did not contain: a helper holding a guaranteed chunk does real work and so finishes
*inside* the armed window, where a late idle helper does not. A fix that works by accident is
worse than one that fails, because it closes the investigation. And the test is a *profiling*
test, about whether the counter is wired; constraining how `parallel_for` distributes work to
make a count come out is asserting the wrong thing harder.
The caller-participation path is designed behaviour; a test that fails when it engages is a
test that a feature did not fire.

The right fix asks what the test is actually for. The counter increments in `run_task`, so it
sees exactly the work that went through the scheduler - which is the property worth checking,
and it can be checked with work the test controls. The node now launches `extra` tasks through
the scheduler and awaits them: each one passes `run_task` and is counted, so every run
contributes at least `1 + extra` by construction. The `parallel_for` stays, because the stat
exists to expose that fan-out, but its slices only add to a bound the test no longer depends
on. The assertion is about the counter; the scheduler is free to do what it likes.

The general rule has two halves. When a test asserts that concurrency *happened* - helpers
ran, the inline arm was taken, a resume crossed threads - it must either arrange for that
outcome or report visibly when it was not exercised (`parallel_for inherits relaxation` prints
a note when the caller claimed every chunk). But first ask whether the test is really about
that outcome at all. Here it was not, and the forced version would have encoded a scheduling
policy into a profiling test for no reason beyond making a number go up.

### 4.7 A clean batch is a bound, not a proof

Twenty clean runs at an observed 1-in-25 rate is evidence the rate is now below about 1-in-7,
nothing more. The working rule: after fixing a race, run at least three times the observed
period, and when the residual is a *different* failure, say so - the instinct to read it as
noise is exactly how the second thread-local bug hid behind the first.

## 5. What does not help

- **Reading the code harder.** Section 3 is four examples of that failing.
- **TSan alone.** It found none of these, and for three of them it could not have. TSan checks
  the *source's* memory operations against a happens-before graph, and that graph was fine -
  every thread-local access was correctly scoped. The defect was the optimizer emitting a load
  from the wrong thread's slot, and to TSan a thread-local load is trivially unracy: one
  thread touches it by definition. The model TSan uses is exactly the model the compiler
  broke. A miscompile is no more visible to it than a wrong `+` in the codegen. The teardown
  ordering it could in principle flag, given the interleaving; the discarded-op case was a
  diagnostic firing on a legal race. ASan is worse placed still - a stale slot is valid,
  allocated memory.
- **TSan as currently wired is narrower than it looks.** Two things about the job, both
  checkable in `tsan/run.sh`: it compiles at `-O1`, where clang does not take the hoist that
  the MSVC Release build takes, so it measures a different binary; and its source list is the
  four samples plus `tsan_main.cpp` - **the test suite is not compiled into it at all**.
  Every coroutine, guard and lend test written this week has had no TSan coverage by
  construction. The job is a real and valuable check on the game-frame workload; it is not a
  check on the tests, and a green result was being read as one.
- **More configurations, run once each.** Seven configurations running once gave a 75% chance
  of missing a 1-in-25 defect entirely. Depth beats breadth for this class.

### 5.1 What the sanitizers are for, restated

They catch the class they model - a genuine data race in race-free-looking source, a
use-after-free, an out-of-bounds - and they do it far more reliably than repetition. Keep them.
But this week's defects were dominated by a class *outside* that model: correct source, wrong
binary, and it is specifically a coroutine problem because no other construct lets one
function body run on two threads. For that class the only detection is repetition on the real
optimized binary, the only proof is the negative check, and the only prevention is structural
- which is why privatising the thread-locals behind out-of-line accessors mattered more than
any test.

Two changes follow for the TSan job: add `tests/` to its source list so the suite runs under
it, and stop reading a green result as covering the coroutine/thread-local surface.

### 5.2 What the suite found the first time it ran under TSan

Adding the suite turned out to need three things, not one: the source list, the `main`
collision (`tsan_main.cpp` already had the `TS_TSAN_NO_MAIN` seam, the CMake target just did
not use it), and a portable harness - `tests/harness.cpp` included `windows.h` for the
death-test spawn, so the suite had never compiled on Linux at all. With those done the first
run reported three races, none of which TSan had ever had the chance to see:

- **A use-after-free in `Signal::trigger()`**, the real one. `settle()` wakes its waiters
  before running its own tail, by design. The woken thread can be the one holding the last
  `Signal`: it returns from `sync()`, drops the handle, and frees the block while the
  triggering thread is still inside `settle()` - `pthread_cond_destroy` racing
  `pthread_cond_broadcast`. Every other path to `complete()` goes through a `Task_ptr` the
  caller holds, which pins the block; `trigger()` went through the raw pointer. Fixed by
  pinning. `Frame_gate::open` reaches the same code, so one fix covers both.
- **A test calling `TS_CHECK` from two raw `std::thread`s** on the harness's plain counters.
  Test code, harmless in effect, fixed by handing results back to the test thread.

The race the section above says TSan is for is exactly the first one - a genuine data race in
race-free-looking source, found in minutes by the tool built to find it, in code the tool had
been prevented from seeing for the library's whole life.
- **More configurations, run once each.** Seven configurations running once gave a 75% chance
  of missing a 1-in-25 defect entirely. Depth beats breadth for this class.

## 6. Order

4.1 and 4.4 first - they are small, they are the enabling pieces, and the repeat mode is what
every later measurement uses. Then 4.2, which is three lines of YAML once 4.1 exists. 4.3 is
the largest and can follow. 4.5, 4.6 and 4.7 are rules, not code; they belong in `CLAUDE.md`.
