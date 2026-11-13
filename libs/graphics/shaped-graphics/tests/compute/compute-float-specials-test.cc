#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/float.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/float_specials.sgl: what a float is on the GPU, against EVAL-6's IEEE number.

ASYNC_INVOCABLE_TEST("sg - an SGL float keeps NaNs, infinities, signed zeros and the order of a sum on the GPU",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    // EMIT-185: vulkan keeps the specials where the device has the execution mode, and WebGPU keeps nothing above .fast
    if (!ctx->supports(sg::feature::float_specials))
        SKIP("this device keeps no NaNs, infinities and signed zeros a shader asks for");

    auto const pipeline = co_await shaders::float_specials::probe.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::float_specials::float_specials>();

    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto const least_denormal = cc::bit_cast<float>(u32(1));
    auto const inputs = ctx->persistent.create_buffer_from_data(
        cc::vector<float>{0.0f, 1.0f, 1.0e30f, 1.0e-38f, least_denormal, 1.0e-3f}, sg::buffer_usage::readonly_buffer);
    auto const results = ctx->persistent.create_buffer_from_data(cc::vector<float>::create_defaulted(10), usage);
    auto const comparisons = ctx->persistent.create_buffer_from_data(cc::vector<u32>::create_defaulted(5), usage);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::float_specials::float_specials{.inputs = inputs.as_readonly_buffer(),
                                                .results = results.as_readwrite_buffer(),
                                                .comparisons = comparisons.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const results_back = cmd->download.data_from_buffer(results);
    auto const comparisons_back = cmd->download.data_from_buffer(comparisons);
    ctx->submit_command_list(cc::move(cmd));

    auto const got = co_await results_back.data();
    REQUIRE(got.size() == 10);
    auto const bits = [&](isize i) { return cc::bit_cast<u32>(got[i]); };
    CHECK(bits(0) == 0x7f800000u);
    CHECK(cc::is_nan(got[1]));
    // an infinity times zero, whose `(one / zero) * zero` Intel's driver cancels to 1 even from strict DXIL and SPIR-V,
    // on dx12 and on vulkan: bug-sg-intel-dx12-zero-times-inf
    auto const& adapter = ctx->metrics.adapter();
    if (adapter.is_software || adapter.vendor_id != 0x8086)
        CHECK(cc::is_nan(got[2]));
    // -0 + 0 is +0
    CHECK(bits(3) == 0x00000000u);
    CHECK(bits(4) == 0x80000000u);
    // (1e30 + 1) - 1e30 rounds the 1 away
    CHECK(got[5] == 0.0f);
    CHECK(bits(6) == 0xff800000u);

    auto const compared = co_await comparisons_back.data();
    REQUIRE(compared.size() == 5);
    // nan != nan
    CHECK(compared[0] == 1u);
    // nan == nan
    CHECK(compared[1] == 0u);
    // nan < 1 or nan >= 1
    CHECK(compared[2] == 0u);
    // inf > 1e30
    CHECK(compared[3] == 1u);

    // A copy keeps a denormal's bits on every device.
    CHECK(bits(9) == 1u);
    // Metal's safe math flushes them, read and written, measured on an M4: why metal reports no float_denormals.
    // A device that kept them would fail here, and could then report the feature and keep `.ieee`.
    if (ctx->backend() == sg::backend_kind::metal)
    {
        CHECK(bits(7) == 0u);
        CHECK(bits(8) == 0u);
        CHECK(compared[4] == 0u);
    }
}
