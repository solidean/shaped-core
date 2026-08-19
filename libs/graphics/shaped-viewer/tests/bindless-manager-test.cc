#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-viewer/resources/bindless_manager.hh>

using namespace cc::primitive_defines;

// sv::bindless_manager on a real device (WARP or hardware): the group serves the SAME handle while the
// mirrors are unchanged, recreates only on change, and validates its lock/unlock protocol.
// The slot mechanics are covered CPU-side by slot-table-test.cc.

namespace
{
[[nodiscard]] sg::texture_2d make_texture(sg::context& ctx)
{
    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 16;
    desc.height = 16;
    desc.usage = sg::texture_usage::readonly_texture;
    return sg::texture_2d::from_raw(ctx.persistent.create_raw_texture(desc));
}
} // namespace

TEST("sv bindless_manager - unchanged mirrors serve the same group, changes recreate it")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context& ctx = *ctx_r.value();

    auto manager = sv::bindless_manager::create(ctx, {.buffer_count = 4, .texture_2d_count = 4});

    auto const buf = ctx.persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    auto const tex = make_texture(ctx);

    auto const buf_slot
        = manager.acquire(sg::as_buffer_view(sg::buffer<byte>::from_raw(buf).as_readonly_buffer().to_raw()));
    auto const tex_slot = manager.acquire(tex.as_readonly_view());
    CHECK(buf_slot != sv::bindless_buffer_slot::invalid);
    CHECK(tex_slot != sv::bindless_texture_2d_slot::invalid);

    auto const group = manager.lock_group();
    CHECK(group != nullptr);
    manager.unlock_group(group);

    // Next epoch, same working set: same slots, same group — nothing was reuploaded.
    ctx.advance_epoch_and_wait_for_idle();
    CHECK(manager.acquire(tex.as_readonly_view()) == tex_slot);
    auto const group_again = manager.lock_group();
    CHECK(group_again.get() == group.get());
    manager.unlock_group(group_again);

    // A new acquire dirties the mirror: the next lock serves a recreated group.
    ctx.advance_epoch_and_wait_for_idle();
    auto const tex2 = make_texture(ctx);
    auto const tex2_slot = manager.acquire(tex2.as_readonly_view());
    CHECK(tex2_slot != tex_slot);
    auto const group_recreated = manager.lock_group();
    CHECK(group_recreated.get() != group.get());
    manager.unlock_group(group_recreated);

    ctx.advance_epoch_and_wait_for_idle();
}

TEST("sv bindless_manager - lock/unlock protocol violations assert")
{
    auto ctx_r = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    if (ctx_r.has_error())
        SKIP("no Direct3D 12 device (hardware or WARP)");
    sg::context& ctx = *ctx_r.value();

    auto manager = sv::bindless_manager::create(ctx, {.buffer_count = 4, .texture_2d_count = 4});
    auto const tex = make_texture(ctx);
    (void)manager.acquire(tex.as_readonly_view());

    // Unlock without a lock.
    auto const group = manager.lock_group();
    manager.unlock_group(group);
    CHECK_ASSERTS(manager.unlock_group(group));

    // Acquires and a second lock are refused while locked; unlock must get the served group back.
    auto const locked = manager.lock_group();
    CHECK(manager.is_locked());
    CHECK_ASSERTS(manager.acquire(tex.as_readonly_view()));
    CHECK_ASSERTS(manager.lock_group());
    CHECK_ASSERTS(manager.unlock_group(nullptr));
    manager.unlock_group(locked);

    // Lock and unlock must happen in the same epoch.
    auto const cross_epoch = manager.lock_group();
    ctx.advance_epoch_and_wait_for_idle();
    CHECK_ASSERTS(manager.unlock_group(cross_epoch));
}
