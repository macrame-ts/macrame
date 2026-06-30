#include "frame.h"
#include "systems.h"

namespace sample
{

// Nodes are added in frame order so the conflict tiebreak (declaration index)
// matches intent: early readers of world_xf_prev run before the swap; transform
// propagation runs before the late readers of world_xf. All ordering is derived
// from store access — no explicit edges needed here.
ts::Static_task_graph build_frame_graph(World& w)
{
    ts::Static_task_graph g;

    g.add_node(&tick_input, w.input);
    g.add_node(&tick_networking, w.input, w.net);
    g.add_node(&tick_streaming, w.input, w.assets);
    g.add_node(&tick_gameplay, w.world_xf_prev, w.input, w.net, w.game_state);
    g.add_node(&tick_navigation, w.nav, w.world_xf_prev, w.paths);
    g.add_node(&tick_ai, w.world_xf_prev, w.paths, w.game_state, w.intents);
    g.add_node(&tick_animation, w.skeletons, w.intents, w.local_xf);
    g.add_node(&tick_physics, w.velocities, w.game_state, w.bodies);
    g.add_node(&tick_propagation, w.local_xf, w.bodies, w.world_xf);
    g.add_node(&tick_cloth, w.world_xf, w.cloth);
    g.add_node(&tick_culling, w.world_xf, w.renderables, w.visibility);
    g.add_node(&tick_particles, w.world_xf, w.particles);
    g.add_node(&tick_audio, w.world_xf, w.audio_out);
    g.add_node(&tick_render, w.world_xf, w.visibility, w.particles, w.draw_lists);
    g.add_node(&tick_ui, w.game_state, w.ui);
    g.add_node(&tick_debug, w.game_state, w.world_xf);
    g.add_node(&tick_swap, w.world_xf, w.world_xf_prev);

    g.compile();
    return g;
}

} // namespace sample
