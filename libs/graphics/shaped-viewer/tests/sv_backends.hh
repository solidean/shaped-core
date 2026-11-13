#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>

namespace sv_test
{
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
} // namespace sv_test
