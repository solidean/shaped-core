#include "metal-test-common.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/metal/metal_native_scope.hh>

using namespace cc::primitive_defines;

// metal_native_scope: foreign code encoding onto an sg command list.
//
// The foreign work is a texture copy in an encoder of its own, which is what MetalFX does too.
// Both directions have to hold across the scope, in one list so no submit orders them for free:
//   - the foreign copy reads what sg uploaded just before it, which the fence the scope updated orders;
//   - sg's download reads what the foreign copy wrote, which the fence the foreign code updated orders.
// On an M4 the blits serialize anyway: with both fence waits removed this passed 200 runs at 64 x 64 and 50 at
// 2048 x 2048, so it pins the protocol working rather than catching a missing wait.

namespace
{
namespace mtl = sg::backend::metal;

constexpr auto k_size = 64;

[[nodiscard]] sg::texture_2d copyable_texture(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    });
}
} // namespace

ASYNC_TEST("sg metal - a native scope orders foreign encoders against sg's in one list")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const src = copyable_texture(*ctx);
    auto const dst = copyable_texture(*ctx);

    auto source = cc::vector<byte>::create_uninitialized(k_size * k_size * 4);
    for (auto i = 0; i < source.size(); ++i)
        source[i] = byte(u8((i * 29 + 3) & 0xFF));

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(src.raw(), source);
    {
        mtl::native_texture_access const textures[] = {
            {.texture = src.raw(), .access = sg::access_flag::copy_read, .stages = sg::pipeline_stage_flag::copy},
            {.texture = dst.raw(), .access = sg::access_flag::copy_write, .stages = sg::pipeline_stage_flag::copy},
        };
        auto const native = mtl::metal_native_scope::open(*cmd, textures);
        CHECK(native.device() != nullptr);
        CHECK(native.compiler() != nullptr);
        REQUIRE(native.fence() != nullptr);

        auto* const encoder = native.command_buffer()->computeCommandEncoder();
        encoder->waitForFence(native.fence(), MTL::StageBlit);
        encoder->copyFromTexture(native.texture(src.raw()), native.texture(dst.raw()));
        encoder->updateFence(native.fence(), MTL::StageBlit);
        encoder->endEncoding();
    }
    auto future = cmd->download.bytes_from_texture(dst.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == source.size());

    auto mismatches = 0;
    for (auto i = 0; i < source.size(); ++i)
        if (bytes.value()[i] != source[i])
            ++mismatches;
    CHECK(mismatches == 0).context(cc::format("{} of {} bytes differ", mismatches, source.size()));
}
