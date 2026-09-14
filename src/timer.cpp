#include "ts/timer.h"
#include "ts/coroutine_support.h"
#include "ts/fatal.h"
#include "ts/guarded.h"   // global_scheduler

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Windows 10 1803+. Without it a waitable timer, like a condition variable timeout, wakes on
// the system timer tick (~15.6 ms by default), which is a whole period of a 60 Hz tick.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace ts
{
namespace
{

using Clock = std::chrono::steady_clock;

// One armed wait. Owned by its heap entry; the cancel callback it carries captures a raw
// pointer to it, so the callback's lifetime is nested inside the state's.
struct Sleep_state : detail::Ref_counted<Sleep_state>
{
    detail::Task_ptr block;                 // the returned task's block
    Priority priority = Priority::normal;   // the delivery task's priority
    bool live = true;                       // neither fired nor cancelled; guarded by the timer mutex
    // Held while live: the wakeup comes from a thread the scheduler does not run, so the
    // deadlock net must not read a quiescent pool as a deadlock (`External_wait`).
    std::unique_ptr<External_wait> outstanding;
    std::optional<Cancel_callback> on_cancel;
};

struct Entry
{
    Clock::time_point deadline;
    std::uint64_t serial;   // FIFO among equal deadlines
    detail::Ref_ptr<Sleep_state> state;
};

// Heap order for `std::push_heap`/`pop_heap`: the earliest deadline at the front.
struct Later
{
    bool operator()(const Entry& a, const Entry& b) const noexcept
    {
        return a.deadline != b.deadline ? a.deadline > b.deadline : a.serial > b.serial;
    }
};

// A wakeup to deliver once the timer mutex is released.
struct Wakeup
{
    detail::Task_ptr block;
    Priority priority;
    std::unique_ptr<External_wait> outstanding;   // released only after the delivery is queued
};

// Settle `block` from a task at `priority`, never inline: settling resumes awaiting coroutines
// on the settling thread, and neither the timer thread nor a cancelling thread may run them.
void deliver(Wakeup wakeup, bool cancelled)
{
    (void)ts::launch([block = std::move(wakeup.block), cancelled]() mutable
    {
        if (cancelled)
            block->cancel();
        else
            block->complete();
    }, { .priority = wakeup.priority, .name = "ts::sleep wakeup" });
}

// The deadline keeper: a min-heap of armed sleeps and the one thread that waits for its head.
class Timer_service
{
public:
    Timer_service()
    {
#if defined(_WIN32)
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer_ == nullptr)
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);   // pre-1803 fallback
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (timer_ == nullptr || wake_ == nullptr)
            ts::fatal("ts::sleep: could not create the timer thread's wait objects");
#endif
    }

    // Arm `state` for `deadline`, starting the thread if it is not running.
    void arm(Clock::time_point deadline, detail::Ref_ptr<Sleep_state> state)
    {
        bool earlier;
        {
            std::scoped_lock lock(mutex_);
            if (!thread_.joinable())
            {
                stopping_ = false;
                thread_ = std::thread([this] { run(); });
            }
            state->outstanding = std::make_unique<External_wait>();
            ++live_;
            earlier = heap_.empty() || deadline < heap_.front().deadline;
            heap_.push_back(Entry{ deadline, next_serial_++, std::move(state) });
            std::push_heap(heap_.begin(), heap_.end(), Later{});
        }
        if (earlier)
            wake();
    }

    // The cancel callback's body: settle the wait cancelled now. The entry stays in the heap
    // until its deadline and is dropped there.
    void cancel(Sleep_state* state)
    {
        Wakeup wakeup;
        {
            std::scoped_lock lock(mutex_);
            if (!state->live)
                return;
            state->live = false;
            --live_;
            wakeup = Wakeup{ state->block, state->priority, std::move(state->outstanding) };
        }
        deliver(std::move(wakeup), true);
    }

    void shutdown(bool check_armed) noexcept
    {
        std::vector<Entry> dropped;   // destroyed after the lock: a state's cancel callback takes it
        {
            std::scoped_lock lock(mutex_);
            if (!thread_.joinable())
                return;
#if TS_SAFETY_CHECKS
            if (check_armed && live_ != 0)
            {
                ts::fatal("destroy_scheduler with a ts::sleep still armed - await or cancel every sleep "
                          "(and every Periodic::next()) before tearing the scheduler down");
            }
#else
            (void)check_armed;
#endif
            stopping_ = true;
            for (Entry& e : heap_)
                e.state->live = false;
            dropped = std::move(heap_);
            heap_.clear();
            live_ = 0;
        }
        wake();
        thread_.join();
    }

private:
    void run()
    {
        std::vector<Entry> due;
        std::vector<Wakeup> fire;
        std::unique_lock lock(mutex_);
        while (!stopping_)
        {
            if (heap_.empty())
            {
                park(lock, nullptr);
                continue;
            }
            const Clock::time_point head = heap_.front().deadline;
            if (Clock::now() < head)
            {
                park(lock, &head);
                continue;
            }
            const Clock::time_point now = Clock::now();
            while (!heap_.empty() && heap_.front().deadline <= now)
            {
                std::pop_heap(heap_.begin(), heap_.end(), Later{});
                due.push_back(std::move(heap_.back()));
                heap_.pop_back();
            }
            for (Entry& e : due)
            {
                if (e.state->live)
                {
                    e.state->live = false;
                    --live_;
                    fire.push_back(Wakeup{ e.state->block, e.state->priority, std::move(e.state->outstanding) });
                }
            }
            lock.unlock();
            for (Wakeup& wakeup : fire)
                deliver(std::move(wakeup), false);
            fire.clear();
            due.clear();
            lock.lock();
        }
    }

    // Wait until `deadline` (null = indefinitely) or a `wake()`, whichever comes first. The
    // caller re-checks the heap under the lock afterwards, so a spurious return is harmless.
    void park(std::unique_lock<std::mutex>& lock, const Clock::time_point* deadline)
    {
#if defined(_WIN32)
        if (deadline != nullptr)
        {
            using Hundred_ns = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
            const long long ticks = std::chrono::ceil<Hundred_ns>(*deadline - Clock::now()).count();
            if (ticks <= 0)
                return;
            LARGE_INTEGER due;
            due.QuadPart = -ticks;   // negative = relative
            SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
            lock.unlock();
            HANDLE handles[2] = { wake_, timer_ };
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            lock.lock();
        }
        else
        {
            lock.unlock();
            WaitForSingleObject(wake_, INFINITE);
            lock.lock();
        }
#else
        if (deadline != nullptr)
            cv_.wait_until(lock, *deadline);
        else
            cv_.wait(lock);
#endif
    }

    // Callable with or without the mutex held: the parked thread re-checks under it, and the
    // auto-reset event (or the condition variable's predicate loop) keeps a wake that lands
    // before the park from being lost.
    void wake()
    {
#if defined(_WIN32)
        SetEvent(wake_);
#else
        cv_.notify_one();
#endif
    }

    std::mutex mutex_;
    std::vector<Entry> heap_;
    std::uint64_t next_serial_ = 0;
    int live_ = 0;
    bool stopping_ = false;
    std::thread thread_;
#if defined(_WIN32)
    HANDLE timer_ = nullptr;
    HANDLE wake_ = nullptr;
#else
    std::condition_variable cv_;
#endif
};

// Created on first use and never destroyed: program exit stops its thread through
// `timer_shutdown` (called from the scheduler holder's teardown), and leaving the object alive
// means that call can never reach a destroyed service, whatever the static destruction order.
std::atomic<Timer_service*> g_service{ nullptr };

Timer_service& timer_service()
{
    if (Timer_service* service = g_service.load(std::memory_order_acquire))
        return *service;
    auto* fresh = new Timer_service();
    Timer_service* expected = nullptr;
    if (!g_service.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
    {
        delete fresh;
        return *expected;
    }
    return *fresh;
}

} // namespace

Task<void> sleep_until(Clock::time_point deadline, Sleep_options opts, std::source_location site)
{
    if (global_scheduler().single_threaded())
    {
        ts::fatal("ts::sleep in worker-less mode - there is no worker to deliver the wakeup on, and the "
                  "timer thread must not run the waiting task itself");
    }
    detail::Task_ptr block = detail::make_bare_block();
    detail::set_task_name(block, Named(site));
    Task<void> result(block);
    if (opts.token.is_cancel_requested())
    {
        block->cancel();
        return result;
    }
    if (deadline <= Clock::now())
    {
        block->complete();
        return result;
    }

    detail::Ref_ptr<Sleep_state> state = detail::make_ref<Sleep_state>();
    state->block = block;
    state->priority = detail::resolved_priority(opts.priority);
    Timer_service& service = timer_service();
    service.arm(deadline, state);
    // Registered after arming, so a token requested in between settles through `cancel`. A
    // token already requested runs the callback here, in the constructor.
    state->on_cancel.emplace(opts.token, [&service, raw = state.get()] { service.cancel(raw); });
    return result;
}

Task<void> sleep(Clock::duration duration, Sleep_options opts, std::source_location site)
{
    return sleep_until(Clock::now() + duration, std::move(opts), site);
}

Periodic::Periodic(Clock::duration period, Sleep_options opts, std::source_location site)
    : period_(period)
    , next_deadline_(Clock::now() + period)
    , opts_(std::move(opts))
    , site_(site)
{
    if (period <= Clock::duration::zero())
        ts::fatal("ts::Periodic: the period must be positive");
}

Task<int> Periodic::next()
{
    if (opts_.token.is_cancel_requested())
        co_return 0;
    Clock::time_point now = Clock::now();
    if (now < next_deadline_)
    {
        co_await sleep_until(next_deadline_, opts_, site_);
        if (opts_.token.is_cancel_requested())
            co_return 0;
        now = Clock::now();
    }
    const int due = 1 + static_cast<int>((now - next_deadline_) / period_);
    next_deadline_ += period_ * due;
    co_return due;
}

void Periodic::reset()
{
    next_deadline_ = Clock::now() + period_;
}

namespace detail
{
void timer_shutdown(bool check_armed) noexcept
{
    if (Timer_service* service = g_service.load(std::memory_order_acquire))
        service->shutdown(check_armed);
}
}

} // namespace ts
