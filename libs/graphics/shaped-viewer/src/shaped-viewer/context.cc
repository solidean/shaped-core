#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/context.hh>
#include <shaped-viewer/shader_library.hh>

#if SV_DEFAULT_BACKEND_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#elif SV_DEFAULT_BACKEND_METAL
#include <shaped-graphics/backends/metal/metal_context.hh> // sg::create_metal_context
#elif SV_DEFAULT_BACKEND_WEBGPU
#include <shaped-graphics/backends/webgpu/webgpu_context.hh> // sg::request_webgpu_context
#endif

namespace sv
{
namespace
{
/// The caller's provider, unset until `set_acquire_context` is called.
///
/// It lives here rather than in the header so the only way to reach it is the setter — nothing can read it, and no
/// translation unit can race the others to initialize it.
context_provider g_acquire_context;

/// The one context viewers run on, null until a provider, the default or the page's request answered.
///
/// One cell for the synchronous and the awaited acquisition alike, so whichever answered first is what the other hands back.
/// Under a lock for the same reason `acquire_shader_library` is: two threads racing would each build a device,
/// which is the one outcome the memoization exists to prevent.
cc::mutex<sg::context_handle>& viewer_context()
{
    static auto cached = cc::mutex<sg::context_handle>(nullptr);
    return cached;
}
} // namespace

void set_acquire_context(context_provider provider)
{
    g_acquire_context = cc::move(provider);
}

cc::result<sg::context_handle> impl::acquire_default_context()
{
#if SV_DEFAULT_BACKEND_DX12
    // WARP keeps the viewer usable on a machine with no suitable GPU.
    auto ctx = sg::create_dx12_context({.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp});
    if (ctx.has_error())
        return cc::error("shaped-viewer: could not create a Direct3D 12 context (no adapter, and WARP was refused)");
    return ctx.value();
#elif SV_DEFAULT_BACKEND_METAL
    // Metal has no software device, so a host below the Metal 4 floor has no default at all.
    auto ctx = sg::create_metal_context();
    if (ctx.has_error())
        return cc::error("shaped-viewer: could not create a Metal context (no Metal 4 device)");
    return ctx.value();
#elif SV_DEFAULT_BACKEND_WEBGPU
    return cc::error("shaped-viewer: a WebGPU device is requested, never created on the spot — acquire it with "
                     "sv::acquire_viewer_context_async, or loop with sv::interactive_async, which does");
#else
    return cc::error("shaped-viewer: built with no default graphics backend — call sv::set_acquire_context to supply "
                     "one");
#endif
}

cc::result<sg::context_handle> acquire_viewer_context()
{
    // One context for the process: whoever answers is asked once, and every later viewer gets that same handle.
    // So running viewers in succession costs one device rather than one per viewer, and two live viewers share
    // resources rather than duplicating every upload.
    //
    // The memoization lives here rather than inside a provider, so a caller writing one needs no static of their own.
    return viewer_context().lock(
        [](sg::context_handle& ctx) -> cc::result<sg::context_handle>
        {
            if (ctx != nullptr)
                return ctx;

            auto r = g_acquire_context ? g_acquire_context() : impl::acquire_default_context();

            // A failure is deliberately not cached: it leaves a caller free to call `set_acquire_context` and try again,
            // which is exactly what someone who hit the no-backend error is about to do.
            if (r.has_error())
                return r;
            if (r.value() == nullptr)
                return cc::error("shaped-viewer: the context provider returned no context");

            ctx = r.value();
            return ctx;
        });
}

cc::shared_async<cc::result<sg::context_handle>> acquire_viewer_context_async()
{
#if SV_DEFAULT_BACKEND_WEBGPU
    if (!g_acquire_context)
    {
        auto const cached = viewer_context().lock([](sg::context_handle const& ctx) { return ctx; });
        if (cached != nullptr)
            co_return cached;

        auto const requested = co_await cc::async_as_result(sg::request_webgpu_context());
        if (requested.has_error())
            co_return cc::result<sg::context_handle>(cc::error(cc::any_error(
                cc::format("shaped-viewer: no WebGPU device: {}", requested.error().underlying().to_string()))));

        // Two loops awaiting their first frame at once both request one; the first to answer is the one kept.
        co_return viewer_context().lock(
            [&](sg::context_handle& ctx)
            {
                if (ctx == nullptr)
                    ctx = requested.value();
                return ctx;
            });
    }
#endif
    co_return acquire_viewer_context();
}

cc::shared_async<cc::unit> background_work(sg::context& ctx)
{
    // Upstream first, which is the order settled() needs: pipeline builds and compiles queue their cache stores before they resolve.
    auto backlogs = cc::vector<cc::async_backlog const*>();
    backlogs.push_back(&ctx.backlog);
    if (auto const library = acquire_shader_library(); library.has_value())
        backlogs.push_back(&library.value()->backlog());
    backlogs.push_back(&bcache::default_cache().backlog());
    return cc::async_backlog::settled(backlogs);
}
} // namespace sv
