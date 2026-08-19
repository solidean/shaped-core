#include <clean-core/container/span.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh> // sg::named_view
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/raw_buffer.hh>
#include <shaped-graphics/resource/raw_texture.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>

using namespace cc::primitive_defines;

// Array bindings (binding.count > 1): a named_view supplies exactly `count` views, one per element, and a
// vacant element is a null-handle view that still names its arm (dimension + format for a texture).
// Each test is an INVOCABLE_TEST run against every available backend (see tests/context/context-test.cc).
// The dispatch side — declare_array_*_access and its accounting — needs a shader and lives in the DXC-gated
// end-to-end suite (shaped-shader-compiler-dxc's tests).

namespace
{
// A readonly 2D texture usable as an array element.
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

// A vacant texture element: a null handle that still carries the dimension + format the null descriptor needs.
[[nodiscard]] sg::raw_texture_view vacant_texture_2d()
{
    return {.access = sg::view_class::readonly,
            .texture = nullptr,
            .view_dimension = sg::texture_view_dimension::tex_2d,
            .format = sg::pixel_format::rgba8_unorm};
}

// A vacant raw-buffer element: a null handle with the readonly raw shape.
[[nodiscard]] sg::raw_buffer_view vacant_raw_buffer()
{
    return {.access = sg::view_class::readonly, .shape = sg::view_shape::raw, .buffer = nullptr};
}

// One texture array binding of `count` elements.
[[nodiscard]] sg::binding texture_array_binding(u32 count)
{
    return {.name = "Textures", .set = 0, .index = 0, .count = count, .type = sg::binding_type::readonly_texture};
}
} // namespace

INVOCABLE_TEST("sg - array binding accepts a partially vacant element list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(8);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    // Elements 0, 3 and 7 are bound; the rest are vacant (null descriptors).
    auto const t0 = make_texture(ctx);
    auto const t3 = make_texture(ctx);
    auto const t7 = make_texture(ctx);
    auto nv = sg::named_view{.name = "Textures", .views = {}};
    for (isize i = 0; i < 8; ++i)
        nv.views.push_back(vacant_texture_2d());
    nv.views[0] = t0.as_readonly_view();
    nv.views[3] = t3.as_readonly_view();
    nv.views[7] = t7.as_readonly_view();

    auto group = ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group != nullptr);
}

INVOCABLE_TEST("sg - array binding accepts an all-vacant element list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(4);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto nv = sg::named_view{.name = "Textures", .views = {}};
    for (isize i = 0; i < 4; ++i)
        nv.views.push_back(vacant_texture_2d());

    auto group = ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group != nullptr);
}

INVOCABLE_TEST("sg - buffer array binding accepts bound and vacant elements", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    sg::binding const b
        = {.name = "Buffers", .set = 0, .index = 0, .count = 4, .type = sg::binding_type::readonly_raw_buffer};
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto buf = ctx->persistent.create_raw_buffer(256, sg::buffer_usage::readonly_buffer);
    REQUIRE(buf != nullptr);

    auto nv = sg::named_view{.name = "Buffers", .views = {}};
    nv.views.push_back(sg::buffer<byte>::from_raw(buf).as_readonly_buffer());
    nv.views.push_back(vacant_raw_buffer());
    nv.views.push_back(vacant_raw_buffer());
    nv.views.push_back(sg::buffer<byte>::from_raw(buf).as_readonly_buffer());

    auto group = ctx->persistent.create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group != nullptr);
}

INVOCABLE_TEST("sg - array binding rejects a wrong-size element list", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(4);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    // 3 views for a count-4 binding: an array takes exactly one view per element.
    auto nv = sg::named_view{.name = "Textures", .views = {}};
    for (isize i = 0; i < 3; ++i)
        nv.views.push_back(vacant_texture_2d());

    auto group = ctx->persistent.try_create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group.has_error());
}

INVOCABLE_TEST("sg - scalar binding rejects an element list of the wrong size", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(1);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    auto const tex = make_texture(ctx);
    auto nv = sg::named_view{.name = "Textures", .views = {}};
    nv.views.push_back(tex.as_readonly_view());
    nv.views.push_back(tex.as_readonly_view());

    auto group = ctx->persistent.try_create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group.has_error());
}

INVOCABLE_TEST("sg - array binding rejects a mismatched element", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(2);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    // Element 1 is a buffer view in a texture array: wrong arm, rejected per element.
    auto nv = sg::named_view{.name = "Textures", .views = {}};
    nv.views.push_back(vacant_texture_2d());
    nv.views.push_back(vacant_raw_buffer());

    auto group = ctx->persistent.try_create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group.has_error());
}

INVOCABLE_TEST("sg - scalar binding rejects a null-handle view", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);

    auto const b = texture_array_binding(1);
    auto layout = ctx->uncached.create_binding_group_layout(cc::span<sg::binding const>(&b, 1));
    REQUIRE(layout != nullptr);

    // A null-handle view is only valid as a vacant ARRAY element; a scalar binding must bind a resource.
    auto nv = sg::named_view{.name = "Textures", .views = {}};
    nv.views.push_back(vacant_texture_2d());

    auto group = ctx->persistent.try_create_binding_group(layout, cc::span<sg::named_view const>(&nv, 1));
    CHECK(group.has_error());
}
