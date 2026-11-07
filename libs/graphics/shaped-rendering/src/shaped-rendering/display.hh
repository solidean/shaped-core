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

/// The light that scatters in a lens, which `sr::bloom_routine` applies; docs/bloom.md is the definition.
struct sr::bloom_settings
{
    /// The share of the light above the knee that scatters; 0 is off.
    f32 strength = 0.0f;

    /// Scales the kernel's radii, which are shares of the image's height.
    f32 radius_scale = 1.0f;

    /// Where a pixel starts to scatter, as light shown after exposure, so 1 is display white.
    /// What the display can show stays true; 0 scatters all light, which veils the whole image.
    f32 knee = 1.0f;
};

/// What turns a linear image into the values a display shows: an exposure, then a curve.
struct sr::display_transform
{
    /// In stops: the radiance is multiplied by `2^exposure` before the curve.
    f32 exposure = 0.0f;

    display_curve curve = display_curve::none;

    /// Set, the exposure is metered from the image and adapts toward middle gray, and `exposure` is the compensation added to it.
    /// Whoever draws the image meters it, with `sr::exposure_meter_routine`; the transform itself holds no state.
    /// A viewer adapts toward each reading; a still capture takes the reading of its finished image whole, so the same image gets the same exposure.
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
