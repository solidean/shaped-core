#pragma once

#include <shaped-xr/xr.hh>

namespace sxr::impl
{
/// Whether OpenXR takes `name` as an action's or an action set's: lowercase letters, digits, `-`, `_` and `.`, not empty.
[[nodiscard]] bool is_valid_action_name(cc::string_view name);

/// The first thing OpenXR would refuse about `actions`, or empty: a bad or repeated name, or a per-hand binding that
/// names no hand, or a vibration bound to an input.
[[nodiscard]] cc::string problem_of(cc::span<action_description const> actions);

/// The bindings of one interaction profile, each the index of its action and its path.
struct profile_bindings
{
    cc::string profile;
    cc::vector<isize> actions;
    cc::vector<cc::string> paths;
};

/// The bindings grouped by profile, in the order the profiles first appear, since OpenXR suggests them a profile at a time.
[[nodiscard]] cc::vector<profile_bindings> bindings_by_profile(cc::span<action_description const> actions);
} // namespace sxr::impl
