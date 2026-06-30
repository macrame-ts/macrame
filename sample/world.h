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

// The ECS mock: a handful of component stores, each access-controlled. Systems
// declare which stores they read/write; the frame graph derives ordering from
// that. (Skeleton subset; more stores are added as systems are filled in.)
struct World
{
    explicit World(int entities)
        : skeletons{ entities }
        , local_xf{ entities }
        , velocities{ entities }
        , bodies{ entities }
        , world_xf{ entities }
    {}

    ts::Thread_safe<Float_store> skeletons;   // anim input
    ts::Thread_safe<Float_store> local_xf;    // anim output
    ts::Thread_safe<Float_store> velocities;  // input output / physics input
    ts::Thread_safe<Float_store> bodies;      // physics output
    ts::Thread_safe<Float_store> world_xf;    // composed; read-hot
};

} // namespace sample
