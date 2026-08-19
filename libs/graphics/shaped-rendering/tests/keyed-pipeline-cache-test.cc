#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>

#include <memory>

// keyed_pipeline_cache on a dx12 WARP device.
// The cache is generic, so a stand-in "pipeline" (a shared_ptr<int>) exercises its dedup / warm / clear / error behavior exactly.
// Only `init` needs a real context, which is why these are WARP-gated.
// The real sg::raster_pipeline path — the default template arg, built through ctx.cached.acquire_raster_pipeline — is exercised by sr::blit_routine.

namespace
{
using fake_handle = std::shared_ptr<int const>;

sg::context_handle make_context()
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sr - keyed pipeline cache builds one pipeline per key")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto builds = 0;
    auto cache = sr::keyed_pipeline_cache<sg::pixel_format, int>();
    cache.init(*ctx,
               [&builds](sg::context&, sg::pixel_format) -> cc::shared_async<fake_handle>
               {
                   ++builds;
                   return cc::make_async_from_value<fake_handle>(std::make_shared<int const>(builds));
               });

    // Same key twice: built once, and the second acquire returns the same node.
    auto const a = cache.acquire(sg::pixel_format::rgba8_unorm);
    auto const b = cache.acquire(sg::pixel_format::rgba8_unorm);
    CHECK(builds == 1);
    CHECK(a.get() == b.get());

    // A different key builds again.
    (void)cache.acquire(sg::pixel_format::bgra8_unorm);
    CHECK(builds == 2);
}

TEST("sr - keyed pipeline cache prepare warms without rebuilding")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto builds = 0;
    auto cache = sr::keyed_pipeline_cache<sg::pixel_format, int>();
    cache.init(*ctx,
               [&builds](sg::context&, sg::pixel_format) -> cc::shared_async<fake_handle>
               {
                   ++builds;
                   return cc::make_async_from_value<fake_handle>(std::make_shared<int const>(builds));
               });

    cache.prepare(sg::pixel_format::rgba8_unorm);
    auto const a = cache.acquire(sg::pixel_format::rgba8_unorm);
    auto const b = cache.acquire(sg::pixel_format::rgba8_unorm);

    // The prepared node is reused: built exactly once across prepare + two acquires.
    CHECK(builds == 1);
    CHECK(a.get() != nullptr);
    CHECK(a.get() == b.get());
}

TEST("sr - keyed pipeline cache re-init clears the cache")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto cache = sr::keyed_pipeline_cache<sg::pixel_format, int>();

    auto first_builds = 0;
    cache.init(*ctx,
               [&first_builds](sg::context&, sg::pixel_format) -> cc::shared_async<fake_handle>
               {
                   ++first_builds;
                   return cc::make_async_from_value<fake_handle>(std::make_shared<int const>(1));
               });
    (void)cache.acquire(sg::pixel_format::rgba8_unorm);
    CHECK(first_builds == 1);

    // Re-init (a reload) drops the old entry, so the same key builds again through the new callback.
    auto second_builds = 0;
    cache.init(*ctx,
               [&second_builds](sg::context&, sg::pixel_format) -> cc::shared_async<fake_handle>
               {
                   ++second_builds;
                   return cc::make_async_from_value<fake_handle>(std::make_shared<int const>(2));
               });
    (void)cache.acquire(sg::pixel_format::rgba8_unorm);
    CHECK(second_builds == 1);
}

TEST("sr - keyed pipeline cache surfaces a build failure")
{
    auto const ctx = make_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto cache = sr::keyed_pipeline_cache<sg::pixel_format, int>();
    cache.init(
        *ctx, [](sg::context&, sg::pixel_format) -> cc::shared_async<fake_handle>
        { return cc::make_async_from_error<fake_handle>(cc::async_error::make_error(cc::any_error("build failed"))); });

    // The sync try_ form collapses the failure to a result error.
    auto const r = cache.try_acquire(sg::pixel_format::rgba8_unorm);
    CHECK(r.has_error());

    // The async form carries it on the error channel.
    auto node = cache.acquire_async(sg::pixel_format::rgba8_unorm);
    auto const driven = cc::try_async_blocking_get(node);
    CHECK(driven.has_error());
}
