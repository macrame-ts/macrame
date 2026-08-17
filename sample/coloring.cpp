// Interaction coloring -- the cross-entity pattern for parallel loops whose
// items write shared neighbors (docs/pattern-farming.md 2.39). A cloth solver
// is the canonical case: distance constraints connect particle pairs, and a
// Gauss-Seidel iteration must see this iteration's corrections immediately, so
// the deferral idioms (stage-and-apply, mailboxes) are the wrong tool -- they
// would turn the solver into plain Jacobi and wreck convergence.
//
// The pattern: color the constraint graph so no two constraints sharing a
// particle get the same color. All constraints of one color touch disjoint
// particles, so a color band runs in parallel with no races -- and, because
// each particle is written by at most one helper per band, the result is
// independent of chunking and stealing: bit-deterministic under any
// scheduling. Bands run sequentially, so band k+1 sees band k's corrections --
// the within-iteration propagation deferral cannot give. Coloring is domain
// code (a greedy pass here, run once at setup); the library contributes
// `ts::parallel_for_colored` -- grant-inheriting helpers fanned out once for
// the whole iterations x bands solve, band transitions as atomic phase
// advances rather than per-band fork/join -- and the node that owns it.
//
// Islands (disconnected constraint graphs -- two separate cloth patches below)
// need no special handling: their constraints never conflict, so the greedy
// pass packs them into the same few colors and they parallelize within every
// band automatically.
//
// Everything runs under one node's grant on `Guarded<Cloth>`; the harness sees
// one declared write. The intra-band disjointness is the pattern's own
// invariant -- validated once after coloring (`coloring_valid`), not by the
// harness (sub-object granularity is outside its scope).
// Runs twice and checks the runs are bit-identical, and that the cloth
// converged (max stretch within tolerance).

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/parallel_for.h"
#include "ts/static_task_graph.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sample
{

namespace
{

// Verlet state: velocity is implied by `pos - prev`, so constraint projection
// implicitly damps it -- the standard pairing for a relaxation solver.
struct Particle
{
    float x = 0.0f, y = 0.0f;
    float px = 0.0f, py = 0.0f;   // previous position
    bool pinned = false;
};

struct Constraint
{
    int a = 0, b = 0;
    float rest = 0.0f;
};

// The cloth system: particles + distance constraints, instrumented like any
// guarded object. Solver methods are called from `parallel_for` helpers, which
// inherit the owning node's grant, so the checks pass off the hot declaration.
class Cloth
{
public:
    // A w x h patch of spacing-length constraints, top row pinned. Returns the
    // island's first particle index (patches are disconnected islands).
    int add_patch(float x0, float y0, int w, int h, float spacing)
    {
        TS_CHECK_ACCESS();
        int base = static_cast<int>(particles_.size());
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                float x = x0 + i * spacing, y = y0 + j * spacing;
                particles_.push_back({ x, y, x, y, j == 0 });
            }
        }
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                if (i + 1 < w)
                    constraints_.push_back({ base + j * w + i, base + j * w + i + 1, spacing });
                if (j + 1 < h)
                    constraints_.push_back({ base + j * w + i, base + (j + 1) * w + i, spacing });
            }
        }
        return base;
    }

    void integrate(float dt, float gravity)
    {
        TS_CHECK_ACCESS();
        ts::parallel_for(static_cast<int>(particles_.size()), [this, dt, gravity](int i)
        {
            Particle& p = particles_[i];
            if (p.pinned)
                return;
            constexpr float damping = 0.99f;
            float nx = p.x + (p.x - p.px) * damping;
            float ny = p.y + (p.y - p.py) * damping + gravity * dt * dt;
            p.px = p.x;
            p.py = p.y;
            p.x = nx;
            p.y = ny;
        });
    }

    // One constraint relaxation: move both endpoints toward rest length. Within
    // a color band no other helper touches these two particles.
    void relax(int ci)
    {
        TS_CHECK_ACCESS();
        const Constraint& c = constraints_[ci];
        Particle& pa = particles_[c.a];
        Particle& pb = particles_[c.b];
        float dx = pb.x - pa.x, dy = pb.y - pa.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f)
            return;
        float err = (len - c.rest) / len;
        float wa = 0.0f, wb = 0.0f;   // correction split; a pinned endpoint's share goes to the other
        if (!pa.pinned && !pb.pinned)
            wa = wb = 0.5f;
        else if (!pa.pinned)
            wa = 1.0f;
        else if (!pb.pinned)
            wb = 1.0f;
        pa.x += dx * err * wa;
        pa.y += dy * err * wa;
        pb.x -= dx * err * wb;
        pb.y -= dy * err * wb;
    }

    const std::vector<Constraint>& constraints() const { TS_CHECK_ACCESS(); return constraints_; }
    const std::vector<Particle>& particles() const { TS_CHECK_ACCESS(); return particles_; }

    float max_stretch() const
    {
        TS_CHECK_ACCESS();
        float worst = 0.0f;
        for (const Constraint& c : constraints_)
        {
            float dx = particles_[c.b].x - particles_[c.a].x;
            float dy = particles_[c.b].y - particles_[c.a].y;
            float err = std::fabs(std::sqrt(dx * dx + dy * dy) - c.rest) / c.rest;
            worst = std::max(worst, err);
        }
        return worst;
    }

private:
    std::vector<Particle> particles_;
    std::vector<Constraint> constraints_;
};

// Greedy coloring of the constraint graph: two constraints conflict iff they
// share a particle; each takes the smallest color unused by its neighbors.
// Domain code, run once at setup -- a grid needs 4 colors regardless of size.
std::vector<std::vector<int>> color_constraints(const std::vector<Constraint>& cs, int particle_count)
{
    std::vector<unsigned> used(particle_count, 0);   // per-particle bitmask of adjacent colors
    std::vector<std::vector<int>> bands;
    for (int ci = 0; ci < static_cast<int>(cs.size()); ++ci)
    {
        unsigned taken = used[cs[ci].a] | used[cs[ci].b];
        int color = 0;
        while (taken & (1u << color))
            ++color;
        if (color >= static_cast<int>(bands.size()))
            bands.resize(color + 1);
        bands[color].push_back(ci);
        used[cs[ci].a] |= 1u << color;
        used[cs[ci].b] |= 1u << color;
    }
    return bands;
}

// The pattern's invariant, checked once: within a band, no particle appears twice.
bool coloring_valid(const std::vector<std::vector<int>>& bands, const std::vector<Constraint>& cs, int particle_count)
{
    std::vector<int> last_band(particle_count, -1);
    for (int b = 0; b < static_cast<int>(bands.size()); ++b)
    {
        for (int ci : bands[b])
        {
            if (last_band[cs[ci].a] == b || last_band[cs[ci].b] == b)
                return false;
            last_band[cs[ci].a] = b;
            last_band[cs[ci].b] = b;
        }
    }
    return true;
}

struct Coloring_stats
{
    int particles = 0;
    int constraints = 0;
    int colors = 0;
    bool valid = false;
    float stretch = 0.0f;
    std::vector<float> snapshot;   // final positions, compared bit-exact across runs

    bool operator==(const Coloring_stats& o) const
    {
        return particles == o.particles && constraints == o.constraints && colors == o.colors &&
            valid == o.valid && stretch == o.stretch && snapshot == o.snapshot;
    }
};

Coloring_stats run_coloring_frames(int frames)
{
    ts::Guarded<Cloth> cloth{ ts::Named{} };

    // Two disconnected patches -- islands. Colored together, solved together.
    cloth.access([](Cloth& c)
    {
        c.add_patch(0.0f, 0.0f, 40, 30, 1.0f);
        c.add_patch(100.0f, 0.0f, 20, 20, 1.0f);
    }).sync();

    Coloring_stats st;
    std::vector<std::vector<int>> bands;
    cloth.access([&](const Cloth& c)
    {
        st.particles = static_cast<int>(c.particles().size());
        st.constraints = static_cast<int>(c.constraints().size());
        bands = color_constraints(c.constraints(), st.particles);
        st.valid = coloring_valid(bands, c.constraints(), st.particles);
    }).sync();
    st.colors = static_cast<int>(bands.size());

    ts::Static_task_graph g;
    g.add_node(ts::Named{}, [&bands](Cloth& c)
    {
        c.integrate(1.0f / 60.0f, -9.8f);
        // The colored driver: helpers fan out once for the whole
        // iterations x bands solve; band transitions are atomic phase
        // advances, not per-band fork/join. Same bit-deterministic result.
        constexpr int iterations = 8;
        ts::parallel_for_colored(bands, iterations, [&c](int ci)
        {
            c.relax(ci);   // disjoint endpoints within a band
        });
    }, cloth);
    g.compile();

    for (int f = 0; f < frames; ++f)
        g.execute().sync();

    cloth.access([&st](const Cloth& c)
    {
        st.stretch = c.max_stretch();
        st.snapshot.reserve(c.particles().size() * 2);
        for (const Particle& p : c.particles())
        {
            st.snapshot.push_back(p.x);
            st.snapshot.push_back(p.y);
        }
    }).sync();
    return st;
}

} // namespace

void run_coloring_sample()
{
    constexpr int frames = 60;
    Coloring_stats a = run_coloring_frames(frames);
    Coloring_stats b = run_coloring_frames(frames);

    std::printf("coloring sample: %d particles, %d constraints (2 islands), %d colors, "
                "coloring %s, max stretch %.2f%%, %s\n",
        a.particles, a.constraints, a.colors,
        a.valid ? "valid" : "INVALID (bug)",
        a.stretch * 100.0f,
        a == b ? "bit-deterministic across runs" : "NON-DETERMINISTIC (bug)");
}

} // namespace sample
