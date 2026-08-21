#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/bindless_array.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>

using namespace cc::primitive_defines;

// sg::bindless_array over one array binding of a staging group: identity, per-epoch index lifetime, and the
// lock/unlock protocol.
// Minting the snapshot stays the caller's — the array shares the group's handle and only maps views to indices.
// The slot mechanics underneath are covered CPU-side by slot-table-test.cc.
// Each test is an INVOCABLE_TEST run against every available backend (see tests/context/context-test.cc).

namespace
{
[[nodiscard]] sg::texture_2d make_texture(sg::context_handle const& ctx)
{
    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 16;
    desc.height = 16;
    desc.usage = sg::texture_usage::readonly_texture;
    return sg::texture_2d::from_raw(ctx->persistent.create_raw_texture(desc));
}

[[nodiscard]] sg::readonly_buffer_view<byte> make_buffer(sg::context_handle const& ctx)
{
    auto const raw = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    return sg::buffer<byte>::from_raw(raw).as_readonly_buffer();
}

// Two bindless tables in one group, each in its own register space (index 0 each), the way a bindless group
// is laid out: a category is addressed with no register-offset math.
[[nodiscard]] sg::staging_binding_group_handle make_group(sg::context_handle const& ctx, u32 count)
{
    sg::binding const bindings[]
        = {{.name = "Buffers", .space = 1, .index = 0, .count = count, .type = sg::binding_type::readonly_raw_buffer},
           {.name = "Textures",
            .space = 2,
            .index = 0,
            .count = count,
            .type = sg::binding_type::readonly_texture,
            .texture_dimension = sg::texture_view_dimension::tex_2d}};
    return ctx->persistent.create_staging_binding_group(ctx->uncached.create_binding_group_layout(bindings));
}
} // namespace

INVOCABLE_TEST("sg - an unchanged bindless working set serves the same snapshot", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto group = make_group(ctx, 4);
    REQUIRE(group != nullptr);
    auto buffers = sg::bindless_array::for_binding(*ctx, group, "Buffers");
    auto textures = sg::bindless_array::for_binding(*ctx, group, "Textures");
    CHECK(buffers.capacity() == 4);

    auto const buf = make_buffer(ctx);
    auto const tex = make_texture(ctx);
    auto const buf_index = buffers.acquire(buf);
    auto const tex_index = textures.acquire(tex.as_readonly_view());
    CHECK(buffers.occupied_count() == 1);

    auto const snapshot = group->snapshot();
    CHECK(snapshot != nullptr);

    // Next epoch, same working set: same indices, no descriptor touched — so the group is not dirty and the
    // cached snapshot is served again.
    ctx->advance_epoch_and_wait_for_idle();
    CHECK(buffers.acquire(buf) == buf_index);
    CHECK(textures.acquire(tex.as_readonly_view()) == tex_index);
    CHECK(group->snapshot().get() == snapshot.get());

    // A new view mints an index and writes its descriptor: the next snapshot is a new group.
    ctx->advance_epoch_and_wait_for_idle();
    auto const tex2 = make_texture(ctx);
    CHECK(textures.acquire(tex2.as_readonly_view()) != tex_index);
    CHECK(group->snapshot().get() != snapshot.get());

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - two bindless arrays over one group are independent", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto group = make_group(ctx, 4);
    REQUIRE(group != nullptr);
    auto buffers = sg::bindless_array::for_binding(*ctx, group, "Buffers");
    auto textures = sg::bindless_array::for_binding(*ctx, group, "Textures");

    // Each array indexes its own binding, so the first view of either lands at element 0.
    CHECK(buffers.acquire(make_buffer(ctx)) == 0);
    CHECK(textures.acquire(make_texture(ctx).as_readonly_view()) == 0);

    // A lock covers the array it was taken on and nothing else.
    buffers.lock();
    CHECK(!textures.is_locked());
    CHECK(textures.acquire(make_texture(ctx).as_readonly_view()) == 1);
    buffers.unlock();

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - a full bindless array clears the descriptors it reclaims", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // The seam only these cover: the stale sweep has to reach the group, or the table and the descriptors
    // drift apart and the reused element still points at the resource it was reclaimed from.
    auto group = make_group(ctx, 2);
    REQUIRE(group != nullptr);
    auto textures = sg::bindless_array::for_binding(*ctx, group, "Textures");
    (void)sg::bindless_array::for_binding(*ctx, group, "Buffers"); // so the group is fully wired

    auto const a = make_texture(ctx);
    auto const b = make_texture(ctx);
    (void)textures.acquire(a.as_readonly_view());
    (void)textures.acquire(b.as_readonly_view());
    CHECK(textures.occupied_count() == 2);
    auto const before = group->snapshot();

    // Next epoch, a third view into a full array: both stale elements are cleared on the group, and the new
    // view takes one of them.
    ctx->advance_epoch_and_wait_for_idle();
    auto const c = make_texture(ctx);
    auto const c_index = textures.acquire(c.as_readonly_view());
    CHECK(c_index < textures.capacity());
    CHECK(textures.occupied_count() == 1);

    // The unset_array_element calls left the group in a mintable state, and something did change.
    auto const after = group->snapshot();
    CHECK(after != nullptr);
    CHECK(after.get() != before.get());

    ctx->advance_epoch_and_wait_for_idle();
}

INVOCABLE_TEST("sg - bindless lock/unlock protocol violations assert", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto group = make_group(ctx, 4);
    REQUIRE(group != nullptr);
    auto textures = sg::bindless_array::for_binding(*ctx, group, "Textures");
    (void)sg::bindless_array::for_binding(*ctx, group, "Buffers"); // so the group is fully wired
    auto const tex = make_texture(ctx);
    (void)textures.acquire(tex.as_readonly_view());

    // Unlock without a lock.
    textures.lock();
    textures.unlock();
    CHECK_ASSERTS(textures.unlock());

    // Acquires and a second lock are refused while locked.
    textures.lock();
    CHECK(textures.is_locked());
    CHECK_ASSERTS(textures.acquire(tex.as_readonly_view()));
    CHECK_ASSERTS(textures.lock());
    textures.unlock();

    // Lock and unlock must happen in the same epoch.
    textures.lock();
    ctx->advance_epoch_and_wait_for_idle();
    CHECK_ASSERTS(textures.unlock());
}

INVOCABLE_TEST("sg - the scoped bindless lock unlocks at scope exit", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto group = make_group(ctx, 4);
    REQUIRE(group != nullptr);
    auto textures = sg::bindless_array::for_binding(*ctx, group, "Textures");
    auto const tex = make_texture(ctx);
    (void)textures.acquire(tex.as_readonly_view());

    {
        auto const lock = textures.lock_scoped();
        CHECK(textures.is_locked());
        CHECK_ASSERTS(textures.acquire(tex.as_readonly_view()));
    }
    CHECK(!textures.is_locked());

    // A moved-from lock is disarmed: exactly one unlock happens, from the survivor.
    {
        auto lock = textures.lock_scoped();
        [[maybe_unused]] auto const moved = cc::move(lock);
        CHECK(textures.is_locked());
    }
    CHECK(!textures.is_locked());

    ctx->advance_epoch_and_wait_for_idle();
}
