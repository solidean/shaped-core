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

/// How a session gets a blend mode: the mode it ends up with, and whether the cameras are a layer of our own under the
/// frame, which is how `XR_FB_passthrough` works, rather than the runtime's own blending.
struct blend_choice
{
    blend_mode shown = blend_mode::opaque;
    bool uses_camera_layer = false;
};

/// What asking for `wanted` gets, given the modes the runtime composes itself and whether a camera layer is available.
[[nodiscard]] blend_choice choose_blend(blend_mode wanted, cc::span<blend_mode const> runtime_modes, bool has_camera_layer);

/// `c` as a log reads it.
[[nodiscard]] char const* name_of(capability c);

/// The extensions `c` needs enabled, its dependencies included, as terminated strings that live forever.
[[nodiscard]] cc::span<char const* const> required_extensions(capability c);

/// Every extension of `capabilities`, each once.
[[nodiscard]] cc::vector<char const*> extensions_of(cc::span<capability const> capabilities);

/// Whether a capability whose extensions are all enabled is one this system really has.
[[nodiscard]] bool is_supported(capability c, system_support const& support);
} // namespace sxr::impl
