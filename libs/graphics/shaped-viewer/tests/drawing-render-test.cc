#include "viewer_test_env.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/all.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>

using namespace cc::primitive_defines;

// Drawings on a device: placed in the atlas, drawn by the viewer renderer, read back.

namespace
{
/// A closed axis-aligned square from (0, 0) to (size, size).
[[nodiscard]] sv::path square(f32 size)
{
    return sr::slug_outline::rectangle(tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(size, size)));
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
    co_await ctx.idle_completion();

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
