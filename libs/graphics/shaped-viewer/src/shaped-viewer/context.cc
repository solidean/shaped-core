#include <shaped-viewer/context.hh>

#if SV_HAS_DEFAULT_BACKEND
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#endif

namespace sv
{
cc::result<sg::context_handle> impl::acquire_default_context()
{
#if SV_HAS_DEFAULT_BACKEND
    auto hardware = sg::create_dx12_context({});
    if (!hardware.has_error())
        return hardware.value();

    // WARP keeps the viewer usable on a machine with no suitable GPU, which is also what every headless test does.
    auto warp = sg::create_dx12_context({.use_warp = true});
    if (!warp.has_error())
        return warp.value();

    return cc::error("shaped-viewer: could not create a Direct3D 12 context (no adapter, and WARP was refused)");
#else
    return cc::error("shaped-viewer: built with no default graphics backend — set sv::acquire_context to supply one");
#endif
}

cc::result<sg::context_handle> acquire_viewer_context()
{
    // One context for the process: whoever answers is asked once, and every later viewer gets that same handle.
    // So running viewers in succession costs one device rather than one per viewer, and two live viewers share
    // resources rather than duplicating every upload.
    //
    // The memoization lives here rather than inside a provider, so a caller writing one needs no static of their own.
    static sg::context_handle cached;
    if (cached != nullptr)
        return cached;

    auto r = acquire_context ? acquire_context() : impl::acquire_default_context();

    // A failure is deliberately not cached: it leaves a caller free to set `acquire_context` and try again, which is
    // exactly what someone who hit the no-backend error is about to do.
    if (r.has_error())
        return r;
    if (r.value() == nullptr)
        return cc::error("shaped-viewer: the context provider returned no context");

    cached = r.value();
    return cached;
}
} // namespace sv
