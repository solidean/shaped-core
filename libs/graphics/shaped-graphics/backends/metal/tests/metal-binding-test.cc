#include "metal-test-common.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>

// The bind path: layouts, and the argument buffer a binding group encodes.
//
// These drive the backend-typed creates rather than the sg scopes, because those hand back a `cc::result` whose message
// says what was wrong — where a scope's throwing façade leaves a test able to report only that something threw.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
struct particle
{
    u32 a, b, c, d;
};

[[nodiscard]] sg::binding structured_binding(cc::string name, u32 index)
{
    return {
        .name = cc::move(name),
        .space = 0,
        .index = index,
        .count = 1,
        .type = sg::binding_type::readwrite_structured_buffer,
    };
}
} // namespace

TEST("sg metal - a binding group encodes a buffer view")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const b = structured_binding("Data", 0);
    auto layout
        = ctx->create_metal_binding_group_layout(cc::span<sg::binding const>(&b, 1), {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());

    // One binding at index 0 occupies one argument slot; that count is what sizes the argument buffer.
    CHECK(layout.value()->argument_slot_count() == 1);

    auto const buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    auto const nv = sg::named_view{.name = "Data", .view = sg::buffer<particle>::from_raw(buffer).as_readwrite_buffer()};

    auto group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                 sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()).context(group.has_error() ? group.error().to_string() : cc::string());

    // The argument buffer is what an MTL4 argument table binds, so a group with no address is not bindable at all.
    CHECK(group.value()->argument_address() != 0);
}

TEST("sg metal - an array binding occupies consecutive argument slots")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The slot count is not the highest index: an array takes `count` slots from its own index, which is what an MSL
    // `[[id(n)]]` array addresses.
    auto const bindings = cc::array<sg::binding>{
        structured_binding("First", 0),
        {.name = "Table", .space = 0, .index = 1, .count = 4, .type = sg::binding_type::readonly_structured_buffer},
    };

    auto layout = ctx->create_metal_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()).context(layout.has_error() ? layout.error().to_string() : cc::string());
    CHECK(layout.value()->argument_slot_count() == 5);
}

TEST("sg metal - a layout refuses two bindings at one index")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Metal, like SPIR-V and WGSL, has one namespace per group, so two bindings at one index is not a layout it can
    // express — and silently aliasing them onto one argument slot is what refusing this prevents.
    // HLSL makes the collision look normal, which is why the check is worth having rather than assuming.
    auto const bindings = cc::array<sg::binding>{structured_binding("A", 0), structured_binding("B", 0)};

    auto const layout = ctx->create_metal_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    CHECK(layout.has_error());
}

TEST("sg metal - a binding group refuses a view of the wrong kind")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const b = structured_binding("Data", 0);
    auto layout
        = ctx->create_metal_binding_group_layout(cc::span<sg::binding const>(&b, 1), {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    // A raw view where the binding wants a structured one: the shapes are what `sg::accepts` separates, and binding
    // the wrong one would have the shader read through a pointer of the wrong stride.
    auto const buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    auto const nv = sg::named_view{.name = "Data", .view = buffer->as_raw_readwrite({.offset = 0, .size = 256})};

    auto const group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                       sg::lifetime_scope::persistent);
    CHECK(group.has_error());
}

TEST("sg metal - a binding group refuses an unknown binding name")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto const b = structured_binding("Data", 0);
    auto layout
        = ctx->create_metal_binding_group_layout(cc::span<sg::binding const>(&b, 1), {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto const buffer = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readwrite_buffer);
    auto const nv
        = sg::named_view{.name = "Absent", .view = sg::buffer<particle>::from_raw(buffer).as_readwrite_buffer()};

    auto const group = ctx->create_metal_binding_group(layout.value(), cc::span<sg::named_view const>(&nv, 1), {},
                                                       sg::lifetime_scope::persistent);
    CHECK(group.has_error());
}
