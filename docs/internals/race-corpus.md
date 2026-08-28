# Race Corpus: Real-World Game/Engine Concurrency Bugs vs. This Library's Safety Stack

*Corpus study, 2026-07. Companion to [limits.md](limits.md) (the honest
catalog of what the harness does and does not catch) and
[research-deepdive.md](research-deepdive.md) §13 (the cross-system safety
comparison). Every incident below is a real, publicly documented concurrency
bug — an issue-tracker entry, a fix PR, a postmortem, or a security
writeup — fetched and verified during collection (July 2026). Paragraphs
numbered §N.M; the full source index is at the end.*

---

## 1. Question, method, and how to read the verdicts

**§1.1 — The question.** Across concurrency bugs that actually occur in
game and engine development, what fraction falls in classes this library's
safety stack catches? The stack under test: the runtime access harness
(`TS_CHECK_ACCESS` against a per-task `Access_context`, fatal on undeclared
access in `TS_SAFETY_CHECKS` builds), the lifetime fatals (`Guarded`/graph
destruction with outstanding work, stale-inherited-grant epoch detection),
the `TS_ENSURE` blocking-`sync()`-under-grant diagnostic, the structural
properties (derived edges from declared access, mode-aware pipe arbitration,
canonical-order acquisition, caller participation + retraction, refcounted
task payload ownership), and TSan on Linux CI as the complementary oracle.

**§1.2 — The validation logic.** The declared-access approach is validated
if the classes it catches *dominate* the classes that occur — not by having
zero holes. limits.md already concedes the holes ([limits.md](limits.md)
§2–§3); this document measures whether the mass of real-world failures sits
inside or outside them.

**§1.3 — Corpus construction.** Five source veins, collected independently:
the Godot GitHub tracker (rich, public, triaged threading bugs), the Unreal
Engine public tracker (issues.unrealengine.com), Unity (public discussions +
a shipped-game tracker; Unity's own issue tracker proved largely
unfetchable post-migration), other open-source engines (Bevy, O3DE, Flax,
Stride) plus Chromium as an adjacent large-C++ case, and published
postmortems/talks. Verification rule: an incident enters the corpus only if
its primary page was actually fetched and the described mechanism confirmed;
every row carries a link. Distinct root causes only — recurrences of one
root cause are folded into a single row (noted where relevant).

**§1.4 — The counterfactual rule.** A verdict answers: *if the state
involved had lived behind this library's primitives (`Guarded<T>`, graph
nodes with declared access, tasks) and the developer had made the same
category of mistake, what would have happened?* This is necessarily a
judgment call — flagged per-row via the confidence column — but it is the
only honest way to compare across engines: the incidents happened in five
different concurrency models, and the constant is the *mistake*, not the
API. Where the raced state is not user-declarable at all (driver objects,
engine-internal machinery behind an opaque API), the verdict is
out-of-scope regardless of mechanism.

**§1.5 — Verdict vocabulary.** Seven values, ordered from strongest to
weakest for our stack:

| Verdict | Meaning |
|---|---|
| **harness-dev-build** | The mistake is an access without the protection the code intended; the analogue touches a `Guarded<T>` without a grant and fatals at the first instrumented access in a `TS_SAFETY_CHECKS` build. |
| **lifetime-fatal** | One of the specific lifetime shapes we fatal on: `Guarded`/graph destroyed with queued or held work, a snapshot grant outliving its window (stale-grant epoch), a recorder outliving its journal. |
| **TS_ENSURE** | The blocking-`sync()`-under-active-grant diagnostic fires (debugger break + report). Diagnosis, not prevention. |
| **prevented-by-construction** | The bug shape cannot arise under the model: derived edges + mode-aware pipe arbitration (no hand-wired dependency to forget), canonical-order acquisition (no lock-order inversion), caller participation + retraction (no fork-join pool-exhaustion deadlock), refcounted task-payload ownership (no unowned in-flight pointer). |
| **TSan-CI-only** | Invisible to the harness (escaped reference, uninstrumented path, library-internal machinery) but caught by a memory-access-tracking oracle — our Linux TSan CI leg. |
| **not-caught** | Inside the model's blind spot and typically beyond TSan too: correctly-synchronized wrong-order/wrong-interleaving semantics (limits.md §3.1–3.2). |
| **out-of-scope** | Engine/driver/platform-internal; no user-side declaration model of any design applies. |

**§1.6 — Confidence.** High = root cause diagnosed in the source (fix
linked or maintainer analysis) and the classification unambiguous; med =
clear symptom plus credible mechanism, or a verdict resting on a
counterfactual judgment; low = symptom-level report only.

---

## 2. Taxonomy

**§2.1 — The eight classes.** Rows below use these classes; the mapping to
our stack is the a-priori expectation the corpus then tests.

| # | Class | A-priori expectation for our stack |
|---|---|---|
| 1 | Undeclared/unsynchronized shared access — code touched shared state without the protection the codebase intended | The harness's home turf: fatal at first instrumented access in dev |
| 2 | Thread-affinity violation — main/render-thread-only API called off-thread | Caught, with a caveat: our model replaces affinity with grants, so the catch requires the touched state to be guarded |
| 3 | Cross-thread lifetime / use-after-free — object destroyed while a task/thread still uses it | Specific shapes fatal (dtor-with-outstanding-work, stale-grant epoch); the rest TSan/ASan |
| 4 | Escaped/stashed reference used later | The documented harness hole (limits.md §2.3); TSan-only |
| 5 | Ordering/semantic race — correctly synchronized, wrong order/interleaving (check-then-act, lost update, frame-order) | Not catchable by access declaration; some instances *prevented* by derived edges / deterministic order / cancellation propagation |
| 6 | Deadlock / blocking misuse | `TS_ENSURE` diagnoses the sync-under-grant shape; lock-order and pool-exhaustion shapes prevented structurally if the data lives in the model |
| 7 | Atomics/low-level misuse — wrong memory order, torn access, DIY sync | Not caught; sometimes TSan |
| 8 | Engine/driver/platform-internal race | Out of scope for any user-side model |

**§2.2 — A note on class 8.** Class-8 rows are kept in the corpus
deliberately: they measure how much of the observed failure mass no
user-side safety model can reach, which bounds any honest marketing claim.
Aggregates are reported both with and without them.

---

## 3. The corpus

### 3.1 Godot (15 incidents)

**§3.1.1 — Table.**

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| G1 | [godot#95616](https://github.com/godotengine/godot/issues/95616) | ResourceLoader | Loader kept accessing `task.resource` after setting `THREAD_LOAD_LOADED` and unlocking; concurrent `load_threaded_get` consumed/nulled it | 1 | harness-dev-build | high |
| G2 | [godot#63461](https://github.com/godotengine/godot/issues/63461) | ResourceLoader | Semaphore pointer copied under lock, waited on after unlock; loader thread deleted it in the gap (ASan UAF) | 4 | TSan-CI-only | med |
| G3 | [godot#96115](https://github.com/godotengine/godot/issues/96115) | Resource signals | Deferred `connect_changed` ran at the wrong time vs. threaded scene instantiation; connections silently lost | 5 | not-caught | high |
| G4 | [godot#54099](https://github.com/godotengine/godot/issues/54099) | AudioServer | Main thread freed `AudioStreamPlayer` while the audio thread was mixing it (ASan UAF) | 3 | lifetime-fatal | med |
| G5 | [godot PR#77143](https://github.com/godotengine/godot/pull/77143) | WorkerThreadPool | Load task waits for a not-yet-started load while all pool slots busy; plus waiter buried under a new task on its stack | 6 | prevented-by-construction | high |
| G6 | [godot#83677](https://github.com/godotengine/godot/issues/83677) | ClassDB rwlock | Main thread blocked on the write lock vs. the doc-gen thread on the read side; editor wedged at 0% CPU | 6 | prevented-by-construction | low |
| G7 | [godot#109554](https://github.com/godotengine/godot/issues/109554) | Physics (separate thread) | Direct space/body state queries read state the physics thread concurrently mutates; crashes minutes in | 1 | harness-dev-build | high |
| G8 | [godot#53496](https://github.com/godotengine/godot/issues/53496) | Physics BVH | `_update_shapes()` entered from main (`test_body_motion`) and the physics thread at once; BVH corruption | 1 | harness-dev-build | high |
| G9 | [godot#101894](https://github.com/godotengine/godot/issues/101894) | Physics shape list | Concurrent add/remove on an intrusive pending-update list corrupted an element; drain loop spins forever | 1 | harness-dev-build | med |
| G10 | [godot#98577](https://github.com/godotengine/godot/issues/98577) | TranslationServer/UI | Threaded translation load fired a notification that rebuilt UI theme caches off-main; segfault in `HashMap::clear()` | 2 | harness-dev-build | high |
| G11 | [godot#80581](https://github.com/godotengine/godot/issues/80581) | Scene instantiation | `AudioStreamPlayer` ctor connects to the audio singleton's signal from a worker during documented-thread-safe instantiation | 2 | harness-dev-build | med |
| G12 | [godot#84046](https://github.com/godotengine/godot/issues/84046) | Deferred calls + hot reload | Queued lambda's captured objects deallocated by script hot reload before the deferred call ran; double free | 3 | lifetime-fatal | med |
| G13 | [godot#109471](https://github.com/godotengine/godot/issues/109471) | Deferred signal emission | `RefCounted` freed before its queued `emit.call_deferred()` from a pool thread executed (4.5 regression) | 3 | prevented-by-construction | med |
| G14 | [godot#71216](https://github.com/godotengine/godot/issues/71216) | Shutdown | Autoload destroyed while a loader thread still blocked on its `Semaphore`; `pthread_cond_destroy` with a waiter hangs exit | 6 | lifetime-fatal | med |
| G15 | [godot#61650](https://github.com/godotengine/godot/issues/61650) | Multithreaded rendering | Thread Model = Multi-Threaded segfaults instantly at startup (4.0 alpha); engine-internal init race | 8 | out-of-scope | low |

**§3.1.2 — Notes.** G1: the access continued past the end of the
protection window — in our model the accessor's grant ends with its scope
and a later touch has no grant (the stale-grant epoch check is the exact
diagnostic for the inherited form). G2: a pointer read under the lock,
stashed, then dereferenced outside it — the canonical escaped-reference
hole of limits.md §2.3; only an access-tracking oracle sees it. G3:
deferral itself was the safety mechanism; the interleaving of deferral vs.
threaded instantiation was the bug — access declarations carry no model of
*when* the connection should land. G4/G12: destroying an object with work
still queued/running against it is precisely the shape our `Guarded`/graph
destruction fatals exist for; the general captured-object case (G12's
non-guarded captures) degrades to ASan/TSan. G5: both sub-shapes (waiting
on un-started work under pool exhaustion; a waiter buried by nested
execution) are the oversubscription deadlock that retraction + deep inline
`sync()` was designed to break — Godot's fix (detect waiters, spawn extra
threads, report `ERR_BUSY` on stack burial) is the ad-hoc form of the same
idea. G6: under the pipe model neither thread holds a lock across a wait
(accesses are queued jobs, not held rwlocks), so the wedge shape has no
analogue — low confidence because the full engine context is complex. G7–G9:
three separate physics-thread bugs with one signature: a second entry
point mutated state the command-queue discipline was supposed to own —
undeclared access through an instrumented surface, the harness's home
turf. G10/G11: affinity violations arriving via non-obvious paths (a
notification cascade; a constructor side effect) — in grant terms, a
context-less thread touching guarded state faults at the first instrumented
access, regardless of how surprising the call path is. G13: the engine's
deferred-emit mechanism failed to keep its target alive; our task blocks
own their payload via refcount, so the default shape does not arise. G14:
destruction of a sync primitive with a blocked waiter is the
dtor-with-outstanding-work fatal, fired at teardown. G15: nothing a user
declaration could reach; included as the engine-internal representative.

### 3.2 Unreal Engine (15 incidents)

**§3.2.1 — Table.**

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| U1 | [UE-259543](https://issues.unrealengine.com/issue/UE-259543) | GameplayAbilitySystem | Cue Add deferred to a streaming callback while Remove does a JIT class lookup — Remove can run before Add; cues wedge | 5 | not-caught | high |
| U2 | [UE-191796](https://issues.unrealengine.com/issue/UE-191796) | Animation | 5.2 moved the double-buffered bone swap into parallel end-of-frame updates; follower components sometimes processed before the leader — stale bones | 5 | prevented-by-construction | med |
| U3 | [UE-152571](https://issues.unrealengine.com/issue/UE-152571) | Niagara/Audio | `PostInitProperties` on the async loading thread reached `FAudioCommandFence::BeginFence`; `check(IsInGameThread())` fired in packaged games | 2 | harness-dev-build | high |
| U4 | [UE-106091](https://issues.unrealengine.com/issue/UE-106091) | Audio (Ambisonics) | Submix registration ran as an audio-thread command on a task worker; game-thread-only getter asserted — editor fine, packaged crash | 2 | harness-dev-build | high |
| U5 | [UE-77756](https://issues.unrealengine.com/issue/UE-77756) | GameInstance/Timers | `FTimerManager::SetGameInstance` invoked from the async loading thread; `check(IsInGameThread())` crash at startup | 2 | harness-dev-build | high |
| U6 | [UE-222366](https://issues.unrealengine.com/issue/UE-222366) | PSO precaching | Decal PSO precache from the async loading thread constructed a `TStrongObjectPtr` (game-thread-only, GC rooting); `checkf` fired while streaming levels | 2 | harness-dev-build | high |
| U7 | [UE-228253](https://issues.unrealengine.com/issue/UE-228253) | VisualLogger | GameThread flushed the log while an anim worker wrote an entry via `FVisualLogEntry::MoveTo`; shared entry state mutated by both | 1 | harness-dev-build | high |
| U8 | [UE-240302](https://issues.unrealengine.com/issue/UE-240302) | Pose Search (cook) | Async indexing cancelled but `BeginCache` proceeded; `ParallelFor` workers indexed inconsistent state — OOB reads | 5 | not-caught | med |
| U9 | [UE-223522](https://issues.unrealengine.com/issue/UE-223522) | Incremental GC + Slate | Slate resource-manager CS held across GC slices; loading thread blocks on it; GameThread spins waiting to destroy that thread — three-way circular wait | 6 | not-caught | med |
| U10 | [UE-291472](https://issues.unrealengine.com/issue/UE-291472) | Materials/GC | `AddReferencedObjects` missed `ReferencedTextureCollections`; GC freed the collection while a material was already queued for rendering | 3 | lifetime-fatal | high |
| U11 | [UE-217692](https://issues.unrealengine.com/issue/UE-217692) | D3D12 RHI | Buffer defrag raced view creation inside the RHI; fix defers view creation to the RHI thread | 8 | out-of-scope | high |
| U12 | [UE-224491](https://issues.unrealengine.com/issue/UE-224491) | Editor / GWarn | The global feedback context (incl. the user-cancel flag) shared between SlowTasks and async Blueprint indexing; stale cancel flag drove unsafe teardown | 1 | harness-dev-build | med |
| U13 | [UE-230511](https://issues.unrealengine.com/issue/UE-230511) | Niagara GPU counts | `TArray::RangeCheck` crash on Android Vulkan, "potentially caused by a race condition on the rendering thread" | 1 | harness-dev-build | low |
| U14 | [UE-168068](https://issues.unrealengine.com/issue/UE-168068) | Morph targets | Morph-target delete tripped `check(IsInGameThread() \|\| IsInGarbageCollectorThread())` | 2 | harness-dev-build | med |
| U15 | [UE forum](https://forums.unrealengine.com/t/async-texture2d-import-garbage-collection-crash/475276) | User code + GC | Async texture import checked `IsGarbageCollecting()` then created UObjects — GC began in the gap; "Creating UObjects while Collecting Garbage is not allowed!" | 5 | not-caught | med |

**§3.2.2 — Notes.** The tracker's strongest pattern is U3–U6/U14: one
recurring shape — object construction/side effects on the async loading
thread reaching game-thread-only APIs — repeatedly shipping as
packaged-build crashes because UE's `check()` affinity guards are the
sampling oracle that happened to be compiled in. These are exactly the
grant-less access our harness fatals on in dev; the observation that they
*shipped* is the §6 coverage caveat, not a class miss. U2 is the corpus's
cleanest prevented-by-construction case: the regression was manufacturing
parallelism without the leader→follower dependency — under declared access
the follower's read conflicts with the leader's write and `compile()`
derives the edge; there is no separate dependency wiring to forget. U1,
U8, U15 are check-then-act shapes (is-the-class-loaded, is-cancelled,
is-GC-running) — correctly synchronized, wrong interleaving; no
declaration model sees them (limits.md §3.1). U8's overlap window would
*likely* be narrowed by cancellation-as-completion-state (a cancelled
prerequisite settles dependents cancelled rather than letting them run),
but that is a redesign, not a catch — verdict stays not-caught. U9's
three-way wedge involves GC machinery and held critical sections across
slices — `TS_ENSURE` would flag the blocking-wait-under-grant component,
but the cycle itself is a liveness bug the model does not see. U10: the
missing-GC-root shape maps to our refcounted-ownership + dtor-fatal pair —
queued work referencing a destroyed guarded object is a fatal, not a silent
UAF.

### 3.3 Unity (6 incidents)

**§3.3.1 — Table.**

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| N1 | [daggerfall-unity#2355](https://github.com/Interkarma/daggerfall-unity/issues/2355) | Shipped game | `FindObjectsOfType()` called from worker threads at 19 sites; API has no main-thread guard — intermittent access violations tied to scene transitions | 2 | harness-dev-build | high |
| N2 | [Unity discussions](https://discussions.unity.com/t/random-crash-in-entities-graphics-systems-in-build/1511889) | Entities Graphics | Build-only random crash (5 min–5 h) inside package-provided rendering jobs reading chunk data; never reproduces in-editor where safety checks run | 8 | out-of-scope | high |
| N3 | [Unity discussions](https://discussions.unity.com/t/main-thread-deadlock-with-unity-2018-3-1f1/728813) | Engine internals (iOS) | `AssetBundle.Unload(true)` after scene unload permanently wedges in the engine's own spin-lock during playable teardown | 8 | out-of-scope | high |
| N4 | [Unity discussions](https://discussions.unity.com/t/solved-bizarre-error-unityexception-get_gameobject-can-only-be-called-from-the-main-thread/707577) | Ad-SDK callback | Third-party SDK invoked its reward callback on a background thread; developer touched a `GameObject` there — Unity's affinity guard threw | 2 | harness-dev-build | med |
| N5 | [Unity discussions](https://discussions.unity.com/t/invalidoperationexception-the-previously-scheduled-job-reads-from-the-nativearray/779183) | ECS jobs | Manually scheduling another system's job bypassed automatic dependency propagation; a writer job was scheduled against an array a reader still used | 5 | prevented-by-construction | high |
| N6 | [Unity discussions](https://discussions.unity.com/t/native-arrays-cause-crashes-in-standalone-builds-2019-2a14/741968) | NativeArray alloc | Correct user code crashed standalone-release only; engine allocation/leak-tracking divergence between build configurations | 8 | out-of-scope | med |

**§3.3.2 — Notes.** N1 is the sharpest affinity datum in the corpus: Unity
enforces main-thread affinity per-API, and this API simply lacked the
check — the bug shipped as native corruption instead of a caught
exception. A uniform per-object grant check has no per-API coverage list
to be incomplete (any instrumented touch without a grant faults),
though our §2.1-of-limits.md uninstrumented-method caveat is the same
hazard one level down. N4 is the same class where the guard *did* exist —
caught as an exception, fixed with a main-thread dispatcher; the
industrial affinity check working as designed. N5: the failure mode was a
hand-wired `JobHandle` chain bypassed — our model has no hand-wired chain
to bypass (edges derive from declared access; the pipe arbitrates at
runtime regardless), so the shape is structural, not disciplinary. The
collection pass also surfaced a pattern worth recording: Unity's classes
1/5 appear overwhelmingly as *in-editor safety-system rejections* (the
declared-access validator working), while the bugs that ship are class-2
coverage gaps and class-8 engine internals — direct field evidence for the
declared-access approach catching its home classes when it is on.

### 3.4 Other open-source engines (10 incidents)

**§3.4.1 — Table.**

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| B1 | [bevy#4657](https://github.com/bevyengine/bevy/issues/4657) | Bevy ECS access checker | `Or<(Without<A>, Without<B>)>` recorded as two independent `add_without`s — checker judged overlapping queries disjoint; two `&mut` to the same component across parallel systems | 1 | prevented-by-construction | med |
| B2 | [bevy PR#14615](https://github.com/bevyengine/bevy/pull/14615) | Bevy query transmute | Dense-marked transmuted query skipped sparse filter checks, iterating entities outside the validated access set — aliased `&mut` UB | 1 | TSan-CI-only | med |
| B3 | [bevy PR#17616](https://github.com/bevyengine/bevy/pull/17616) | Bevy `DeferredWorld` | The param registered wrong with `FilteredAccessSet`, so conflicts vs. other system params went undetected; executor scheduled aliased mutable access | 1 | prevented-by-construction | med |
| B4 | [bevy#5743](https://github.com/bevyengine/bevy/issues/5743) | Bevy task pool | `par_for_each` deadlocks when more systems fork-join than pool threads: every worker blocked waiting, none free to run the chunks that would release them | 6 | prevented-by-construction | high |
| O1 | [o3de#4267](https://github.com/o3de/o3de/issues/4267) | O3DE Atom models | `Model::Create` on job workers intermittently corrupts vertex buffers (positions/normals zeroed) — unsynchronized shared buffer state | 1 | harness-dev-build | med |
| O2 | [o3de#18623](https://github.com/o3de/o3de/issues/18623) | O3DE streaming pool | Main thread holds `m_tileMutex` expanding a mip chain; copy-queue thread needs it for shutdown cleanup — deadlock (introduced by a fix for a different race) | 6 | prevented-by-construction | med |
| O3 | [o3de#15149](https://github.com/o3de/o3de/issues/15149) | O3DE Atom jobs | A job calls `BlockUntilLoadComplete()`, which needs the main thread's `DispatchEvents()`; main is blocked waiting for that job — circular wait | 6 | TS_ENSURE | high |
| F1 | [flax#116](https://github.com/FlaxEngine/FlaxEngine/issues/116) | Flax scene API | Spawning prefabs from C# `Task.Run` crashes the editor regardless of user-side locking; scene mutation is main-thread-only | 2 | harness-dev-build | med |
| F2 | [flax#4041](https://github.com/FlaxEngine/FlaxEngine/issues/4041) | Flax scene/render | Adding a model actor intermittently overlaps render-thread traversal ("Adding/removing actors during rendering is not supported"); no user-side deferral API exists | 8 | out-of-scope | med |
| S1 | [stride PR#2722](https://github.com/stride3d/stride/pull/2722) | Stride physics events | Bepu contact events collected and dispatched from physics worker threads into single-threaded structures — index-OOB crashes; fix moves dispatch to the main thread | 1 | harness-dev-build | med |

**§3.4.2 — Notes.** B1/B3 are the most instructive rows in the corpus for
this library specifically: in both, the *declaration layer itself* was
buggy — accesses were declared correctly and the conflict computation
mis-judged them, so Rust's compile-time guarantee was silently voided.
Our stack has a property Bevy's does not: the static derivation
(`compile()`) and the runtime arbitration (mode-aware `pipe_acquire`,
writer-exclusive) are *independent* layers — a wrong derived schedule still
hits the pipe, which serializes the conflict at runtime. Verdict
prevented-by-construction on that two-layer argument, med confidence
because it is a counterfactual about our own hypothetical bugs. B2 is the
complementary case — a soundness hole in library internals — and the
honest verdict is TSan-CI-only: that is exactly what our Linux TSan leg
exists to sweep. B4 is the fork-join pool-exhaustion deadlock again (cf.
G5), independently rediscovered in a Rust engine — caller participation in
`parallel_for` plus retraction removes the shape. O3 is the corpus's one
clean `TS_ENSURE` row: a blocking wait inside a job on work owned by the
blocked thread — the diagnostic's exact target shape, and the waiting rules
("never block inside a node") it enforces. O2 is a classic two-mutex
lock-order inversion; under `Guarded` there are no user-held mutexes and
multi-object acquisition is canonical-order by pipe address, so the
inversion cannot be expressed. F2 is included as the second engine-internal
row: the reporter *had* no safe API to call — no user-side model helps.

### 3.5 Chromium (adjacent large-C++ system, 3 incidents)

**§3.5.1 — Table.** Not gamedev; included because all three are fully
root-caused postmortems of exactly the shapes the taxonomy needs
calibrated, in a codebase with Chromium's sequence-checker discipline (the
closest large-C++ analogue to a runtime affinity/access harness).

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| C1 | [CVE-2019-13720 RCA](https://googleprojectzero.github.io/0days-in-the-wild/0day-RCAs/2019/CVE-2019-13720.html) | WebAudio | Render thread took `process_lock_` before using `reverb_`; the main thread's `SetBuffer(nullptr)` freed `reverb_` *without* the lock — UAF, exploited in the wild | 3 | harness-dev-build | high |
| C2 | [Project Zero: racing MIDI](https://projectzero.google/2016/02/racing-midi-messages-in-chrome.html) | MIDI browser process | A task posted to `send_thread_` captured `base::Unretained(client)`; renderer death destroyed the object before the task ran — virtual call on freed memory | 3 | TSan-CI-only | med |
| C3 | [TSan practice paper §7.1](https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/35604.pdf) | `RefCounted` | Non-atomic refcount incremented/decremented from multiple threads; occasional double-`delete` corrupted the allocator — a months-old "top crasher" with stacks far from the cause; TSan found it in a single run | 7 | TSan-CI-only | high |

**§3.5.2 — Notes.** C1 reads as class 3 but the *mistake* is class-1
shaped: the freeing side skipped the lock the codebase intended
("the lack of `MutexLocker locker(process_lock_)` prior to freeing") — in
grant terms, a write through an instrumented method without a grant,
caught at first access in dev. C2 is the escaped-unowned-pointer shape:
`base::Unretained` is the deliberate opt-out of ownership, and its analogue
(capturing a raw `T*` into a task) is limits.md §2.3 verbatim — TSan-only.
The contrast is the point: default-owned task payloads (our blocks refcount
what they carry) make C2's shape opt-in rather than ambient. C3 is the
corpus's only class-7 row and the canonical symptom-far-from-cause
exhibit: no declaration model sees a torn refcount, and the access-tracking
oracle resolved in one run what months of crash triage could not — the
clearest single argument for keeping TSan CI an advertised part of the
safety story.

### 3.6 Published postmortems and talks (7 incidents)

**§3.6.1 — Table.** First-party writeups of threading bugs in shipped or
in-development games. The vein is real but thin (see §6.5): studios rarely
publish named, dated threading bugs; Factorio and ROBLOX are the clean
incident-style writeups, Naughty Dog's GDC 2015 fiber talk documents bug
classes they actually hit, and Dawson's thread-pool investigation
anonymizes the product.

| ID | Source | System | What happened | Class | Verdict | Conf |
|---|---|---|---|---|---|---|
| P1 | [Gyrling, GDC 2015](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) | Naughty Dog engine (fibers) | Compiler legally caches the TLS base address per function; a job that fiber-switches mid-function resumes on a different worker with the previous thread's TLS pointer | 2 | out-of-scope | high |
| P2 | [Gyrling, GDC 2015](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) | Naughty Dog engine (fibers) | OS mutexes/semaphores/CVs assume the locking thread unlocks; fibers migrate across threads, so the whole primitive family had to be banned and rebuilt on atomics | 2 | prevented-by-construction | med |
| P3 | [Gyrling, GDC 2015](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf) | Naughty Dog engine (fibers) | Jobs submitted from non-worker threads + pure spin locks produced priority-inversion deadlocks (a pinned spinner burns the core the lock holder needs); fixed with adaptive mutexes | 6 | not-caught | med |
| P4 | [Factorio FFF-364](https://factorio.com/blog/post/fff-364) | Factorio belt update | Belt groups update on different threads, but an inserter can be woken by lines owned by different groups — activation order would depend on thread timing, and order defines simulation state: a lockstep desync. Fix: per-thread wake-up lists, merged and dispatched deterministically by the main thread | 5 | prevented-by-construction | high |
| P5 | [Factorio FFF-415](https://www.factorio.com/blog/post/fff-415) | Factorio chunk generation | Immediate chunk generation fanned out across all cores such that the core count changed generation results slightly — different hardware computed different worlds; latent seven years | 5 | not-caught | high |
| P6 | [ROBLOX Mobile postmortem](https://www.gamedeveloper.com/production/postmortem-roblox-mobile) | ROBLOX Mobile (iOS) | Chat data read from the platform text-box control on a non-UI thread; this one bug was ~60% of all post-launch crashes; the fix took five minutes once telemetry surfaced it | 2 | harness-dev-build | med |
| P7 | [Dawson, In Praise of Idleness](https://randomascii.wordpress.com/2012/06/05/in-praise-of-idleness/) | Unnamed game thread pool | `SetEvent` fired while the master still held the pool spinlock; all workers woke and spun, starving the master — 20–40 ms whole-machine stalls with zero work done (one `SetEvent` measured at 12.4 ms) | 6 | prevented-by-construction | med |

**§3.6.2 — Notes.** P1–P3 are engine-infrastructure bugs, and the verdicts
differ by shape. P1 is a toolchain-level thread-identity break — no
user-side declaration model reaches compiler TLS caching; out-of-scope.
P2's hazard family (sync primitives with thread affinity vs. a scheduler
that migrates execution) simply does not exist under a threads-not-fibers
scheduler whose blocking story is retraction and caller participation
rather than mid-function migration — the shape cannot be expressed, hence
prevented, at the price of the fiber model's benefits. P3 is honest
not-caught: a liveness/priority bug in hand-rolled sync — declared access
has no model of it (the library's own `spin_then_block`/`handoff` idle
policies are the packaged form of ND's adaptive-mutex fix, but a user
determined to write spin locks is outside the model). P4 is the strongest
prevented row in the corpus: Factorio's fix — per-producer staging lists
merged in a deterministic order on commit — is structurally the
`Deferred<T>`/journal design (per-recorder staging, deterministic
recorder-order cut, FIFO within), shipped as a primitive rather than
hand-built per subsystem. P5 is the flip side and stays not-caught: when
the *result* of a parallel reduction depends on the decomposition, no
scheduling determinism fixes it (our `parallel_for` has the same property;
determinism of dispatch order is not determinism of partitioning). P6: the
platform UI control is external state, but the app-side mistake — touching
UI-owned state from a worker — is the grant model's bread and butter *if*
the app routes UI access through a guarded proxy; med confidence for
exactly that "if". P7: the wake-path design (signal outside the lock, wake
one, not all) is library-owned in our model — `signal_submit` wakes one
worker per submit and the eventcount parks properly — so the shape is the
library's to get right once, verified by its own benchmarks, not a per-game
hand-rolled hazard. Excluded from the corpus for honesty: Factorio
FFF-340's two "deep desyncs" (Lua table iteration order via a `nil`
placeholder; an unsaved cached unit-group speed) — determinism bugs in a
lockstep sim but single-threaded, not concurrency; and Bungie/Destiny —
no fetchable talk describing a concrete threading bug was verified within
budget, so per the verification rule nothing is reported.

---

## 4. Aggregates

**§4.1 — Corpus size.** 56 verified incidents: Godot 15, Unreal 15,
Unity 6, other OSS engines 10, Chromium 3, postmortems/talks 7. Of these,
7 are class-8/platform-internal, leaving **49 classifiable** incidents —
bugs a user-side concurrency model could in principle address.

**§4.2 — Distribution by taxonomy class.**

| Class | Count | % of 56 | Note |
|---|---|---|---|
| 1 — undeclared/unsynchronized access | 12 | 21.4% | The harness's home turf |
| 2 — thread-affinity violation | 13 | 23.2% | The single largest class |
| 3 — cross-thread lifetime/UAF | 6 | 10.7% | |
| 4 — escaped/stashed reference | 1 | 1.8% | The documented hole — rare in the record (but see §6.1) |
| 5 — ordering/semantic race | 8 | 14.3% | Incl. all determinism/desync incidents |
| 6 — deadlock/blocking misuse | 9 | 16.1% | |
| 7 — atomics/low-level misuse | 1 | 1.8% | Rare in trackers (see §6.1) |
| 8 — engine/platform-internal | 6 | 10.7% | Plus P1's toolchain case verdict-wise |

**§4.3 — Distribution by verdict.**

| Verdict | Count | % of 56 | % of 49 classifiable |
|---|---|---|---|
| harness-dev-build | 21 | 37.5% | 42.9% |
| lifetime-fatal | 4 | 7.1% | 8.2% |
| TS_ENSURE | 1 | 1.8% | 2.0% |
| **caught in dev builds (subtotal)** | **26** | **46.4%** | **53.1%** |
| prevented-by-construction | 12 | 21.4% | 24.5% |
| **caught or prevented (subtotal)** | **38** | **67.9%** | **77.6%** |
| TSan-CI-only | 4 | 7.1% | 8.2% |
| **caught, prevented, or TSan (subtotal)** | **42** | **75.0%** | **85.7%** |
| not-caught | 7 | 12.5% | 14.3% |
| out-of-scope | 7 | 12.5% | — |

**§4.4 — Class × verdict cross-tabulation** (49 classifiable rows; the
out-of-scope column carries the 6 class-8 rows plus P1).

| Class | harness | lifetime | TS_ENSURE | prevented | TSan | not-caught |
|---|---|---|---|---|---|---|
| 1 (12) | 9 | — | — | 2 | 1 | — |
| 2 (12*) | 11 | — | — | 1 | — | — |
| 3 (6) | 1 | 3 | — | 1 | 1 | — |
| 4 (1) | — | — | — | — | 1 | — |
| 5 (8) | — | — | — | 3 | — | 5 |
| 6 (9) | — | 1 | 1 | 5 | — | 2 |
| 7 (1) | — | — | — | — | 1 | — |

*Class 2 has 13 rows; P1 (out-of-scope) is excluded from this table.

**§4.5 — The bottom line, stated honestly.** Of 49 classifiable incidents:
**53% fall in classes the harness stack catches in a dev build** (43%
harness fatal at first undeclared access, 8% lifetime fatals, 2%
`TS_ENSURE`); **a further 24% are prevented by construction** — the bug
shape cannot be expressed under derived edges, pipe arbitration,
canonical-order acquisition, caller participation/retraction, or owned
task payloads — for **78% caught-or-prevented**; **8% are TSan-only**
(escaped references, library-internal machinery, torn low-level access),
which the Linux TSan CI leg covers, bringing the stack total to **86%**;
and **14% are uncatchable by any access-declaration model** — semantic
ordering races and liveness bugs, concentrated exactly where limits.md
§3.1–3.2 predicted. Including the platform-internal rows, one in eight
real-world incidents (12.5%) is beyond *any* user-side model — the ceiling
any safety story should be honest about.

**§4.6 — Reading the two headline classes.** Classes 1+2 together are
44.6% of the corpus — and 20 of those 25 rows verdict as
harness-dev-build. This is the validation of §1.2: the mistake developers
actually make most — touching shared state without the protection the
codebase intended, in its plain form or its affinity spelling — is
precisely the mistake the harness exists to catch. Conversely the
harness's documented sharpest hole, the escaped reference (class 4),
appears **once** in 56 public incidents; the caveat in §6.1 (escapes are
underdiagnosed, not necessarily rare) applies, but the recorded
distribution does not support treating that hole as disqualifying.

---

## 5. Cross-domain base rates (proxy evidence, not gamedev)

**§5.1 — Why these are here.** The corpus above is what game/engine
trackers *record*. Three larger studies from adjacent domains provide
base rates against survivorship effects; they are labeled proxy and
weighted as corroboration only.

**§5.2 — Uber's Go race corpus (~2,000 races, 6 months, ~50M LOC
monorepo).** [Blog](https://www.uber.com/en-IN/blog/dynamic-data-race-detection-in-go-code/)
+ [arXiv 2204.00764](https://arxiv.org/abs/2204.00764). Their
language-agnostic root-cause table: missing/partial locking 470,
thread-safe API contract violations 369, missing/incorrect atomics 40,
mutating globals 24, other 63 — i.e. the two dominant categories (~87% of
categorized causes) are class-1/class-2 mass, mirroring the corpus's 45%.
Go-specific capture patterns (accidental capture-by-reference in
goroutines 121, shared `err` 50, loop variable 48) are the *escape*
family: a closure silently aliasing enclosing state — Go's spelling of
class 4, and far more common there than in the gamedev record, supporting
§6.1's suspicion that class 4 is underdiagnosed rather than rare.
Deployment posture: periodic detector sweeps, not production enforcement —
the model limits.md §4.1 adopts for TSan.

**§5.3 — Lu et al., ASPLOS 2008 (105 real concurrency bugs from MySQL/
Apache/Mozilla/OpenOffice).** [Paper](https://pages.cs.wisc.edu/~shanlu/paper/asplos122-lu.pdf).
74 non-deadlock / 31 deadlock. Of non-deadlock bugs, 97% are atomicity or
order violations (atomicity 51/74, order 24/74) — atomicity violations map
mostly to class 1 (a protection the code intended but didn't hold across
the window), order violations to class 5. Three findings bear directly on
this library's design bets: **96% of all bugs manifest with ≤2 threads**
and **66% of non-deadlock bugs involve a single variable** — supporting
per-object-instance declaration granularity as the right unit; and **97%
of deadlocks involve ≤2 resources** — exactly the shape canonical-order
acquisition eliminates. Their fix data (only 27% of non-deadlock bugs
were fixed by adding/changing locks) also confirms that "add a lock" is
not the dominant real-world remedy — restructuring is, which is the
`Versioned`/split-state posture of limits.md §4.2.

**§5.4 — TSan in practice (Serebryany & Iskhodzhanov, WBIA 2009).**
[Paper](https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/35604.pdf).
A few dozen races found and fixed in Chromium; the headline case is C3
above. The paper gives no harmful-vs-benign ratio but notes the most
frequent race is the simplest (unsynchronized access to a built-in-type
variable — class-1 shape again) and that overhead ran 20–50× on Google
tests — the quantitative case for the two-tier posture: a ~1 ns always-on
harness for the dominant classes, a heavyweight periodic oracle for the
rest.

**§5.5 — Rust, reused from research-deepdive.md §13.3 (Qin et al., PLDI
2020).** Even under a total compile-time model: 25 of 41 non-blocking
concurrency bugs were in *safe* code, and all 59 blocking bugs
(deadlocks) were in safe code — 30 of them double-locks induced by the
ownership model itself. The transfer to this corpus: a sound
declaration/ownership layer relocates failure into the semantic and
liveness classes (5 and 6) rather than eliminating it — which is exactly
where this corpus's not-caught rows sit. No stronger evidence exists that
the 14% residue is a property of the *problem*, not of this particular
implementation.

---

## 6. Threats to validity

**§6.1 — Survivorship and diagnosis bias.** Trackers contain *diagnosed*
bugs. Races that shipped and were never root-caused — the "random crash in
build" residue — are underrepresented, and they skew toward the classes
hardest to diagnose: escaped references, torn access, engine internals
(N2 is the visible tip: build-only, five-minutes-to-five-hours, defeated
stress testing, unresolved). This inflates the harness-caught fraction to
an unknown degree; the Uber data (§5.2), where a total oracle found
capture/escape races in the hundreds, suggests class 4's true rate is well
above its 1.8% here. The honest reading: the *caught* percentages are
upper-ish bounds; the *class ranking* (1+2 dominant, 5+6 substantial) is
robust across every vein and both proxy studies.

**§6.2 — Counterfactual verdicts.** Every verdict is a judgment about a
port that never happened (§1.4). The prevented-by-construction rows are
the most exposed: 4 of 12 are high-confidence (G5, B4, N5, P4 — where the
shipped fix is structurally identical to the library's mechanism), 7 med,
1 low. Sensitivity: discarding every non-high prevented row entirely
still leaves caught-plus-prevented at 30/49 = 61% — the validation
conclusion survives the pessimistic bound.

**§6.3 — Taxonomy ambiguity.** Many incidents straddle classes (C1 is a
lifetime symptom of a class-1 mistake; every affinity violation is also
unsynchronized access with extra structure; G14 is a deadlock caused by a
lifetime bug). Classification follows the *developer's mistake*, not the
symptom; the cross-tab (§4.4) is the check that no single reclassification
would move the bottom line by more than ~2 points.

**§6.4 — Proxy transfer.** The Go/Rust/Chromium numbers come from
server/browser codebases with different concurrency idioms (channels,
sequences, ownership). They are used only to corroborate class *ranking*
and to bound §6.1 — never summed into the gamedev percentages.

**§6.5 — Vein coverage.** Unity's issue tracker was effectively
unfetchable post-migration, so Unity rests on forum threads and one
shipped-game tracker — likely under-sampling Unity's class-1/5 incidents
(which its own safety system converts into in-editor rejections that never
become bug reports; see §3.3.2 — itself evidence for the approach, but it
thins the record). First-party postmortems of named shipped threading bugs
are rare; Destiny yielded nothing verifiable. Godot is the deepest vein
and may overweight engine-internal idioms (servers, command queues)
relative to game-code bugs.

**§6.6 — Dev-build coverage dependence.** A harness verdict means "caught
*if* a checked build exercises the path." The UE async-loading rows
(U3–U6) shipped precisely because the editor and the packaged game
exercised different paths — the same gap applies to this library
(limits.md §2.6). The corpus measures class coverage, not path coverage;
nothing here weakens the standing advice to run dev-build soak tests and
the stress harness.

---

## §final — Conclusions

**§F.1 — The validation holds.** Of 49 classifiable real-world incidents,
53% fall in classes the stack catches in dev builds and a further 24%
cannot be expressed under the model at all — 78% caught-or-prevented,
86% with the TSan CI leg, against a 14% residue that limits.md already
concedes and Qin et al. show survives even a total compile-time model
(§5.5). The approach is validated by dominance, exactly per §1.2 — not by
completeness, which no system in the record achieves.

**§F.2 — Affinity is the biggest class, and per-API guards are the wrong
tool for it.** Class 2 is 23% of the corpus, and 11 of its 12 classifiable
rows verdict harness-dev-build. The failures came from *incomplete guard
lists*: UE's `check(IsInGameThread())` sites catch only where someone
wrote the check, and Unity's unguarded `FindObjectsOfType` became 60% of a
shipped game's crashes (N1, P6). A uniform per-object grant check has no
per-API list — but its analogue hole is the uninstrumented method
(limits.md §2.1). Action: the TODO 1.4 clang-tidy completeness check is
not a nice-to-have; it is what keeps this library from re-growing Unity's
hole one level down.

**§F.3 — The deadlock story is prevention, and the corpus proves the
shape is universal.** 5 of 9 class-6 rows are prevented-by-construction,
and the pool-exhaustion fork-join deadlock was independently rediscovered
in Godot (G5) and Bevy (B4) — both fixed with ad-hoc forms of what
retraction + caller participation package. Lu's data (97% of deadlocks
involve ≤2 resources, §5.3) says canonical-order acquisition covers
essentially the whole real-world deadlock space *for state inside the
model*. Action: keep retraction and ordered acquisition load-bearing in
the public pitch; they are the best-evidenced structural claims the
library has.

**§F.4 — Bevy's declaration-layer bugs are the argument for the runtime
pipe.** B1/B3: accesses correctly declared, conflict computation wrong,
compile-time guarantee silently void. This library's derived schedule and
its runtime pipe arbitration are independent layers — a mis-derived edge
still meets a writer-exclusive acquire at runtime. Action: never let an
optimization make runtime acquisition *trust* `compile()`'s disjointness
proof; the redundancy is the defense the record says is needed.

**§F.5 — The residue is ordering, and determinism infrastructure is the
right spend.** 5 of the 7 not-caught rows are class-5 semantic ordering
(G3, U1, U8, U15, P5). Nothing catches these — but P4 shows Factorio
hand-building exactly the deterministic staged-commit the journal layer
ships (per-recorder FIFO, deterministic cut), and the pipe's FIFO plus
deterministic declaration order make cross-access reasoning stable where
comparable systems are timing-dependent. Action: keep documenting that
declarations order *accesses*, not *answers* (limits.md §3.1 /
ordering-ambiguity.md), and keep the deterministic-by-default properties —
they convert the uncatchable class from Heisenbug to reproducible bug,
which the Factorio seven-year row (P5) shows is the difference that
matters.

---

## Source index

**Godot** — [#95616](https://github.com/godotengine/godot/issues/95616),
[#63461](https://github.com/godotengine/godot/issues/63461),
[#96115](https://github.com/godotengine/godot/issues/96115),
[#54099](https://github.com/godotengine/godot/issues/54099),
[PR #77143](https://github.com/godotengine/godot/pull/77143),
[#83677](https://github.com/godotengine/godot/issues/83677),
[#109554](https://github.com/godotengine/godot/issues/109554),
[#53496](https://github.com/godotengine/godot/issues/53496),
[#101894](https://github.com/godotengine/godot/issues/101894),
[#98577](https://github.com/godotengine/godot/issues/98577),
[#80581](https://github.com/godotengine/godot/issues/80581),
[#84046](https://github.com/godotengine/godot/issues/84046),
[#109471](https://github.com/godotengine/godot/issues/109471),
[#71216](https://github.com/godotengine/godot/issues/71216),
[#61650](https://github.com/godotengine/godot/issues/61650); related:
[PR #72491](https://github.com/godotengine/godot/pull/72491) (same root
cause as #53496, folded).

**Unreal Engine** — [UE-259543](https://issues.unrealengine.com/issue/UE-259543),
[UE-191796](https://issues.unrealengine.com/issue/UE-191796),
[UE-152571](https://issues.unrealengine.com/issue/UE-152571),
[UE-106091](https://issues.unrealengine.com/issue/UE-106091),
[UE-77756](https://issues.unrealengine.com/issue/UE-77756),
[UE-222366](https://issues.unrealengine.com/issue/UE-222366),
[UE-228253](https://issues.unrealengine.com/issue/UE-228253),
[UE-240302](https://issues.unrealengine.com/issue/UE-240302),
[UE-223522](https://issues.unrealengine.com/issue/UE-223522),
[UE-291472](https://issues.unrealengine.com/issue/UE-291472),
[UE-217692](https://issues.unrealengine.com/issue/UE-217692),
[UE-224491](https://issues.unrealengine.com/issue/UE-224491),
[UE-230511](https://issues.unrealengine.com/issue/UE-230511),
[UE-168068](https://issues.unrealengine.com/issue/UE-168068),
[async-texture GC forum thread](https://forums.unrealengine.com/t/async-texture2d-import-garbage-collection-crash/475276).

**Unity** — [daggerfall-unity #2355](https://github.com/Interkarma/daggerfall-unity/issues/2355),
[Entities Graphics build crash](https://discussions.unity.com/t/random-crash-in-entities-graphics-systems-in-build/1511889),
[2018.3 iOS deadlock](https://discussions.unity.com/t/main-thread-deadlock-with-unity-2018-3-1f1/728813),
[main-thread exception thread](https://discussions.unity.com/t/solved-bizarre-error-unityexception-get_gameobject-can-only-be-called-from-the-main-thread/707577),
[job dependency bypass thread](https://discussions.unity.com/t/invalidoperationexception-the-previously-scheduled-job-reads-from-the-nativearray/779183),
[NativeArray standalone crash](https://discussions.unity.com/t/native-arrays-cause-crashes-in-standalone-builds-2019-2a14/741968).

**Bevy / O3DE / Flax / Stride** — [bevy #4657](https://github.com/bevyengine/bevy/issues/4657),
[bevy PR #14615](https://github.com/bevyengine/bevy/pull/14615),
[bevy PR #17616](https://github.com/bevyengine/bevy/pull/17616),
[bevy #5743](https://github.com/bevyengine/bevy/issues/5743),
[o3de #4267](https://github.com/o3de/o3de/issues/4267),
[o3de #18623](https://github.com/o3de/o3de/issues/18623),
[o3de #15149](https://github.com/o3de/o3de/issues/15149),
[flax #116](https://github.com/FlaxEngine/FlaxEngine/issues/116),
[flax #4041](https://github.com/FlaxEngine/FlaxEngine/issues/4041),
[stride PR #2722](https://github.com/stride3d/stride/pull/2722).

**Chromium / security** — [CVE-2019-13720 root-cause analysis](https://googleprojectzero.github.io/0days-in-the-wild/0day-RCAs/2019/CVE-2019-13720.html),
[Project Zero: Racing MIDI messages in Chrome](https://projectzero.google/2016/02/racing-midi-messages-in-chrome.html).

**Postmortems / talks** — [Gyrling, "Parallelizing the Naughty Dog Engine
Using Fibers", GDC 2015 (slides)](https://media.gdcvault.com/gdc2015/presentations/Gyrling_Christian_Parallelizing_The_Naughty.pdf),
[Factorio FFF-364](https://factorio.com/blog/post/fff-364),
[Factorio FFF-415](https://www.factorio.com/blog/post/fff-415),
[Factorio FFF-340](https://factorio.com/blog/post/fff-340) (examined,
excluded — single-threaded determinism bugs),
[ROBLOX Mobile postmortem](https://www.gamedeveloper.com/production/postmortem-roblox-mobile),
[Dawson, "In Praise of Idleness"](https://randomascii.wordpress.com/2012/06/05/in-praise-of-idleness/).

**Base-rate studies (proxy)** — [Uber: Dynamic Data Race Detection in Go
Code](https://www.uber.com/en-IN/blog/dynamic-data-race-detection-in-go-code/),
[Tu/Liu et al., "A Study of Real-World Data Races in Golang", arXiv
2204.00764](https://arxiv.org/abs/2204.00764),
[Lu et al., "Learning from Mistakes", ASPLOS 2008](https://pages.cs.wisc.edu/~shanlu/paper/asplos122-lu.pdf),
[Serebryany & Iskhodzhanov, "ThreadSanitizer: data race detection in
practice", WBIA 2009](https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/35604.pdf),
[Qin et al., PLDI 2020 (via research-deepdive.md §13.3)](https://songlh.github.io/paper/rust-study.pdf).
