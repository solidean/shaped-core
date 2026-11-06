#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-xr/xr.hh>

namespace sxr::impl
{
/// What the system answers beyond listing an extension, each false where its extension was not enabled.
struct system_support
{
    bool spatial_entity = false;
    bool space_discovery = false;
    bool space_persistence = false;
    bool environment_depth = false;
    bool hand_removal = false;
    bool environment_raycast = false;
};

/// `c` as a log reads it.
[[nodiscard]] char const* name_of(capability c);

/// The extensions `c` needs enabled, its dependencies included, as terminated strings that live forever.
[[nodiscard]] cc::span<char const* const> required_extensions(capability c);

/// Every extension of `capabilities`, each once.
[[nodiscard]] cc::vector<char const*> extensions_of(cc::span<capability const> capabilities);

/// Whether a capability whose extensions are all enabled is one this system really has.
[[nodiscard]] bool is_supported(capability c, system_support const& support);
} // namespace sxr::impl
