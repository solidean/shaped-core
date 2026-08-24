#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// sv::gpu_resource_manager: the epoch tick every window may call, and the lock the staging group's owner holds
// while a snapshot is bound.
// The layout it is built over is covered without a device in bindless-tables-test.cc.

namespace
{
[[nodiscard]] sv::bindless_config only(sv::bindless_table table, u32 count)
{
    return {.tables = cc::vector<sv::bindless_table_budget>{{.table = table, .count = count}}};
}

[[nodiscard]] sg::texture_2d make_texture(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 8, .height = 8, .usage = sg::texture_usage::readonly_texture});
}
} // namespace

TEST("sv - the resource manager's epoch tick is idempotent")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    // Several windows drawing into one epoch each call it, and only the first one does anything — which is what
    // lets none of them own the tick.
    auto const e = m.current_epoch();
    m.advance_to(ctx.current_epoch());
    m.advance_to(ctx.current_epoch());
    CHECK(m.current_epoch() == e);

    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    CHECK(m.current_epoch() != e);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the resource manager declares only its configured tables")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    CHECK(m.has_table(sv::bindless_table::textures_2d));
    CHECK(m.table_capacity(sv::bindless_table::textures_2d) == 4);

    CHECK(!m.has_table(sv::bindless_table::textures_cube));
    CHECK(m.table_capacity(sv::bindless_table::textures_cube) == 0);
    CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_cube, make_texture(ctx).as_readonly_view()));

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the resource manager refuses acquires while frozen")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const tex = make_texture(ctx);
    (void)m.acquire_texture(sv::bindless_table::textures_2d, tex.as_readonly_view());

    // The manual pair, which freeze is the RAII form of.
    m.lock();
    CHECK(m.is_locked());
    CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view()));
    CHECK_ASSERTS(m.lock());
    m.unlock();
    CHECK(!m.is_locked());
    CHECK_ASSERTS(m.unlock());

    {
        auto const bound = m.freeze();
        CHECK(m.is_locked());
        CHECK(bound.group() != nullptr);
        CHECK_ASSERTS((void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view()));

        // Advancing under a live snapshot would invalidate the very indices it was taken for.
        CHECK_ASSERTS(m.advance_to(sg::epoch(u64(ctx.current_epoch()) + 1)));
    }
    CHECK(!m.is_locked());

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - two freezes in one epoch keep the first's indices")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // The multi-window invariant: window A records against its snapshot, then window B acquires more in the same epoch.
    // B's mints must not disturb what A already handed the GPU, which is what sg's "reclaim only what was NOT
    // acquired this epoch" rule buys.
    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    m.advance_to(ctx.current_epoch());

    auto const a = make_texture(ctx);
    auto const a_index = m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view());
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == a_index);
    }

    auto const b = make_texture(ctx);
    auto const b_index = m.acquire_texture(sv::bindless_table::textures_2d, b.as_readonly_view());
    CHECK(b_index != a_index);
    CHECK(m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view()) == a_index);

    {
        auto const bound = m.freeze();

        // Re-acquiring a resident view is deduplicated: an access declaration naming one element twice is not
        // what a dispatch expects.
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 2);

        // A table nobody touched declares nothing, which is a perfectly good thing to declare.
        CHECK(bound.elements(sv::bindless_table::textures_cube).empty());
    }

    // The epoch tick clears the declaration lists, so the next epoch declares what that epoch acquired.
    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).empty());
    }

    ctx.advance_epoch_and_wait_for_idle();
}
