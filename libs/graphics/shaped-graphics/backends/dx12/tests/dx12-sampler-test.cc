#include "dx12-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/sampler.hh>

// dx12 samplers: the sampler -> D3D12 translation (pure) and, on WARP, that a root signature
// with a static + a dynamic sampler and a binding group with a dynamic sampler are accepted by the debug
// layer. No sampling shader is dispatched — this covers the descriptor/root-signature wiring only.

namespace
{
namespace dx12 = sg::backend::dx12;

sg::texture_description sampled_tex()
{
    sg::texture_description d;
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = 16;
    d.height = 16;
    d.usage = sg::texture_usage::readonly_texture;
    return d;
}
} // namespace

TEST("sg dx12 - sampler translates to a D3D12 sampler desc")
{
    // A trilinear clamping sampler with 4x anisotropy.
    sg::sampler s;
    s.address_u = sg::sampler_address_mode::clamp_edge;
    s.address_v = sg::sampler_address_mode::clamp_border;
    s.address_w = sg::sampler_address_mode::mirror_repeat;
    s.max_anisotropy = 4;
    s.border_color = sg::sampler_border_color::opaque_white;

    D3D12_SAMPLER_DESC const d = dx12::to_d3d12_sampler_desc(s);
    CHECK(d.Filter == D3D12_FILTER_ANISOTROPIC); // anisotropy overrides the per-axis filters
    CHECK(d.AddressU == D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CHECK(d.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    CHECK(d.AddressW == D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
    CHECK(d.MaxAnisotropy == 4u);
    CHECK(d.ComparisonFunc == D3D12_COMPARISON_FUNC_NEVER); // non-comparison sampler
    CHECK(d.BorderColor[0] == 1.0f);
    CHECK(d.BorderColor[3] == 1.0f);

    // A point-sampled comparison ("shadow") sampler encodes the comparison reduction.
    sg::sampler shadow;
    shadow.min_filter = sg::sampler_filter::nearest;
    shadow.mag_filter = sg::sampler_filter::nearest;
    shadow.mip_filter = sg::sampler_filter::nearest;
    shadow.compare = sg::compare_op::less_equal;
    D3D12_SAMPLER_DESC const ds = dx12::to_d3d12_sampler_desc(shadow);
    CHECK(ds.Filter
          == D3D12_ENCODE_BASIC_FILTER(D3D12_FILTER_TYPE_POINT, D3D12_FILTER_TYPE_POINT, D3D12_FILTER_TYPE_POINT,
                                       D3D12_FILTER_REDUCTION_TYPE_COMPARISON));
    CHECK(ds.ComparisonFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL);

    // The static-sampler form carries the register address + the enum border color.
    D3D12_STATIC_SAMPLER_DESC const st
        = dx12::to_d3d12_static_sampler_desc(s, /*register*/ 3, /*space*/ 1, D3D12_SHADER_VISIBILITY_ALL);
    CHECK(st.ShaderRegister == 3u);
    CHECK(st.RegisterSpace == 1u);
    CHECK(st.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);
    CHECK(st.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER);
}

TEST("sg dx12 - a layout with static + dynamic samplers and a group build on WARP")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // A sampled texture (t0), one dynamic sampler (s0), and one static sampler (s1).
    sg::binding const bindings[] = {
        {.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture},
        {.name = "Dyn", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
        {.name = "Static", .set = 0, .index = 1, .count = 1, .type = sg::binding_type::sampler},
    };
    sg::named_sampler const statics[]
        = {{.name = "Static", .sampler = {.address_u = sg::sampler_address_mode::clamp_edge}}};

    auto layout = c.create_dx12_binding_group_layout(bindings, statics, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value()); // group layout: an SRV range, a SAMPLER range, and one baked static sampler desc
    CHECK(layout.value()->sampler_slots.size() == 1); // only the dynamic sampler is a table entry
    CHECK(layout.value()->view_slots.size() == 1);

    auto tex = c.create_dx12_texture(sampled_tex(), sg::allocation_info{});
    REQUIRE(tex.has_value());
    auto const typed = sg::texture_2d::from_raw(tex.value());

    sg::named_view const views[] = {{.name = "Tex", .view = typed.as_readonly_view()}};
    sg::named_sampler const dyn[] = {{.name = "Dyn", .sampler = {.mag_filter = sg::sampler_filter::nearest}}};

    auto group = c.create_dx12_binding_group(layout.value(), views, dyn, sg::lifetime_scope::persistent);
    REQUIRE(group.has_value()); // CreateSampler into the sampler heap + both descriptor tables — debug-layer clean
}

TEST("sg dx12 - static sampler naming no binding is rejected")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    sg::binding const bindings[] = {
        {.name = "Dyn", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
    };
    sg::named_sampler const statics[] = {{.name = "Nope", .sampler = {}}}; // matches no sampler binding

    auto layout = c.create_dx12_binding_group_layout(bindings, statics, sg::lifetime_scope::persistent);
    CHECK(!layout.has_value());
}

TEST("sg dx12 - a missing dynamic sampler is rejected at group creation")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    sg::binding const bindings[] = {
        {.name = "Dyn", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
    };
    auto layout = c.create_dx12_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    // No samplers provided → the dynamic "Dyn" binding is unfilled.
    auto group = c.create_dx12_binding_group(layout.value(), {}, {}, sg::lifetime_scope::persistent);
    CHECK(!group.has_value());

    // A sampler named for a binding that does not exist is also rejected.
    sg::named_sampler const wrong[] = {{.name = "Ghost", .sampler = {}}};
    auto group2 = c.create_dx12_binding_group(layout.value(), {}, wrong, sg::lifetime_scope::persistent);
    CHECK(!group2.has_value());
}

TEST("sg dx12 - a pipeline-level static sampler bakes into the root signature on WARP")
{
    auto handle = dx12::acquire_warp_context();
    REQUIRE(handle != nullptr);
    auto& c = static_cast<dx12::dx12_context&>(*handle);

    // A group layout with just a texture SRV — no samplers of its own.
    sg::binding const bindings[] = {
        {.name = "Tex", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::readonly_texture},
    };
    auto group_layout = c.create_dx12_binding_group_layout(bindings, {}, sg::lifetime_scope::persistent);
    REQUIRE(group_layout.has_value());

    // The pipeline layout carries an extra register-bound static sampler at s0 — not tied to any group
    // binding — which bakes straight into the root signature.
    sg::pipeline_layout_description pld;
    pld.groups = {group_layout.value()};
    pld.static_samplers.push_back(
        {.binding = {.name = "Samp", .set = 0, .index = 0, .count = 1, .type = sg::binding_type::sampler},
         .sampler = {.address_u = sg::sampler_address_mode::clamp_edge}});

    auto pipeline_layout = c.create_dx12_pipeline_layout(pld, sg::lifetime_scope::persistent);
    REQUIRE(pipeline_layout.has_value()); // root sig: the group's SRV table + one baked static sampler
    CHECK(pipeline_layout.value()->groups.size() == 1);
}
