#pragma once

#include <clean-core/common/utility.hh> // cc::unit
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/bench/result.hh>
#include <nexus/bench/run_config.hh>

// Measuring async work: each iteration is a graph the harness awaits, timed from starting it to its resolve.
//
// A separate header because it is the one bench entry point that names an async type, and nexus/bench/bench.hh stays
// free of them.

namespace nx::bench::impl
{
/// The engine behind run_async, type-erased so the sampling logic is compiled once.
/// `iteration` starts one iteration and hands back its graph, cold or already running.
cc::shared_async<result> run_awaited(cc::string_view name,
                                     run_config cfg,
                                     cc::unique_function<cc::shared_async<cc::unit>()> iteration);
} // namespace nx::bench::impl

namespace nx::bench
{
/// Measure async work: `body` starts one iteration and returns the graph it produced, and the harness awaits it.
///
/// **One iteration is one sample.**
/// A graph cannot be batched the way a nanosecond body can — each one crosses a scheduler — so there is no batch
/// calibration, no hardware-counter pass and no harness-overhead estimate, and `cfg.batch` is ignored.
/// Warmup still runs: `warmup_iterations` when set, `warmup_time_secs` otherwise, and at least one either way.
///
/// **Awaited where it is called**, so the time includes whatever the graph spends queued behind other work on the same
/// scheduler.
/// That is usually the point — latency as a caller sees it — and an ASYNC_BENCHMARK runs alone, so the only other work
/// is the graph's own.
///
/// An iteration whose graph fails ends the run on that failure, like any other await.
///
///     ASYNC_BENCHMARK("sg stream - upload latency")
///     {
///         auto ctx = co_await make_context();
///         co_await nx::bench::run_async("4 MiB", [&] { return upload(ctx, four_mib).completion(); });
///     }
template <class Body>
cc::shared_async<result> run_async(cc::string_view name, run_config const& cfg, Body body)
{
    return impl::run_awaited(name, cfg,
                             [body = cc::move(body)]() mutable -> cc::shared_async<cc::unit>
                             {
                                 auto const graph = body(); // named: the coroutine outlives this full-expression
                                 (void)co_await graph;
                             });
}

/// The same, with the default config.
template <class Body>
cc::shared_async<result> run_async(cc::string_view name, Body body)
{
    return bench::run_async(name, run_config::standard(), cc::move(body));
}
} // namespace nx::bench
