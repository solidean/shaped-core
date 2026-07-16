#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

#include <cstddef> // offsetof

// Geometry- and tessellation-stage raster pipelines, end to end on WARP. Each compiles the extra stage,
// builds an sg::raster_pipeline that includes it, renders over a cleared target, and reads the target back
// to prove the stage actually ran and its output reached the rasterizer. Same rhythm as
// end-to-end-raster-test.cc's plain vertex+pixel triangle, one stage further down the pipeline.

namespace
{
// A bare position-only vertex — the geometry test feeds it a single point, the tessellation test three
// control points of a patch. Both stages synthesize their own color, so no color attribute is needed.
struct pos_vertex
{
    float position[3];
};
} // namespace

template <>
struct sg::vertex_layout_of<pos_vertex>
{
    static sg::vertex_type_layout get()
    {
        return {.stride = sizeof(pos_vertex),
                .attributes = {
                    {.semantic = "POSITION",
                     .format = sg::vertex_attribute_format::vec3f,
                     .offset = offsetof(pos_vertex, position)},
                }};
    }
};

namespace
{
constexpr char const* geometry_hlsl = R"(
struct vs_input  { float3 position : POSITION; };
struct vs_output { float4 position : SV_Position; };
struct gs_output { float4 position : SV_Position; float4 color : COLOR; };

vs_output main_vs(vs_input input)
{
    vs_output o;
    o.position = float4(input.position, 1.0);
    return o;
}

// Amplify a single point into a triangle centered on it — the distinctive geometry-shader capability
// (one primitive in, several out). The emitted triangle is painted green.
[maxvertexcount(3)]
void main_gs(point vs_output input[1], inout TriangleStream<gs_output> stream)
{
    float2 c = input[0].position.xy;
    float2 offsets[3] = { float2(0.0, 0.8), float2(-0.8, -0.8), float2(0.8, -0.8) };
    for (int i = 0; i < 3; ++i)
    {
        gs_output o;
        o.position = float4(c + offsets[i], 0.0, 1.0);
        o.color = float4(0, 1, 0, 1);
        stream.Append(o);
    }
    stream.RestartStrip();
}

float4 main_ps(gs_output input) : SV_Target { return input.color; }
)";

constexpr char const* tessellation_hlsl = R"(
struct vs_input  { float3 position : POSITION; };
struct vs_output { float3 position : POSITION; };

vs_output main_vs(vs_input input)
{
    vs_output o;
    o.position = input.position;
    return o;
}

struct hs_const_output
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};
struct hs_output { float3 position : POSITION; };

// Tessellation factors of 1 = no subdivision: the patch comes out as the original triangle, just routed
// through the hull + domain stages (proving that path executes).
hs_const_output main_hs_const(InputPatch<vs_output, 3> patch)
{
    hs_const_output o;
    o.edges[0] = 1.0;
    o.edges[1] = 1.0;
    o.edges[2] = 1.0;
    o.inside = 1.0;
    return o;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("main_hs_const")]
hs_output main_hs(InputPatch<vs_output, 3> patch, uint id : SV_OutputControlPointID)
{
    hs_output o;
    o.position = patch[id].position;
    return o;
}

struct ds_output { float4 position : SV_Position; float4 color : COLOR; };

[domain("tri")]
ds_output main_ds(hs_const_output constants, float3 bary : SV_DomainLocation, const OutputPatch<hs_output, 3> patch)
{
    ds_output o;
    float3 p = bary.x * patch[0].position + bary.y * patch[1].position + bary.z * patch[2].position;
    o.position = float4(p, 1.0);
    o.color = float4(0, 1, 0, 1);
    return o;
}

float4 main_ps(ds_output input) : SV_Target { return input.color; }
)";
} // namespace

TEST("ssc::dxc + dx12 - geometry shader amplifies a point into a triangle")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = geometry_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader gs = compile("main_gs", sg::shader_stage::geometry);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    auto pipeline_layout = ctx.uncached.create_pipeline_layout({});
    REQUIRE(pipeline_layout != nullptr);

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.geometry_shader = cc::move(gs);
    desc.fragment_shader = cc::move(ps);
    desc.vertex_input = sg::vertex_input_layout::create<pos_vertex>();
    desc.topology = sg::primitive_topology::point_list;
    desc.rasterization.cull = sg::cull_mode::none;
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto pipeline = ctx.uncached.create_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    sg::texture_description td;
    td.format = sg::pixel_format::rgba8_unorm;
    td.dimension = sg::texture_dimension::d2;
    td.width = W;
    td.height = H;
    td.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    auto tex = ctx.persistent.create_raw_texture(td);
    REQUIRE(tex != nullptr);
    sg::texture_2d const typed(tex);

    // A single point at the target center — the geometry shader expands it into the triangle.
    pos_vertex const verts[1] = {{{0.0f, 0.0f, 0.0f}}};
    auto vbuf = ctx.persistent.create_raw_buffer(cc::isize(sizeof(verts)),
                                                 sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vbuf != nullptr);

    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(vbuf, cc::span<pos_vertex const>(verts));
    ctx.submit_command_list(cc::move(up));

    auto cmd = ctx.create_command_list();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 0, 1, 1))}});
        cmd->raster.bind_pipeline(*pipeline);
        cmd->raster.bind_vertex_buffers({sg::buffer<pos_vertex>::from_raw(vbuf).as_vertex_buffer()});
        cmd->raster.draw({.vertex_range = {.offset = 0, .size = 1}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = ctx.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(W) * cc::isize(H) * 4);
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());
    auto texel = [&](int x, int y) { return px + (cc::isize(y) * W + x) * 4; };

    auto const* center = texel(W / 2, H / 2);
    CHECK(center[0] == 0);
    CHECK(center[1] == 255);
    CHECK(center[2] == 0);
    CHECK(center[3] == 255);

    auto const* corner = texel(0, 0);
    CHECK(corner[0] == 0);
    CHECK(corner[1] == 0);
    CHECK(corner[2] == 255);
    CHECK(corner[3] == 255);
}

TEST("ssc::dxc + dx12 - tessellation (hull + domain) renders a patch triangle")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = tessellation_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader hs = compile("main_hs", sg::shader_stage::tessellation_control);
    sg::compiled_shader ds = compile("main_ds", sg::shader_stage::tessellation_evaluation);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    auto pipeline_layout = ctx.uncached.create_pipeline_layout({});
    REQUIRE(pipeline_layout != nullptr);

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.tessellation_control_shader = cc::move(hs);
    desc.tessellation_evaluation_shader = cc::move(ds);
    desc.fragment_shader = cc::move(ps);
    desc.vertex_input = sg::vertex_input_layout::create<pos_vertex>();
    desc.topology = sg::primitive_topology::patch_list;
    desc.patch_control_points = 3;
    desc.rasterization.cull = sg::cull_mode::none;
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto pipeline = ctx.uncached.create_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    sg::texture_description td;
    td.format = sg::pixel_format::rgba8_unorm;
    td.dimension = sg::texture_dimension::d2;
    td.width = W;
    td.height = H;
    td.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    auto tex = ctx.persistent.create_raw_texture(td);
    REQUIRE(tex != nullptr);
    sg::texture_2d const typed(tex);

    // Three control points of one triangular patch; tess factors of 1 pass them straight through.
    pos_vertex const verts[3] = {
        {{0.0f, 0.8f, 0.0f}},
        {{-0.8f, -0.8f, 0.0f}},
        {{0.8f, -0.8f, 0.0f}},
    };
    auto vbuf = ctx.persistent.create_raw_buffer(cc::isize(sizeof(verts)),
                                                 sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vbuf != nullptr);

    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(vbuf, cc::span<pos_vertex const>(verts));
    ctx.submit_command_list(cc::move(up));

    auto cmd = ctx.create_command_list();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 0, 1, 1))}});
        cmd->raster.bind_pipeline(*pipeline);
        cmd->raster.bind_vertex_buffers({sg::buffer<pos_vertex>::from_raw(vbuf).as_vertex_buffer()});
        cmd->raster.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = ctx.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(W) * cc::isize(H) * 4);
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());
    auto texel = [&](int x, int y) { return px + (cc::isize(y) * W + x) * 4; };

    auto const* center = texel(W / 2, H / 2);
    CHECK(center[0] == 0);
    CHECK(center[1] == 255);
    CHECK(center[2] == 0);
    CHECK(center[3] == 255);

    auto const* corner = texel(0, 0);
    CHECK(corner[0] == 0);
    CHECK(corner[1] == 0);
    CHECK(corner[2] == 255);
    CHECK(corner[3] == 255);
}
