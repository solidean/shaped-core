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
/// Threading: the callback runs under this cache's mutex, on whichever thread acquired, and only starts the build.
/// Whatever it returns does the work later, so the callback must only read immutable captures.
/// Only the key -> node map is behind that mutex.
/// The context and callback are plain members set by `init`, so `init` must not race in-flight builds — call it at (re)load points, which are serialized with rendering.
/// With no pool installed, builds are driven inline on the calling thread.
template <class Key, class Pipeline = sg::raster_pipeline>
class sr::keyed_pipeline_cache
{
public:
    using handle = std::shared_ptr<Pipeline const>;
    using async_handle = cc::shared_async<handle>;
    using build_fn = cc::unique_function<async_handle(sg::context&, Key const&)>;

    /// Store the context + build callback and CLEAR the cache.
    /// Call from a routine's init_declare on every (re)load: a rebuilt pipeline layout invalidates every pipeline cached against the old one.
    /// Re-`init` both drops them and rebinds the fresh callback.
    ///
    /// The callback returns the async, it does not wait for one.
    /// A build that is already a node (ctx.cached.acquire_raster_pipeline) is handed straight over; one that is not is
    /// cc::make_async_from_value / cc::make_async_from_error away.
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

                // The build's own node IS the entry — this cache adds the key -> node mapping and nothing else.
                // Wrapping it in a frame of our own is what would have to block, and blocking a pool worker on another
                // node parks the very workers that node needs.
                auto node = _build(*_ctx, key);
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
