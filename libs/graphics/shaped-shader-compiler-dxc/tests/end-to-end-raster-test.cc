#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

#include <cstddef> // offsetof

// Raster pipeline + draw end to end on WARP: compile a vertex + pixel shader, build an sg::raster_pipeline,
// upload a triangle's vertices, then render_to a cleared rgba8 target and draw. The read-back target shows
// the triangle's color over the center and the clear color in the corner — proving pipeline creation,
// vertex-buffer bind + input layout, the rendering scope, and the draw all execute.
//
// Driven through the backend-agnostic sg::context API; the dx12 WARP device is only how the context exists.

namespace
{
// Matches the HLSL vs_input: POSITION (float3) + COLOR (float4).
struct vertex
{
    float position[3];
    float color[4];
};

constexpr char const* triangle_hlsl = R"(
struct vs_input  { float3 position : POSITION; float4 color : COLOR; };
struct vs_output { float4 position : SV_Position; float4 color : COLOR; };

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = float4(input.position, 1.0);
    output.color = input.color;
    return output;
}

float4 main_ps(vs_output input) : SV_Target { return input.color; }
)";
} // namespace

template <>
struct sg::vertex_layout_of<vertex>
{
    static sg::vertex_type_layout get()
    {
        return {
            .stride = sizeof(vertex),
            .attributes = {
                {.semantic = "POSITION", .format = sg::vertex_attribute_format::vec3f, .offset = offsetof(vertex, position)},
                {.semantic = "COLOR", .format = sg::vertex_attribute_format::vec4f, .offset = offsetof(vertex, color)},
            }};
    }
};

TEST("ssc::dxc + dx12 - raster pipeline draws a triangle over a cleared target")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    // Compile the vertex + pixel stages from the one source.
    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = triangle_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    // No resource bindings, so the pipeline layout is empty (an IA-only root signature).
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({});
    REQUIRE(pipeline_layout != nullptr);

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.fragment_shader = cc::move(ps);
    desc.vertex_input = sg::vertex_input_layout::create<vertex>();
    desc.topology = sg::primitive_topology::triangle_list;
    desc.rasterization.cull = sg::cull_mode::none; // avoid winding-vs-culling concerns in the test
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto pipeline = ctx.uncached.create_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    // The render target (rgba8, readable back) and the triangle's vertices (all red).
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

    vertex const verts[3] = {
        {{0.0f, 0.8f, 0.0f}, {1, 0, 0, 1}},
        {{-0.8f, -0.8f, 0.0f}, {1, 0, 0, 1}},
        {{0.8f, -0.8f, 0.0f}, {1, 0, 0, 1}},
    };
    auto vbuf = ctx.persistent.create_raw_buffer(cc::isize(sizeof(verts)),
                                                 sg::buffer_usage::vertex_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(vbuf != nullptr);

    // Upload the vertices in their own list so the buffer decays to COMMON before the draw reads it.
    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(vbuf, cc::span<vertex const>(verts));
    ctx.submit_command_list(cc::move(up));

    // Clear to blue, then draw the red triangle over it.
    auto cmd = ctx.create_command_list();
    {
        auto pass
            = cmd->raster.render_to({.color_targets = {typed.as_render_target_view().cleared(tg::vec4f(0, 0, 1, 1))}});
        cmd->raster.bind_pipeline(*pipeline);
        cmd->raster.bind_vertex_buffers({sg::buffer<vertex>::from_raw(vbuf).as_vertex_buffer()});
        cmd->raster.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(tex);
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = ctx.wait_for(future);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == cc::isize(W) * cc::isize(H) * 4);
    auto const* px = reinterpret_cast<cc::u8 const*>(bytes.value().data());

    auto texel = [&](int x, int y) { return px + (cc::isize(y) * W + x) * 4; };

    // Center is inside the triangle -> the red vertex color; a top corner is above the apex -> the blue clear.
    auto const* center = texel(W / 2, H / 2);
    CHECK(center[0] == 255);
    CHECK(center[1] == 0);
    CHECK(center[2] == 0);
    CHECK(center[3] == 255);

    auto const* corner = texel(0, 0);
    CHECK(corner[0] == 0);
    CHECK(corner[1] == 0);
    CHECK(corner[2] == 255);
    CHECK(corner[3] == 255);
}
