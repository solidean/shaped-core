#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

/// One image faded into another, in place: `destination = mix(destination, source, weight)`.
///
/// In place because that is what a crossfade actually needs.
/// Two images blended into a third costs a third full-size texture for the frames a fade lasts.
/// The two inputs then have to be kept apart from the output for the barrier tracker.
/// Reading and writing the destination as an image per pixel needs neither.
/// The cost is that `destination` is consumed — a caller who needs it afterwards writes it somewhere else first.
///
/// It reads nothing but the two images, so it has no idea what it is fading.
/// A caller crossfading two denoisers is doing exactly what one crossfading two exposures is.
///
/// A render routine **parametrized on the destination's format**, which its shader names.
/// `prewarm(ctx, format)` starts the compile for the destinations a caller will hand it.
class sr::mix_routine : public sg::render_routine<mix_routine, sg::pixel_format>
{
public:
    /// Blends `source` into `destination` by `weight`, which must be in [0, 1].
    ///
    /// The two must have the same extent, and they must not be the same texture.
    /// Blending an image into itself is a no-op written the expensive way.
    /// `destination` must carry `image` usage, in a float or normalized format the device reads and writes as an image.
    /// `weight` 0 and 1 are still dispatched rather than skipped: a caller stepping a fade reaches them on the frames
    /// where the answer is one input exactly, and branching here would make those frames differ in cost for nothing.
    ///
    /// False while the shader for `destination`'s format is still compiling, or after a compile that did not build, which init logged.
    /// `destination` is untouched either way, which for a fade means the frame shows whichever image it already held.
    [[nodiscard]] static bool execute(sg::command_list& cmd,
                                      sg::texture_2d const& destination,
                                      sg::texture_2d const& source,
                                      f32 weight);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
