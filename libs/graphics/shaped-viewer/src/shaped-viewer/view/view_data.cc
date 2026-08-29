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

        // The view's own resolution: an accumulator sized to anything but the image it accumulates would have
        // nothing to blend into.
        //
        // rgba32_float rather than half, because the accumulation is uncapped.
        // The raygen weights a frame by 1 / (n + 1) and half floats carry ~3 decimal digits, so the mean would stop
        // moving a couple of thousand frames in — right where an uncapped estimate is still converging.
        out.push_back({.id = temporal_id::accumulation(u8(i)), .format = sg::pixel_format::rgba32_float});
    }

    return out;
}
} // namespace sv
