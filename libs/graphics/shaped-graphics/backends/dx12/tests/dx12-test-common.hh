#pragma once

#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_expected_messages.hh>

// Shared helper for the dx12 backend test binary (shaped-graphics-dx12-test).
// The tests are split per topic across several .cc files; this is the one piece they all reuse.
// Header-only.
//
// Contexts are handed around as dx12_context_handle rather than sg::context_handle, so a test that inspects backend guts needs no downcast.
// Driving still goes through the abstract API — see libs/graphics/shaped-graphics/docs/testing.md.
//
// Most tests are INVOCABLE_TESTs taking the context the entry driver (dx12-entry.cc) built — one per adapter, for the whole run.
// scoped_transfer_sizes is for those that shrink its transfer staging for a while.
// The other helpers are for the few that need a context of their own: pristine state, a creation-time knob, creation or teardown, or more than one context.

namespace sg::backend::dx12
{
struct transfer_sizes;
class scoped_transfer_sizes;

/// The backend-typed view of a freshly created context.
/// Passes an error through untouched, so a caller can SKIP.
inline cc::result<dx12_context_handle> as_test_context(cc::result<sg::context_handle> ctx)
{
    if (ctx.has_error())
    {
        // The reason, not just the fact: a caller's `REQUIRE(ctx.has_value())` reports neither the HRESULT nor which step failed, and creation is the step that breaks under contention.
        cc::eprintln("[dx12-test] context creation failed: {}", ctx.error().to_string());
        return cc::error(cc::move(ctx).error());
    }

    return std::static_pointer_cast<dx12_context>(ctx.value());
}

/// A context for a test to own: the hardware adapter or WARP where there is none, debug layer on, so a validation message fails the test through the log rule.
/// `config` supplies the backend knobs the test is about — the adapter and the debug layer are set here regardless.
/// Only for a test whose subject is the context itself: pristine epoch/pool state, a knob, creation or teardown.
/// Errors on the rare host with no adapter at all, so a caller can SKIP.
inline cc::result<dx12_context_handle> make_test_context(dx12_config config = {})
{
    config.adapter = sg::backend::dx12::dx12_adapter::hardware_or_warp;
    config.activate_global_debug_layer = true;
    return as_test_context(sg::create_dx12_context(config));
}

/// make_test_context as a bare handle, for a test that needs a fresh context and no knobs.
/// nullptr when it could not be created; as_test_context has already said why.
inline dx12_context_handle make_fresh_context()
{
    auto ctx = make_test_context();
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace sg::backend::dx12

/// The transfer staging sizes a test sets on a context for a while; 0 leaves that size as it is.
struct sg::backend::dx12::transfer_sizes
{
    isize upload_ring_bytes = 0;
    isize download_ring_bytes = 0;
    isize async_upload_window_bytes = 0;
    isize async_download_window_bytes = 0;
};

/// Sets `sizes` on a context, and restores each size it set to `dx12_config{}`'s when it goes out of scope, however the test ends.
/// So the context must have been created with the default sizes, as the entry drivers' shared ones are.
/// Both ends advance the epoch when a ring budget changes, since that is where a pending budget applies.
/// A window size applies before the next transfer is staged, so it needs no advance.
class sg::backend::dx12::scoped_transfer_sizes
{
public:
    scoped_transfer_sizes(sg::context& ctx, transfer_sizes const& sizes) : _ctx(ctx), _sizes(sizes) { apply(sizes); }

    ~scoped_transfer_sizes()
    {
        auto const defaults = dx12_config{};
        apply({.upload_ring_bytes = _sizes.upload_ring_bytes > 0 ? defaults.upload_ring_bytes : 0,
               .download_ring_bytes = _sizes.download_ring_bytes > 0 ? defaults.download_ring_bytes : 0,
               .async_upload_window_bytes = _sizes.async_upload_window_bytes > 0 ? defaults.async_upload_window_bytes : 0,
               .async_download_window_bytes
               = _sizes.async_download_window_bytes > 0 ? defaults.async_download_window_bytes : 0});
    }

    scoped_transfer_sizes(scoped_transfer_sizes const&) = delete;
    scoped_transfer_sizes(scoped_transfer_sizes&&) = delete;
    scoped_transfer_sizes& operator=(scoped_transfer_sizes const&) = delete;
    scoped_transfer_sizes& operator=(scoped_transfer_sizes&&) = delete;

private:
    void apply(transfer_sizes const& sizes)
    {
        if (sizes.async_upload_window_bytes > 0)
            _ctx.upload.set_async_window_size(sizes.async_upload_window_bytes);
        if (sizes.async_download_window_bytes > 0)
            _ctx.download.set_async_window_size(sizes.async_download_window_bytes);
        if (sizes.upload_ring_bytes > 0)
            _ctx.upload.set_inline_budget(sizes.upload_ring_bytes);
        if (sizes.download_ring_bytes > 0)
            _ctx.download.set_budget(sizes.download_ring_bytes);
        if (sizes.upload_ring_bytes > 0 || sizes.download_ring_bytes > 0)
            _ctx.advance_epoch();
    }

    sg::context& _ctx;
    transfer_sizes _sizes;
};

namespace sg::backend::dx12
{
/// Requires the async upload's staging window to be `bytes`, which says a `scoped_transfer_sizes` took effect.
/// Call once an upload staged under it has landed: the copy actor adopts a size before it stages, and only then writes the field.
inline void require_async_upload_window(dx12_context const& c, isize bytes)
{
    REQUIRE(c._upload_async._window_bytes == bytes);
}

/// The same for the async download's staging window.
inline void require_async_download_window(dx12_context const& c, isize bytes)
{
    REQUIRE(c._download_async._window_bytes == bytes);
}
} // namespace sg::backend::dx12
