#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

// Inline constants (dx12 root constants) end to end on WARP: Out[i] = i*scale + bias, where {scale, bias}
// are written straight onto the command list via cmd.compute.set_inline_constants rather than through a
// bound buffer. The cbuffer register (b0) is declared on the pipeline layout as inline_constants and kept
// out of the group layout. Two dispatches in one list also prove the partial (offset) update: the second
// rewrites only bias while the earlier scale persists in the root signature.
//
// Everything is driven through the backend-agnostic sg::context API — the dx12 WARP device is only how the
// context is created, never the driver of the work.

namespace
{
constexpr int count = 256; // multiple of the 64-thread workgroup

// Out at u0 comes from a group; Params at b0 is supplied as inline constants (excluded from the group).
constexpr char const* inline_hlsl = R"(
RWStructuredBuffer<uint> Out : register(u0, space0);
cbuffer Params : register(b0, space0)
{
    uint scale;
    uint bias;
};
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Out[tid.x] = tid.x * scale + bias;
}
)";

struct params
{
    sg::u32 scale;
    sg::u32 bias;
};
} // namespace

TEST("ssc::dxc + dx12 - inline constants drive Out[i] = i*scale + bias")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto ctx_r = sg::create_dx12_context({.use_warp = true});
    REQUIRE(ctx_r.has_value());
    sg::context& ctx = *ctx_r.value();

    ssc::dxc::shader_description sd;
    sd.stage = sg::shader_stage::compute;
    sd.entry_point = "main";
    sd.model = ssc::dxc::shader_model::sm_6_8;
    sd.source = inline_hlsl;
    auto shader_r = comp.value().compile(sd);
    REQUIRE(shader_r.has_value());
    sg::compiled_shader const shader = cc::move(shader_r.value());

    // The group covers only the Out UAV; the Params cbuffer becomes the pipeline layout's inline constants.
    // block_size is set to 8 (two uints) to match what the shader consumes — reflection rounds a cbuffer up
    // to 16 bytes, but root constants have no such alignment requirement.
    cc::vector<sg::binding> out_bindings;
    for (auto const& b : shader.bindings)
        if (b.type != sg::binding_type::uniform_buffer)
            out_bindings.push_back(b);
    REQUIRE(out_bindings.size() == 1);

    auto group_layout = ctx.uncached.create_binding_group_layout(out_bindings);
    REQUIRE(group_layout != nullptr);

    sg::pipeline_layout_description pld;
    pld.groups = {group_layout};
    pld.inline_constants = sg::binding{
        .name = "Params",
        .set = 0,
        .index = 0,
        .count = 1,
        .type = sg::binding_type::uniform_buffer,
        .block_size = cc::isize(sizeof(params)),
    };
    auto pipeline_layout = ctx.uncached.create_pipeline_layout(pld);
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    // Two independent outputs so the two dispatches don't alias: out1 for the full set, out2 for the partial.
    auto const byte_size = cc::isize(count) * cc::isize(sizeof(sg::u32));
    auto out1_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto out2_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out1_buf != nullptr);
    REQUIRE(out2_buf != nullptr);

    sg::named_view const g1_view{.name = "Out", .view = out1_buf->as_readwrite_buffer<sg::u32>()};
    sg::named_view const g2_view{.name = "Out", .view = out2_buf->as_readwrite_buffer<sg::u32>()};
    auto group1 = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&g1_view, 1));
    auto group2 = ctx.persistent.create_binding_group(group_layout, cc::span<sg::named_view const>(&g2_view, 1));
    REQUIRE(group1 != nullptr);
    REQUIRE(group2 != nullptr);

    // One list, two dispatches. First: full replace {scale=3, bias=7} -> out1[i] = i*3 + 7. Second: partial
    // update of bias only (offset 4) to 100, with scale=3 carried over -> out2[i] = i*3 + 100.
    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *group1);
    disp->compute.set_inline_constants(params{.scale = 3, .bias = 7});
    disp->compute.dispatch_threads(count);
    disp->compute.bind_group(0, *group2);
    disp->compute.set_inline_constants(sg::u32(100), cc::isize(sizeof(sg::u32)));
    disp->compute.dispatch_threads(count);
    ctx.submit_command_list(cc::move(disp));

    auto read_back = [&](sg::raw_buffer_handle const& buf) -> cc::vector<sg::u32>
    {
        auto down = ctx.create_command_list();
        auto future = down->download.data_from_buffer<sg::u32>(buf, 0, count);
        ctx.submit_command_list(cc::move(down));
        auto const data = ctx.wait_for(future);
        cc::vector<sg::u32> result;
        for (auto const v : data.value())
            result.push_back(v);
        return result;
    };

    auto const r1 = read_back(out1_buf);
    auto const r2 = read_back(out2_buf);
    REQUIRE(r1.size() == cc::isize(count));
    REQUIRE(r2.size() == cc::isize(count));

    bool ok1 = true;
    bool ok2 = true;
    for (int i = 0; i < count; ++i)
    {
        if (r1[i] != cc::u32(i) * 3 + 7)
            ok1 = false;
        if (r2[i] != cc::u32(i) * 3 + 100)
            ok2 = false;
    }
    CHECK(ok1);
    CHECK(ok2);
}
