#pragma once

#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Shared helper for the dx12 backend test binary (shaped-graphics-dx12-test). The tests are split
// per topic across several .cc files; this is the one piece they all reuse. Header-only.

namespace sg::backend::dx12
{
/// The shared WARP (software adapter) context for the whole dx12 test binary: created once on first use
/// and reused by every test, so we don't pay device creation per test. WARP is present on any Windows host,
/// so tests built on it also run headless on CI. Returns nullptr on the rare host without WARP.
///
/// The default for tests that just need *a* live context. State persists across tests (epochs advance,
/// pools fill, the transient heap keeps its budget) — a test that asserts pristine state (the epoch
/// counter, allocator/list pool counts) must take a fresh context via make_warp_context() instead. Tests
/// run serially, so the single shared context is never touched concurrently.
inline sg::context_handle acquire_warp_context()
{
    // Function-local static in an inline function: one instance across all test TUs. Shut down
    // automatically when the handle is released at program exit.
    static sg::context_handle const ctx = []() -> sg::context_handle
    {
        auto c = sg::create_dx12_context({.use_warp = true});
        return c.has_value() ? c.value() : nullptr;
    }();
    return ctx;
}

/// A fresh, unshared WARP context — for the few tests that need pristine epoch/pool state. Most tests
/// should use acquire_warp_context(). nullptr on the rare host without WARP.
inline sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace sg::backend::dx12
