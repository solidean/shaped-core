#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// Array bindings end to end on WARP: reflection yields the array counts, a group binds a partially vacant
// element list (null descriptors for the vacant ones), and the dispatch declares which elements it reads via
// declare_array_*_access — the accounting rule that every bound array binding must be declared included.

namespace
{
constexpr u32 b0_value = 11;
constexpr u32 b3_value = 300;
constexpr u8 texel_value = 200;

// Bufs and Texs are bounded arrays in their own register spaces; the shader statically reads
// elements {0, 3} of Bufs and element {1} of Texs — exactly what the dispatch declares.
constexpr char const* array_hlsl = R"(
ByteAddressBuffer Bufs[4] : register(t0, space1);
Texture2D<float4> Texs[4] : register(t0, space2);
RWStructuredBuffer<uint> Out : register(u0, space0);
[numthreads(1, 1, 1)]
void main()
{
    Out[0] = Bufs[0].Load(0) + Bufs[3].Load(0);
    Out[1] = uint(round(Texs[1].Load(int3(0, 0, 0)).r * 255.0));
}
)";

} // namespace

INVOCABLE_TEST("ssc::dxc + dx12 - array bindings: partial fill, declared access, readback",
               (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    ssc::dxc::shader_description sd;
    sd.stage = sg::shader_stage::compute;
    sd.entry_point = "main";
    sd.model = ssc::dxc::shader_model::sm_6_8;
    sd.source = array_hlsl;
    auto shader_r = comp.value().compile(sd);
    REQUIRE(shader_r.has_value());
    sg::compiled_shader const shader = cc::move(shader_r.value());

    // Reflection: the arrays surface with count == 4 in their declared sets.
    cc::vector<sg::binding> set0;
    cc::vector<sg::binding> set1;
    cc::vector<sg::binding> set2;
    for (auto const& b : shader.bindings)
    {
        if (b.set == 0)
            set0.push_back(b);
        else if (b.set == 1)
            set1.push_back(b);
        else
            set2.push_back(b);
    }
    REQUIRE(set0.size() == 1);
    REQUIRE(set1.size() == 1);
    REQUIRE(set2.size() == 1);
    CHECK(set1[0].name == "Bufs");
    CHECK(set1[0].count == 4);
    CHECK(set1[0].type == sg::binding_type::readonly_raw_buffer);
    CHECK(set2[0].name == "Texs");
    CHECK(set2[0].count == 4);
    CHECK(set2[0].type == sg::binding_type::readonly_texture);
    // The declared dimension rides the binding — what the backend builds vacant elements' null SRVs from.
    CHECK(set2[0].texture_dimension == sg::texture_view_dimension::tex_2d);

    auto group_layout0 = ctx.uncached.create_binding_group_layout(set0);
    auto group_layout1 = ctx.uncached.create_binding_group_layout(set1);
    auto group_layout2 = ctx.uncached.create_binding_group_layout(set2);
    REQUIRE(group_layout0 != nullptr);
    REQUIRE(group_layout1 != nullptr);
    REQUIRE(group_layout2 != nullptr);

    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {group_layout0, group_layout1, group_layout2}});
    REQUIRE(pipeline_layout != nullptr);
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    // Elements: Bufs[0] and Bufs[3] are real buffers, Texs[1] a real texture; the rest stay vacant.
    auto b0_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto b3_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto out_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(b0_buf != nullptr);
    REQUIRE(b3_buf != nullptr);
    REQUIRE(out_buf != nullptr);

    sg::texture_description tex_desc;
    tex_desc.format = sg::pixel_format::rgba8_unorm;
    tex_desc.dimension = sg::texture_dimension::d2;
    tex_desc.width = 4;
    tex_desc.height = 4;
    tex_desc.usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst;
    auto tex = ctx.persistent.create_raw_texture(tex_desc);
    REQUIRE(tex != nullptr);

    // Upload the inputs: the two buffer words and a constant-valued texture.
    cc::vector<u32> const b0_data = {b0_value, 0, 0, 0};
    cc::vector<u32> const b3_data = {b3_value, 0, 0, 0};
    cc::vector<byte> texels;
    for (isize i = 0; i < 4 * 4 * 4; ++i)
        texels.push_back(byte(texel_value));
    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(b0_buf, b0_data);
    up->upload.data_to_buffer(b3_buf, b3_data);
    up->upload.bytes_to_texture(tex, texels);
    ctx.submit_command_list(cc::move(up));

    // Groups: Out at slot 0; the arrays at slots 1 and 2, vacant elements as the vacant marker.
    sg::named_view const g0_view = {.name = "Out", .views = {sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()}};
    auto g0 = ctx.persistent.create_binding_group(group_layout0, cc::span<sg::named_view const>(&g0_view, 1));
    REQUIRE(g0 != nullptr);

    auto bufs_nv = sg::named_view{.name = "Bufs", .views = {}};
    bufs_nv.views.push_back(sg::buffer<byte>::from_raw(b0_buf).as_readonly_buffer());
    bufs_nv.views.push_back(sg::vacant_view{});
    bufs_nv.views.push_back(sg::vacant_view{});
    bufs_nv.views.push_back(sg::buffer<byte>::from_raw(b3_buf).as_readonly_buffer());
    auto g1 = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&bufs_nv, 1));
    REQUIRE(g1 != nullptr);

    auto texs_nv = sg::named_view{.name = "Texs", .views = {}};
    for (isize i = 0; i < 4; ++i)
        texs_nv.views.push_back(sg::vacant_view{});
    texs_nv.views[1] = sg::texture_2d::from_raw(tex).as_readonly_view();
    auto g2 = ctx.persistent.create_binding_group(group_layout2, cc::span<sg::named_view const>(&texs_nv, 1));
    REQUIRE(g2 != nullptr);

    // Dispatch, declaring exactly the elements the shader reads.
    auto disp = ctx.create_command_list();
    disp->compute.bind_pipeline(*pipeline);
    disp->compute.bind_group(0, *g0);
    disp->compute.bind_group(1, *g1);
    disp->compute.bind_group(2, *g2);
    sg::array_buffer_access const buf_access[] = {
        {.index = 0, .stages = sg::pipeline_stage_flag::compute, .access = sg::access_flag::shader_read},
        {.index = 3, .stages = sg::pipeline_stage_flag::compute, .access = sg::access_flag::shader_read},
    };
    sg::array_texture_access const tex_access[] = {
        {.index = 1,
         .stages = sg::pipeline_stage_flag::compute,
         .access = sg::access_flag::shader_read,
         .layout = sg::texture_layout::shader_readonly},
    };
    disp->compute.declare_array_buffer_access("Bufs", buf_access);
    disp->compute.declare_array_texture_access("Texs", tex_access);
    disp->compute.dispatch_groups(1);
    ctx.submit_command_list(cc::move(disp));

    auto down = ctx.create_command_list();
    auto future = down->download.data_from_buffer<u32>(out_buf, 0, 2);
    ctx.submit_command_list(cc::move(down));

    auto const data = ctx.wait_for(future);
    REQUIRE(data.value().size() == 2);
    CHECK(data.value()[0] == b0_value + b3_value);
    CHECK(data.value()[1] == u32(texel_value));
}

INVOCABLE_TEST("ssc::dxc + dx12 - array bindings: the accounting rule", (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    ssc::dxc::shader_description sd;
    sd.stage = sg::shader_stage::compute;
    sd.entry_point = "main";
    sd.model = ssc::dxc::shader_model::sm_6_8;
    sd.source = array_hlsl;
    auto shader_r = comp.value().compile(sd);
    REQUIRE(shader_r.has_value());
    sg::compiled_shader const shader = cc::move(shader_r.value());

    cc::vector<sg::binding> set0;
    cc::vector<sg::binding> set1;
    cc::vector<sg::binding> set2;
    for (auto const& b : shader.bindings)
    {
        if (b.set == 0)
            set0.push_back(b);
        else if (b.set == 1)
            set1.push_back(b);
        else
            set2.push_back(b);
    }

    auto group_layout0 = ctx.uncached.create_binding_group_layout(set0);
    auto group_layout1 = ctx.uncached.create_binding_group_layout(set1);
    auto group_layout2 = ctx.uncached.create_binding_group_layout(set2);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {group_layout0, group_layout1, group_layout2}});
    auto pipeline = ctx.uncached.create_compute_pipeline({.shader = shader, .layout = pipeline_layout});
    REQUIRE(pipeline != nullptr);

    auto out_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readwrite_buffer);
    REQUIRE(out_buf != nullptr);
    sg::named_view const g0_view = {.name = "Out", .views = {sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()}};
    auto g0 = ctx.persistent.create_binding_group(group_layout0, cc::span<sg::named_view const>(&g0_view, 1));

    // All-vacant arrays: null descriptors read as zero, so no data setup is needed here.
    auto bufs_nv = sg::named_view{.name = "Bufs", .views = {}};
    auto texs_nv = sg::named_view{.name = "Texs", .views = {}};
    for (isize i = 0; i < 4; ++i)
    {
        bufs_nv.views.push_back(sg::vacant_view{});
        texs_nv.views.push_back(sg::vacant_view{});
    }
    auto g1 = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&bufs_nv, 1));
    auto g2 = ctx.persistent.create_binding_group(group_layout2, cc::span<sg::named_view const>(&texs_nv, 1));
    REQUIRE(g1 != nullptr);
    REQUIRE(g2 != nullptr);

    // A dispatch with an undeclared bound array binding trips the accounting assert.
    {
        auto disp = ctx.create_command_list();
        disp->compute.bind_pipeline(*pipeline);
        disp->compute.bind_group(0, *g0);
        disp->compute.bind_group(1, *g1);
        disp->compute.bind_group(2, *g2);
        CHECK_ASSERTS(disp->compute.dispatch_groups(1));
        ctx.drop_command_list(cc::move(disp));
    }

    // Empty-span declarations say "unused" and satisfy the accounting; the dispatch then runs.
    {
        auto disp = ctx.create_command_list();
        disp->compute.bind_pipeline(*pipeline);
        disp->compute.bind_group(0, *g0);
        disp->compute.bind_group(1, *g1);
        disp->compute.bind_group(2, *g2);
        disp->compute.declare_array_buffer_access("Bufs", {});
        disp->compute.declare_array_texture_access("Texs", {});
        disp->compute.dispatch_groups(1);
        ctx.submit_command_list(cc::move(disp));
        ctx.advance_epoch_and_wait_for_idle();
    }
}
