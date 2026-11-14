#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-graphics/routine/routine_registry.hh>

namespace sr_test
{
struct backend_entry;
}

// The backends compiled into shaped-rendering-test, each an entry driver that invokes every sg::context_handle test on one context of it.
// Each tests/backends/<backend>-entry.cc registers its driver here at static init, and backends.cc aliases every
// invocable to one run per driver, so `dev.py test "sr - <name>"` runs that test on each backend this binary has.
// The same shape as shaped-graphics-test's; libs/base/nexus/docs/invocable-tests.md is the alias mechanism.

struct sr_test::backend_entry
{
    cc::string driver; ///< the entry driver's test name, e.g. "sr vulkan"
    cc::string invoke; ///< the group it invokes under, e.g. "vulkan"
};

namespace sr_test
{
/// The registered backends; a function-local static, so the order the entry files initialize in does not matter.
cc::vector<backend_entry>& backends();

/// Appends a backend; returns true so it can seed a static-init bool.
inline bool register_backend(cc::string driver, cc::string invoke)
{
    backends().push_back(backend_entry{.driver = cc::move(driver), .invoke = cc::move(invoke)});
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

/// What a driver awaits as each test's last step, inside that test, so what the test left behind is its own work.
/// The routines it asked for and left pending come up first.
/// Otherwise the next test's first tick starts their phases, and one still running when that test ends is billed to it as work it left.
/// Then the shader print and assert records it left in flight are decoded and logged.
/// A shader's failed assert is so the error of the test that dispatched it, rather than of whichever test or driver next retires its epoch.
inline cc::shared_async<cc::unit> settled(sg::context_handle ctx)
{
    (void)co_await ctx->routines.idle_completion();
    co_await ctx->diagnostics_settled();
}

inline cc::unique_function<cc::shared_async<cc::unit>()> settle_each(sg::context_handle ctx)
{
    return [ctx = cc::move(ctx)] { return settled(ctx); };
}
} // namespace sr_test
