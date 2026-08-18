#include "benchmarks.h"
#include "bench_harness.h"

#include "ts/deferred.h"
#include "ts/guarded.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

// --- staged commands (the Deferred/Versioned closure tier) ---------------------------
// Per-command cost of stage and apply, the typed-lane tier's before baseline.
// "stg" series stage a batch then discard it (stage cost only); "s+a" series
// stage then commit inline from a held write access (stage + apply + destroy per
// command; subtract the matching "stg sbo" to isolate apply). "typed flr" is the
// typed tier's target floor: a plain vector push of the same payload.

struct Stage_target
{
    std::uint64_t acc = 0;
    std::array<std::uint64_t, 8> cells{};
};

constexpr std::uint64_t stage_batch = 10000;

// small capture: fits move_only_function's SBO - no per-command heap
std::vector<double> bench_stage_sbo()
{
    ts::Guarded<Stage_target> target{ ts::Named{} };
    ts::Deferred<Stage_target> deferred{ target };
    auto rec = deferred.recorder();

    return measure([&]() -> uint64_t
    {
        for (std::uint64_t i = 0; i < stage_batch; ++i)
            rec.stage([i](Stage_target& t) { t.acc += i; });
        deferred.discard();
        return stage_batch;
    });
}

// oversized capture: forces the closure onto the heap (malloc at stage, free at destroy)
std::vector<double> bench_stage_heap()
{
    ts::Guarded<Stage_target> target{ ts::Named{} };
    ts::Deferred<Stage_target> deferred{ target };
    auto rec = deferred.recorder();

    return measure([&]() -> uint64_t
    {
        std::array<std::uint64_t, 16> fat{};
        for (std::uint64_t i = 0; i < stage_batch; ++i)
        {
            fat[0] = i;
            rec.stage([fat](Stage_target& t) { t.acc += fat[0]; });
        }
        deferred.discard();
        return stage_batch;
    });
}

// the typed-lane target floor: the same payload as a plain vector append
std::vector<double> bench_stage_typed_floor()
{
    std::vector<std::uint64_t> lane;

    return measure([&]() -> uint64_t
    {
        for (std::uint64_t i = 0; i < stage_batch; ++i)
            lane.push_back(i);
        lane.clear();
        return stage_batch;
    });
}

// stage + apply, homogeneous stream: one closure type, the branch predictor's best case
std::vector<double> bench_stage_apply_homogeneous()
{
    ts::Guarded<Stage_target> target{ ts::Named{} };
    ts::Deferred<Stage_target> deferred{ target };
    auto rec = deferred.recorder();

    return measure([&]() -> uint64_t
    {
        for (std::uint64_t i = 0; i < stage_batch; ++i)
            rec.stage([i](Stage_target& t) { t.acc += i; });
        target.access([&](Stage_target&) { (void)deferred.commit(); }).sync();  // inline arm
        return stage_batch;
    });
}

// stage + apply, mixed stream: eight distinct closure types interleaved - each
// apply jumps to a different body, the indirect-call mispredict case
std::vector<double> bench_stage_apply_mixed()
{
    ts::Guarded<Stage_target> target{ ts::Named{} };
    ts::Deferred<Stage_target> deferred{ target };
    auto rec = deferred.recorder();

    return measure([&]() -> uint64_t
    {
        for (std::uint64_t i = 0; i < stage_batch; i += 8)
        {
            rec.stage([i](Stage_target& t) { t.cells[0] += i; });
            rec.stage([i](Stage_target& t) { t.cells[1] ^= i; });
            rec.stage([i](Stage_target& t) { t.cells[2] += i * 3; });
            rec.stage([i](Stage_target& t) { t.cells[3] |= i; });
            rec.stage([i](Stage_target& t) { t.cells[4] += i >> 1; });
            rec.stage([i](Stage_target& t) { t.cells[5] ^= i << 1; });
            rec.stage([i](Stage_target& t) { t.cells[6] += i * 5; });
            rec.stage([i](Stage_target& t) { t.cells[7] -= i; });
        }
        target.access([&](Stage_target&) { (void)deferred.commit(); }).sync();  // inline arm
        return stage_batch;
    });
}

} // namespace

void run_staged_commands_bench()
{
    std::printf("\nstaged commands (Deferred closure tier; s+a minus stg sbo isolates apply):\n");
    report("stg sbo", bench_stage_sbo());
    report("stg heap", bench_stage_heap());
    report("typed flr", bench_stage_typed_floor());
    report("s+a hom", bench_stage_apply_homogeneous());
    report("s+a mix", bench_stage_apply_mixed());
}
