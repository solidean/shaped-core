#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>

// The package this test target declares itself, generated into the build dir; see sc_add_shader_package in
// shaped-graphics' CMakeLists.
#include <sg_test_shaders.hh>

using namespace cc::primitive_defines;

// **One draw's fragment-shader writes, read by the next draw in the same pass.**
//
// libs/graphics/shaped-graphics/docs/concepts/barriers.md gives the caller no barrier API at all: sg "inserts the GPU
// barriers that order those accesses", and a caller that binds the same read-write buffer to two draws in a row is
// owed exactly that.
// Each backend pays for it differently — dx12 records a UAV barrier, vulkan ends the pass and reopens it, metal closes
// and reopens its render encoder because MTL4 refuses the fragment stage as a barrier source — so what is shared is
// the result rather than the mechanism, which is what makes this a tier-1 test.
//
// **It can pass by timing rather than by ordering.** A 4x4 draw is short, and a GPU that happened to finish A before
// starting B produces the right pixels with no barrier at all — so a failure here is real and a pass is not proof on
// its own.
// The metal tier-2 suite asserts the mechanism directly, which is the half this cannot reach.

namespace
{
constexpr auto k_size = 4;
constexpr auto k_count = k_size * k_size;

/// The pipeline of one hazard draw: no vertex input, one group, and `fragment` as the fragment stage.
[[nodiscard]] sg::async_raster_pipeline make_hazard_pipeline(sg::context& ctx,
                                                             sg::pipeline_layout_handle layout,
                                                             sg::compiled_shader const& vertex,
                                                             sg::compiled_shader const& fragment)
{
    auto desc = sg::raster_pipeline_description{
        .layout = cc::move(layout),
        .vertex_shader = vertex,
        .fragment_shader = fragment,
        // No culling: which way the full-screen triangle happens to wind is not what this asserts.
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    return ctx.cached.acquire_raster_pipeline(desc);
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a draw sees what the previous draw's fragment shader wrote",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;

    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        SKIP("no DXC compiler to build the hazard shaders");

    auto lib = slib::shader_library();
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sg::test::shaders::package());

    auto const vertex = sg::test::shaders::fragment_hazard.vertex.vs_main->acquire(ctx);
    auto const writer = sg::test::shaders::fragment_hazard.fragment.ps_write->acquire(ctx);
    auto const reader = sg::test::shaders::fragment_hazard.fragment.ps_read->acquire(ctx);
    co_await cc::async_settled(vertex);
    co_await cc::async_settled(writer);
    co_await cc::async_settled(reader);

    auto const* const vertex_compiled = vertex->try_value();
    auto const* const writer_compiled = writer->try_value();
    auto const* const reader_compiled = reader->try_value();
    if (vertex_compiled == nullptr || writer_compiled == nullptr || reader_compiled == nullptr)
        SKIP("this context accepts no format the hazard shaders compile to");

    // The bindings are the fragment stage's: the vertex stage reads nothing.
    auto const group_layout = ctx.cached.acquire_binding_group_layout(writer_compiled->bindings);
    auto const layout = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {group_layout}});

    auto const writer_pipeline = make_hazard_pipeline(ctx, layout, *vertex_compiled, *writer_compiled);
    auto const reader_pipeline = make_hazard_pipeline(ctx, layout, *vertex_compiled, *reader_compiled);
    co_await cc::async_settled(writer_pipeline);
    co_await cc::async_settled(reader_pipeline);

    auto const* const writer_state = writer_pipeline->try_value();
    auto const* const reader_state = reader_pipeline->try_value();
    REQUIRE(writer_state != nullptr);
    REQUIRE(reader_state != nullptr);
    REQUIRE(*writer_state != nullptr);
    REQUIRE(*reader_state != nullptr);

    auto const results
        = ctx.persistent.create_buffer<u32>(k_count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_dst);
    auto const target = ctx.persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    });

    auto cmd = ctx.create_command_list();

    // Zeroed first, so a read that raced ahead of the write reads 0 rather than whatever the allocation held.
    auto const zeros = cc::vector<u32>::create_filled(k_count, u32(0));
    cmd->upload.data_to_buffer(results, zeros);

    auto const group = ctx.transient.create_binding_group(
        group_layout, {{.name = "gResults", .view = results.as_readwrite_buffer()}});

    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);

        scope.bind_pipeline(**writer_state);
        scope.bind_group(0, *group);
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});

        scope.bind_pipeline(**reader_state);
        scope.bind_group(0, *group);
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx.submit_command_list(cc::move(cmd));

    auto const bytes = co_await future.bytes();
    REQUIRE(bytes.span().size() == k_count * 4);
    auto const pixels = bytes.span();

    // `ps_write` writes `index + 1` per pixel and `ps_read` scales it by 1/255, so texel i reads back as i + 1 — and a
    // texel the second draw found unwritten reads back as 0.
    auto wrong = 0;
    auto first_wrong = -1;
    for (auto i = 0; i < k_count; ++i)
    {
        auto const value = int(u8(pixels[i * 4]));
        auto const delta = value - (i + 1);
        if ((delta < 0 ? -delta : delta) > 1)
        {
            ++wrong;
            if (first_wrong < 0)
                first_wrong = i;
        }
    }

    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels wrong; texel {} read back as {}, expected {}", wrong, k_count, first_wrong,
                            first_wrong >= 0 ? int(u8(pixels[first_wrong * 4])) : 0, first_wrong + 1));
}
