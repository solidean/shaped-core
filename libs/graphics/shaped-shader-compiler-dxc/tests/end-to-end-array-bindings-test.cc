#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// Array bindings end to end: reflection yields the array counts, a group binds a partially vacant
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

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - array bindings: partial fill, declared access, readback",
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
        if (b.space == 0u)
            set0.push_back(b);
        else if (b.space == 1u)
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
    sg::named_view const g0_view = {.name = "Out", .view = sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()};
    auto g0 = ctx.persistent.create_binding_group(group_layout0, cc::span<sg::named_view const>(&g0_view, 1));
    REQUIRE(g0 != nullptr);

    auto buf_elements = cc::vector<sg::raw_view>();
    buf_elements.push_back(sg::buffer<byte>::from_raw(b0_buf).as_readonly_buffer());
    buf_elements.push_back(sg::vacant_view{});
    buf_elements.push_back(sg::vacant_view{});
    buf_elements.push_back(sg::buffer<byte>::from_raw(b3_buf).as_readonly_buffer());
    auto const bufs_nv = sg::named_view{.name = "Bufs", .view = cc::move(buf_elements)};
    auto g1 = ctx.persistent.create_binding_group(group_layout1, cc::span<sg::named_view const>(&bufs_nv, 1));
    REQUIRE(g1 != nullptr);

    auto tex_elements = cc::vector<sg::raw_view>();
    for (isize i = 0; i < 4; ++i)
        tex_elements.push_back(sg::vacant_view{});
    tex_elements[1] = sg::texture_2d::from_raw(tex).as_readonly_view();
    auto const texs_nv = sg::named_view{.name = "Texs", .view = cc::move(tex_elements)};
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

    auto const data = co_await future.data();
    REQUIRE(data.size() == 2);
    CHECK(data[0] == b0_value + b3_value);
    CHECK(data[1] == u32(texel_value));
}

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - array bindings: the accounting rule", (sg::context_handle const& handle))
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
        if (b.space == 0u)
            set0.push_back(b);
        else if (b.space == 1u)
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
    sg::named_view const g0_view = {.name = "Out", .view = sg::buffer<u32>::from_raw(out_buf).as_readwrite_buffer()};
    auto g0 = ctx.persistent.create_binding_group(group_layout0, cc::span<sg::named_view const>(&g0_view, 1));

    // All-vacant arrays: null descriptors read as zero, so no data setup is needed here.
    auto buf_elements = cc::vector<sg::raw_view>();
    auto tex_elements = cc::vector<sg::raw_view>();
    for (isize i = 0; i < 4; ++i)
    {
        buf_elements.push_back(sg::vacant_view{});
        tex_elements.push_back(sg::vacant_view{});
    }
    auto const bufs_nv = sg::named_view{.name = "Bufs", .view = cc::move(buf_elements)};
    auto const texs_nv = sg::named_view{.name = "Texs", .view = cc::move(tex_elements)};
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
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }
}

// --- the same, through the raster path ---------------------------------------------------------------

namespace
{
constexpr u32 raster_b0_value = 40;
constexpr u32 raster_b3_value = 90;

// A fullscreen triangle from SV_VertexID, so the draw needs no vertex buffer and no input layout.
// The pixel shader reads elements {0, 3} of Bufs and element {1} of Texs and writes each into its own channel,
// which is what the read-back target checks.
constexpr char const* raster_array_hlsl = R"(
ByteAddressBuffer Bufs[4] : register(t0, space0);
Texture2D<float4> Texs[4] : register(t0, space1);

float4 main_vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2(float((id << 1) & 2), float(id & 2));
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 main_ps() : SV_Target
{
    return float4(float(Bufs[0].Load(0)) / 255.0,
                  float(Bufs[3].Load(0)) / 255.0,
                  Texs[1].Load(int3(0, 0, 0)).r,
                  1.0);
}
)";
} // namespace

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - raster array bindings: partial fill, declared access, readback",
                     (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = raster_array_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    // Only the pixel shader binds anything, so its reflection alone carries both arrays.
    cc::vector<sg::binding> bufs_set;
    cc::vector<sg::binding> texs_set;
    for (auto const& b : ps.bindings)
    {
        if (b.space == 0u)
            bufs_set.push_back(b);
        else
            texs_set.push_back(b);
    }
    REQUIRE(bufs_set.size() == 1);
    REQUIRE(texs_set.size() == 1);
    CHECK(bufs_set[0].count == 4);
    CHECK(texs_set[0].count == 4);
    // Both arrays are declared in the fragment stage, which is what the declarations below name too.
    CHECK(texs_set[0].visibility.has(sg::shader_stage::fragment));

    auto bufs_layout = ctx.uncached.create_binding_group_layout(bufs_set);
    auto texs_layout = ctx.uncached.create_binding_group_layout(texs_set);
    REQUIRE(bufs_layout != nullptr);
    REQUIRE(texs_layout != nullptr);

    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {bufs_layout, texs_layout}});
    REQUIRE(pipeline_layout != nullptr);

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.fragment_shader = cc::move(ps);
    desc.topology = sg::primitive_topology::triangle_list;
    desc.rasterization.cull = sg::cull_mode::none;
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto pipeline = co_await ctx.cached.acquire_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    // Elements: Bufs[0] and Bufs[3] are real buffers, Texs[1] a real texture; the rest stay vacant.
    auto b0_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    auto b3_buf = ctx.persistent.create_raw_buffer(16, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
    REQUIRE(b0_buf != nullptr);
    REQUIRE(b3_buf != nullptr);

    sg::texture_description tex_desc;
    tex_desc.format = sg::pixel_format::rgba8_unorm;
    tex_desc.dimension = sg::texture_dimension::d2;
    tex_desc.width = 4;
    tex_desc.height = 4;
    tex_desc.usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst;
    auto tex = ctx.persistent.create_raw_texture(tex_desc);
    REQUIRE(tex != nullptr);

    constexpr int W = 16;
    constexpr int H = 16;
    sg::texture_description rt_desc;
    rt_desc.format = sg::pixel_format::rgba8_unorm;
    rt_desc.dimension = sg::texture_dimension::d2;
    rt_desc.width = W;
    rt_desc.height = H;
    rt_desc.usage = sg::texture_usage::render_target | sg::texture_usage::copy_src;
    auto target = ctx.persistent.create_raw_texture(rt_desc);
    REQUIRE(target != nullptr);

    cc::vector<u32> const b0_data = {raster_b0_value, 0, 0, 0};
    cc::vector<u32> const b3_data = {raster_b3_value, 0, 0, 0};
    cc::vector<byte> texels;
    for (isize i = 0; i < 4 * 4 * 4; ++i)
        texels.push_back(byte(texel_value));
    auto up = ctx.create_command_list();
    up->upload.data_to_buffer(b0_buf, b0_data);
    up->upload.data_to_buffer(b3_buf, b3_data);
    up->upload.bytes_to_texture(tex, texels);
    ctx.submit_command_list(cc::move(up));

    auto buf_elements = cc::vector<sg::raw_view>();
    buf_elements.push_back(sg::buffer<byte>::from_raw(b0_buf).as_readonly_buffer());
    buf_elements.push_back(sg::vacant_view{});
    buf_elements.push_back(sg::vacant_view{});
    buf_elements.push_back(sg::buffer<byte>::from_raw(b3_buf).as_readonly_buffer());
    auto const bufs_nv = sg::named_view{.name = "Bufs", .view = cc::move(buf_elements)};
    auto bufs_group = ctx.persistent.create_binding_group(bufs_layout, cc::span<sg::named_view const>(&bufs_nv, 1));
    REQUIRE(bufs_group != nullptr);

    auto tex_elements = cc::vector<sg::raw_view>();
    for (isize i = 0; i < 4; ++i)
        tex_elements.push_back(sg::vacant_view{});
    tex_elements[1] = sg::texture_2d::from_raw(tex).as_readonly_view();
    auto const texs_nv = sg::named_view{.name = "Texs", .view = cc::move(tex_elements)};
    auto texs_group = ctx.persistent.create_binding_group(texs_layout, cc::span<sg::named_view const>(&texs_nv, 1));
    REQUIRE(texs_group != nullptr);

    // The declarations name the fragment stage alone, which is the point of declaring rather than inferring:
    // the scalar path keys every bound view of a draw to vertex | fragment.
    sg::array_buffer_access const buf_access[] = {
        {.index = 0, .stages = sg::pipeline_stage_flag::fragment, .access = sg::access_flag::shader_read},
        {.index = 3, .stages = sg::pipeline_stage_flag::fragment, .access = sg::access_flag::shader_read},
    };
    sg::array_texture_access const tex_access[] = {
        {.index = 1,
         .stages = sg::pipeline_stage_flag::fragment,
         .access = sg::access_flag::shader_read,
         .layout = sg::texture_layout::shader_readonly},
    };

    auto const typed_target = sg::texture_2d::from_raw(target);
    auto cmd = ctx.create_command_list();
    {
        auto pass = cmd->raster.render_to(
            {.color_targets = {typed_target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
        pass.bind_pipeline(*pipeline);
        pass.bind_group(0, *bufs_group);
        pass.bind_group(1, *texs_group);
        pass.declare_array_buffer_access("Bufs", buf_access);
        pass.declare_array_texture_access("Texs", tex_access);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(target);
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.size() == isize(W) * isize(H) * 4);
    auto const* const px = reinterpret_cast<u8 const*>(bytes.data());
    auto const* const center = px + (isize(H / 2) * W + W / 2) * 4;
    CHECK(center[0] == u8(raster_b0_value));
    CHECK(center[1] == u8(raster_b3_value));
    CHECK(center[2] == texel_value);
}

ASYNC_INVOCABLE_TEST("ssc::dxc + dx12 - raster array bindings: the accounting rule", (sg::context_handle const& handle))
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    REQUIRE(handle != nullptr);
    sg::context& ctx = *handle;

    auto compile = [&](char const* entry, sg::shader_stage stage) -> sg::compiled_shader
    {
        ssc::dxc::shader_description sd;
        sd.source = raster_array_hlsl;
        sd.entry_point = entry;
        sd.stage = stage;
        sd.model = ssc::dxc::shader_model::sm_6_8;
        auto r = comp.value().compile(sd);
        REQUIRE(r.has_value());
        return cc::move(r.value());
    };
    sg::compiled_shader vs = compile("main_vs", sg::shader_stage::vertex);
    sg::compiled_shader ps = compile("main_ps", sg::shader_stage::fragment);

    cc::vector<sg::binding> bufs_set;
    cc::vector<sg::binding> texs_set;
    for (auto const& b : ps.bindings)
    {
        if (b.space == 0u)
            bufs_set.push_back(b);
        else
            texs_set.push_back(b);
    }

    auto bufs_layout = ctx.uncached.create_binding_group_layout(bufs_set);
    auto texs_layout = ctx.uncached.create_binding_group_layout(texs_set);
    auto pipeline_layout = ctx.uncached.create_pipeline_layout({.groups = {bufs_layout, texs_layout}});

    sg::raster_pipeline_description desc;
    desc.layout = pipeline_layout;
    desc.vertex_shader = cc::move(vs);
    desc.fragment_shader = cc::move(ps);
    desc.topology = sg::primitive_topology::triangle_list;
    desc.rasterization.cull = sg::cull_mode::none;
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});
    auto pipeline = co_await ctx.cached.acquire_raster_pipeline(desc);
    REQUIRE(pipeline != nullptr);

    // All-vacant arrays: null descriptors read as zero, so no data setup is needed here.
    auto buf_elements = cc::vector<sg::raw_view>();
    auto tex_elements = cc::vector<sg::raw_view>();
    for (isize i = 0; i < 4; ++i)
    {
        buf_elements.push_back(sg::vacant_view{});
        tex_elements.push_back(sg::vacant_view{});
    }
    auto const bufs_nv = sg::named_view{.name = "Bufs", .view = cc::move(buf_elements)};
    auto const texs_nv = sg::named_view{.name = "Texs", .view = cc::move(tex_elements)};
    auto bufs_group = ctx.persistent.create_binding_group(bufs_layout, cc::span<sg::named_view const>(&bufs_nv, 1));
    auto texs_group = ctx.persistent.create_binding_group(texs_layout, cc::span<sg::named_view const>(&texs_nv, 1));
    REQUIRE(bufs_group != nullptr);
    REQUIRE(texs_group != nullptr);

    sg::texture_description rt_desc;
    rt_desc.format = sg::pixel_format::rgba8_unorm;
    rt_desc.dimension = sg::texture_dimension::d2;
    rt_desc.width = 8;
    rt_desc.height = 8;
    rt_desc.usage = sg::texture_usage::render_target;
    auto target = ctx.persistent.create_raw_texture(rt_desc);
    REQUIRE(target != nullptr);
    auto const typed_target = sg::texture_2d::from_raw(target);

    // A draw with an undeclared bound array binding trips the accounting assert, exactly as a dispatch does.
    {
        auto cmd = ctx.create_command_list();
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {typed_target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
            pass.bind_pipeline(*pipeline);
            pass.bind_group(0, *bufs_group);
            pass.bind_group(1, *texs_group);
            CHECK_ASSERTS(pass.draw({.vertex_range = {.offset = 0, .size = 3}}));
        }
        ctx.drop_command_list(cc::move(cmd));
    }

    // Empty-span declarations say "unused" and satisfy the accounting; the draw then runs.
    {
        auto cmd = ctx.create_command_list();
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {typed_target.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))}});
            pass.bind_pipeline(*pipeline);
            pass.bind_group(0, *bufs_group);
            pass.bind_group(1, *texs_group);
            pass.declare_array_buffer_access("Bufs", {});
            pass.declare_array_texture_access("Texs", {});
            pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        ctx.submit_command_list(cc::move(cmd));
        ctx.advance_epoch();
        co_await ctx.idle_completion();
    }
}
