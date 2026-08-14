#include <shaped-viewer/view/view_data.hh>

namespace sv
{
layer const* primary_scene_3d(view_data const& v)
{
    for (auto const& l : v.layers)
        if (l.kind == layer_kind::scene_3d)
            return &l;
    return nullptr;
}

layer& ensure_scene_3d(view_data& v)
{
    for (auto& l : v.layers)
        if (l.kind == layer_kind::scene_3d)
            return l;

    // A traced layer writes no meaningful alpha, so it composites by overwriting whatever sits below it.
    v.layers.push_back({.kind = layer_kind::scene_3d, .blend = layer_blend::replace});
    return v.layers.back();
}
} // namespace sv
