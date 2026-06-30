#include "frame.h"
#include "systems.h"

namespace sample
{

ts::Static_task_graph build_frame_graph(World& world)
{
    ts::Static_task_graph g;

    // Access-derived edges:
    //   input(W velocities) < physics(R velocities)
    //   animation(W local_xf) < propagation(R local_xf)
    //   physics(W bodies)     < propagation(R bodies)
    //   propagation(W world_xf) < render(R world_xf)
    // => animation runs parallel to input+physics; both feed propagation; render last.
    g.add_node(&tick_input, world.velocities);
    g.add_node(&tick_animation, world.skeletons, world.local_xf);
    g.add_node(&tick_physics, world.velocities, world.bodies);
    g.add_node(&tick_propagation, world.local_xf, world.bodies, world.world_xf);
    g.add_node(&tick_render, world.world_xf);

    g.compile();
    return g;
}

} // namespace sample
