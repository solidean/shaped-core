#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>          // offsetof
#include <clean-core/thread/async.hh> // cc::async_blocking_get
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// ctx.cached.acquire_raster_pipeline: dedup, and what the key covers.
//
// It lives here rather than beside the other pipeline_cache tests because it needs real vertex/fragment bytecode, which
// only a shader compiler produces — the dx12 backend tests have no compiler to reach.
//
// The claim under test is that no state baked into a PSO is missing from the key.
// A missed field would hand a caller back a pipeline built for different state, silently and only sometimes, so each
// mutation below is asserted alone.
// Every variant is a description a backend can really build, so each acquire is a genuine PSO build.

namespace
{
// Matches the HLSL vs_input.
struct cache_vertex
{
    float position[3];
};

constexpr char const* cache_hlsl = R"(
struct vs_input  { float3 position : POSITION; };
struct vs_output { float4 position : SV_Position; };

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = float4(input.position, 1.0);
    return output;
}

vs_output alt_vs(vs_input input)
{
    vs_output output;
    output.position = float4(input.position * 0.5, 1.0);
    return output;
}

float4 main_ps(vs_output input) : SV_Target { return float4(1, 1, 1, 1); }
float4 alt_ps(vs_output input) : SV_Target { return float4(0, 0, 0, 1); }
)";
} // namespace

template <>
struct sg::vertex_layout_of<cache_vertex>
{
    static sg::vertex_type_layout get()
    {
        return {.stride = sizeof(cache_vertex),
                .attributes = {{.semantic = "POSITION",
                                .format = sg::vertex_attribute_format::vec3f,
                                .offset = offsetof(cache_vertex, position)}}};
    }
};

INVOCABLE_TEST("ssc::dxc + dx12 - ctx.cached dedups raster pipelines and keys every baked state",
               (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = cache_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader const vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader const alt_vs = compile("alt_vs", sg::shader_stage::vertex);
    sg::compiled_shader const ps = compile("main_ps", sg::shader_stage::fragment);
    sg::compiled_shader const alt_ps = compile("alt_ps", sg::shader_stage::fragment);

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({});
    REQUIRE(pipeline_layout != nullptr);

    // A depth-stencil format is in the base description so the depth and stencil mutations below stay buildable —
    // enabling either against no attachment is what a backend rejects, and the key is what is under test here.
    auto const base_description = [&]
    {
        auto desc = sg::raster_pipeline_description{
            .layout = pipeline_layout,
            .vertex_shader = vs,
            .fragment_shader = ps,
            .vertex_input = sg::vertex_input_layout::create<cache_vertex>(),
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil_format = sg::pixel_format::depth32_float_stencil8,
        };
        desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
        return desc;
    };

    // Dedup: identical descriptions share one async node, and it resolves to a real pipeline.
    auto const base = ctx.cached.acquire_raster_pipeline(base_description());
    auto const base_again = ctx.cached.acquire_raster_pipeline(base_description());
    REQUIRE(base != nullptr);
    CHECK(base.get() == base_again.get());

    sg::raster_pipeline_handle const built = cc::async_blocking_get(base);
    CHECK(built != nullptr);

    // A distinct pipeline layout is a distinct pipeline, for otherwise identical shaders and state.
    auto other_layout_description = base_description();
    other_layout_description.layout = ctx.uncached.create_pipeline_layout({});
    REQUIRE(other_layout_description.layout.get() != pipeline_layout.get());
    auto const other_layout = ctx.cached.acquire_raster_pipeline(other_layout_description);
    CHECK(base.get() != other_layout.get());
    (void)cc::try_async_blocking_get(other_layout);

    auto const differs = [&](cc::string_view what, cc::function_ref<void(sg::raster_pipeline_description&)> mutate)
    {
        auto desc = base_description();
        mutate(desc);
        auto const node = ctx.cached.acquire_raster_pipeline(desc);
        CHECK(base.get() != node.get()).context(what);
        // Identity is the claim, but this is a real PSO build on the ambient scheduler — finished here rather than left
        // running past the test.
        (void)cc::try_async_blocking_get(node);
    };

    differs("the vertex shader", [&](auto& d) { d.vertex_shader = alt_vs; });
    differs("the fragment shader", [&](auto& d) { d.fragment_shader = alt_ps; });
    differs("a color-target format", [](auto& d) { d.color_targets[0].format = sg::pixel_format::rgba16_float; });
    differs("enabling blending", [](auto& d) { d.color_targets[0].blend = sg::blend_state{}; });
    differs("a blend factor",
            [](auto& d)
            {
                d.color_targets[0].blend = sg::blend_state{
                    .color = {.source = sg::blend_factor::src_alpha, .target = sg::blend_factor::one_minus_src_alpha}};
            });
    differs("a write mask", [](auto& d) { d.color_targets[0].write_mask = sg::color_channel::r; });
    differs("an extra color target",
            [](auto& d) { d.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm}); });
    differs("the topology", [](auto& d) { d.topology = sg::primitive_topology::triangle_strip; });
    // Ignored by the backend for a non-patch topology, so only the key can tell these two apart.
    differs("the patch control point count", [](auto& d) { d.patch_control_points = 3; });
    differs("the cull mode", [](auto& d) { d.rasterization.cull = sg::cull_mode::back; });
    differs("the fill mode", [](auto& d) { d.rasterization.fill = sg::fill_mode::wireframe; });
    differs("the front face", [](auto& d) { d.rasterization.front = sg::front_face::clockwise; });
    differs("depth clipping", [](auto& d) { d.rasterization.depth_clip_enabled = false; });
    differs("the depth bias", [](auto& d) { d.rasterization.depth_bias = 1.0f; });
    differs("the slope-scaled depth bias", [](auto& d) { d.rasterization.depth_bias_slope = 1.0f; });
    differs("the depth-bias clamp", [](auto& d) { d.rasterization.depth_bias_clamp = 1.0f; });
    differs("the depth test", [](auto& d) { d.depth_stencil.depth_test = true; });
    differs("the depth write", [](auto& d) { d.depth_stencil.depth_write = true; });
    differs("the depth comparison", [](auto& d) { d.depth_stencil.depth_compare = sg::compare_op::greater; });
    differs("the stencil test", [](auto& d) { d.depth_stencil.stencil_test = true; });
    differs("a stencil mask", [](auto& d) { d.depth_stencil.stencil_read_mask = 0x0F; });
    differs("a front stencil op", [](auto& d) { d.depth_stencil.front.pass = sg::stencil_op::replace; });
    differs("a back stencil op", [](auto& d) { d.depth_stencil.back.pass = sg::stencil_op::replace; });
    differs("the depth-stencil format", [](auto& d) { d.depth_stencil_format = sg::pixel_format::depth32_float; });
    differs("the sample count", [](auto& d) { d.sample_count = 4; });
    differs("a vertex attribute offset", [](auto& d) { d.vertex_input.attributes[0].offset = 4; });
    differs("a vertex slot stride", [](auto& d) { d.vertex_input.slots[0].stride = 32; });
    differs("a per-instance vertex slot", [](auto& d) { d.vertex_input.slots[0].per_instance = true; });
}

INVOCABLE_TEST("ssc::dxc + dx12 - the cached PSO blob is not part of the raster key", (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    ssc::dxc::shader_description sd;
    sd.source = cache_hlsl;
    sd.entry_point = "main_vs";
    sd.stage = sg::shader_stage::vertex;
    sd.model = ssc::dxc::shader_model::sm_6_8;
    auto vs_r = comp.value().compile(sd);
    REQUIRE(vs_r.has_value());

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({});
    REQUIRE(pipeline_layout != nullptr);

    // A depth-only pipeline, so this description cannot collide with the one above.
    auto const describe = [&]
    {
        return sg::raster_pipeline_description{
            .layout = pipeline_layout,
            .vertex_shader = vs_r.value(),
            .vertex_input = sg::vertex_input_layout::create<cache_vertex>(),
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = true},
            .depth_stencil_format = sg::pixel_format::depth32_float,
        };
    };

    auto const plain = ctx.cached.acquire_raster_pipeline(describe());
    REQUIRE(plain != nullptr);
    sg::raster_pipeline_handle const built = cc::async_blocking_get(plain);
    REQUIRE(built != nullptr);

    // The blob only accelerates a build, so feeding one back must hit the same entry rather than splitting it.
    auto seeded = describe();
    seeded.cached_pipeline = built->cached_pipeline_data();
    auto const from_blob = ctx.cached.acquire_raster_pipeline(seeded);
    CHECK(plain.get() == from_blob.get());
}
