#pragma once
//
// DESIGN SKETCH — illustrative pseudo-C++. Not wired into the build, does not
// compile. Exploring the public surface of Thread_safe<T> + Static_task_graph
// + dynamic async. Implementation is elided (comments mark where it lives).
//

#include <concepts>
#include <initializer_list>
#include <type_traits>

namespace ts
{

// ===========================================================================
// Task<R> — handle to an async result; composable via continuations.
// ===========================================================================
template<typename R>
class Task
{
public:
    // Attach a continuation that runs after this task completes, on a worker.
    // Returns a task for the continuation's result, so chains compose.
    template<typename Fn>
    auto then(Fn&& fn) -> Task<std::invoke_result_t<Fn, R>>;

    // Pure ordering edges (no data passed) — kind-(2) dependencies. Distinct
    // from then(), which chains a continuation on the result. Prerequisite
    // result types are irrelevant, hence variadic over them.
    // build.after(sim); // build runs after sim
    template<typename... Rs> Task& after(const Task<Rs>&... prerequisites);
    template<typename... Rs> Task& before(const Task<Rs>&... successors);

    bool is_ready() const;

    // Block until ready. Discouraged inside a worker task (stalls / can deadlock):
    // an optional strict mode asserts if called from within a task. Prefer then().
    R get();
};
// Task<void>: then(fn) takes no argument; otherwise identical.

// ===========================================================================
// Thread_safe<T> — the only sanctioned way to touch a T across threads.
//
// You never receive a bare T&. You hand a functor to async(); the scheduler
// invokes it with a reference once non-conflicting access has been granted.
// Access mode is DEDUCED from the functor's parameter const-ness:
//     functor(T&)        -> exclusive (write) access
//     functor(const T&)  -> shared    (read)  access
// ===========================================================================
template<typename T>
class Thread_safe
{
public:
    // constructs the wrapped T in place
    template<typename... Args>
    explicit Thread_safe(Args&&... args);

    // identity matters; non-copyable/movable
    Thread_safe(const Thread_safe&) = delete;
    Thread_safe& operator=(const Thread_safe&) = delete;

    // --- dynamic, single-object access ------------------------------------
    // write: functor invocable with T&
    template<typename Fn> requires std::invocable<Fn, T&>
    auto async(Fn&& fn) -> Task<std::invoke_result_t<Fn, T&>>;

    // read: functor invocable with const T&
    template<typename Fn> requires std::invocable<Fn, const T&>
    auto async(Fn&& fn) const -> Task<std::invoke_result_t<Fn, const T&>>;

private:
    T instance_;
    // Per-instance identity + access-scheduling state (the serial/RW gate) lives here.
    // Only the scheduling machinery may reach instance_:
    friend class Static_task_graph;
    template<typename Fn, typename... Us>
    friend auto async(Fn&&, Us&...); // multi-object entry (below)
};

// ===========================================================================
// Multi-object dynamic access (atomic over the whole set).
//
// NOTE: this cannot cleanly be a static member of Thread_safe (it's a class
// template — `Thread_safe<?>::async` has no natural <?>). A free function reads
// better and unifies with the single-object case: async(fn, a) == a.async(fn).
//
// Per-object mode is deduced from each functor parameter's const-ness:
//     async([](Physics& p, const Nav& n){ ... }, physics, nav);  // p:write, n:read
//
// The scheduler acquires the entire set in a canonical order (by instance id),
// never incrementally — so multi-object access cannot deadlock.
// ===========================================================================
template<typename Fn, typename... Ts>
auto async(Fn&& fn, Thread_safe<Ts>&... objs)
    -> Task<std::invoke_result_t<Fn, /* per-arg const-qualified */ Ts&...>>;

// ===========================================================================
// Static_task_graph — declare nodes + access sets + ordering up front, compile
// once, execute many times. Two edge kinds:
//   (1) access-based: inferred from which Thread_safe<> each node touches and
//       how (R/W). The graph serializes conflicting access, parallelizes the
//       rest. The user does NOT hand-order these.
//   (2) order-based: explicit prerequisites between node handles.
// ===========================================================================
class Static_task_graph
{
public:
    // Add a node: functor + the Thread_safe<> instances it accesses. Returns a
    // normal Task — order edges are expressed with Task::after/before, the same
    // handle dynamic tasks use. (Execution is deferred to execute(), so wiring
    // order edges after creation is fine here.)
    // Access mode per object is deduced from the functor's parameter const-ness.
    //   add_node([](Physics& p, const Nav& n){ ... }, physics, nav);
    // (name candidates: add_node / add_task / emplace / node)
    template<typename Fn, typename... Ts>
    auto add_node(Fn&& fn, Thread_safe<Ts>&... access)
        -> Task<std::invoke_result_t<Fn, /* per-arg const-qualified */ Ts&...>>;

    // Resolve access conflicts into ordering, detect cycles, and report
    // ambiguities (two nodes with conflicting access and no order edge between
    // them — the scheduler can't know which must run first). Bevy-style.
    void compile();

    // Run the compiled graph; returns a completion handle. Re-runnable: build
    // and compile() once, then execute() repeatedly — once-off use and a
    // per-tick engine loop are the same path. Node functors persist across runs
    // (they capture the owning systems, not per-run state); each execute()
    // installs fresh per-task Access_contexts and produces a new completion handle.
    Task<void> execute(class Scheduler&);

private:
    // nodes, per-node access table (instance id -> R/W), explicit order edges
};

// ===========================================================================
// SAFETY HARNESS — runtime access verification (dev / checked builds only).
//
// Division of responsibility:
//   * the SCHEDULER guarantees no two CONFLICTING declared tasks run at once.
//   * the HARNESS catches a task touching an instance it did NOT declare:
//     escaped/stored references, access from a raw (non-task) thread, or use
//     after the granting task ended. This is what turns "C++ can't prevent a
//     reference escaping" into a DETECTABLE runtime fault.
//
// Mechanism: the scheduler installs a thread-local Access_context before each
// task body and restores it after. Every public method of a wrapped
// Thread_unsafe_T calls the harness first; it looks `this` up in the context.
// ===========================================================================

enum class Access { read_only, read_write };

// Per-task permission set. Small by design: most tasks touch 1-3 instances, so
// an inline array + linear scan beats a hash set. Identity of an instance is
// its address (== &Thread_safe<T>::instance_, which is also a method's `this`).
class Access_context
{
public:
    // grants(x, read_only) is true if the context holds read_only OR read_write on x.
    bool grants(const void* instance, Access mode) const noexcept;
    bool is_exclusive() const noexcept; // "grant everything" (single-threaded phases)
private:
    // inline (address, mode) pairs; built by the scheduler from a task's
    // declared Thread_safe<> set (single, multi, or a static-graph node).
};

namespace detail
{
    // null when this thread is not executing a task -> any guarded access faults.
    extern thread_local const Access_context* current_access;

    [[noreturn]] void access_violation(const char* type_name, Access mode) noexcept;
}

// The harness. Overloaded on this-const-ness so R/W is deduced from the
// method's own const-ness (same rule as async functors):
//   non-const method -> this is T*       -> needs write
//   const     method -> this is const T* -> needs read
template<typename T>
inline void access_check(T* self) noexcept
{
    const Access_context* ctx = detail::current_access;
    if (!ctx || !ctx->grants(self, Access::read_write))
        detail::access_violation(typeid(T).name(), Access::read_write);
}
template<typename T>
inline void access_check(const T* self) noexcept
{
    const Access_context* ctx = detail::current_access;
    if (!ctx || !ctx->grants(self, Access::read_only))
        detail::access_violation(typeid(T).name(), Access::read_only);
}

// Drop at the top of EVERY public method of a Thread_unsafe_T.
// Compiles to nothing when checks are off (shipping builds).
#if TS_SAFETY_CHECKS
    #define TS_CHECK_ACCESS() ::ts::access_check(this)
#else
    #define TS_CHECK_ACCESS() ((void)0)
#endif

// Installed by the worker around a task body. RAII save/restore, so nested /
// inline task execution and continuations each see their own context.
class Access_scope
{
public:
    explicit Access_scope(const Access_context& ctx) noexcept
        : prev_(detail::current_access) { detail::current_access = &ctx; }
    ~Access_scope() { detail::current_access = prev_; }
    Access_scope(const Access_scope&) = delete;
private:
    const Access_context* prev_;
};

// Legitimate access outside the task system (init, teardown, tests): installs
// an exclusive context granting everything on this thread for its lifetime.
class Exclusive_access_scope { /* sets an is_exclusive() context */ };

} // namespace ts


// ===========================================================================
// USAGE SKETCH
// ===========================================================================
#if 0
using namespace ts;

// A wrapped Thread_unsafe_T encapsulates its state and guards every public
// method. R/W is deduced from each method's const-ness, matching the context.
class Physics
{
public:
    // non-const -> read_write
    void simulate()
    {
        TS_CHECK_ACCESS();
        // ...mutate...
    }

    // const -> read_only
    Vec3 gravity() const
    {
        TS_CHECK_ACCESS();
        return g_;
    }

private:
    Vec3 g_;
};

Thread_safe<Physics> physics;
Thread_safe<Nav_mesh> nav;
Thread_safe<Render_scene> render;

// --- dynamic, single object ---
Task<float> q = nav.async([](const Nav_mesh& n){ return n.nearest(player); }); // read
q.then([](float d){ /* runs after q, on a worker */ });
physics.async([](Physics& p){ p.apply_impulse(...); }); // write

// --- dynamic, multiple objects (atomic across both) ---
async([](Physics& p, const Nav_mesh& n){ p.constrain_to(n); }, physics, nav);

// --- static graph ---
Static_task_graph g;
auto sim = g.add_node([](Physics& p){ p.simulate(); }, physics);
auto build = g.add_node([](const Physics& p, Nav_mesh& n){ n.rebuild_from(p); }, physics, nav);
auto draw = g.add_node([](const Physics& p, const Nav_mesh& n, Render_scene& r){ r.extract(p, n); }, physics, nav, render);

build.after(sim); // explicit order edge (kind 2)

// access edges (kind 1) inferred at compile():
//   sim: writes physics
//   build: reads physics, writes nav
//   draw: reads physics, reads nav, writes render
// -> writer-before-reader on physics orders sim < build, sim < draw; nav writer
//    build < reader draw; render write in draw is exclusive. compile() also flags
//    that sim and draw both touch physics with no order edge if that were ambiguous.

g.compile(); // once

// once-off:
g.execute(scheduler);

// or as a per-tick engine loop — same graph, re-run every frame:
while (running)
    g.execute(scheduler); // each run installs fresh Access_contexts
#endif
