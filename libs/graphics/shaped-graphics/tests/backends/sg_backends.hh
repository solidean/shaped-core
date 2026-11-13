#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/fwd.hh> // sg::context_handle

namespace sg_test
{
struct backend_entry;
struct context_factory;
} // namespace sg_test

// Backends compiled into the sg API test binary (shaped-graphics-test).
// Each backend entry file (tests/backends/<backend>-entry.cc) registers its driver here at static-init.
// The alias setup (tests/backends/backends.cc) reads the table to build, per context_handle invocable, one alias fragment per backend.
// See libs/base/nexus/docs/invocable-tests.md for the alias mechanism.

struct sg_test::backend_entry
{
    cc::string driver; ///< the entry-point driver test name, e.g. "sg dx12 warp backend"
    cc::string invoke; ///< the nx::invoke_tests group it dispatches under, e.g. "dx12-warp"
};

// A way to make a fresh context of one backend, for a test or benchmark that needs its own rather than the driver's.
struct sg_test::context_factory
{
    cc::string backend; ///< e.g. "dx12"
    /// A context as a benchmark wants it, with no validation slowing it down.
    cc::unique_function<cc::result<sg::context_handle>()> create;
    /// A context as a test running beside the drivers needs it: validated as theirs are.
    /// The dx12 debug layer is process-wide and must be on before the first device, so a test that created one without it
    /// first would fail every dx12 driver after it; empty where `create` already serves.
    cc::unique_function<cc::result<sg::context_handle>()> create_validated;

    [[nodiscard]] cc::result<sg::context_handle> create_for_test() const
    {
        return create_validated.is_valid() ? create_validated() : create();
    }
};

namespace sg_test
{

// The registered backends (function-local static: order-independent across TUs). Defined in backends.cc.
cc::vector<backend_entry>& backends();

// Appends a backend; returns true so it can seed a static-init bool.
inline bool register_backend(cc::string driver, cc::string invoke)
{
    backends().push_back(backend_entry{.driver = cc::move(driver), .invoke = cc::move(invoke)});
    return true;
}

// The registered context factories, one per backend compiled in.
// Defined in backends.cc.
cc::vector<context_factory>& context_factories();

// Appends a factory; returns true so it can seed a static-init bool.
inline bool register_context_factory(cc::string backend,
                                     cc::unique_function<cc::result<sg::context_handle>()> create,
                                     cc::unique_function<cc::result<sg::context_handle>()> create_validated = {})
{
    context_factories().push_back(context_factory{.backend = cc::move(backend),
                                                  .create = cc::move(create),
                                                  .create_validated = cc::move(create_validated)});
    return true;
}

/// What a driver awaits after each test of its sequence on one shared context: a sweep, once that test submitted anything.
/// Advancing reclaims nothing, so epochs a test left in flight otherwise hold their memory, the upload ring above all, until a later test happens to sweep.
/// Bounded at two in flight, as a frame loop is.
inline cc::unique_function<cc::shared_async<cc::unit>()> sweep_after_each(sg::context_handle ctx)
{
    auto seen = ctx->metrics.stats()[sg::stat::command_lists_submitted];
    return [ctx = cc::move(ctx), seen]() mutable
    {
        auto const submitted = ctx->metrics.stats()[sg::stat::command_lists_submitted];
        if (submitted == seen)
            return cc::make_async_from_value(cc::unit{});
        seen = submitted;
        return ctx->epochs_in_flight_completion(2);
    };
}

/// What a driver awaits as each test's last step, inside that test: the shader print and assert records it left in flight, decoded and logged.
/// A shader's failed assert is then the error of the test that dispatched it, rather than of whichever test or driver next retires its epoch.
inline cc::unique_function<cc::shared_async<cc::unit>()> settle_each(sg::context_handle ctx)
{
    return [ctx = cc::move(ctx)] { return ctx->diagnostics_settled(); };
}
} // namespace sg_test
