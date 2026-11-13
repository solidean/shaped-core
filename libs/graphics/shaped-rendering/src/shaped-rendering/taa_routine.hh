#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

/// One frame of temporal antialiasing: what `sr::taa_routine` reads and writes.
///
/// Every texture is the same size; `current` must be drawn at a sub-pixel jitter that changes from frame to frame, or
/// the history has nothing new to gather.
struct sr::taa_desc
{
    /// This frame, drawn at the jitter: any float format, loaded rather than filtered.
    sg::texture_2d current;

    /// In pixels, where each pixel's surface is now minus where it was last frame, both without the jitter; two channels.
    sg::texture_2d motion;

    /// A depth that is smaller nearer the camera, one channel: which pixel of a neighbourhood lends it its motion.
    sg::texture_2d depth;

    /// Last frame's `history_write`, rgba16_float; bound even where `history_valid` is false, when it is not read.
    sg::texture_2d history_read;
    bool history_valid = false;

    /// Where this frame's history goes, rgba16_float; must not be `history_read`.
    sg::texture_2d history_write;

    /// Where the resolved image goes, in the format the routine instance is for; its alpha is `current`'s.
    sg::texture_2d output;

    /// The share of the history a pixel keeps each frame, from 0 for none to below 1.
    f32 feedback = 0.9f;

    /// How far the history may stray from the neighbourhood's mean color, in its standard deviations; smaller ghosts
    /// less and flickers more.
    f32 clip_width = 1.25f;
};

/// Temporal antialiasing's resolve: `taa_desc::current` blended into the reprojected history, clipped to the colors its
/// own neighbourhood spans, written to `output` and to the next frame's history in one pass.
///
/// **Parametrized on the output's pixel format**, which a pipeline bakes in, as `sr::blit_routine` is.
/// The design and its sources are in shaders/taa.sgl.
class sr::taa_routine : public sg::render_routine<taa_routine, sg::pixel_format>
{
public:
    /// Records the resolve into `cmd`, or declines while its pipeline builds.
    ///
    /// Must be called with no rendering scope open: it opens its own over `d.output` and `d.history_write`.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, taa_desc const& d);

    /// Whether `execute` would run for an output of `format` now, rather than decline while its pipeline builds.
    /// A caller drawing its frame somewhere else for the resolve asks first, so a declined resolve never loses a frame.
    [[nodiscard]] static bool is_ready(sg::context& ctx, sg::pixel_format format);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_raster_pipeline _pipeline;
};
