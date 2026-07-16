#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

// The whole chain, end to end: HLSL through the DXC wrapper (which now reflects texture + sampler
// bindings), a binding_group_layout + pipeline_layout built straight from that reflection, a real texture + a dynamic sampler
// bound to a compute dispatch on WARP, and the sampled result read back and verified. Everything is driven
// through the backend-agnostic sg::context API — the dx12 WARP device is only how the context is created.
//
// Two passes, because texture upload isn't implemented yet: pass 1 writes a known pattern into the texture
// as a storage image (UAV); pass 2 samples it as a sampled texture (SRV) through a point/clamp sampler.
// The barrier system inserts the storage -> shader_read transition between them.

namespace
{
constexpr int N = 8; // 8x8 texture / dispatch

// Pass 1: write Dst[x,y] = y*N + x. RWTexture2D<float> -> an R32_FLOAT storage texture (typed UAV store).
constexpr char const* fill_hlsl = R"(
RWTexture2D<float> Dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Dst[tid.xy] = (float)(tid.y * 8 + tid.x);
}
)";

// Pass 2: point-sample the texel centers (so the fetch is exact) and write to a structured buffer.
constexpr char const* sample_hlsl = R"(
Texture2D<float>          Src  : register(t0);
SamplerState              Samp : register(s0);
RWStructuredBuffer<float> Out  : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 uv = ((float2)tid.xy + 0.5) / 8.0;
    Out[tid.y * 8 + tid.x] = Src.SampleLevel(Samp, uv, 0);
}
)";

sg::compiled_shader compile_compute(ssc::dxc::compiler& comp, char const* source)
{
    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.entry_point = "main";
    desc.model = ssc::dxc::shader_model::sm_6_8;
    desc.source = source;
    auto r = comp.compile(desc);
    REQUIRE(r.has_value());
    return cc::move(r.value());
}
} // namespace

TEST("ssc::dxc + dx12 - end to end: reflect a texture+sampler, sample on WARP, read back")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    constexpr int count = N * N;

    sg::compiled_shader const fill = compile_compute(comp.value(), fill_hlsl);
    sg::compiled_shader const sample = compile_compute(comp.value(), sample_hlsl);

    // The point of the reflection change: the sample shader yields the texture + sampler + UAV bindings.
    bool tex_ok = false, samp_ok = false, out_ok = false;
    for (auto const& b : sample.bindings)
    {
        if (b.name == cc::string_view("Src"))
            tex_ok = b.type == sg::binding_type::readonly_texture;
        else if (b.name == cc::string_view("Samp"))
            samp_ok = b.type == sg::binding_type::sampler;
        else if (b.name == cc::string_view("Out"))
            out_ok = b.type == sg::binding_type::readwrite_structured_buffer;
    }
    CHECK(tex_ok);
    CHECK(samp_ok);
    CHECK(out_ok);

    // Resources: an R32_FLOAT texture usable as both a storage image (pass 1) and a sampled texture (pass 2),
    // and a structured output buffer the sampled values land in.
    sg::texture_description td;
    td.format = sg::pixel_format::r32_float;
    td.dimension = sg::texture_dimension::d2;
    td.width = N;
    td.height = N;
    td.usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture;
    auto tex_h = ctx.persistent.create_raw_texture(td);
    REQUIRE(tex_h != nullptr);
    sg::texture_2d const tex(tex_h);

    auto buf = ctx.persistent.create_raw_buffer(cc::isize(count) * cc::isize(sizeof(float)),
                                                sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(buf != nullptr);

    // Pipelines + layouts, built straight from the reflected bindings.
    auto fill_group_layout = ctx.uncached.create_binding_group_layout(fill.bindings);
    REQUIRE(fill_group_layout != nullptr);
    auto fill_pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {fill_group_layout}});
    REQUIRE(fill_pipeline_layout != nullptr);
    auto fill_pipe = ctx.uncached.create_compute_pipeline({.shader = fill, .layout = fill_pipeline_layout});
    REQUIRE(fill_pipe != nullptr);

    auto sample_group_layout = ctx.uncached.create_binding_group_layout(sample.bindings);
    REQUIRE(sample_group_layout != nullptr);
    auto sample_pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {sample_group_layout}});
    REQUIRE(sample_pipeline_layout != nullptr);
    auto sample_pipe = ctx.uncached.create_compute_pipeline({.shader = sample, .layout = sample_pipeline_layout});
    REQUIRE(sample_pipe != nullptr);

    // Groups: pass 1 binds the texture as a UAV; pass 2 binds it as an SRV + a dynamic point/clamp sampler.
    sg::named_view const fill_views[] = {{.name = "Dst", .view = tex.as_readwrite_view()}};
    auto fill_group = ctx.persistent.create_binding_group(fill_group_layout, fill_views);
    REQUIRE(fill_group != nullptr);

    sg::named_view const sample_views[] = {
        {.name = "Src", .view = tex.as_readonly_view()},
        {.name = "Out", .view = sg::buffer<float>::from_raw(buf).as_readwrite_buffer()},
    };
    sg::named_sampler const sample_samplers[] = {{.name = "Samp",
                                                  .sampler = {.min_filter = sg::sampler_filter::nearest,
                                                              .mag_filter = sg::sampler_filter::nearest,
                                                              .mip_filter = sg::sampler_filter::nearest,
                                                              .address_u = sg::sampler_address_mode::clamp_edge,
                                                              .address_v = sg::sampler_address_mode::clamp_edge}}};
    auto sample_group = ctx.persistent.create_binding_group(sample_group_layout, sample_views, sample_samplers);
    REQUIRE(sample_group != nullptr);

    // Record both passes; the barrier system transitions the texture storage -> shader_read between them.
    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(*fill_pipe);
    disp->compute.bind_group(0, *fill_group);
    disp->compute.dispatch_threads(N, N, 1);
    disp->compute.bind_pipeline(*sample_pipe);
    disp->compute.bind_group(0, *sample_group);
    disp->compute.dispatch_threads(N, N, 1);
    ctx.submit_command_list(cc::move(disp));

    // Read the sampled values back.
    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<float>(buf, 0, count);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.has_value());

    // Point-sampling texel centers reproduces exactly what pass 1 wrote: Out[i] == i.
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != float(i))
            ok = false;
    CHECK(ok);
}
