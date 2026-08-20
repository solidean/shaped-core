#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>

using namespace cc::primitive_defines;

// staging_binding_group: a mutable descriptor image that mints immutable binding_groups.
// What these pin is the contract around it — the snapshot cache, which setter family a binding accepts, the
// bounds every element index is checked against, and what "set the whole array" clears.
// Whether a snapshot's descriptors actually drive a dispatch needs a shader, so it lives in the dx12 suite
// (backends/dx12/tests/dx12-staging-binding-group-test.cc).
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

// A hand-written texture binding must set texture_dimension — a vacant element's null descriptor is
// synthesized from the binding alone.
[[nodiscard]] sg::binding texture_binding(u32 count)
{
    return {.name = "Textures",
            .set = 0,
            .index = 0,
            .count = count,
            .type = sg::binding_type::readonly_texture,
            .texture_dimension = sg::texture_view_dimension::tex_2d};
}

[[nodiscard]] sg::binding_group_layout_handle texture_layout(sg::context_handle const& ctx, u32 count)
{
    auto const b = texture_binding(count);
    return ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
}

// A staging group over one texture binding of `count` elements.
[[nodiscard]] sg::staging_binding_group_handle make_staging(sg::context_handle const& ctx, u32 count)
{
    return ctx->persistent.create_staging_binding_group(texture_layout(ctx, count));
}
} // namespace

INVOCABLE_TEST("sg - a staging binding must be set, even to nothing", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 8);
    REQUIRE(staging != nullptr);
    auto const slot = staging->slot_of("Textures");

    // The group starts fully vacant and an all-vacant array is a legitimate bindless table — but a binding
    // nobody mentioned is a wiring bug, so saying "deliberately empty" is what snapshot() asks for.
    CHECK_ASSERTS(staging->snapshot());

    staging->unset_array(slot);
    CHECK(staging->snapshot() != nullptr);
}

INVOCABLE_TEST("sg - an empty staging range counts as setting the binding", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 8);
    REQUIRE(staging != nullptr);
    auto const slot = staging->slot_of("Textures");

    // Writes no descriptor at all, and still answers the demand — the caller has looked at this binding.
    staging->set_array_range(slot, 0, {});
    CHECK(staging->snapshot() != nullptr);
}

INVOCABLE_TEST("sg - staging binding group caches an unchanged snapshot", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 8);
    REQUIRE(staging != nullptr);

    auto const tex = make_texture(ctx);
    auto const slot = staging->slot_of("Textures");
    staging->set_array_element(slot, 2, tex.as_readonly_view());
    CHECK(staging->is_dirty());

    auto const first = staging->snapshot();
    CHECK(!staging->is_dirty());

    // No set in between: the same group comes back, so an unchanged frame rebinds nothing and copies nothing.
    CHECK(staging->snapshot() == first);

    staging->set_array_element(slot, 5, tex.as_readonly_view());
    CHECK(staging->is_dirty());
    CHECK(staging->snapshot() != first);
}

INVOCABLE_TEST("sg - a staging snapshot survives later mutation", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 4);
    REQUIRE(staging != nullptr);

    auto const slot = staging->slot_of("Textures");
    staging->set_array_element(slot, 0, make_texture(ctx).as_readonly_view());
    auto const held = staging->snapshot();
    REQUIRE(held != nullptr);

    // The held snapshot is immutable and keeps its own resources alive — mutating the builder only dirties it.
    staging->unset_array_element(slot, 0);
    staging->set_array_element(slot, 3, make_texture(ctx).as_readonly_view());
    CHECK(staging->snapshot() != held);
    CHECK(held != nullptr);
}

INVOCABLE_TEST("sg - staging binding group resolves names to slots", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 4);
    REQUIRE(staging != nullptr);

    auto const slot = staging->slot_of("Textures");
    CHECK(slot != sg::binding_slot::invalid);
    CHECK(staging->slot_of("Nope") == sg::binding_slot::invalid);

    // The slot indirects to the binding's shape, which is what bounds every element index.
    CHECK(staging->is_array(slot));
    CHECK(staging->array_size(slot) == 4);

    auto scalar = make_staging(ctx, 1);
    REQUIRE(scalar != nullptr);
    CHECK(!scalar->is_array(scalar->slot_of("Textures")));
    CHECK(scalar->array_size(scalar->slot_of("Textures")) == 1);
}

INVOCABLE_TEST("sg - setting a whole staging array clears what the run does not cover", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 4);
    REQUIRE(staging != nullptr);
    auto const slot = staging->slot_of("Textures");

    auto const tex = make_texture(ctx);
    staging->set_array_element(slot, 0, tex.as_readonly_view());
    staging->set_array_element(slot, 3, tex.as_readonly_view());
    auto const filled = staging->snapshot();
    REQUIRE(filled != nullptr);

    // set_array REPLACES the array: elements 0 and 3 go vacant even though this run never mentions them.
    sg::raw_view const one[] = {tex.as_readonly_view()};
    staging->set_array(slot, 1, one);
    CHECK(staging->is_dirty());
    CHECK(staging->snapshot() != filled);

    // set_array_range PATCHES it, leaving everything outside the run alone.
    staging->set_array_range(slot, 2, one);
    CHECK(staging->is_dirty());
    CHECK(staging->snapshot() != nullptr);

    staging->unset_array(slot);
    CHECK(staging->snapshot() != nullptr);
}

INVOCABLE_TEST("sg - staging binding group demands its scalar bindings", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // A scalar has nothing to be "deliberately empty" about — it is set to a view, and only then snapshots.
    auto staging = make_staging(ctx, 1);
    REQUIRE(staging != nullptr);

    CHECK_ASSERTS(staging->snapshot());

    staging->set_binding("Textures", make_texture(ctx).as_readonly_view());
    CHECK(staging->snapshot() != nullptr);

    // Once set it stays set: a scalar has no unset, it is only ever set to another view.
    staging->set_binding("Textures", make_texture(ctx).as_readonly_view());
    CHECK(staging->snapshot() != nullptr);
}

INVOCABLE_TEST("sg - a staging scalar binding can be set to an empty view", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    // An empty scalar is a VALUE, not an absence: sg::tlas_view{} is the null acceleration structure every ray misses.
    // It goes in through set_binding like any other view, and satisfies the demand above.
    sg::binding const b
        = {.name = "Scene", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::acceleration_structure};
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto staging = ctx->persistent.create_staging_binding_group(layout);
    REQUIRE(staging != nullptr);

    CHECK_ASSERTS(staging->snapshot()); // never set at all — still a wiring bug
    staging->set_binding("Scene", sg::tlas_view{});
    CHECK(staging->snapshot() != nullptr);
}

INVOCABLE_TEST("sg - a staging setter rejects the wrong binding shape", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto array = make_staging(ctx, 4);
    auto scalar = make_staging(ctx, 1);
    REQUIRE(array != nullptr);
    REQUIRE(scalar != nullptr);

    auto const tex = make_texture(ctx);
    auto const array_slot = array->slot_of("Textures");
    auto const scalar_slot = scalar->slot_of("Textures");

    // Nothing here picks an element for you: an array takes the array family, a scalar takes set_binding.
    CHECK_ASSERTS(array->set_binding(array_slot, tex.as_readonly_view()));
    CHECK_ASSERTS(scalar->set_array_element(scalar_slot, 0, tex.as_readonly_view()));
    CHECK_ASSERTS(scalar->unset_array_element(scalar_slot, 0));
    CHECK_ASSERTS(scalar->unset_array(scalar_slot));
}

INVOCABLE_TEST("sg - staging element indices are bounds-checked", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 4);
    REQUIRE(staging != nullptr);
    auto const slot = staging->slot_of("Textures");

    auto const tex = make_texture(ctx);
    sg::raw_view const two[] = {tex.as_readonly_view(), tex.as_readonly_view()};

    CHECK_ASSERTS(staging->set_array_element(slot, 4, tex.as_readonly_view()));
    CHECK_ASSERTS(staging->set_array_element(slot, -1, tex.as_readonly_view()));
    CHECK_ASSERTS(staging->unset_array_element(slot, 4));
    CHECK_ASSERTS(staging->set_array_range(slot, 3, two)); // 3 + 2 > 4
    CHECK_ASSERTS(staging->set_array(slot, 3, two));
    CHECK_ASSERTS(staging->unset_array_range(slot, 2, 3));
    CHECK_ASSERTS(staging->set_array_element(sg::binding_slot::invalid, 0, tex.as_readonly_view()));
}

INVOCABLE_TEST("sg - a staging setter rejects a view of the wrong kind", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto staging = make_staging(ctx, 4);
    REQUIRE(staging != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    REQUIRE(buf != nullptr);

    // A buffer view in a texture array — the slot came from the layout, so a wrong kind is a caller bug.
    CHECK_ASSERTS(staging->set_array_element(staging->slot_of("Textures"), 0,
                                             sg::raw_view(sg::buffer<byte>::from_raw(buf).as_readonly_buffer())));

    // Clearing is what unset_* is for; a set never takes a vacant view.
    CHECK_ASSERTS(staging->set_array_element(staging->slot_of("Textures"), 0, sg::raw_view(sg::vacant_view{})));
}
