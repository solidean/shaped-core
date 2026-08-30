#include "vulkan-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// GPU timestamps against a real device.
//
// The tier-1 test pins the portable contract — validity, ordering, readiness.
// What is worth a tier-2 test is that the numbers mean something: a timestamp pair around real work must report a
// duration that is positive and plausible, which a backend returning a constant or an uninitialized slot would fail.
//
// Also the vulkan-specific piece: a pool is reset on the HOST, which is what lets a timestamp be recorded inside a
// rendering scope — vkCmdResetQueryPool cannot be.

namespace
{
namespace vulkan = sg::backend::vulkan;
} // namespace

TEST("sg vulkan - a timestamp pair measures real work")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    auto cmd = ctx.create_command_list();
    REQUIRE(cmd != nullptr);
    if (!cmd->query.is_supported())
    {
        ctx.drop_command_list(cc::move(cmd));
        SKIP("no timestamp support on this device");
    }

    // Enough copies that the elapsed time is not lost in the timer's resolution.
    constexpr isize k_bytes = 4 * 1024 * 1024;
    auto src = ctx.persistent.create_raw_buffer(k_bytes, sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);
    auto dst = ctx.persistent.create_raw_buffer(k_bytes, sg::buffer_usage::copy_dst);
    REQUIRE(src != nullptr);
    REQUIRE(dst != nullptr);

    auto const t0 = cmd->query.record_gpu_timestamp();
    for (int i = 0; i < 16; ++i)
        cmd->copy.buffer_bytes_region({.src = src, .dst = dst, .size_in_bytes = k_bytes});
    auto const t1 = cmd->query.record_gpu_timestamp();
    ctx.submit_command_list(cc::move(cmd));

    auto const s0 = ctx.wait_for_seconds(t0);
    auto const s1 = ctx.wait_for_seconds(t1);
    REQUIRE(s0.has_value());
    REQUIRE(s1.has_value());

    // Positive, and not absurd: a plain copy of this size cannot take a second, and a backend reporting the same
    // tick twice — or an unreset slot — fails the lower bound.
    auto const elapsed = s1.value() - s0.value();
    CHECK(elapsed > 0.0);
    CHECK(elapsed < 1.0);
}

TEST("sg vulkan - a timestamp records inside a rendering scope")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    auto probe = ctx.create_command_list();
    REQUIRE(probe != nullptr);
    bool const supported = probe->query.is_supported();
    ctx.drop_command_list(cc::move(probe));
    if (!supported)
        SKIP("no timestamp support on this device");

    auto target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 16, .height = 16, .usage = sg::texture_usage::render_target});
    REQUIRE(target.raw() != nullptr);

    // The case the host reset exists for: leasing a pool here would otherwise record vkCmdResetQueryPool inside the
    // instance, which Vulkan forbids.
    // The validation listener is what makes this test mean anything — without it, a wrong reset is invisible.
    auto cmd = ctx.create_command_list();
    sg::gpu_timestamp inside;
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        inside = cmd->query.record_gpu_timestamp();
    }
    ctx.submit_command_list(cc::move(cmd));

    CHECK(inside.is_valid());
    CHECK(ctx.wait_for_ticks(inside).has_value());
}
