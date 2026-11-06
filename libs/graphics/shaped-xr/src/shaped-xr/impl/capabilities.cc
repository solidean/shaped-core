#include <clean-core/string/string_view.hh>
#include <shaped-xr/impl/capabilities.hh>

namespace
{
constexpr char const* passthrough_extensions[] = {"XR_FB_passthrough"};
constexpr char const* depth_extensions[] = {"XR_META_environment_depth"};
constexpr char const* plane_extensions[] = {"XR_FB_spatial_entity", "XR_META_spatial_entity_discovery", "XR_FB_scene"};
constexpr char const* mesh_extensions[]
    = {"XR_FB_spatial_entity", "XR_META_spatial_entity_discovery", "XR_FB_scene", "XR_META_spatial_entity_mesh"};
constexpr char const* anchor_extensions[] = {"XR_FB_spatial_entity"};
constexpr char const* persistent_anchor_extensions[]
    = {"XR_FB_spatial_entity", "XR_META_spatial_entity_discovery", "XR_META_spatial_entity_persistence"};
constexpr char const* hit_test_extensions[] = {"XR_EXT_future", "XR_META_environment_raycast"};
} // namespace

sxr::impl::blend_choice sxr::impl::choose_blend(blend_mode wanted,
                                                cc::span<blend_mode const> runtime_modes,
                                                bool has_camera_layer)
{
    if (wanted == blend_mode::opaque)
        return {};
    // the layer goes first: it is what ran over Quest Link, whatever the runtime lists beside it
    if (wanted == blend_mode::alpha_blend && has_camera_layer)
        return {.shown = blend_mode::alpha_blend, .uses_camera_layer = true};
    for (auto const m : runtime_modes)
        if (m == wanted)
            return {.shown = wanted};
    return {};
}

char const* sxr::impl::name_of(capability c)
{
    switch (c)
    {
    case capability::passthrough:
        return "passthrough";
    case capability::environment_depth:
        return "environment depth";
    case capability::scene_planes:
        return "scene planes";
    case capability::scene_meshes:
        return "scene meshes";
    case capability::anchors:
        return "anchors";
    case capability::persistent_anchors:
        return "persistent anchors";
    case capability::hit_tests:
        return "hit tests";
    }
    return "unknown";
}

cc::span<char const* const> sxr::impl::required_extensions(capability c)
{
    switch (c)
    {
    case capability::passthrough:
        return passthrough_extensions;
    case capability::environment_depth:
        return depth_extensions;
    case capability::scene_planes:
        return plane_extensions;
    case capability::scene_meshes:
        return mesh_extensions;
    case capability::anchors:
        return anchor_extensions;
    case capability::persistent_anchors:
        return persistent_anchor_extensions;
    case capability::hit_tests:
        return hit_test_extensions;
    }
    return {};
}

cc::vector<char const*> sxr::impl::extensions_of(cc::span<capability const> capabilities)
{
    auto extensions = cc::vector<char const*>();
    for (auto const c : capabilities)
        for (auto const* name : required_extensions(c))
        {
            auto is_new = true;
            for (auto const* seen : extensions)
                is_new = is_new && cc::string_view(seen) != name;
            if (is_new)
                extensions.push_back(name);
        }
    return extensions;
}

bool sxr::impl::is_supported(capability c, system_support const& support)
{
    switch (c)
    {
    case capability::passthrough:
        return true; // the listing is all XR_FB_passthrough has to say
    case capability::environment_depth:
        return support.environment_depth;
    case capability::scene_planes:
    case capability::scene_meshes:
        return support.spatial_entity && support.space_discovery;
    case capability::anchors:
        return support.spatial_entity;
    case capability::persistent_anchors:
        return support.spatial_entity && support.space_discovery && support.space_persistence;
    case capability::hit_tests:
        return support.environment_raycast;
    }
    return false;
}
