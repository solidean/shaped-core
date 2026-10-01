#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <sgl_modules/marking.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// A binding group declared once, in an SGL module, and listed by pipelines of two other files.
// The host creates one group from the module's generated type and binds it at slot 0 of both, which sg accepts only
// where both pipelines hold the very layout the group was created against.

ASYNC_INVOCABLE_TEST("sg - one group of a module's binding serves pipelines of two files that use the module",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    constexpr auto side = 4;
    auto const writing = co_await ctx->cached.acquire_raster_pipeline(shaders::module_marking.writing);
    auto const tinting = co_await ctx->cached.acquire_raster_pipeline(shaders::module_tinting.tinting);

    auto const corners = ctx->persistent.create_buffer_from_data(
        cc::vector<sgl_modules::marking::corner>{{.position = tg::vec3f(-1, -1, 0)},
                                                 {.position = tg::vec3f(3, -1, 0)},
                                                 {.position = tg::vec3f(-1, 3, 0)}},
        sg::buffer_usage::vertex_buffer);
    auto const hits
        = ctx->persistent.create_buffer_from_data(cc::vector<float>::create_filled(side * side, 0.0f),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = side,
                                             .height = side,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    // one group, from the type the module's own entry generated
    auto const layout = ctx->cached.acquire_binding_group_layout<sgl_modules::marking::marks>();
    auto const group = ctx->persistent.create_binding_group(
        layout, sgl_modules::marking::marks{.tint = tg::vec4f(0, 0, 1, 1), .hits = hits.as_readwrite_buffer()});

    auto cmd = ctx->create_command_list();
    {
        auto pass = cmd->raster.render_to(
            sgl_modules::marking::target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        pass.bind_vertex_buffers({corners.as_vertex_buffer()});
        for (auto const* p : {writing.get(), tinting.get()})
        {
            pass.bind_pipeline(*p);
            pass.bind_group(0, *group);
            pass.draw({.vertex_range = {.offset = 0, .size = 3}, .instance_range = {.offset = 0, .size = 1}});
        }
    }
    auto const pixels = cmd->download.bytes_from_texture(image.raw());
    auto const written = cmd->download.data_from_buffer(hits);
    ctx->submit_command_list(cc::move(cmd));

    // the first pipeline wrote every element, and the second colored every pixel with the group's tint
    auto const values = co_await written.data();
    REQUIRE(values.size() == side * side);
    for (auto i = 0; i < side * side; ++i)
        CHECK(values[i] == float(i + 1));
    auto const bytes = co_await pixels.bytes();
    REQUIRE(bytes.size() == side * side * 4);
    for (auto i = 0; i < side * side; ++i)
    {
        CHECK(u8(bytes[i * 4 + 0]) == 0);
        CHECK(u8(bytes[i * 4 + 2]) == 255);
    }
}
