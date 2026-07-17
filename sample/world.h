#pragma once

#include "ts/access.h"
#include "ts/guarded.h"

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
// system copies `world_xf` -> `world_xf_prev` for the next frame. This is what
// lets many systems read transforms without serializing against the writer.
struct World
{
    explicit World(int n)
        : skeletons{ n }, nav{ n }, renderables{ n }, velocities{ n }, asset_source{ n }
        , input{ n }, net{ n }, assets{ n }, game_state{ n }, paths{ n }, intents{ n }
        , local_xf{ n }, bodies{ n }, world_xf{ n }, world_xf_prev{ n }
        , cloth{ n }, visibility{ n }, particles{ n }, audio_out{ n }, draw_lists{ n }, ui{ n }
    {}

    // read-only static inputs (no system writes them this frame)
    ts::Guarded<Float_store> skeletons;
    ts::Guarded<Float_store> nav;
    ts::Guarded<Float_store> renderables;
    ts::Guarded<Float_store> velocities;
    ts::Guarded<Float_store> asset_source;   // Streaming loads from this

    // single-writer outputs
    ts::Guarded<Float_store> input;          // Input
    ts::Guarded<Float_store> net;            // Networking
    ts::Guarded<Float_store> assets;         // Streaming
    ts::Guarded<Float_store> game_state;     // Gameplay
    ts::Guarded<Float_store> paths;          // Navigation
    ts::Guarded<Float_store> intents;        // AI
    ts::Guarded<Float_store> local_xf;       // Animation
    ts::Guarded<Float_store> bodies;         // Physics
    ts::Guarded<Float_store> world_xf;       // Transform propagation (read-hot)
    ts::Guarded<Float_store> world_xf_prev;  // Swap (last frame's transforms)
    ts::Guarded<Float_store> cloth;          // Cloth
    ts::Guarded<Float_store> visibility;     // Culling
    ts::Guarded<Float_store> particles;      // Particles
    ts::Guarded<Float_store> audio_out;      // Audio
    ts::Guarded<Float_store> draw_lists;     // Render
    ts::Guarded<Float_store> ui;             // UI
};

} // namespace sample
