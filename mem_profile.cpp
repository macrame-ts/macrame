#include "mem_profile.h"

// The allocation profiler overrides GLOBAL operator new/delete, so it is compiled in only
// when `TS_MEM_PROFILE` is defined (rebuild with that define to profile). The default build
// gets the stub at the bottom -- zero impact on the normal allocator.
#if defined(TS_MEM_PROFILE)

#include "guarded.h"
#include "task.h"
#include "parallel_for.h"
#include "static_task_graph.h"

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
// CRT allocator -- the fastest realistic allocator (thread-local, lock-free, recycles) -- to
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
}

void run_mem_profile()
{
    constexpr int k = 4000;

    // Anchor: cost of one malloc+free of a block-sized chunk on this machine (cache-hot,
    // single-thread, uncontended -> a LOWER bound on the real per-op cost).
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

    ts::Guarded<int> a{ 0 }, b{ 0 };

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

    measure("then", k, [&a]
    {
        (void)a.async([](const int& v) { return v; }).then([](int v) { return v + 1; }).sync();
    });

    measure("when_all", k, [&a, &b]
    {
        (void)ts::when_all(a.async([](const int& v) { return v; }),
                           b.async([](const int& v) { return v; }))
            .then([](int x, int y) { return x + y; })
            .sync();
    });

    measure("parallel_for(64)", k, []
    {
        ts::parallel_for(64, [](int) {});
    });

    {
        ts::Guarded<int> g1{ 0 }, g2{ 0 }, g3{ 0 };
        ts::Static_task_graph graph;
        graph.add_node([](int& x) { x = 1; }, g1);
        graph.add_node([](const int& x, int& y) { y = x + 1; }, g1, g2);
        graph.add_node([](const int& x, const int& y, int& z) { z = x + y; }, g1, g2, g3);
        graph.compile();
        measure("graph execute", k, [&graph]
        {
            graph.execute().sync();
        });
    }

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
