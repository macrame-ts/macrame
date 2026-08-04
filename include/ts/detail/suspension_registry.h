#pragma once

// The deadlock report's third tier (docs/waiting-rule-policy.md §7): a registry of every
// LIVE suspension -- which task is suspended, what it awaits, and what it holds. The
// quiescence net (`Rule::deadlock_net`) can say "nothing can make progress"; this says who
// was stuck on what, which is the difference between a report and a debugging session.
//
// It exists because tier 2 (the circular-wait edge registry) only records suspensions
// that hold a grant. The shape it misses is the two-hop cycle: task N holds G1 and awaits foreign
// task T, while T awaits G1 -- T holds nothing, so tier 2 never sees it, and neither
// suspension is on a pipe from N's side.
//
// Cost, and why it is on by default outside shipping. Per suspension: one thread-local read
// and a doubly-linked insert under a SHARDED mutex, plus the same at resume. Sharding is
// what keeps it flat -- a single global mutex touched by every suspend and every resume is
// a scaling hazard, not a constant one, and this is a library whose whole point is that
// many workers suspend at once. A record is inserted by the suspending thread and removed by
// the RESUMING thread (a coroutine resumes wherever its awaited work settled), so the shard
// mutex is genuinely needed -- the tempting lock-free per-thread list does not apply here.
// Records live inside the awaiter, which lives inside the coroutine frame, so nothing is
// allocated.
//
// Gated by `TS_SUSPENSION_REGISTRY` (ts/rules.h). With it off, every type and call below
// compiles to nothing.

#include "ts/guarded.h"
#include "ts/named.h"
#include "ts/rules.h"
#include "ts/task.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ts::detail
{

#if TS_SUSPENSION_REGISTRY

// One live suspension. Embedded in the awaiter; never heap-allocated.
struct Suspension_record
{
    Suspension_record* prev = nullptr;
    Suspension_record* next = nullptr;
    unsigned shard = 0;

    Named task{ nullptr };            // the suspended task
    Named awaited_task{ nullptr };    // set when awaiting a task
    const Pipe* awaited_pipe = nullptr;   // set when awaiting an object's turn/guard
    bool joining_scope = false;       // set when awaiting the frame's own children

    // Held pipe-backed grants, as their epoch sources (the report resolves them to names --
    // the pipes outlive the report by construction: we are wedged, nothing is being freed).
    static constexpr int max_held = 4;
    const std::atomic<std::uint64_t>* held[max_held] = {};
    int held_count = 0;
};

// Shards. A fixed array rather than a per-thread registration list: no teardown ordering
// question, and the fatal walks all of them once, while dying.
inline constexpr unsigned suspension_shard_count = 64;

struct alignas(64) Suspension_shard
{
    std::mutex mutex;
    Suspension_record* head = nullptr;
};

inline Suspension_shard suspension_shards[suspension_shard_count];

// Shard by the RECORD's address, not by the suspending thread. A record is linked by the
// suspending thread and unlinked by the RESUMING one -- a coroutine resumes wherever its
// awaited work settled -- so a per-thread shard would put the two halves of every
// suspension on different threads hammering one mutex and one cache line. Keying on the
// record makes both halves hit the same shard while distinct suspensions spread out; the
// record lives in a coroutine frame, so addresses are naturally spread by the allocator.
// Measured: this is what removes the multi-worker penalty (numbers in TODO 6.13).
inline unsigned suspension_shard_of(const void* record) noexcept
{
    auto bits = reinterpret_cast<std::uintptr_t>(record);
    return static_cast<unsigned>((bits >> 6) % suspension_shard_count);
}

// Fill in what the current context holds. Cheap: the access context is at most eight
// entries and this runs only on a genuine suspension.
inline void suspension_capture_held(Suspension_record& record) noexcept
{
#if TS_SAFETY_CHECKS
    if (current_access == nullptr)
        return;
    current_access->for_each_epoch([&record](const std::atomic<std::uint64_t>* epoch)
    {
        if (record.held_count < Suspension_record::max_held)
            record.held[record.held_count++] = epoch;
    });
#else
    (void)record;
#endif
}

inline void suspension_link(Suspension_record& record) noexcept
{
    record.task = current_task ? current_task->name : Named{ nullptr };
    suspension_capture_held(record);
    record.shard = suspension_shard_of(&record);
    Suspension_shard& shard = suspension_shards[record.shard];
    std::scoped_lock lock(shard.mutex);
    record.prev = nullptr;
    record.next = shard.head;
    if (shard.head != nullptr)
        shard.head->prev = &record;
    shard.head = &record;
}

// Unlinks on the RESUMING thread, which is generally not the one that linked -- hence the
// record remembering its shard.
inline void suspension_unlink(Suspension_record& record) noexcept
{
    Suspension_shard& shard = suspension_shards[record.shard];
    std::scoped_lock lock(shard.mutex);
    if (record.prev != nullptr)
        record.prev->next = record.next;
    else if (shard.head == &record)
        shard.head = record.next;
    if (record.next != nullptr)
        record.next->prev = record.prev;
    record.prev = nullptr;
    record.next = nullptr;
}

// Number of live suspensions. Advisory (it races the workers), and reported by the deadlock
// net even before the per-record detail, because "6 tasks are suspended" is already a
// diagnosis.
inline int suspension_count() noexcept
{
    int count = 0;
    for (Suspension_shard& shard : suspension_shards)
    {
        std::scoped_lock lock(shard.mutex);
        for (const Suspension_record* record = shard.head; record != nullptr; record = record->next)
            ++count;
    }
    return count;
}

// RAII bracket for an awaiter: link on construction, unlink on destruction. The awaiters
// use the explicit calls instead (their link and unlink live in different functions --
// `await_suspend` and `await_resume`), so this is for straight-line uses.
class Suspension_scope
{
public:
    Suspension_scope() noexcept { suspension_link(record_); }
    ~Suspension_scope() { suspension_unlink(record_); }

    Suspension_scope(const Suspension_scope&) = delete;
    Suspension_scope& operator=(const Suspension_scope&) = delete;

    Suspension_record& record() noexcept { return record_; }

private:
    Suspension_record record_;
};

#else   // TS_SUSPENSION_REGISTRY == 0 -- everything compiles away, including the storage.

struct Suspension_record
{
};

inline void suspension_link(Suspension_record&) noexcept {}
inline void suspension_unlink(Suspension_record&) noexcept {}
inline int suspension_count() noexcept { return -1; }   // -1 = "not tracked in this build"

#endif

} // namespace ts::detail
