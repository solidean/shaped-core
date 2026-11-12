#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/gpu_affine3.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

namespace
{
/// A distinct map per seed, with no symmetric part and every translation lane in use, so a row read as a column or
/// one row moved shows in the result.
slib::gpu_affine3 map_of(f32 k)
{
    return {tg::vec4f(1, k, 0, 2), tg::vec4f(0, 1, 2 * k, -1), tg::vec4f(k, 3, 1, 4)};
}

/// A position mapped by `a`, row by row, as CHK-408 states an affine3 does.
tg::vec3f apply(slib::gpu_affine3 const& a, tg::vec3f p)
{
    auto result = tg::vec3f();
    for (auto r = 0; r < 3; ++r)
        result[r] = a.rows[r][0] * p[0] + a.rows[r][1] * p[1] + a.rows[r][2] * p[2] + a.rows[r][3];
    return result;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL affine3 in GPU memory is its three rows, in a block, a buffer and a struct (CHK-518)",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::affine_memory::place.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::affine_memory::placing>();

    constexpr auto k_count = 4;
    auto xfs = cc::vector<slib::gpu_affine3>();
    auto items = cc::vector<shaders::affine_memory::placed>::create_defaulted(k_count);
    for (auto i = 0; i < k_count; ++i)
    {
        xfs.push_back(map_of(f32(i + 2)));
        items[i].tag = f32(10 * i);
        items[i].xf = map_of(f32(-i - 1));
    }
    auto const camera = map_of(0.5f);

    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const xf_buffer = ctx->persistent.create_buffer_from_data(xfs, sg::buffer_usage::readonly_buffer);
    auto const item_buffer = ctx->persistent.create_buffer_from_data(items, sg::buffer_usage::readonly_buffer);
    auto const points = ctx->persistent.create_buffer<tg::vec4f>(k_count, usage);
    auto const written = ctx->persistent.create_buffer<slib::gpu_affine3>(k_count, usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::affine_memory::placing{
                                                               .lead = 0.25f,
                                                               .camera = camera,
                                                               .xfs = xf_buffer.as_readonly_buffer(),
                                                               .items = item_buffer.as_readonly_buffer(),
                                                               .points = points.as_readwrite_buffer(),
                                                               .written = written.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_count);
    auto const points_back = cmd->download.data_from_buffer(points);
    auto const written_back = cmd->download.data_from_buffer(written);
    ctx->submit_command_list(cc::move(cmd));

    auto const got_points = co_await points_back.data();
    auto const got_written = co_await written_back.data();
    REQUIRE(got_points.size() == k_count);
    REQUIRE(got_written.size() == k_count);
    for (auto i = 0; i < k_count; ++i)
    {
        // every value is a small integer or a quarter, so the GPU's products are exact
        auto const p = apply(camera, apply(xfs[i], apply(items[i].xf, tg::vec3f(1, 2, 3))));
        CHECK((got_points[i] == tg::vec4f(p[0], p[1], p[2], items[i].tag + 0.25f))).context(cc::format("item {}", i));
        CHECK((got_written[i].rows[0] == xfs[i].rows[2])).context(cc::format("item {}", i));
        CHECK((got_written[i].rows[1] == items[i].xf.rows[0])).context(cc::format("item {}", i));
        CHECK((got_written[i].rows[2] == camera.rows[1])).context(cc::format("item {}", i));
    }
}
