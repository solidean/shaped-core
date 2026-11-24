#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
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

// The SGL fixture in tests/shaders/sgl/norm16_images.sgl: images of rg16_unorm and rgba16_snorm stored to, clamped to
// their range, and read back as the floats their codes stand for.

namespace
{
constexpr int k_extent = 16;

sg::texture_2d make_image(sg::context_handle const& ctx, sg::pixel_format format)
{
    return sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = format,
        .dimension = sg::texture_dimension::d2,
        .width = k_extent,
        .height = k_extent,
        .usage = sg::texture_usage::image | sg::texture_usage::copy_src,
    }));
}

/// Every texel's 16-bit codes, read as `T`, against `expected` channel by channel; a GPU may round a code either way.
template <class T, int Channels>
int count_mismatches(cc::span<byte const> bytes, T const (&expected)[Channels])
{
    auto wrong = 0;
    for (auto i = 0; i < k_extent * k_extent * Channels; ++i)
    {
        auto code = T(0);
        cc::memcpy(&code, bytes.data() + i * 2, 2);
        auto const difference = int(code) - int(expected[i % Channels]);
        wrong += difference < -1 || difference > 1 ? 1 : 0;
    }
    return wrong;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL shader stores 16-bit normalized images, clamped, and reads them back as it wrote "
                     "them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::extended_image_formats))
        SKIP("this context has no 16-bit normalized images");

    auto const lean = make_image(ctx, sg::pixel_format::rg16_unorm);
    auto const signed_image = make_image(ctx, sg::pixel_format::rgba16_snorm);

    // (0.25, 1.5) stores 0.25 * 65535 and the clamped 1; (-0.25, 0.75, -2, 1) stores -0.25 * 32767, 0.75 * 32767, the
    // clamped -1 and 1, where -1 is -32767 and never -32768
    {
        auto const pipeline = co_await shaders::norm16_images::store_norm16.acquire_pipeline(*ctx);
        auto const layout = ctx->cached.acquire_binding_group_layout<shaders::norm16_images::normals>();
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::norm16_images::normals{.lean = lean.as_image_view<sg::pixel_format::rg16_unorm>(),
                                            .rough = signed_image.as_image_view<sg::pixel_format::rgba16_snorm>()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(k_extent, k_extent);
        auto const lean_bytes = cmd->download.bytes_from_texture(lean.raw());
        auto const signed_bytes = cmd->download.bytes_from_texture(signed_image.raw());
        ctx->submit_command_list(cc::move(cmd));

        auto const unorm_node = lean_bytes.bytes();
        auto const& unorm = co_await unorm_node;
        REQUIRE(unorm.size() == isize(k_extent * k_extent * 4));
        constexpr u16 expected_unorm[] = {16384, 65535};
        CHECK(count_mismatches(unorm.span(), expected_unorm) == 0);

        auto const snorm_node = signed_bytes.bytes();
        auto const& snorm = co_await snorm_node;
        REQUIRE(snorm.size() == isize(k_extent * k_extent * 8));
        constexpr i16 expected_snorm[] = {-8192, 24575, -32767, 32767};
        CHECK(count_mismatches(snorm.span(), expected_snorm) == 0);
    }

    // a `mut` image reads each code back as its float: the unorm pair swapped, the snorm channels negated
    if (!ctx->supports(sg::feature::readwrite_image_formats))
        SKIP("this context reads and writes no 16-bit normalized image in one shader");
    if (ctx->backend() == sg::backend_kind::metal || ctx->backend() == sg::backend_kind::webgpu)
        SKIP("SGL writes no `mut` 16-bit normalized image for msl or wgsl, whose devices never read and write one "
             "(EMIT-280)");
    {
        auto const pipeline = co_await shaders::norm16_images::flip_norm16.acquire_pipeline(*ctx);
        auto const layout = ctx->cached.acquire_binding_group_layout<shaders::norm16_images::flipped>();
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::norm16_images::flipped{.lean = lean.as_image_view<sg::pixel_format::rg16_unorm>(),
                                            .rough = signed_image.as_image_view<sg::pixel_format::rgba16_snorm>()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(k_extent, k_extent);
        auto const lean_bytes = cmd->download.bytes_from_texture(lean.raw());
        auto const signed_bytes = cmd->download.bytes_from_texture(signed_image.raw());
        ctx->submit_command_list(cc::move(cmd));

        auto const unorm_node = lean_bytes.bytes();
        auto const& unorm = co_await unorm_node;
        REQUIRE(unorm.size() == isize(k_extent * k_extent * 4));
        constexpr u16 expected_unorm[] = {65535, 16384};
        CHECK(count_mismatches(unorm.span(), expected_unorm) == 0);

        auto const snorm_node = signed_bytes.bytes();
        auto const& snorm = co_await snorm_node;
        REQUIRE(snorm.size() == isize(k_extent * k_extent * 8));
        constexpr i16 expected_snorm[] = {8192, -24575, 32767, -32767};
        CHECK(count_mismatches(snorm.span(), expected_snorm) == 0);
    }
}

ASYNC_INVOCABLE_TEST("sg - a sampled texture of each 16-bit normalized format keeps the bytes uploaded to it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports(sg::feature::extended_image_formats))
        SKIP("this context has no 16-bit normalized textures");

    for (auto const format :
         {sg::pixel_format::r16_unorm, sg::pixel_format::r16_snorm, sg::pixel_format::rg16_unorm,
          sg::pixel_format::rg16_snorm, sg::pixel_format::rgba16_unorm, sg::pixel_format::rgba16_snorm})
    {
        auto const texture = ctx->persistent.create_raw_texture({
            .format = format,
            .dimension = sg::texture_dimension::d2,
            .width = k_extent,
            .height = k_extent,
            .usage = sg::texture_usage::texture | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
        });
        auto bytes = cc::vector<byte>();
        for (auto i = 0; i < k_extent * k_extent * sg::format_block_size(format); ++i)
            bytes.push_back(byte((i * 37 + 11) & 0xFF));

        auto cmd = ctx->create_command_list();
        cmd->upload.bytes_to_texture(texture, bytes);
        auto const downloaded = cmd->download.bytes_from_texture(texture);
        ctx->submit_command_list(cc::move(cmd));
        auto const node = downloaded.bytes();
        auto const& back = co_await node;
        REQUIRE(back.size() == bytes.size());
        auto wrong = 0;
        for (auto i = isize(0); i < bytes.size(); ++i)
            wrong += back[i] != bytes[i] ? 1 : 0;
        CHECK(wrong == 0).dump("format", int(format));
    }
}
