#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>

#include <type_traits>

// cc::async_parallel_for — body over every index of [0, n), across the threads of the scheduler it runs on, as one async.
//
//   co_await cc::async_parallel_for(n, [&](isize i) { out[i] = f(in[i]); });              // per index
//   co_await cc::async_parallel_for(n, [&](isize begin, isize end) { ... });              // per chunk
//   co_await cc::async_parallel_for(n, body, {.grain = 1024});                            // for bodies of a few ns
//
// "Parallel loops" in libs/base/clean-core/docs/systems/async.md is the model; the measurements behind it in libs/base/clean-core/docs/benchmarks/async-benchmark.md.
// Cold like every async: nothing runs until it is awaited, required or scheduled.
// The body is owned, moved in, so a loop held across a suspension cannot dangle; capture by reference what it works on.
// A body that throws fails the loop, as async_all fails on a failed member: the remaining chunks are dropped, the ones running finish.

/// How a parallel loop splits its range.
struct cc::parallel_for_options
{
    /// Indices a thread claims at a time; 0 picks n / (64 * the scheduler's parallelism), at least 1.
    /// A claim is a CAS on the claiming thread's own slice, so a small grain is cheap: only bodies of a few nanoseconds want a larger one.
    isize grain = 0;
};

namespace cc
{
namespace impl
{
[[nodiscard]] shared_async<cc::unit> async_parallel_for_ranges(isize n,
                                                               unique_function<void(isize, isize)> body,
                                                               parallel_for_options options);
}

/// Run `body` over [0, n): as `body(i)` per index, or as `body(begin, end)` per disjoint chunk.
/// Settles once every index ran, or with the first failure of a body.
///
/// On a pool it runs on every thread that is idle or goes idle while it lasts, the awaiting one included, each taking chunks until none is left.
/// It runs inline, on the awaiting thread, when the scheduler is single-threaded or the range is a grain or less.
/// Chunks run in no particular order and on any thread, so `body` must be safe to call concurrently with itself.
/// The body is destroyed on the thread that resumes the awaiter, before it resumes.
template <class F>
[[nodiscard]] shared_async<cc::unit> async_parallel_for(isize n, F body, parallel_for_options options = {})
{
    if constexpr (std::is_invocable_v<F&, isize, isize>)
    {
        return impl::async_parallel_for_ranges(n, unique_function<void(isize, isize)>(cc::move(body)), options);
    }
    else
    {
        static_assert(std::is_invocable_v<F&, isize>, "the body takes one index, (isize i), or one chunk, (isize "
                                                      "begin, isize end)");
        return impl::async_parallel_for_ranges(n,
                                               unique_function<void(isize, isize)>(
                                                   [body = cc::move(body)](isize begin, isize end) mutable
                                                   {
                                                       for (auto i = begin; i < end; ++i)
                                                           body(i);
                                                   }),
                                               options);
    }
}
} // namespace cc
