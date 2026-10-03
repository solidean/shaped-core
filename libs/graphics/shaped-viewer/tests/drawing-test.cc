#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/rendering/render_plan.hh>

using namespace cc::primitive_defines;

// Drawings — 2D vector content built once and instanced — from the value a caller builds, through the manager that
// places it in the atlas, to the plan that draws it and the pixels it lands on.

namespace
{
/// A closed axis-aligned square from (0, 0) to (size, size).
[[nodiscard]] sv::path square(f32 size)
{
    return sv::path::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(size, size)));
}

/// A view at `resolution` whose one layer is of `kind`, holding `placements`.
[[nodiscard]] sv::view_data view_with(char const* name,
                                      tg::vec2i resolution,
                                      sv::layer_kind kind,
                                      cc::vector<sv::drawing_placement> placements)
{
    auto v = sv::view_data{};
    v.id = sv::view_id::from_string(name);
    v.resolution = resolution;
    v.resolution_follows_layout = false;
    v.layers.push_back({.kind = kind, .blend = sv::layer_blend::over, .drawings = cc::move(placements)});
    return v;
}
} // namespace

TEST("sv::drawing - the hash follows every layer's geometry, rule and colour, in order")
{
    auto a = sv::drawing();
    a.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    auto b = sv::drawing();
    b.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    CHECK(a.hash() == b.hash());

    auto recoloured = sv::drawing();
    recoloured.add_fill(square(1), {.color = tg::vec4f(0, 1, 0, 1)});
    CHECK(recoloured.hash() != a.hash());

    auto even_odd = sv::drawing();
    even_odd.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1), .rule = sr::slug_fill_rule::even_odd});
    CHECK(even_odd.hash() != a.hash());

    // two layers in the other order are another drawing: the later one draws on top
    auto ab = sv::drawing();
    ab.add_fill(square(1)).add_fill(square(2));
    auto ba = sv::drawing();
    ba.add_fill(square(2)).add_fill(square(1));
    CHECK(ab.hash() != ba.hash());
}

TEST("sv::drawing - a stroke is a nonzero layer of the area it covers, and a fill closes an open path")
{
    tg::pos2f const points[] = {tg::pos2f(0, 0), tg::pos2f(10, 0), tg::pos2f(10, 10)};
    auto const open = sv::path::polyline(points);

    auto d = sv::drawing();
    d.add_stroke(open, {.color = tg::vec4f(0, 1, 0, 1), .width = 2, .dashes = {3, 1}});
    REQUIRE(d.layers().size() == 1);
    auto const& stroke = d.layers()[0];
    CHECK(stroke.outline.is_closed());
    CHECK(stroke.outline.fill_rule == sr::slug_fill_rule::nonzero);
    CHECK(stroke.color == tg::vec4f(0, 1, 0, 1));

    // a stroke that covers nothing adds no layer
    d.add_stroke(open, {.width = 0});
    CHECK(d.layers().size() == 1);

    d.add_fill(open);
    REQUIRE(d.layers().size() == 2);
    CHECK(d.layers()[1].outline.is_closed());
    CHECK(d.layers()[1].outline.curves.size() == 3);

    // the dash pattern is part of the geometry, so of the hash
    auto solid = sv::drawing();
    solid.add_stroke(open, {.color = tg::vec4f(0, 1, 0, 1), .width = 2});
    auto dashed = sv::drawing();
    dashed.add_stroke(open, {.color = tg::vec4f(0, 1, 0, 1), .width = 2, .dashes = {3, 1}});
    CHECK(solid.hash() != dashed.hash());
}

TEST("sv::drawing - a nested drawing is copied in under its frame, tinted")
{
    auto badge = sv::drawing();
    badge.add_fill(square(1), {.color = tg::vec4f(1, 0.5f, 0, 1)});

    auto d = sv::drawing();
    d.add_drawing(badge, {.at = tg::pos2f(10, 20),
                          .x_axis = tg::vec2f(0, 1),
                          .y_axis = tg::vec2f(-1, 0),
                          .scale = 4,
                          .tint = tg::vec4f(1, 1, 1, 0.5f)});
    REQUIRE(d.layers().size() == 1);
    // the square's (1, 0) corner lands one scaled x axis from `at`, and its (0, 1) corner one scaled y axis
    auto const& curves = d.layers()[0].outline.curves;
    CHECK(curves[0].p1 == tg::pos2f(10, 20));
    CHECK(curves[0].p3 == tg::pos2f(10, 24));
    CHECK(curves[2].p3 == tg::pos2f(6, 20));
    CHECK(d.layers()[0].color == tg::vec4f(1, 0.5f, 0, 0.5f));

    // a copy: changing the source afterwards changes nothing here
    auto const before = d.hash();
    badge.add_fill(square(2));
    CHECK(d.layers().size() == 1);
    CHECK(d.hash() == before);
}

TEST("sv::drawing_manager - a set is placed once, and a lone drawing is its own set")
{
    auto set = sv::drawing_set();
    auto arrow = sv::drawing();
    arrow.add_fill(square(1));
    auto logo = sv::drawing();
    logo.add_fill(square(4), {.color = tg::vec4f(0, 0, 1, 1)}).add_fill(square(2));
    auto const arrow_id = set.add(arrow);
    auto const logo_id = set.add(logo);
    CHECK(u32(arrow_id) == 0);
    CHECK(u32(logo_id) == 1);

    auto manager = sv::drawing_manager();
    auto const id = manager.acquire(set);
    CHECK(manager.acquire(set) == id); // through the set's cache slot
    CHECK(manager.count() == 1);

    // one record per layer, consecutive within a drawing
    CHECK(manager.record_count(id, 0) == 1);
    CHECK(manager.record_count(id, 1) == 2);
    CHECK(manager.atlas().record_count() == 3);

    // an equal set built separately is the same content, so the same id
    auto copy = sv::drawing_set();
    (void)copy.add(arrow);
    (void)copy.add(logo);
    CHECK(manager.acquire(copy) == id);

    // a lone drawing is keyed apart from a set holding only it
    auto only = sv::drawing_set();
    (void)only.add(arrow);
    auto const lone = manager.acquire(arrow);
    CHECK(lone != manager.acquire(only));
    CHECK(manager.acquire(arrow) == lone);
}

TEST("sv - a canvas layer's drawings become one 2D job in its target, in logical pixels")
{
    auto def = sv::viewer_definition{};
    def.content_scale = 2.0f;
    def.views.push_back(
        view_with("canvas", tg::vec2i(200, 100), sv::layer_kind::canvas, {sv::drawing_placement{.record_count = 1}}));
    def.root_view = sv::view_index(0);

    auto const plan = sv::build_render_plan(def, tg::vec2i(200, 100), 0, {});
    REQUIRE(plan.validate());
    REQUIRE(plan.drawing_jobs.size() == 1);
    CHECK(plan.traces.empty());

    auto const& job = plan.drawing_jobs[0];
    CHECK(!job.is_3d);
    CHECK(job.logical_size == tg::vec2f(100, 50));

    auto const draws = plan.draws_of(u32(plan.targets.size() - 1));
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].kind == sv::draw_kind::drawings);
    CHECK(draws[0].job == 0);

    // logical (0, 0) is the top-left corner, and logical (100, 50) the bottom-right, at a content scale of 2
    auto const clip = [&](f32 x, f32 y)
    {
        auto const p = job.object_to_clip * tg::vec4f(x, y, 0, 1);
        return tg::vec2f(p[0] / p[3], p[1] / p[3]);
    };
    CHECK(clip(0, 0) == tg::vec2f(-1, 1));
    CHECK(clip(100, 50) == tg::vec2f(1, -1));
}

TEST("sv - a scene holding only drawings draws them without a trace")
{
    auto def = sv::viewer_definition{};
    def.views.push_back(
        view_with("scene", tg::vec2i(64, 64), sv::layer_kind::scene_3d, {sv::drawing_placement{.record_count = 1}}));
    def.root_view = sv::view_index(0);

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    REQUIRE(plan.validate());
    CHECK(plan.traces.empty());
    REQUIRE(plan.drawing_jobs.size() == 1);
    CHECK(plan.drawing_jobs[0].is_3d);
}

TEST("sv - a layer with no drawings adds no job")
{
    auto def = sv::viewer_definition{};
    def.views.push_back(view_with("empty", tg::vec2i(64, 64), sv::layer_kind::canvas, {}));
    def.root_view = sv::view_index(0);

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    REQUIRE(plan.validate());
    CHECK(plan.drawing_jobs.empty());
    CHECK(plan.draws.empty());
}
