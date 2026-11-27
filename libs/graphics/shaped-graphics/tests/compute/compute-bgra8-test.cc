#include "../shaders/shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/bgra8_image.sgl: an `out` image of bgra8_unorm, which SPIR-V names no format
// for, stored to and read back in its byte order.

ASYNC_INVOCABLE_TEST("sg - an SGL shader stores a bgra8_unorm image, whose bytes hold blue, green, red and alpha",
                     (sg::context_handle const& ctx))
{
    constexpr int extent = 16;
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::extended_image_formats))
        SKIP("this context stores no bgra8_unorm image");

    auto const image = sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::bgra8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = extent,
        .height = extent,
        .usage = sg::texture_usage::image | sg::texture_usage::copy_src,
    }));
    auto const pipeline = co_await shaders::bgra8_image::store_bgra8.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::bgra8_image::swizzled>();
    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::bgra8_image::swizzled{.img = image.as_image_view<sg::pixel_format::bgra8_unorm>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(extent, extent);
    auto const downloaded = cmd->download.bytes_from_texture(image.raw());
    ctx->submit_command_list(cc::move(cmd));

    auto const node = downloaded.bytes();
    auto const& bytes = co_await node;
    REQUIRE(bytes.size() == isize(extent * extent * 4));
    // 0.5 is 127.5 codes, which a GPU may round either way
    constexpr int expected[] = {0, 128, 255, 64};
    auto wrong = 0;
    for (auto i = isize(0); i < bytes.size(); ++i)
    {
        auto const difference = int(bytes[i]) - expected[i % 4];
        wrong += difference < -1 || difference > 1 ? 1 : 0;
    }
    CHECK(wrong == 0)
        .dump("first texel", cc::format("{} {} {} {}", int(bytes[0]), int(bytes[1]), int(bytes[2]), int(bytes[3])));
}
