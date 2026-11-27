#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <typed-geometry/spatial/grid.hh>

using namespace cc::primitive_defines;

namespace
{
/// The steps of a whole walk.
[[nodiscard]] cc::vector<tg::grid_step<3, f32>> walk(tg::uniform_grid3f const& grid, tg::ray3f const& r, f32 t_min, f32 t_max)
{
    auto steps = cc::vector<tg::grid_step<3, f32>>();
    for (auto const step : grid.cells_along(r, t_min, t_max))
        steps.push_back(step);
    return steps;
}

auto const unit = tg::uniform_grid3f{.origin = tg::pos3f::zero, .cell_size = tg::vec3f(1)};
} // namespace

TEST("tg uniform_grid - a point's cell is the floor of where it lies, and a cell's box spans it")
{
    auto const grid = tg::uniform_grid3f{.origin = tg::pos3f(1, 0, 0), .cell_size = tg::vec3f(2, 4, 8)};
    CHECK(grid.cell_at(tg::pos3f(1, 0, 0)) == tg::pos3i(0, 0, 0));
    CHECK(grid.cell_at(tg::pos3f(0.5f, -0.1f, 8)) == tg::pos3i(-1, -1, 1)); // below the origin, and on a face: the upper cell
    CHECK(grid.box_of(tg::pos3i(-1, 2, 0)) == tg::aabb3f(tg::pos3f(-1, 8, 0), tg::pos3f(1, 12, 8)));
}

TEST("tg uniform_grid - a ray along an axis walks every cell it crosses, each stretch from face to face")
{
    auto const steps = walk(unit, tg::ray3f(tg::pos3f(0.5f, 0.5f, 0.5f), tg::vec3f(1, 0, 0)), 0.f, 3.f);
    REQUIRE(steps.size() == 4);
    for (auto i = 0; i < 4; ++i)
        CHECK(steps[i].cell == tg::pos3i(i, 0, 0));
    CHECK(steps[0].along.start == 0.f);
    CHECK(steps[0].along.end == 0.5f);
    CHECK(steps[1].along.end == 1.5f);
    CHECK(steps[3].along.end == 3.f); // the last stops at t_max, inside its cell
}

TEST("tg uniform_grid - a ray heading down from a face starts in the cell below it")
{
    auto const steps = walk(unit, tg::ray3f(tg::pos3f(2, 0.5f, 0.5f), tg::vec3f(-1, 0, 0)), 0.f, 1.5f);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].cell == tg::pos3i(1, 0, 0));
    CHECK(steps[0].along.end == 1.f);
    CHECK(steps[1].cell == tg::pos3i(0, 0, 0));
}

TEST("tg uniform_grid - a ray through an edge steps across both faces at once, never into a cell it only touches")
{
    // From the middle of cell (0, 0, 0) straight through the edge at x = y = 1.
    auto const steps = walk(unit, tg::ray3f(tg::pos3f(0.5f, 0.5f, 0.5f), tg::vec3f(1, 1, 0)), 0.f, 1.f);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].cell == tg::pos3i(0, 0, 0));
    CHECK(steps[1].cell == tg::pos3i(1, 1, 0));
    CHECK(steps[0].along.end == steps[1].along.start);
}

TEST("tg uniform_grid - a ray parallel to an axis never crosses it, and t_min starts the walk partway")
{
    auto const steps = walk(unit, tg::ray3f(tg::pos3f(0.25f, 0.75f, 0), tg::vec3f(0, 0, 1)), 2.5f, 4.f);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].cell == tg::pos3i(0, 0, 2));
    CHECK(steps[0].along.start == 2.5f);
    CHECK(steps[1].cell == tg::pos3i(0, 0, 3));
}

TEST("tg uniform_grid - a walk of one point is its cell, and a reversed range is nothing")
{
    auto const r = tg::ray3f(tg::pos3f(0.5f, 0.5f, 0.5f), tg::vec3f(1, 0, 0));
    auto const point = walk(unit, r, 1.f, 1.f);
    REQUIRE(point.size() == 1);
    CHECK(point[0].cell == tg::pos3i(1, 0, 0));
    CHECK(walk(unit, r, 2.f, 1.f).empty());
}

TEST("tg uniform_grid - a walk can stop anywhere, even along an unbounded ray")
{
    auto const r = tg::ray3f(tg::pos3f(0.5f, 0.5f, 0.5f), tg::vec3f(1, 0, 0));
    auto count = 0;
    for (auto const step : unit.cells_along(r, 0.f, tg::traits::highest<f32>()))
    {
        if (step.cell[0] == 5)
            break;
        ++count;
    }
    CHECK(count == 5);
}

TEST("tg uniform_grid - random walks are contiguous, each stretch lies in its cell, and no cell is skipped")
{
    auto rng = cc::random(17);
    auto const grid = tg::uniform_grid3f{.origin = tg::pos3f(-3, 1, 2), .cell_size = tg::vec3f(1.5f, 0.75f, 2)};
    for (auto round = 0; round < 200; ++round)
    {
        auto const origin = tg::pos3f(rng.uniform(-10.f, 10.f), rng.uniform(-10.f, 10.f), rng.uniform(-10.f, 10.f));
        auto dir = tg::vec3f(rng.uniform(-1.f, 1.f), rng.uniform(-1.f, 1.f), rng.uniform(-1.f, 1.f));
        if (round % 4 == 0)
            dir[round % 3] = 0.f; // some rays run parallel to a face
        if (dir == tg::vec3f::zero)
            continue;
        auto const t_max = rng.uniform(0.5f, 30.f);
        auto const steps = walk(grid, tg::ray3f(origin, dir), 0.f, t_max);
        REQUIRE(!steps.empty());
        CHECK(steps[0].along.start == 0.f);
        CHECK(steps.back().along.end == t_max);

        auto visited = cc::set<tg::pos3i>();
        for (auto i = isize(0); i < steps.size(); ++i)
        {
            auto const& s = steps[i];
            CHECK(s.along.start < s.along.end);
            if (i > 0)
                CHECK(s.along.start == steps[i - 1].along.end); // contiguous, bit for bit
            CHECK(visited.insert(s.cell));                      // each cell once

            // the middle of the stretch lies in the cell's box, up to rounding at its faces
            auto const box = grid.box_of(s.cell);
            auto const middle = tg::ray3f(origin, dir).at(0.5f * (s.along.start + s.along.end));
            for (auto k = 0; k < 3; ++k)
            {
                CHECK(middle[k] >= box.min[k] - 1e-4f);
                CHECK(middle[k] <= box.max[k] + 1e-4f);
            }
        }

        // every point sampled along the ray lies in a cell the walk visited
        for (auto j = 0; j <= 64; ++j)
        {
            auto const t = t_max * f32(j) / 64.f;
            auto const p = tg::ray3f(origin, dir).at(t);
            auto const cell = grid.cell_at(p);
            // a sample exactly on a face may round into the neighbor across it, which the walk only touched
            auto const box = grid.box_of(cell);
            auto on_face = false;
            for (auto k = 0; k < 3; ++k)
                on_face = on_face || p[k] - box.min[k] < 1e-4f || box.max[k] - p[k] < 1e-4f;
            if (!on_face)
                CHECK(visited.contains(cell));
        }
    }
}

TEST("tg uniform_grid - far from the origin a long walk stays contiguous and in its cells")
{
    auto const r = tg::ray3f(tg::pos3f(100000.5f, 3.25f, -7.5f), tg::vec3f(0.6f, 0.0f, 0.8f));
    auto const steps = walk(unit, r, 0.f, 2000.f);
    CHECK(steps.size() > 2000);
    for (auto i = isize(1); i < steps.size(); ++i)
        CHECK(steps[i].along.start == steps[i - 1].along.end);
    auto const last = unit.box_of(steps.back().cell);
    auto const end = r.at(2000.f);
    CHECK(end[0] >= last.min[0] - 0.05f);
    CHECK(end[0] <= last.max[0] + 0.05f);
}
