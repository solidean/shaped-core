#pragma once

#include <shaped-graphics/backends/dx12/dx12_context.hh>

// Shared helper for the dx12 backend test binary (shaped-graphics-dx12-test). The tests are split
// per topic across several .cc files; this is the one piece they all reuse. Header-only.

namespace sg::backend::dx12
{
/// Fresh WARP (software adapter) context for a test, or nullptr on the rare host without WARP.
/// WARP is present on any Windows host, so tests built on it also run on headless CI.
inline sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace sg::backend::dx12
