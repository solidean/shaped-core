#pragma once

#include <nexus/test.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

// Shared setup for the metal tier-2 suite (shaped-graphics-metal-test).
//
// Metal has no software device — there is no WARP here — so a host below the backend's floor cannot create a context at
// all, and a test that finds none SKIPs rather than passing.
// A suite that silently passes when it found no device gets more dangerous the more it covers.

namespace sg::backend::metal::test
{
/// A fresh context, or nullptr where this host is below the floor (no metal device, pre-26 OS, or a pre-Metal-4 GPU).
[[nodiscard]] inline metal_context_handle make_context(metal_config const& config = {})
{
    auto ctx = sg::create_metal_context(config);
    if (ctx.has_error())
        return nullptr;

    return std::static_pointer_cast<metal_context>(ctx.value());
}
} // namespace sg::backend::metal::test
