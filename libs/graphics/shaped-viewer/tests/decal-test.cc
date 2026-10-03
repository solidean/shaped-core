#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/drawing/decal.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>

using namespace cc::primitive_defines;

// Decals on the CPU: the projector the tracer reads, and the atlas their shapes live in.
// decal-trace-test.cc holds what the trace paints with them.

namespace
{
/// Where a row of a decal record takes `p`.
[[nodiscard]] f32 apply(tg::vec4f row, tg::pos3f p)
{
    return row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
}

/// A set of `count` squares, sized by `seed` so two sets of different seeds are different content.
[[nodiscard]] sv::drawing_set squares(int count, f32 seed)
{
    auto set = sv::drawing_set();
    for (auto i = 0; i < count; ++i)
    {
        auto d = sv::drawing();
        d.add_fill(sv::path::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(seed + f32(i), seed + f32(i)))));
        (void)set.add(d);
    }
    return set;
}
} // namespace

TEST("sv::decal - the projector's rows take a placed point back to the drawing, and its depth to [-1, 1]")
{
    // Stretched and sheared axes, off the origin: the rows are an inverse, so a shortcut through the axes alone shows.
    auto const placement = sv::decal_placement{.first_record = 3,
                                               .record_count = 2,
                                               .bounds = tg::aabb2f(tg::pos2f(-1, -2), tg::pos2f(4, 5)),
                                               .at = tg::pos3f(1, 2, 3),
                                               .x_axis = tg::vec3f(2, 0, 0),
                                               .y_axis = tg::vec3f(0.5f, 0, -3),
                                               .depth = 0.25f,
                                               .tint = 0x80ff00ffu};
    auto const g = sv::impl::decal_record_of(placement, 7);

    // The projection runs along y cross x, here (0, -6, 0) normalized.
    CHECK(g.direction == tg::vec3f(0, -1, 0));

    for (auto const c : {tg::vec3f(0, 0, 0), tg::vec3f(1.5f, -2, 0.5f), tg::vec3f(-1, 4, -1)})
    {
        auto const p
            = placement.at + placement.x_axis * c[0] + placement.y_axis * c[1] + g.direction * (placement.depth * c[2]);
        CHECK(tg::abs(apply(g.to_x, p) - c[0]) < 1e-5f);
        CHECK(tg::abs(apply(g.to_y, p) - c[1]) < 1e-5f);
        CHECK(tg::abs(apply(g.to_depth, p) - c[2]) < 1e-5f);
    }

    CHECK(g.bounds == tg::vec4f(-1, -2, 4, 5));
    CHECK(g.first_shape == 7);
    CHECK(g.shape_count == 2);
    CHECK(g.tint == 0x80ff00ffu);
    CHECK(tg::abs(g.unit_length[0] - 2.0f) < 1e-6f);
    CHECK(tg::abs(g.unit_length[1] - tg::vec3f(0.5f, 0, -3).length()) < 1e-6f);
}

TEST("sv::drawing_manager - a decal lives in the decal atlas, keyed apart from the same set drawn as an instance")
{
    auto manager = sv::drawing_manager();
    auto const set = squares(3, 1);

    manager.begin_frame(sg::epoch(1));
    auto const drawn = manager.acquire(set);
    auto const decal = manager.acquire_decal(set);
    CHECK(decal != drawn);
    CHECK(manager.page_of(decal) == sv::drawing_manager::decal_page);
    CHECK(manager.page_of(drawn) != sv::drawing_manager::decal_page);
    CHECK(manager.acquire_decal(set) == decal);

    // Its records are the decal atlas's, which holds nothing else.
    CHECK(manager.record_count(decal, 2) == 1);
    CHECK(manager.decal_atlas().record_count() == 3);

    // A lone drawing and a one-element set holding it are different keys here too.
    auto const& d = set.drawings()[0];
    auto only = sv::drawing_set();
    (void)only.add(d);
    CHECK(manager.acquire_decal(d) != manager.acquire_decal(only));
}

TEST("sv::drawing_manager - a full decal atlas starts over, unless a decal drew from it this frame")
{
    // One row: five hundred squares fit, two such sets pass its 819 records.
    auto manager = sv::drawing_manager(1, 2);
    auto const a_set = squares(500, 1);
    auto const b_set = squares(500, 1000);

    manager.begin_frame(sg::epoch(1));
    auto const a = manager.acquire_decal(a_set);
    (void)manager.first_record(a, 0);

    SECTION("a frame that has not drawn from it empties it")
    {
        manager.begin_frame(sg::epoch(2));
        auto const b = manager.acquire_decal(b_set);
        CHECK(!manager.contains(a));
        CHECK(manager.record_count(b, 499) == 1);
        CHECK(manager.decal_atlas().record_count() == 500);
    }

    SECTION("a frame that drew from it keeps it, and the set that does not fit draws nothing")
    {
        nx::expect_warning("the decal atlas is full of this frame's decals*");
        auto const b = manager.acquire_decal(b_set);
        CHECK(manager.contains(a));
        CHECK(manager.record_count(b, 0) == 0);
        CHECK(manager.record_count(a, 0) == 1);
    }
}
