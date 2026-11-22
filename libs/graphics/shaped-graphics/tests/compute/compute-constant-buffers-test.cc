#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// One group with plain members and two `constants[T]` members of different structs, each its own constant buffer at the
// slot its declaration gives it, between the group's resources (CHK-539).
ASYNC_INVOCABLE_TEST("sg - a group binds constants[T] buffers of its own beside the block of its plain members",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::constant_buffers::read_blocks.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::constant_buffers::blocks>();

    // every value is a small integer or a half, so the GPU's products are exact
    auto const view = cc::vector<shaders::constant_buffers::view_params>{
        {.scale = tg::vec4f(1, 2, 3, 4), .offset = tg::vec4f(10, 20, 30, 40)}};
    auto const light
        = cc::vector<shaders::constant_buffers::light_params>{{.color = tg::vec3f(1, 0.5f, 2), .intensity = 4}};
    auto const view_buffer = ctx->persistent.create_buffer_from_data(view, sg::buffer_usage::constants_buffer);
    auto const light_buffer = ctx->persistent.create_buffer_from_data(light, sg::buffer_usage::constants_buffer);
    auto const results
        = ctx->persistent.create_buffer<tg::vec4f>(3, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::constant_buffers::blocks{
                                                               .factor = 2,
                                                               .bias = -3,
                                                               .view = view_buffer.as_constants_buffer(),
                                                               .results = results.as_readwrite_buffer(),
                                                               .light = light_buffer.as_constants_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(3);
    auto const back = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await back.data();
    REQUIRE(got.size() == 3);
    // the whole view read as one value, and a field of it read in place
    CHECK(got[0] == tg::vec4f(12, 24, 36, 48));
    // the light's fields, beside the block's plain members
    CHECK(got[1] == tg::vec4f(4, 2, 8, -3));
    CHECK(got[2] == tg::vec4f(2, -3, 4, 40));
}
