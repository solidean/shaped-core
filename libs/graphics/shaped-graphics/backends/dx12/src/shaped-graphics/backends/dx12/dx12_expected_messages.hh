#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>

// The debug-layer advisories sg provokes on purpose, and the predicate over them.
//
// A public header rather than a test one because there are two listeners, in two binaries, and they must agree.
// The tier-2 suite installs its own from dx12-test-common.hh; the tier-1 entry driver in
// libs/graphics/shaped-graphics/tests/backends/dx12-entry.cc installs another over a context it only holds as an
// sg::context_handle, and that binary cannot include a header out of another target's tests/ folder.
// Two copies is what the tier-1 one being written without this list cost.
// The first tier-1 test to clear a render target failed on both adapters, against a message the tier-2 list had
// already understood.

namespace sg::backend::dx12
{
/// Debug-layer advisories sg provokes on purpose, matched as substrings.
/// Each entry is a decision, not a mute button: the message is understood, and the alternative is worse or does not exist yet.
/// Anything not listed fails the test, so a NEW warning is still loud.
inline constexpr cc::string_view k_expected_validation_messages[] = {
    // A perf advisory: D3D12 wants an optimized clear value at texture creation, matching what the target is later cleared to.
    // sg::texture_description has no such field, and inventing one that disagrees with the actual clear is worse than none —
    // a mismatch is its own, louder message.
    // See libs/graphics/shaped-graphics/docs/TODO.md for the optional field that would let a caller opt in.
    "did not pass any clear value to resource creation",

    // A command list carrying only barriers is a legitimate sg shape — a list opened purely to transition resources.
    "recorded only Barrier commands",
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
