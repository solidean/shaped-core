#include <nexus/test.hh>
#include <shaped-rendering/slug_routine.hh>
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

TEST("sv::drawing - the hash follows every layer's geometry, rule and color, in order")
{
    auto a = sv::drawing();
    a.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    auto b = sv::drawing();
    b.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    CHECK(a.hash() == b.hash());

    auto recolored = sv::drawing();
    recolored.add_fill(square(1), {.color = tg::vec4f(0, 1, 0, 1)});
    CHECK(recolored.hash() != a.hash());

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
    CHECK(manager.atlas(0).record_count() == 3);

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

namespace
{
/// An annotation whose parts are told apart by their first record: content 1, disk 10, ring 11, segment 12, dot 13.
[[nodiscard]] sv::annotation_record test_annotation(tg::pos3f anchor, sv::annotation_occluded occluded)
{
    auto const part = [](u32 record) { return sv::drawing_placement{.first_record = record, .record_count = 1}; };
    auto a = sv::annotation_record{.anchor = anchor,
                                   .box_size = tg::vec2f(40, 20),
                                   .occluded = occluded,
                                   .leader = sv::leader_shape::elbow,
                                   .leader_width = 2.0f,
                                   .hidden_dashes = {4, 3},
                                   .marker_radius = 3.5f,
                                   .disk = part(10),
                                   .ring = part(11),
                                   .segment = part(12),
                                   .dot = part(13)};
    auto content = part(1);
    content.at = tg::pos3f(2, 3, 0);
    a.content.push_back(content);
    return a;
}

/// World xy in [-1, 1] straight onto clip space, every point at depth one quarter.
[[nodiscard]] tg::mat4f flat_camera()
{
    auto m = tg::mat4f::identity;
    m[2, 2] = 0.0f;
    m[3, 2] = 0.25f;
    return m;
}

[[nodiscard]] cc::vector<sv::drawing_placement> parts_of(cc::span<sv::drawing_placement const> all, u32 record)
{
    auto out = cc::vector<sv::drawing_placement>();
    for (auto const& p : all)
        if (p.first_record == record)
            out.push_back(p);
    return out;
}
} // namespace

TEST("sv::annotation - the box sits on the anchor's side, offset away from it, with an elbow leader into it")
{
    // the anchor lands at (100, 225) of a 400 x 300 view: left of the center and below it
    auto placed = cc::vector<sv::drawing_placement>();
    sv::impl::place_annotation(test_annotation(tg::pos3f(-0.5f, -0.5f, 0), sv::annotation_occluded::show),
                               flat_camera(), tg::vec2f(400, 300), placed);

    // so the box goes left and down: its near corner 32 left of and 28 below the anchor
    auto const content = parts_of(placed, 1);
    REQUIRE(content.size() == 1);
    CHECK(content[0].at == tg::pos3f(28 + 2, 253 + 3, 0));

    // every part probes the anchor, at the depth the camera gives it
    for (auto const& p : placed)
    {
        CHECK(p.probe == tg::vec2f(100.0f / 400.0f, 225.0f / 300.0f));
        CHECK(p.probe_depth == 0.25f);
    }

    auto const disk = parts_of(placed, 10);
    REQUIRE(disk.size() == 1);
    CHECK(disk[0].at == tg::pos3f(100, 225, 0));

    // the elbow: from the marker's edge to a knee 10 out from the box's right side, then level into it at mid height
    auto const segments = parts_of(placed, 12);
    REQUIRE(segments.size() == 2);
    auto const knee = tg::pos3f(78, 263, 0);
    auto const first_end = segments[0].at + segments[0].x_axis;
    CHECK((first_end - knee).length() < 1e-4f);
    CHECK(tg::abs((segments[0].at - tg::pos3f(100, 225, 0)).length() - 3.5f) < 1e-4f);
    CHECK(segments[1].at == knee);
    CHECK(segments[1].x_axis == tg::vec3f(-10, 0, 0));
    // as wide as the leader, across it
    CHECK(tg::abs(segments[1].y_axis.length() - 2.0f) < 1e-5f);
    CHECK(parts_of(placed, 13).size() == 1); // the dot that rounds the bend
}

TEST("sv::annotation - a box that would leave the view is pushed back inside its margins")
{
    // near the top-right corner, the box would go further right and up
    auto placed = cc::vector<sv::drawing_placement>();
    sv::impl::place_annotation(test_annotation(tg::pos3f(0.9f, 0.9f, 0), sv::annotation_occluded::show), flat_camera(),
                               tg::vec2f(400, 300), placed);
    auto const content = parts_of(placed, 1);
    REQUIRE(content.size() == 1);
    // its top-left lands 8 in from the right and top edges: 400 - 8 - 40 and 8
    CHECK(content[0].at == tg::pos3f(352 + 2, 8 + 3, 0));
}

TEST("sv::annotation - each occlusion policy decides which parts the probe shows")
{
    auto const run = [](sv::annotation_occluded occluded)
    {
        auto placed = cc::vector<sv::drawing_placement>();
        sv::impl::place_annotation(test_annotation(tg::pos3f(-0.5f, -0.5f, 0), occluded), flat_camera(),
                                   tg::vec2f(400, 300), placed);
        return placed;
    };
    auto const all_are = [](cc::span<sv::drawing_placement const> parts, sr::slug_visibility v)
    {
        for (auto const& p : parts)
            if (p.visibility != v)
                return false;
        return !parts.empty();
    };

    // hidden-line: the box always, the filled marker and solid leader while visible, the ring and dashes while hidden
    auto const hidden_line = run(sv::annotation_occluded::hidden_line);
    CHECK(all_are(parts_of(hidden_line, 1), sr::slug_visibility::always));
    CHECK(all_are(parts_of(hidden_line, 10), sr::slug_visibility::if_visible));
    CHECK(all_are(parts_of(hidden_line, 11), sr::slug_visibility::if_hidden));
    auto solid = isize(0);
    auto dashes = isize(0);
    for (auto const& s : parts_of(hidden_line, 12))
        (s.visibility == sr::slug_visibility::if_visible ? solid : dashes) += 1;
    CHECK(solid == 2);
    CHECK(dashes > 2); // the dashed copy is cut into many pieces

    // hide: everything only while visible, and no hidden look at all
    auto const hide = run(sv::annotation_occluded::hide);
    CHECK(all_are(parts_of(hide, 1), sr::slug_visibility::if_visible));
    CHECK(all_are(parts_of(hide, 10), sr::slug_visibility::if_visible));
    CHECK(parts_of(hide, 11).empty());

    // show: everything, always
    auto const show = run(sv::annotation_occluded::show);
    for (auto const& p : show)
        CHECK(p.visibility == sr::slug_visibility::always);
    CHECK(parts_of(show, 11).empty());
}

TEST("sv::annotation - an anchor behind the camera or outside the view draws nothing")
{
    auto placed = cc::vector<sv::drawing_placement>();
    sv::impl::place_annotation(test_annotation(tg::pos3f(1.5f, 0, 0), sv::annotation_occluded::show), flat_camera(),
                               tg::vec2f(400, 300), placed);
    CHECK(placed.empty());

    auto behind = flat_camera();
    behind[3, 3] = -1.0f; // every point at w = -1
    sv::impl::place_annotation(test_annotation(tg::pos3f(0, 0, 0), sv::annotation_occluded::show), behind,
                               tg::vec2f(400, 300), placed);
    CHECK(placed.empty());
}

TEST("sv - a scene's annotations become a 2D job after its drawings, placed through the camera")
{
    auto def = sv::viewer_definition{};
    def.views.push_back(view_with("annotated", tg::vec2i(64, 64), sv::layer_kind::scene_3d, {}));
    def.views[0].camera = sv::camera::looking_at(tg::pos3d(0, 0, -3), tg::pos3d(0, 0, 0));
    def.views[0].layers[0].annotations.push_back(test_annotation(tg::pos3f(0, 0, 0), sv::annotation_occluded::show));
    def.root_view = sv::view_index(0);

    auto const plan = sv::build_render_plan(def, tg::vec2i(64, 64), 0, {});
    REQUIRE(plan.validate());
    REQUIRE(plan.drawing_jobs.size() == 1);
    auto const& job = plan.drawing_jobs[0];
    CHECK(job.kind == sv::drawing_job_kind::annotations);
    CHECK(!job.is_3d);
    // the anchor at the origin, straight ahead, lands at the view's center
    auto const disk = parts_of(job.placements, 10);
    REQUIRE(disk.size() == 1);
    CHECK(tg::abs(disk[0].at[0] - 32.0f) < 1e-3f);
    CHECK(tg::abs(disk[0].at[1] - 32.0f) < 1e-3f);
}

namespace
{
/// A set of `count` squares, sized by `seed` so two sets of different seeds are different content.
[[nodiscard]] sv::drawing_set squares(int count, f32 seed)
{
    auto set = sv::drawing_set();
    for (auto i = 0; i < count; ++i)
    {
        auto d = sv::drawing();
        d.add_fill(square(seed + f32(i)));
        (void)set.add(d);
    }
    return set;
}
} // namespace

TEST("sv::drawing_manager - a set changed after it was placed is placed again")
{
    auto manager = sv::drawing_manager();
    auto set = squares(1, 1);
    auto const placed = manager.acquire(set);

    // The set's cache slot still names the old placement, which holds no record for the drawing added since.
    (void)set.add(sv::drawing().add_fill(square(5)));
    auto const again = manager.acquire(set);
    CHECK(again != placed);
    CHECK(manager.record_count(again, 1) > 0);

    // A copy carries the slot, and is the same content, so it lands on the same placement.
    auto const copy = set;
    CHECK(manager.acquire(copy) == again);
}

TEST("sv::drawing_manager - a set that does not fit opens a page, and a full manager empties the one drawn longest ago")
{
    // pages one row tall, at most two of them; five hundred squares fit one, but two such sets pass its 819 records
    auto manager = sv::drawing_manager(1, 2);
    auto const a_set = squares(500, 1);
    auto const b_set = squares(500, 1000);
    auto const c_set = squares(500, 2000);

    manager.begin_frame(sg::epoch(1));
    auto const a = manager.acquire(a_set);
    (void)manager.first_record(a, 0);
    manager.begin_frame(sg::epoch(2));
    auto const b = manager.acquire(b_set);
    (void)manager.first_record(b, 0);
    REQUIRE(manager.page_of(a) != manager.page_of(b));
    CHECK(manager.page_count() == 2);

    // a third set needs a page; the one A sits in was drawn from longest ago, so A leaves and C takes its page
    manager.begin_frame(sg::epoch(3));
    auto const a_page = manager.page_of(a);
    auto const c = manager.acquire(c_set);
    CHECK(manager.page_count() == 2);
    CHECK(!manager.contains(a));
    CHECK(manager.contains(b));
    CHECK(manager.page_of(c) == a_page);
    CHECK(manager.record_count(c, 499) == 1);

    // acquiring A again places it anew: B's page is the one not drawn from this frame
    auto const a_again = manager.acquire(a_set);
    CHECK(a_again != a);
    CHECK(!manager.contains(b));
    CHECK(manager.page_of(a_again) != manager.page_of(c));

    SECTION("a page drawn from this frame is never emptied; the manager grows past its limit instead")
    {
        manager.begin_frame(sg::epoch(4));
        (void)manager.first_record(c, 0);
        (void)manager.first_record(a_again, 0);
        nx::expect_warning("every drawing atlas page is in use this frame*");
        auto const b_again = manager.acquire(b_set);
        CHECK(manager.page_count() == 3);
        CHECK(manager.contains(c));
        CHECK(manager.contains(a_again));
        CHECK(manager.page_of(b_again) == 2);
    }
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
