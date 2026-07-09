#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-compiler-dxc/all.hh>

// The point of the group/pipeline-layout split, exercised end to end on WARP: a pipeline layout with two
// group slots lets an entire group be rebound at one slot without disturbing the other. Slot 0 (input A +
// output) stays bound while slot 1's group is swapped between two different B buffers; each dispatch yields
// A + B for its B, proving "same slot-0 group, different slot-1 group". Everything is driven through the
// backend-agnostic sg::context API — the dx12 WARP device is only how the context is created.

namespace
{
constexpr int count = 256; // multiple of the 64-thread workgroup

// A[i] from set 0, B[i] from set 1, Out[i] = A[i] + B[i] into set 0. The two inputs live in different
// register spaces so reflection puts them in different sets -> different binding_group_layouts / slots.
constexpr char const* add_hlsl = R"(
StructuredBuffer<uint>   A   : register(t0, space0);
RWStructuredBuffer<uint> Out : register(u0, space0);
StructuredBuffer<uint>   B   : register(t0, space1);
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Out[tid.x] = A[tid.x] + B[tid.x];
}
)";
} // namespace

TEST("ssc::dxc + dx12 - two-slot pipeline layout: swap the slot-1 group between dispatches")
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
    sd.source = add_hlsl;
    auto shader_r = comp.value().compile(sd);
    REQUIRE(shader_r.has_value());
    sg::compiled_shader const shader = cc::move(shader_r.value());

    // Split the reflected bindings by set into per-group binding lists: set 0 = {A, Out}, set 1 = {B}.
    cc::vector<sg::binding> set0;
    cc::vector<sg::binding> set1;
    for (auto const& b : shader.bindings)
        (b.set == 0 ? set0 : set1).push_back(b);
    REQUIRE(set0.size() == 2);
    REQUIRE(set1.size() == 1);

    auto group_layout0 = ctx.uncached.create_binding_group_layout(set0);
    REQUIRE(group_layout0 != nullptr);
    auto group_layout1 = ctx.uncached.create_binding_group_layout(set1);
    REQUIRE(group_layout1 != nullptr);

    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {group_layout0, group_layout1}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    // Buffers: A[i]=i, and two slot-1 inputs B1[i]=1, B2[i]=100. Out is read back after each dispatch.
    auto const byte_size = cc::isize(count) * cc::isize(sizeof(sg::u32));
    auto a_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto b1_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto b2_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto out_buf
        = ctx.persistent.create_raw_buffer(byte_size, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(a_buf != nullptr);
    REQUIRE(b1_buf != nullptr);
    REQUIRE(b2_buf != nullptr);
    REQUIRE(out_buf != nullptr);

    cc::vector<sg::u32> a_data;
    cc::vector<sg::u32> b1_data;
    cc::vector<sg::u32> b2_data;
    for (int i = 0; i < count; ++i)
    {
        a_data.push_back(cc::u32(i));
        b1_data.push_back(1);
        b2_data.push_back(100);
    }

    // Upload the inputs.
    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(a_buf, a_data);
    up->upload.data_to_buffer(b1_buf, b1_data);
    up->upload.data_to_buffer(b2_buf, b2_data);
    ctx.submit_command_list(cc::move(up));

    // Groups: slot 0 = {A, Out} (bound once), slot 1 = two groups over B1 / B2 (swapped between dispatches).
    sg::named_view const g0_views[] = {
        {.name = "A", .view = a_buf->as_readonly_buffer<sg::u32>()},
        {.name = "Out", .view = out_buf->as_readwrite_buffer<sg::u32>()},
    };
    auto g0 = ctx.persistent.create_binding_group(group_layout0, g0_views);
    REQUIRE(g0 != nullptr);

    sg::named_view const g1_b1_view{.name = "B", .view = b1_buf->as_readonly_buffer<sg::u32>()};
    sg::named_view const g1_b2_view{.name = "B", .view = b2_buf->as_readonly_buffer<sg::u32>()};
    auto g1_b1 = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&g1_b1_view, 1));
    auto g1_b2 = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&g1_b2_view, 1));
    REQUIRE(g1_b1 != nullptr);
    REQUIRE(g1_b2 != nullptr);

    // Dispatch with slot 0 fixed and slot 1 = g1, then read Out back. The handles assert on failure so the
    // lambda stays free of nexus macros (which need the enclosing test frame).
    auto run = [&](sg::binding_group_handle const& g1) -> cc::vector<sg::u32>
    {
        auto disp = ctx.create_command_list();
        disp->compute.bind_pipeline(*pipeline);
        disp->compute.bind_group(0, *g0);
        disp->compute.bind_group(1, *g1);
        disp->compute.dispatch_threads(count);
        ctx.submit_command_list(cc::move(disp));

        auto down = ctx.create_command_list();
        auto future = down->download.data_from_buffer<sg::u32>(out_buf, 0, count);
        ctx.submit_command_list(cc::move(down));

        auto const data = ctx.wait_for(future);
        cc::vector<sg::u32> result;
        for (auto const v : data.value())
            result.push_back(v);
        return result;
    };

    // Same slot-0 group, different slot-1 group => A[i]+1 then A[i]+100.
    auto const r1 = run(g1_b1);
    auto const r2 = run(g1_b2);
    REQUIRE(r1.size() == cc::isize(count));
    REQUIRE(r2.size() == cc::isize(count));

    bool ok1 = true;
    bool ok2 = true;
    for (int i = 0; i < count; ++i)
    {
        if (r1[i] != cc::u32(i) + 1)
            ok1 = false;
        if (r2[i] != cc::u32(i) + 100)
            ok2 = false;
    }
    CHECK(ok1);
    CHECK(ok2);
}
