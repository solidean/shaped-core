#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/display.hh>
#include <shaped-rendering/fwd.hh>

/// The light that scatters in a lens, on a linear image: `out = L - s S(L) + s (k * S(L))`.
///
/// `S(L)` is the light above a soft knee, and `k` a kernel of four Gaussians that sums to 1, sized by the image's height.
/// A pixel gives up what it scatters, so total light is unchanged, and light below the knee only gains what lands on it.
/// libs/graphics/shaped-rendering/docs/bloom.md is the definition, and how close this mip chain comes to it.
class sr::bloom_routine : public sg::render_routine<bloom_routine>
{
public:
    /// How many pixels of blur the chain's level `index` carries, level 0 being half the image's size.
    /// Measured on an impulse, and it doubles per level.
    [[nodiscard]] static f64 level_sigma(int index);

    /// The weight of each level, summing to 1, for an image `height` pixels tall.
    /// Each Gaussian of the kernel is shared between the two levels around its variance.
    [[nodiscard]] static cc::vector<f32> level_weights(int height, f32 radius_scale);

    /// `image`, linear and with `texture` usage, with its light scattered, in a float image of the frame.
    /// `exposure_scale` is what the image will be shown at, which `settings.knee` is relative to.
    /// None while the shaders are compiling, or after a compile that did not build, which init logged.
    [[nodiscard]] static cc::optional<sg::texture_2d> execute(sg::command_list& cmd,
                                                              sg::texture_2d const& image,
                                                              bloom_settings const& settings,
                                                              f32 exposure_scale = 1.0f);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _first_layout;
    sg::binding_group_layout_handle _chain_layout;
    sg::binding_group_layout_handle _finish_layout;
    sg::async_compute_pipeline _first;
    sg::async_compute_pipeline _down;
    sg::async_compute_pipeline _up;
    sg::async_compute_pipeline _finish;
};
