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
// The helpers here are for the few that need a context of their own: pristine epoch/pool state, a backend knob the test is about, or more than one context.

namespace sg::backend::dx12
{
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
