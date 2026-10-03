#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/text_layout.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/drawing/font.hh>

using namespace cc::primitive_defines;

// Drawings on a device: placed in the atlas, drawn by the viewer renderer, read back.

namespace
{
/// A closed axis-aligned square from (0, 0) to (size, size).
[[nodiscard]] sv::path square(f32 size)
{
    return sv::path::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(size, size)));
}
} // namespace

// The whole path on a device: a canvas instancing one red square twice, once from each of two corners, read back.
ASYNC_INVOCABLE_TEST("sv - a canvas draws its drawings into the frame, from the corner each names",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the layout shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto red = sv::drawing();
    red.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    auto const set = resources.drawings.acquire(red);

    auto const size = tg::vec2i(64, 48);
    auto const at = [&](tg::pos3f p, sv::corner from)
    {
        return sv::drawing_placement{.set = set,
                                     .first_record = resources.drawings.first_record(set, 0),
                                     .record_count = resources.drawings.record_count(set, 0),
                                     .at = p,
                                     .x_axis = tg::vec3f(10, 0, 0),
                                     .y_axis = tg::vec3f(0, 10, 0),
                                     .from = from,
                                     .reach = tg::vec2f(10, 10)};
    };

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("canvas");
    v.resolution = size;
    v.resolution_follows_layout = false;
    v.layers.push_back({.kind = sv::layer_kind::canvas,
                        .blend = sv::layer_blend::over,
                        .drawings = {at(tg::pos3f(4, 4, 0), sv::corner::top_left),
                                     at(tg::pos3f(14, 14, 0), sv::corner::bottom_right)}});
    auto def = sv::viewer_definition{};
    def.views.push_back(cc::move(v));
    def.root_view = sv::view_index(0);
    auto const plan = sv::build_render_plan(def, size, 0, {});
    REQUIRE(plan.validate());

    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto store = sv::view_store{};
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               resources.advance_to(ctx.current_epoch());
                                               return sv::viewer_renderer::execute(
                                                   cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                                           }));
    (void)co_await ctx.idle_completion();

    auto read = ctx.create_command_list();
    auto const future = read->download.bytes_from_texture(output.raw());
    ctx.submit_command_list(cc::move(read));
    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == isize(size[0]) * size[1] * 4);
    auto const red_at = [&](int x, int y) { return u8(pixels[(isize(y) * size[0] + x) * 4]); };

    // from the top-left: (4, 4) to (14, 14)
    CHECK(red_at(9, 9) > 250);
    CHECK(red_at(2, 2) < 5);
    // from the bottom-right: its far edges 14 in from the view's, so it spans 24 to 14 in from each
    CHECK(red_at(size[0] - 19, size[1] - 19) > 250);
    CHECK(red_at(size[0] - 10, size[1] - 10) < 5);
    CHECK(red_at(size[0] - 27, size[1] - 27) < 5);
    // between the two, nothing
    CHECK(red_at(30, 24) < 5);

    co_await cc::async_settled(sv::background_work(ctx));
}

// A scene's drawings are tested against the trace's own depth: one in front of a traced quad shows, one behind it does not.
ASYNC_INVOCABLE_TEST("sv - a 3D drawing is hidden by traced geometry in front of it", (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    {
        auto probe = ctx.create_command_list();
        auto const supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!supported)
            SKIP("device reports no ray tracing support");
    }
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the shaders");

    auto resources = sv::gpu_resource_manager::create(ctx);

    // A grey quad at z = 0 across [-1, 1], facing a camera on -z.
    tg::pos3f const quad[] = {tg::pos3f(-1, -1, 0), tg::pos3f(1, -1, 0), tg::pos3f(1, 1, 0),
                              tg::pos3f(-1, -1, 0), tg::pos3f(1, 1, 0),  tg::pos3f(-1, 1, 0)};
    sv_test::pbr_material const grey[] = {{}, {}};
    auto const item = resources.acquire_scene_item(sv_test::as_mesh("quad", quad, grey));
    resources.wait_for_pending_uploads();

    auto red = sv::drawing();
    red.add_fill(square(1), {.color = tg::vec4f(1, 0, 0, 1)});
    auto const set = resources.drawings.acquire(red);
    auto const half_unit = [&](tg::pos3f at)
    {
        return sv::drawing_placement{.set = set,
                                     .first_record = resources.drawings.first_record(set, 0),
                                     .record_count = resources.drawings.record_count(set, 0),
                                     .at = at,
                                     .x_axis = tg::vec3f(0.5f, 0, 0),
                                     .y_axis = tg::vec3f(0, 0.5f, 0)};
    };

    // Right of centre and in front of the quad; left of centre and behind it.
    auto const size = tg::vec2i(96, 96);
    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("occluded");
    v.resolution = size;
    v.resolution_follows_layout = false;
    v.camera = sv::camera::looking_at(tg::pos3d(0, 0, -3), tg::pos3d(0, 0, 0));
    auto& scene = sv::ensure_scene_3d(v);
    scene.items.push_back(item);
    scene.drawings.push_back(half_unit(tg::pos3f(0.2f, -0.25f, -0.5f)));
    scene.drawings.push_back(half_unit(tg::pos3f(-0.7f, -0.25f, 0.5f)));

    auto def = sv::viewer_definition{};
    def.views.push_back(cc::move(v));
    def.root_view = sv::view_index(0);
    auto const plan = sv::build_render_plan(def, size, 0, {});
    REQUIRE(plan.validate());
    REQUIRE(plan.drawing_jobs.size() == 1);
    CHECK(plan.drawing_jobs[0].trace == 0);

    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto store = sv::view_store{};
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               resources.advance_to(ctx.current_epoch());
                                               return sv::viewer_renderer::execute(
                                                   cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                                           }));
    (void)co_await ctx.idle_completion();

    auto read = ctx.create_command_list();
    auto const future = read->download.bytes_from_texture(output.raw());
    ctx.submit_command_list(cc::move(read));
    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == isize(size[0]) * size[1] * 4);
    auto const channel = [&](int x, int y, int c) { return int(u8(pixels[(isize(y) * size[0] + x) * 4 + c])); };

    // The front square's centre, (0.45, 0, -0.5) at depth 2.5, lands right of the image's centre: red, over the quad.
    CHECK(channel(63, 48, 0) > 200);
    CHECK(channel(63, 48, 1) < 60);
    // The back square's centre, (-0.45, 0, 0.5) at depth 3.5, is behind the quad: the quad's own grey shows.
    CHECK(tg::abs(channel(37, 48, 0) - channel(37, 48, 1)) < 30);

    co_await cc::async_settled(sv::background_work(ctx));
}

// Text through the whole path: one glyph from the top-left and the same from the bottom-right, each inside its box.
// The default font is whatever the operating system ships, so this measures with that font rather than with numbers.
ASYNC_INVOCABLE_TEST("sv - canvas text lands inside its laid-out box, from either corner",
                     (sg::context_handle const& ctx_h))
{
    auto& ctx = *ctx_h;
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the layout shaders");
    auto const* const f = sv::default_font();
    if (f == nullptr)
        SKIP("no system UI font to set text in");

    auto resources = sv::gpu_resource_manager::create(ctx);
    auto const size = tg::vec2i(96, 64);
    auto const style = sv::text_style{.size = 32, .color = tg::vec4f(1, 0, 0, 1)};
    auto const box = sr::layout_text(f->face(), "H", {.size = style.size}).box;

    // Placing it the way canvas_ref::add_text does, through the frame-free path a test can drive.
    auto placements = cc::vector<sv::drawing_placement>();
    auto const glyph = f->face().glyph_for(U'H').value();
    auto const set = resources.drawings.glyph_set(*f, glyph);
    auto const index = u32(glyph) % sv::drawing_manager::glyphs_per_set;
    auto const laid = sr::layout_text(f->face(), "H", {.size = style.size});
    for (auto const from : {sv::corner::top_left, sv::corner::bottom_right})
    {
        auto const o = laid.glyphs[0].origin;
        placements.push_back({.set = set,
                              .first_record = resources.drawings.first_record(set, index),
                              .record_count = resources.drawings.record_count(set, index),
                              .at = tg::pos3f(4 + o[0], 4 + o[1], 0),
                              .x_axis = tg::vec3f(laid.scale, 0, 0),
                              .y_axis = tg::vec3f(0, -laid.scale, 0),
                              .tint = sr::pack_rgba8(style.color),
                              .from = from,
                              .reach = tg::vec2f(box.max[0], box.max[1]),
                              .offset = tg::vec2f(o[0], o[1])});
    }

    auto v = sv::view_data{};
    v.id = sv::view_id::from_string("text");
    v.resolution = size;
    v.resolution_follows_layout = false;
    v.layers.push_back({.kind = sv::layer_kind::canvas, .blend = sv::layer_blend::over, .drawings = placements});
    auto def = sv::viewer_definition{};
    def.views.push_back(cc::move(v));
    auto const plan = sv::build_render_plan(def, size, 0, {});
    REQUIRE(plan.validate());

    auto const output
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = size[0],
                                            .height = size[1],
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto store = sv::view_store{};
    REQUIRE(sv_test::frames_until_executed(ctx,
                                           [&](sg::command_list& cmd)
                                           {
                                               resources.advance_to(ctx.current_epoch());
                                               return sv::viewer_renderer::execute(
                                                   cmd, def, plan, resources, store,
                                                   output.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1)));
                                           }));
    (void)co_await ctx.idle_completion();

    auto read = ctx.create_command_list();
    auto const future = read->download.bytes_from_texture(output.raw());
    ctx.submit_command_list(cc::move(read));
    auto const pixels = co_await future.bytes();
    REQUIRE(pixels.size() == isize(size[0]) * size[1] * 4);

    // Ink anywhere in a rect, and none outside both boxes.
    auto const ink_in = [&](int x0, int y0, int x1, int y1)
    {
        auto any = false;
        for (auto y = cc::max(0, y0); y < cc::min(size[1], y1); ++y)
            for (auto x = cc::max(0, x0); x < cc::min(size[0], x1); ++x)
                any = any || u8(pixels[(isize(y) * size[0] + x) * 4]) > 128;
        return any;
    };
    auto const w = int(box.max[0]) + 1;
    auto const h = int(box.max[1]) + 1;
    CHECK(ink_in(4, 4, 4 + w, 4 + h));
    CHECK(ink_in(size[0] - 4 - w, size[1] - 4 - h, size[0] - 4, size[1] - 4));
    // the corner one stays inside the view: nothing in the last four pixels of either edge
    CHECK(!ink_in(size[0] - 3, 0, size[0], size[1]));
    CHECK(!ink_in(0, size[1] - 3, size[0], size[1]));

    co_await cc::async_settled(sv::background_work(ctx));
}
