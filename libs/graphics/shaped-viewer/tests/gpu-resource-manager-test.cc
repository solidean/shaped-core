#include "viewer_test_env.hh"

#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/all.hh>
#include <shaped-viewer/resources/impl/mip_layout.hh>

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
    auto const a_index = u32(m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view()));
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == a_index);
    }

    auto const b = make_texture(ctx);
    auto const b_index = u32(m.acquire_texture(sv::bindless_table::textures_2d, b.as_readonly_view()));
    CHECK(b_index != a_index);
    CHECK(u32(m.acquire_texture(sv::bindless_table::textures_2d, a.as_readonly_view())) == a_index);

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

TEST("sv - a pinned texture is declared and outlives its epoch")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // What a material buffer will hold: an index that stays true across epochs, so the buffer can be uploaded
    // once and cached by content hash rather than re-uploaded whenever the tables move.
    auto m = sv::gpu_resource_manager::create(ctx, {.bindless = only(sv::bindless_table::textures_2d, 4)});
    m.advance_to(ctx.current_epoch());

    auto const tex = make_texture(ctx);
    auto const pin = m.pin_texture(sv::bindless_table::textures_2d, tex.as_readonly_view());
    REQUIRE(pin != nullptr);
    auto const index = pin->index();

    // A pinned element belongs in the access declaration even though nothing acquired it this epoch: a dispatch
    // reaches it through the buffer, never through acquire.
    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == index);
    }

    // Churn the transient working set through later epochs; the pinned index is unmoved.
    for (auto i = 0; i < 3; ++i)
    {
        ctx.advance_epoch_and_wait_for_idle();
        m.advance_to(ctx.current_epoch());
        (void)m.acquire_texture(sv::bindless_table::textures_2d, make_texture(ctx).as_readonly_view());
        CHECK(pin->index() == index);
    }

    ctx.advance_epoch_and_wait_for_idle();
}

namespace
{
/// A `width` x `height` RGBA8 image whose bytes vary with `seed`, plus every mip if `all_mips`.
[[nodiscard]] cc::vector<byte> make_pixels(i32 width, i32 height, u8 seed, bool all_mips)
{
    auto const levels = all_mips ? sv::impl::mip_count_of(width, height) : 1;
    auto bytes = cc::vector<byte>();
    for (auto mip = 0; mip < levels; ++mip)
    {
        auto const size = sv::impl::mip_byte_size(sg::pixel_format::rgba8_unorm, width, height, mip);
        for (auto i = isize(0); i < size; ++i)
            bytes.push_back(byte(u8(i) ^ seed));
    }
    return bytes;
}
} // namespace

TEST("sv - a texture acquire is content-addressed and pins its element")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const base_only = sv::texture_data::create(make_pixels(16, 16, 1, false), sg::pixel_format::rgba8_unorm, 16, 16);
    auto const id = m.acquire_texture(base_only);
    REQUIRE(m.textures.contains(id));

    auto const* const record = m.textures.get_ptr(id);
    REQUIRE(record != nullptr);

    // The chain is allocated in full even though only the base level was supplied, so filling the rest later
    // writes into this texture rather than replacing it — which would move the index a material buffer stored.
    CHECK(record->total_mips == sv::impl::mip_count_of(16, 16));
    CHECK(record->uploaded_mips == 1);
    CHECK(record->state == sv::residency::base_resident);
    CHECK(record->element != nullptr);

    // Same content, same id, no second upload.
    auto const again = sv::texture_data::create(make_pixels(16, 16, 1, false), sg::pixel_format::rgba8_unorm, 16, 16);
    CHECK(m.acquire_texture(again) == id);
    CHECK(m.textures.count() == 1);

    // Different content is a different texture, at a different element.
    auto const other = sv::texture_data::create(make_pixels(16, 16, 2, false), sg::pixel_format::rgba8_unorm, 16, 16);
    auto const other_id = m.acquire_texture(other);
    CHECK(other_id != id);
    CHECK(m.textures.get_ptr(other_id)->index() != record->index());

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the same pixels at a different shape are a different texture")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // The shape is part of the key, not just the bytes: read as 32x8 and as 8x32 these are two textures, and a
    // pool keyed on the bytes alone would hand back the first for the second.
    auto const pixels = make_pixels(32, 8, 3, false);
    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const wide = m.acquire_texture(sv::texture_data::create(pixels, sg::pixel_format::rgba8_unorm, 32, 8));
    auto const tall = m.acquire_texture(sv::texture_data::create(pixels, sg::pixel_format::rgba8_unorm, 8, 32));
    CHECK(wide != tall);
    CHECK(m.textures.count() == 2);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture given every mip is complete")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const full = sv::texture_data::create(make_pixels(8, 8, 4, true), sg::pixel_format::rgba8_unorm, 8, 8,
                                               sv::impl::mip_count_of(8, 8));
    auto const id = m.acquire_texture(full);
    auto const* const record = m.textures.get_ptr(id);
    REQUIRE(record != nullptr);

    // Nothing is left for a follow-up to do, which is what `complete` means — the policy asked for mips and got them.
    CHECK(record->state == sv::residency::complete);
    CHECK(record->uploaded_mips == record->total_mips);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture's element is declared for the epoch that acquired it")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    auto const id
        = m.acquire_texture(sv::texture_data::create(make_pixels(8, 8, 5, false), sg::pixel_format::rgba8_unorm, 8, 8));
    auto const index = m.textures.get_ptr(id)->index();

    {
        auto const bound = m.freeze();
        CHECK(bound.elements(sv::bindless_table::textures_2d).size() == 1);
        CHECK(bound.elements(sv::bindless_table::textures_2d)[0] == index);
    }

    // Its index is pinned, so it survives an epoch nobody re-acquired it in — that is the whole point of the pin.
    ctx.advance_epoch_and_wait_for_idle();
    m.advance_to(ctx.current_epoch());
    CHECK(m.textures.get_ptr(id)->index() == index);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - mip generation is queued, not done inline")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // record_pending_work drives sr::box_filter_mipmap_routine, whose init blocks on its shader compile — so
    // this needs the shared library and a compiler, not just a device.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the mipmap shader");

    auto m = sv::gpu_resource_manager::create(ctx);
    m.advance_to(ctx.current_epoch());

    // An acquire is on the caller's critical path, so the follow-up is queued rather than recorded there.
    auto const id = m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 6, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 1);
    CHECK(m.textures.get_ptr(id)->state == sv::residency::base_resident);

    // Re-acquiring the same content does not queue it a second time.
    (void)m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 6, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 1);

    auto cmd = ctx.create_command_list();
    auto const spent = m.record_pending_work(*cmd);
    ctx.submit_command_list(cc::move(cmd));

    CHECK(spent == sv::impl::mip_count_of(16, 16) - 1);
    CHECK(m.pending_work_count() == 0);
    CHECK(m.textures.get_ptr(id)->state == sv::residency::complete);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - the work budget spreads mip generation across epochs")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    // record_pending_work drives sr::box_filter_mipmap_routine, whose init blocks on its shader compile — so
    // this needs the shared library and a compiler, not just a device.
    auto const& env = sv_test::shared_env();
    if (!env.has_compiler)
        SKIP("no DXC compiler to build the mipmap shader");

    // The microstutter guard: several textures landing at once must not record every chain in one frame.
    // 16x16 is 5 levels, so 4 dispatches each — a budget of 5 admits exactly one per epoch.
    auto m = sv::gpu_resource_manager::create(ctx, {.work = {.max_dispatches_per_epoch = 5}});
    m.advance_to(ctx.current_epoch());

    for (auto seed = u8(0); seed < 3; ++seed)
        (void)m.acquire_texture(
            sv::texture_data::create(make_pixels(16, 16, u8(20 + seed), false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 3);

    auto const drain = [&]
    {
        auto cmd = ctx.create_command_list();
        auto const spent = m.record_pending_work(*cmd);
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch_and_wait_for_idle();
        m.advance_to(ctx.current_epoch());
        return spent;
    };

    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 2);
    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 1);
    CHECK(drain() == 4);
    CHECK(m.pending_work_count() == 0);
    CHECK(drain() == 0);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv - a texture policy that wants no mips queues nothing")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context_handle const ctx_h = ctx_r.value();
    sg::context& ctx = *ctx_h;

    auto m = sv::gpu_resource_manager::create(ctx, {.textures_policy = {.generate_mips = false}});
    m.advance_to(ctx.current_epoch());

    auto const id = m.acquire_texture(
        sv::texture_data::create(make_pixels(16, 16, 7, false), sg::pixel_format::rgba8_unorm, 16, 16));
    CHECK(m.pending_work_count() == 0);

    // It stays at its base level, which is a resolvable state rather than a failure.
    CHECK(m.textures.get_ptr(id)->state == sv::residency::base_resident);

    ctx.advance_epoch_and_wait_for_idle();
}
