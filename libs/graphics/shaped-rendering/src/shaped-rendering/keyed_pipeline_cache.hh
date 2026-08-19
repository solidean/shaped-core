#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/map.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/fwd.hh>

#include <memory>

/// A get-or-create cache of pipelines keyed by a caller-chosen key — one pipeline per key, built once.
///
/// The key is almost always the render-target pixel format: a routine draws the same shaders into whatever target it is handed, and each distinct format needs its own pipeline.
///
/// `Pipeline` defaults to sg::raster_pipeline.
/// The handle type is std::shared_ptr<Pipeline const>, matching sg's own pipeline-handle aliases (so the default `handle` IS sg::raster_pipeline_handle).
///
/// The build callback (given the context + the key) does the actual creation, so the cache stays agnostic to what a pipeline needs.
/// The caller captures its layout and shaders into the callback at `init` time.
///
/// The whole acquire path is const — only `init` mutates — so a const cache can still build lazily.
///
/// Error model: the sync path is a `try_acquire` (-> cc::result) / `acquire` (-> throws) pair.
/// The async path needs no separate `try_`, since a cc::shared_async already carries its outcome — `acquire_async` is the fallible form.
///
/// Threading: a build may run on a pool worker and may be invoked concurrently for distinct keys, so the callback must only read immutable captures.
/// Backend PSO creation is free-threaded where this matters, e.g. dx12.
/// Only the key -> pipeline map is behind a mutex.
/// The context and callback are plain members set by `init`, so `init` must not race in-flight builds — call it at (re)load points, which are serialized with rendering.
/// With no pool installed, builds are driven inline on the calling thread.
template <class Key, class Pipeline = sg::raster_pipeline>
class sr::keyed_pipeline_cache
{
public:
    using handle = std::shared_ptr<Pipeline const>;
    using async_handle = cc::shared_async<handle>;
    using build_fn = cc::unique_function<cc::result<handle>(sg::context&, Key const&)>;

    /// Store the context + build callback and CLEAR the cache.
    /// Call from a routine's init_declare on every (re)load: a rebuilt pipeline layout invalidates every pipeline cached against the old one.
    /// Re-`init` both drops them and rebinds the fresh callback.
    void init(sg::context& ctx, build_fn build)
    {
        _ctx = &ctx;
        _build = cc::move(build);
        _cache.lock([](map_t& m) { m.clear(); });
    }

    /// The pipeline for `key`, get-or-scheduled: one build per key, and a warmed build is reused.
    /// A build failure surfaces as an async error.
    /// Drive with cc::(try_)async_blocking_get, or poll.
    [[nodiscard]] async_handle acquire_async(Key const& key) const
    {
        return _cache.lock(
            [&](map_t& m) -> async_handle
            {
                if (auto* const hit = m.get_ptr(key))
                    return *hit;

                // The frame runs later, possibly on a worker: deep-copy the key.
                // Reach the context + callback through the stable members (they outlive every build; see the threading note).
                auto node = cc::make_async_scheduled<handle>(
                    [ctx = _ctx, build = &_build, key](cc::async_context<handle>& actx) -> cc::async_step_status
                    {
                        auto result = (*build)(*ctx, key);
                        if (result.has_error())
                            return actx.error(cc::move(result).error());
                        return actx.success(cc::move(result).value());
                    });
                m[key] = node;
                return node;
            });
    }

    /// Warm the cache: start the build for `key` and keep the node (fire-and-forget).
    void prepare(Key const& key) const { (void)acquire_async(key); }

    /// Blocking get-or-build, fallible: the built handle or the build error.
    /// This is the form for inside a rendering scope, where an exception must not unwind out past an open command list.
    [[nodiscard]] cc::result<handle> try_acquire(Key const& key) const
    {
        auto node = acquire_async(key);
        auto result = cc::try_async_blocking_get(node);
        if (result.has_error())
            return cc::error(cc::move(result.error().underlying()));
        return cc::move(result.value());
    }

    /// Blocking get-or-build.
    /// Returns the handle; throws on build failure (matching sg's create_*).
    [[nodiscard]] handle acquire(Key const& key) const { return try_acquire(key).or_throw(); }

private:
    using map_t = cc::map<Key, async_handle>;

    sg::context* _ctx = nullptr;
    build_fn _build;
    // Mutable: acquiring is logically a read — the get-or-create is an internal detail behind the mutex.
    mutable cc::mutex<map_t> _cache;
};

namespace sr
{
/// A `keyed_pipeline_cache<Key>` build callback in one line: acquire `desc` through ctx.cached and block on the build.
///
/// The two caches stack on purpose — the keyed cache maps the routine's key to a pipeline and owns reload invalidation, while identity and the build itself belong to ctx.cached.
/// So two routines drawing the same shaders into the same target format share one PSO.
/// Blocking here happens inside the keyed cache's own async frame, where a blocking_get participates in the graph rather than idling.
[[nodiscard]] cc::result<sg::raster_pipeline_handle> build_cached_raster_pipeline(
    sg::context& ctx,
    sg::raster_pipeline_description const& desc);
} // namespace sr
