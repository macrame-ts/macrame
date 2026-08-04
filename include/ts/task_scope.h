#pragma once

// Explicit task scope (docs/coroutine-first.md §4.3) -- the advanced-shapes companion to
// the implicit per-frame scope: several independent scopes, or a scope handed to helpers.
// `scope.launch(fn)` launches eagerly (inheriting the caller's access grant, like
// `ts::launch`) and records the child; `co_await scope.join()` awaits all recorded
// children and empties the scope. Children do NOT gate the owner's completion (that is the
// implicit scope's / `ts::nested`'s job) -- the explicit join is the only gate, which is
// why destroying a scope with unjoined children is fatal (lost children, the analogue of
// the journal's lost-writes fatal). Single-owner, not thread-safe; confine a scope to one
// coroutine/task.

#include "ts/task.h"
#include "ts/coroutine_support.h"

#include <utility>
#include <vector>

namespace ts
{

class Task_scope
{
public:
    Task_scope() = default;

    Task_scope(const Task_scope&) = delete;
    Task_scope& operator=(const Task_scope&) = delete;

    ~Task_scope()
    {
#if TS_SAFETY_CHECKS
        if (!children_.empty())
            ts::fatal("Task_scope destroyed with unjoined children -- co_await scope.join() "
                "before the scope exits");
#endif
    }

    // Launch a child into this scope (eager, grant-inheriting). Returns the task handle for
    // optional individual awaiting; the child stays recorded for `join()` either way. Unlike
    // the detached `ts::launch` (which inherits nothing), a scope child inherits the caller's
    // grant: the child is structurally gated by `co_await scope.join()`, so the grant provably
    // outlives it -- the same soundness condition as `ts::nested` (docs/coroutine-first.md §2).
    template<typename Fn>
        requires detail::Task_body<Fn>
    auto launch(Fn&& fn, Launch_options opts = {})
    {
        auto t = detail::build_bare_task<true>(std::forward<Fn>(fn), std::move(opts),
            std::source_location::current());
        children_.push_back(detail::core_of(t));
        return t;
    }

    // Await every recorded child; empties the scope (a later `join()` covers later launches).
    detail::Join_awaiter join()
    {
        std::vector<detail::Task_ptr> joined = std::move(children_);
        children_.clear();
        return detail::Join_awaiter(std::move(joined));
    }

    bool empty() const noexcept { return children_.empty(); }

private:
    std::vector<detail::Task_ptr> children_;
};

} // namespace ts
