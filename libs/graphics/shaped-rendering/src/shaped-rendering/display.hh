#pragma once

#include <shaped-rendering/fwd.hh>

/// The curve SGL module `display` (shaders/modules/display.sgl) maps exposed radiance through; the values are its `curve_*` constants.
enum class sr::display_curve : sr::u8
{
    /// The exposed radiance itself, unclamped and unencoded: what a float target or a later pass wants.
    none = 0,

    /// Clamped to [0, 1], then the exact sRGB encode.
    srgb = 1,

    /// The identity up to 0.6, then the peak channel rolls off toward 1 with slope 1 at the knee, the other two scaled with it; then the sRGB encode.
    /// An 18% gray reads 118 of 255 under it exactly as under `srgb`, and a highlight keeps its hue instead of clipping to white.
    neutral = 2,
};

/// What turns a linear image into the values a display shows: an exposure, then a curve.
struct sr::display_transform
{
    /// In stops: the radiance is multiplied by `2^exposure` before the curve.
    f32 exposure = 0.0f;

    display_curve curve = display_curve::none;

    /// Set, the exposure is metered from the image and adapts toward middle gray, and `exposure` is the compensation added to it.
    /// Whoever draws the image meters it, with `sr::exposure_meter_routine`; the transform itself holds no state.
    bool automatic = false;

    /// `2^exposure`, the factor module `display`'s `transform` takes.
    [[nodiscard]] f32 exposure_scale() const;
};

namespace sr
{
/// The exposure in stops that shows an image of mean luminance `average` as middle gray, 0.18: what an automatic exposure adapts toward.
/// `average` must be above 0.
[[nodiscard]] f32 exposure_for_average(f32 average);
} // namespace sr
