#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

// The debug-layer advisories sg provokes on purpose, and the predicate over them.
//
// A public header rather than a test one because every binary that runs sg on dx12 must agree on it.
// Each test driver allows these in all of its tests, and a copy per binary would drift: the first tier-1 test to clear a render target once failed on a message the tier-2 list had already understood.

namespace sg::backend::dx12
{
/// Debug-layer advisories sg provokes on purpose, matched as substrings.
/// Each entry is a decision, not a mute button: the message is understood, and the alternative is worse or does not exist yet.
/// Anything not listed fails the test that provoked it, so a NEW warning is still loud.
inline constexpr cc::string_view k_expected_validation_messages[] = {
    // A perf advisory: D3D12 wants an optimized clear value at texture creation, matching what the target is later cleared to.
    // sg::texture_description has no such field, and inventing one that disagrees with the actual clear is worse than none —
    // a mismatch is its own, louder message.
    // See libs/graphics/shaped-graphics/docs/TODO.md for the optional field that would let a caller opt in.
    "did not pass any clear value to resource creation",

    // A command list carrying only barriers is a legitimate sg shape — a list opened purely to transition resources.
    "recorded only Barrier commands",

    // The transient bump heap reuses its placed storage the moment an epoch advances, while the last epoch's placed resources
    // still await their deferred release.
    // The debug layer then finds two resources over one address range and cannot tell which one a view or a build means.
    // The queue serializes their GPU use, so this is ambiguity rather than aliasing; see dx12_context::advance_epoch.
    // It shows only on a frame loop that does not wait for the GPU between epochs, which is what sv::viewer runs.
    "resources contain the GPU Virtual Address range",
};

/// Whether this debug-layer message is one of the advisories above.
[[nodiscard]] inline bool is_expected_validation_message(cc::string_view message)
{
    for (auto const expected : k_expected_validation_messages)
        if (message.contains(expected))
            return true;
    return false;
}
} // namespace sg::backend::dx12
