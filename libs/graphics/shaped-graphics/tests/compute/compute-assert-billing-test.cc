#include "../backends/sg_backends.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/impl/debug_windows.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

// A shader's failed assert nobody declared, judged on a nested run as the drivers run every test: it fails the test that dispatched it, and nothing else.
// The record comes back only once its epoch retires, which the dispatching test never waits for here.

namespace
{
namespace shaders = sg::test::sgl_shaders;

struct subject
{
    sg::context_handle ctx;
};

/// Dispatches diagnostics.sgl's checked doubling over one negative input, and ends before its epoch retires.
cc::shared_async<cc::unit> dispatches_a_failing_assert(subject const& s)
{
    auto& ctx = *s.ctx;
    if (auto* const home = ctx.device_home())
        co_await cc::async_resume_on(*home);

    auto const pipeline = co_await shaders::diagnostics::double_checked.acquire_pipeline(ctx);
    auto const layout = ctx.cached.acquire_binding_group_layout<shaders::diagnostics::checked>();
    auto const values
        = ctx.persistent.create_buffer_from_data(cc::vector<int>{1, -2, 3, 4}, sg::buffer_usage::readwrite_buffer);
    auto cmd = ctx.create_command_list();
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout, shaders::diagnostics::checked{.values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(4);
    ctx.submit_command_list(cc::move(cmd));
    CHECK(true);
}

cc::shared_async<cc::unit> runs_no_shader(subject const&)
{
    CHECK(true);
    co_return;
}

cc::shared_async<cc::unit> drive(subject s)
{
    co_await nx::async_invoke_tests_in_sequence("g",
                                                nx::invocation_options{.after_each = sg_test::sweep_after_each(s.ctx),
                                                                       .finish_each = sg_test::settle_each(s.ctx)},
                                                s);
}

template <class F>
void add_child(nx::test_registry& reg, cc::string name, F* fn)
{
    reg.add_async_invocable_declaration(cc::move(name), {}, cc::arg_types_of(cc::signature_of<F*>{}),
                                        nx::impl::make_async_test_invoker(fn));
}

[[nodiscard]] bool mentions(cc::span<nx::test_error const> errors, cc::string_view text)
{
    auto found = false;
    for (auto const& e : errors)
        found |= e.expr.contains(text) || e.expanded.contains(text);
    return found;
}
} // namespace

TEST("sg - a shader's undeclared failed assert fails the test that dispatched it, and no other", no_scheduler)
{
    if (!sg::impl::debug_windows::k_enabled)
        SKIP("SC_SHADER_DIAGNOSTICS is off, so a shader writes no assert records");
    if (!nx::impl::run_recording_active())
        SKIP("the run has no recorder (--no-recording), so the log rule judges nothing");
    if (sg_test::context_factories().empty())
        SKIP("this binary has no backend");

    for (auto& factory : sg_test::context_factories())
    {
        auto created = factory.create_for_test();
        if (created.has_error())
            continue;
        auto const ctx = created.value();
        (void)sg_test::shader_fixtures();
        if (!sg_test::shaders_reach(*ctx))
            continue;

        nx::test_registry reg;
        add_child(reg, "1 - dispatches a failing assert", &dispatches_a_failing_assert);
        add_child(reg, "2 - runs no shader", &runs_no_shader);
        reg.add_async_declaration("driver", {}, [ctx](nx::impl::async_test_sink& sink)
                                  { nx::impl::submit_test_async(sink, drive(subject{ctx})); });
        auto const exec = nx::execute_tests(nx::test_schedule::create({}, reg), {});

        REQUIRE(exec.executions.size() == 1);
        auto const& driver = exec.executions[0];
        CHECK(!driver.root.is_considered_failing).context(factory.backend);
        REQUIRE(driver.nested.size() == 2);
        CHECK(driver.nested[0].is_considered_failing()).context(factory.backend);
        CHECK(mentions(driver.nested[0].root.errors, "diagnostics.sgl:9: assert failed: a negative input -2"))
            .context(factory.backend);
        CHECK(!driver.nested[1].is_considered_failing()).context(factory.backend);
    }
}
