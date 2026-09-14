#include "ts/timer.h"
#include "ts/fatal.h"
#include "ts/scheduler.h"   // global_scheduler

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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

// The timer's bookkeeping, a second base of every timed block. The heap and the cancel
// callback reach the block through it; the block's own refcount keeps it alive.
struct Timed_fields
{
    detail::Task_control_block* timer_block = nullptr;   // the block this is a base of
    bool timer_live = false;   // armed, neither delivered nor cancelled; guarded by the timer mutex
    bool timer_delivered = false;   // handed to the scheduler, or run in the call; its body will run
    // Declared last, so destroyed first: its destructor waits out a callback firing on another
    // thread, and that callback reads the fields above.
    std::optional<Cancel_callback> timer_on_cancel;
};

// One allocation per wait: the task the caller holds, its body and result, and the timer's
// bookkeeping. Delivery submits this block itself, so a wakeup costs no second task.
template<typename Body, typename R>
struct Timed_executable : detail::Executable<Body, R>, Timed_fields
{
    explicit Timed_executable(Body body)
        : detail::Executable<Body, R>(std::move(body))
    {
        timer_block = this;
    }

    // A wait dropped by `timer_shutdown` is never delivered, so its body never ran; `Executable`
    // expects `run` to have destroyed it.
    ~Timed_executable()
    {
        if (!timer_delivered)
            this->destroy_body();
    }
};

template<typename R, typename Body>
detail::Task_ptr make_timed(Body body, Cancellation_token token, Priority priority, Named name, Timed_fields*& fields)
{
    using Exec = detail::Executable<Body, R>;
    using Wrapper = Timed_executable<Body, R>;
    auto* timed = new Wrapper(std::move(body));
    timed->destroy = [](detail::Task_control_block* c) { delete static_cast<Wrapper*>(c); };
    timed->execute = &Exec::run;
    timed->token = std::move(token);
    timed->flags.priority = priority;
    fields = timed;
    detail::Task_ptr block(timed);
    detail::set_task_name(block, name);
    return block;
}

// An armed wait counts as an `External_wait` from arm to delivery - its wakeup comes from a
// thread the scheduler does not run - without storing one.
void external_wait_add([[maybe_unused]] int delta) noexcept
{
#if TS_RULE_ON(TS_RULE_DEADLOCK_NET)
    detail::outstanding_external_waits.fetch_add(delta, std::memory_order_acq_rel);
#endif
}

// Hand a due or cancelled block to the scheduler, never run it here: running it settles the
// task, and settling resumes awaiting coroutines on the settling thread. `Executable::run`
// settles a block whose token was requested as cancelled, so a fire and a sleep's cancel share
// this path. The external-wait registration is released once the block is queued, so the
// deadlock net never sees a window with neither.
void deliver(detail::Task_ptr block) noexcept
{
    detail::submit_ready(std::move(block));
    external_wait_add(-1);
}

struct Entry
{
    Clock::time_point deadline;
    std::uint64_t serial;   // FIFO among equal deadlines
    detail::Task_ptr block;   // the heap's reference
    Timed_fields* fields;
};

// Heap order for `std::push_heap`/`pop_heap`: the earliest deadline at the front.
struct Later
{
    bool operator()(const Entry& a, const Entry& b) const noexcept
    {
        return a.deadline != b.deadline ? a.deadline > b.deadline : a.serial > b.serial;
    }
};

// The deadline keeper: a min-heap of armed waits and the one thread that waits for its head.
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

    // Arm `block` for `deadline`, starting the thread if it is not running.
    void arm(Clock::time_point deadline, detail::Task_ptr block, Timed_fields* fields)
    {
        bool earlier;
        {
            std::scoped_lock lock(mutex_);
            if (!thread_.joinable())
            {
                stopping_ = false;
                thread_ = std::thread([this] { run(); });
            }
            fields->timer_live = true;
            ++live_;
            external_wait_add(1);
            earlier = heap_.empty() || deadline < heap_.front().deadline;
            heap_.push_back(Entry{ deadline, next_serial_++, std::move(block), fields });
            std::push_heap(heap_.begin(), heap_.end(), Later{});
        }
        if (earlier)
            wake();
    }

    // The cancel callback's body: deliver the wait now. Its entry stays in the heap until the
    // deadline and is dropped there.
    void cancel(Timed_fields* fields)
    {
        detail::Task_ptr block;
        {
            std::scoped_lock lock(mutex_);
            if (!fields->timer_live)
                return;
            fields->timer_live = false;
            fields->timer_delivered = true;
            --live_;
            block = detail::Task_ptr(fields->timer_block);   // safe: live means the heap still holds a reference
        }
        deliver(std::move(block));
    }

    void shutdown(bool check_armed) noexcept
    {
        std::vector<Entry> dropped;   // released after the lock: a block's cancel callback takes it
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
            {
                if (e.fields->timer_live)
                {
                    e.fields->timer_live = false;
                    external_wait_add(-1);
                }
            }
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
        std::vector<detail::Task_ptr> fire;
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
                if (e.fields->timer_live)
                {
                    e.fields->timer_live = false;
                    e.fields->timer_delivered = true;
                    --live_;
                    fire.push_back(std::move(e.block));
                }
            }
            lock.unlock();
            for (detail::Task_ptr& block : fire)
                deliver(std::move(block));
            fire.clear();
            due.clear();   // the heap's references to cancelled waits, released outside the lock
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

void require_workers()
{
    if (global_scheduler().single_threaded())
    {
        ts::fatal("ts::sleep in worker-less mode - there is no worker to deliver the wakeup on, and the "
                  "timer thread must not run the waiting task itself");
    }
}

// Arm `block` for `deadline` and register the cancel callback on `token`. Registered after
// arming, so a token requested in between is caught by the callback, which then runs in its
// constructor.
void arm_with_cancel(Clock::time_point deadline, detail::Task_ptr block, Timed_fields* fields,
    const Cancellation_token& token)
{
    Timer_service& service = timer_service();
    service.arm(deadline, std::move(block), fields);
    fields->timer_on_cancel.emplace(token, [&service, fields] { service.cancel(fields); });
}

} // namespace

Task<void> sleep_until(Clock::time_point deadline, Sleep_options opts, std::source_location site)
{
    require_workers();
    Timed_fields* fields = nullptr;
    // The block carries the caller's token, so `Executable::run` settles it cancelled when the
    // token was requested - on delivery, or here in the call.
    const Priority priority = detail::resolved_priority(opts.priority);
    detail::Task_ptr block = make_timed<void>([] {}, opts.token, priority, Named(site), fields);
    Task<void> result(block);
    if (opts.token.is_cancel_requested() || deadline <= Clock::now())
    {
        fields->timer_delivered = true;
        block->execute(block);   // settles in the call
        return result;
    }
    arm_with_cancel(deadline, std::move(block), fields, opts.token);
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
    require_workers();
    Timed_fields* fields = nullptr;
    // An empty token on the block: a cancelled tick settles completed with 0 (`advance`),
    // never cancelled - awaiting a cancelled value task is fatal.
    const Priority priority = detail::resolved_priority(opts_.priority);
    detail::Task_ptr block = make_timed<int>([this] { return advance(); }, {}, priority, Named(site_), fields);
    Task<int> result(block);
    if (opts_.token.is_cancel_requested() || Clock::now() >= next_deadline_)
    {
        fields->timer_delivered = true;
        block->execute(block);   // settles in the call
        return result;
    }
    arm_with_cancel(next_deadline_, std::move(block), fields, opts_.token);
    return result;
}

int Periodic::advance() noexcept
{
    if (opts_.token.is_cancel_requested())
        return 0;
    const Clock::time_point now = Clock::now();
    const int due = 1 + static_cast<int>((now - next_deadline_) / period_);
    next_deadline_ += period_ * due;
    return due;
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
