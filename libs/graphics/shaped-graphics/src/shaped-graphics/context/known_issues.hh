#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

/// A driver bug that `docs/bugs-external/<id>` reproduces with nothing of ours, and the contexts it affects.
///
/// A test the bug trips over skips by it: `nx::skip_known_issue` under `known_issue_affecting`, so every such skip
/// reads alike and `dev.py test --known-issues` can still try them all.
/// `affects` is keyed to what was measured: a driver past the last version seen affected runs the test again.
struct sg::known_issue
{
    /// The `docs/bugs-external` directory.
    cc::string_view id;
    /// What goes wrong, in one line.
    cc::string_view symptom;
    bool (*affects)(context const& ctx) = nullptr;
};

namespace sg
{
/// Every driver bug sg knows how to recognize.
[[nodiscard]] cc::span<known_issue const> known_issues();

/// The known issue `id`, when it affects `ctx`; nullptr when it does not.
/// `id` must name an entry of known_issues().
[[nodiscard]] known_issue const* known_issue_affecting(context const& ctx, cc::string_view id);
} // namespace sg
