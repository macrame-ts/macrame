#pragma once

#include "access.h"
#include "thread_safe.h"

#include <vector>

namespace sample
{

// A guarded component store (one float per entity). Stands in for an ECS
// component array; every public method asserts the caller declared access.
class Float_store
{
public:
    explicit Float_store(int entities) : data_(entities, 0.0f) {}

    int size() const
    {
        TS_CHECK_ACCESS();
        return static_cast<int>(data_.size());
    }

    float get(int i) const
    {
        TS_CHECK_ACCESS();
        return data_[i];
    }

    void set(int i, float v)
    {
        TS_CHECK_ACCESS();
        data_[i] = v;
    }

private:
    std::vector<float> data_;
};

// The ECS mock. Every store is access-controlled; each written store has a single
// writer system, so the frame graph derives a clean DAG from the declarations.
//
// Transforms are double-buffered: early systems (gameplay/AI/nav) read last
// frame's `world_xf_prev`; transform propagation writes this frame's `world_xf`,
// which the late systems (cloth/culling/particles/audio/render) read; the swap
// system copies world_xf -> world_xf_prev for the next frame. This is what lets
// many systems read transforms without serializing against the writer.
struct World
{
    explicit World(int n)
        : skeletons{ n }, nav{ n }, renderables{ n }, velocities{ n }
        , input{ n }, net{ n }, assets{ n }, game_state{ n }, paths{ n }, intents{ n }
        , local_xf{ n }, bodies{ n }, world_xf{ n }, world_xf_prev{ n }
        , cloth{ n }, visibility{ n }, particles{ n }, audio_out{ n }, draw_lists{ n }, ui{ n }
    {}

    // read-only static inputs (no system writes them this frame)
    ts::Thread_safe<Float_store> skeletons;
    ts::Thread_safe<Float_store> nav;
    ts::Thread_safe<Float_store> renderables;
    ts::Thread_safe<Float_store> velocities;

    // single-writer outputs
    ts::Thread_safe<Float_store> input;          // Input
    ts::Thread_safe<Float_store> net;            // Networking
    ts::Thread_safe<Float_store> assets;         // Streaming
    ts::Thread_safe<Float_store> game_state;     // Gameplay
    ts::Thread_safe<Float_store> paths;          // Navigation
    ts::Thread_safe<Float_store> intents;        // AI
    ts::Thread_safe<Float_store> local_xf;       // Animation
    ts::Thread_safe<Float_store> bodies;         // Physics
    ts::Thread_safe<Float_store> world_xf;       // Transform propagation (read-hot)
    ts::Thread_safe<Float_store> world_xf_prev;  // Swap (last frame's transforms)
    ts::Thread_safe<Float_store> cloth;          // Cloth
    ts::Thread_safe<Float_store> visibility;     // Culling
    ts::Thread_safe<Float_store> particles;      // Particles
    ts::Thread_safe<Float_store> audio_out;      // Audio
    ts::Thread_safe<Float_store> draw_lists;     // Render
    ts::Thread_safe<Float_store> ui;             // UI
};

} // namespace sample
