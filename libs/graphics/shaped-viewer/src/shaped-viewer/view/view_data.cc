#include <clean-core/common/hash.hh>
#include <shaped-viewer/view/view_data.hh>

namespace sv
{
namespace
{
/// What a layer's denoised image is a function of beside its mean and guides: every setting the denoiser or its schedule reads.
/// Not `fresh_samples`, which each call sets for itself, nor `frame_time_ms`, which no member reads yet and a viewer would set every frame.
[[nodiscard]] u64 denoised_reset_hash(render_settings const& s)
{
    auto const& r = s.reconstruct;
    return cc::make_hash(r.denoiser, r.upscaler, r.scale, r.quality, r.sharpness, r.temporal_responsiveness,
                         r.noisy_guides, r.exposure, r.upscale_sharpness, s.temporal_denoise_frames,
                         s.temporal_denoise_fade_frames);
}
} // namespace

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

        // What a raster pass after the trace is occluded by, and what a double-click reads the orbit centre off.
        // The second is why every traced layer keeps one, drawings or not.
        out.push_back({.id = temporal_id::primary_depth(u8(i)), .format = sg::pixel_format::r32_float});

        // A rasterized layer writes its whole G-buffer every frame, denoised or not: it is what a lighting pass after it reads.
        // The slots are the guides', in the guides' formats, so a denoiser or a hybrid pass reads either path's alike.
        // It never denoises, since every frame it draws is final.
        if (v.layers[i].settings.mode == render_mode::raster)
        {
            for (auto const& s : impl::raster_slots)
                out.push_back({.id = s.id(u8(i)), .format = s.format});
            continue;
        }

        if (v.layers[i].settings.reconstruct.denoiser == sr::denoise_method::none)
            continue;

        // The guides blend like the accumulator but never need its precision: they converge in a few frames, and a half
        // float's normal or albedo is far finer than the edge-stop that reads it.
        // Depth stays a full float, since the denoiser compares depths relative to their own size.
        // The denoised image is only ever presented, so half is enough there too.
        out.push_back({.id = temporal_id::normal_guide(u8(i)), .format = sg::pixel_format::rgba16_float});
        out.push_back({.id = temporal_id::depth_guide(u8(i)), .format = sg::pixel_format::r32_float});
        out.push_back({.id = temporal_id::albedo_guide(u8(i)), .format = sg::pixel_format::rgba16_float});
        // The denoised image restarts on every setting that decides it, since a layer at its target traces nothing and
        // denoises again only once nothing has denoised its mean.
        out.push_back({.id = temporal_id::denoised(u8(i)),
                       .format = sg::pixel_format::rgba16_float,
                       .reset_hash = denoised_reset_hash(v.layers[i].settings)});

        // The specular pair, for every member: a split one reads it beside the diffuse albedo, and an unsplit one
        // demodulates by the sum of the two, since a metal's diffuse albedo is zero.
        out.push_back({.id = temporal_id::specular_albedo_guide(u8(i)), .format = sg::pixel_format::rgba16_float});
        out.push_back({.id = temporal_id::roughness_guide(u8(i)), .format = sg::pixel_format::r16_float});

        // A layer that may denoise temporally also keeps this frame's own samples and the motion vectors.
        // `automatic` may, because it picks a temporal member while the mean is young whenever one is supported.
        auto const method = v.layers[i].settings.reconstruct.denoiser;
        if (method == sr::denoise_method::automatic || sr::is_temporal(method))
        {
            out.push_back({.id = temporal_id::frame_samples(u8(i)), .format = sg::pixel_format::rgba16_float});
            out.push_back({.id = temporal_id::motion_guide(u8(i)), .format = sg::pixel_format::rg32_float});

            // Where the spatial member lands while the hand-off between the two is crossfading.
            // Declared for the whole life of the layer rather than for the frames the fade spans: a declaration that
            // came and went would allocate a texture mid-fade, and the first frame of a fade is the one that must not
            // be a step.
            if (v.layers[i].settings.temporal_denoise_fade_frames > 0)
                out.push_back({.id = temporal_id::denoised_crossfade(u8(i)), .format = sg::pixel_format::rgba16_float});

            // The split signal, for a method that filters the two lobes apart.
            // `automatic` counts for the reason it counts above: what it resolves to depends on the device, and this
            // declaration is made before any device is consulted.
            auto const may_read_split = method == sr::denoise_method::automatic
                                     || (sr::required_guides(method) | sr::optional_guides(method))
                                            .has(sr::reconstruct_guide::split_diffuse_specular);
            if (may_read_split)
            {
                out.push_back({.id = temporal_id::frame_diffuse(u8(i)), .format = sg::pixel_format::rgba16_float});
                out.push_back({.id = temporal_id::frame_specular(u8(i)), .format = sg::pixel_format::rgba16_float});

                // Full floats: a hit distance is a world-space length rather than a colour, and a half loses metres
                // of it at the far end of a scene the denoiser still reprojects across.
                out.push_back({.id = temporal_id::hit_distance_guide(u8(i)), .format = sg::pixel_format::rg32_float});
            }
        }
    }

    return out;
}
} // namespace sv
