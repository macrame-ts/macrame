#include "mem_profile.h"

// The allocation profiler overrides global operator new/delete, so it is compiled in only
// when `TS_MEM_PROFILE` is defined (rebuild with that define to profile). The default build
// gets the stub at the bottom - zero impact on the normal allocator.
#if defined(TS_MEM_PROFILE)

#include "ts/guarded.h"
#include "ts/task.h"
#include "ts/coroutine_support.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"

// The game-frame sample is a single self-contained .cpp (no header). Both compositions of
// the same frame are profiled: the compiled graph amortizes its per-run state, the
// graph-free coroutine composition rebuilds it every frame.
namespace sample
{
void game_frame_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
void game_frame_free_stats(int frames, float time_scale, double& avg_ms, double& serial_ms, float& transform0);
}

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>

// --- global operator new/delete overrides with counters ---------------------
namespace
{
std::atomic<long long> g_count{ 0 };
std::atomic<long long> g_bytes{ 0 };
std::atomic<bool> g_on{ false };

// A/B knob: TS_POOL=1 routes allocs through a per-thread recycling free-list instead of the
// CRT allocator - the fastest realistic allocator (thread-local, lock-free, recycles) - to
// compare the allocator's contribution to throughput. Read once before main.
const bool g_pool = (std::getenv("TS_POOL") != nullptr);

inline void note(std::size_t n) noexcept
{
    if (g_on.load(std::memory_order_relaxed))
    {
        g_count.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
    }
}

// Per-thread segregated free-list. Uniform 64B header (holds the class index); raw block is
// 64-aligned so the returned pointer covers all over-alignment we use (<= 64). Everything
// routes through here in pool mode, so alloc/free are symmetric. Cross-thread free lands in
// the freeing thread's list (self-balances).
constexpr int n_class = 28;
constexpr std::size_t hdr = 64;
inline int size_class(std::size_t n) { int i = 0; std::size_t s = 64; while (s < n) { s <<= 1; ++i; } return i; }
inline std::size_t class_size(int i) { return std::size_t(64) << i; }

struct Pool { void* head[n_class] = {}; int cnt[n_class] = {}; };
thread_local Pool t_pool;

inline void* pool_alloc(std::size_t n) noexcept
{
    int i = size_class(n + hdr);
    void* raw;
    if (i < n_class && t_pool.head[i]) { raw = t_pool.head[i]; t_pool.head[i] = *static_cast<void**>(raw); --t_pool.cnt[i]; }
    else raw = _aligned_malloc(i < n_class ? class_size(i) : n + hdr, 64);
    if (!raw) std::abort();
    *static_cast<int*>(raw) = i;
    return static_cast<char*>(raw) + hdr;
}

inline void pool_free(void* p) noexcept
{
    if (!p) return;
    void* raw = static_cast<char*>(p) - hdr;
    int i = *static_cast<int*>(raw);
    if (i >= 0 && i < n_class && t_pool.cnt[i] < 8192) { *static_cast<void**>(raw) = t_pool.head[i]; t_pool.head[i] = raw; ++t_pool.cnt[i]; }
    else _aligned_free(raw);
}

inline void* alloc(std::size_t n, std::size_t /*align*/) noexcept
{
    note(n);
    if (g_pool)
        return pool_alloc(n ? n : 1);   // 64-aligned header covers our alignment needs
    void* p = _aligned_malloc(n ? n : 1, 64);
    if (!p)
        std::abort();
    return p;
}

inline void dealloc(void* p) noexcept
{
    if (g_pool)
        pool_free(p);
    else
        _aligned_free(p);
}
}

void* operator new(std::size_t n) { return alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](std::size_t n) { return alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new(std::size_t n, std::align_val_t a) { return alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return alloc(n, static_cast<std::size_t>(a)); }

void operator delete(void* p) noexcept { dealloc(p); }
void operator delete[](void* p) noexcept { dealloc(p); }
void operator delete(void* p, std::size_t) noexcept { dealloc(p); }
void operator delete[](void* p, std::size_t) noexcept { dealloc(p); }
void operator delete(void* p, std::align_val_t) noexcept { dealloc(p); }
void operator delete[](void* p, std::align_val_t) noexcept { dealloc(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { dealloc(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { dealloc(p); }

// --- the measurement --------------------------------------------------------
namespace
{
template <typename Op>
void measure(const char* name, int k, Op&& op)
{
    op();   // warm up (one-time scheduler / lazy-container init not counted)
    g_count.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
    g_on.store(true, std::memory_order_relaxed);
    for (int i = 0; i < k; ++i)
        op();
    g_on.store(false, std::memory_order_relaxed);

    long long c = g_count.load(std::memory_order_relaxed);
    long long b = g_bytes.load(std::memory_order_relaxed);
    std::printf("  %-18s %7.2f allocs/op   %8.1f bytes/op\n",
                name, static_cast<double>(c) / k, static_cast<double>(b) / k);
}

using Frame_run = void (*)(int, float, double&, double&, float&);

// Per-frame cost of a whole game frame, either composition. Each entry point builds its own
// `World` (and, for the graph, compiles it), so the per-frame figure is taken as a
// difference between an n-frame run and a 1-frame run - the one-time setup cancels.
void measure_frame(const char* name, Frame_run run)
{
    constexpr int frames = 41;   // 40 frames of difference
    double avg_ms = 0.0, serial_ms = 0.0;
    float transform0 = 0.0f;

    auto count = [&](int n, long long& allocs, long long& bytes)
    {
        run(n, 0.05f, avg_ms, serial_ms, transform0);   // warm up
        g_count.store(0, std::memory_order_relaxed);
        g_bytes.store(0, std::memory_order_relaxed);
        g_on.store(true, std::memory_order_relaxed);
        run(n, 0.05f, avg_ms, serial_ms, transform0);
        g_on.store(false, std::memory_order_relaxed);
        allocs = g_count.load(std::memory_order_relaxed);
        bytes = g_bytes.load(std::memory_order_relaxed);
    };

    long long one_allocs = 0, one_bytes = 0, many_allocs = 0, many_bytes = 0;
    count(1, one_allocs, one_bytes);
    count(frames, many_allocs, many_bytes);

    double per = static_cast<double>(frames - 1);
    std::printf("  %-18s %7.1f allocs/frame %7.0f bytes/frame\n", name,
                static_cast<double>(many_allocs - one_allocs) / per,
                static_cast<double>(many_bytes - one_bytes) / per);
}
}

void run_mem_profile()
{
    constexpr int k = 4000;

    // Anchor: cost of one malloc+free of a block-sized chunk on this machine (cache-hot,
    // single-thread, uncontended -> a lower bound on the real per-op cost).
    {
        constexpr int mk = 2000000;
        constexpr std::size_t sz = 300;
        auto t0 = std::chrono::steady_clock::now();
        volatile void* sink = nullptr;
        for (int i = 0; i < mk; ++i)
        {
            void* p = _aligned_malloc(sz, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
            sink = p;
            _aligned_free(p);
        }
        (void)sink;
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / mk;
        std::printf("\nmalloc+free of %zuB: %.1f ns (uncontended lower bound)%s\n",
                    sz, ns, g_pool ? "  [TS_POOL active]" : "");
    }

    std::printf("\nallocation profile (allocs & bytes charged per op, all threads):\n");

    ts::Guarded<int> a{ ts::Named{"a"}, 0 }, b{ ts::Named{"b"}, 0 };

    measure("launch", k, []
    {
        ts::launch([] { return 1; }).sync();
    });

    measure("async write", k, [&a]
    {
        a.async([](int& v) { ++v; }).sync();
    });

    measure("async read", k, [&a]
    {
        (void)a.async([](const int& v) { return v; }).sync();
    });

    measure("coro chain", k, [&a]
    {
        (void)[&a]() -> ts::Task<int>
        {
            int v = co_await a.async([](const int& v) { return v; });
            co_return v + 1;
        }().sync();
    });

    measure("coro join", k, [&a, &b]
    {
        (void)[&a, &b]() -> ts::Task<int>
        {
            ts::Task<int> x = a.async([](const int& v) { return v; });
            ts::Task<int> y = b.async([](const int& v) { return v; });
            co_return co_await x + co_await y;
        }().sync();
    });

    measure("parallel_for(64)", k, []
    {
        ts::parallel_for(64, [](int) {});
    });

    {
        ts::Guarded<int> g1{ ts::Named{"g1"}, 0 }, g2{ ts::Named{"g2"}, 0 }, g3{ ts::Named{"g3"}, 0 };
        ts::Static_task_graph graph;
        graph.add_node(ts::Named{"n1"}, [](int& x) { x = 1; }, g1);
        graph.add_node(ts::Named{"n2"}, [](const int& x, int& y) { y = x + 1; }, g1, g2);
        graph.add_node(ts::Named{"n3"}, [](const int& x, const int& y, int& z) { z = x + y; }, g1, g2, g3);
        graph.compile();
        measure("graph execute", k, [&graph]
        {
            graph.execute().sync();
        });
    }

    std::printf("\ngame frame (sample/game_frame.cpp, 1000 entities, ~30 systems):\n");
    measure_frame("frame graph", &sample::game_frame_stats);
    measure_frame("frame graph-free", &sample::game_frame_free_stats);

    std::printf("\n(block sizeof: Task_control_block = %zu bytes)\n",
                sizeof(ts::detail::Task_control_block));
}

#else   // TS_MEM_PROFILE not defined -> stub (default build, no operator-new override)

#include <cstdio>

void run_mem_profile()
{
    std::printf("mem_profile not compiled in. Rebuild with the TS_MEM_PROFILE define\n"
                "(MSBuild: /p:DefineConstants=TS_MEM_PROFILE, or add TS_MEM_PROFILE to the\n"
                "project's PreprocessorDefinitions), then run --memprofile. Set TS_POOL=1 for\n"
                "the default-vs-pool A/B.\n");
}

#endif
