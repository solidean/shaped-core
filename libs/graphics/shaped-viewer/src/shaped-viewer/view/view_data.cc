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

bool is_traceable(layer const& l)
{
    if (l.kind != layer_kind::scene_3d)
        return false;

    for (auto const& item : l.items)
        if (item.kind == scene_item_kind::triangle_mesh)
            return true;
    return false;
}

cc::vector<temporal_input> temporal_inputs_of(view_data const& v)
{
    auto out = v.temporal_inputs;

    for (auto i = isize(0); i < v.layers.size(); ++i)
    {
        if (!is_traceable(v.layers[i]))
            continue;

        // rgba16_float and the view's own resolution: the estimator's running mean needs the range, and an
        // accumulator sized to anything but the image it accumulates would have nothing to blend into.
        out.push_back({.id = temporal_id::accumulation(u8(i)), .format = sg::pixel_format::rgba16_float});

        // Half floats carry ~3 decimal digits at any magnitude, and every disocclusion test on `hit_t` is relative,
        // so the depth here is as precise as the comparison needs at any scene scale.
        out.push_back({.id = temporal_id::gbuffer(u8(i)), .format = sg::pixel_format::rgba16_float});
    }

    return out;
}
} // namespace sv
