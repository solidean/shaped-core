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

    /// AgX, then the sRGB encode: very bright saturated light desaturates toward white, as on film, and an 18% gray reads a little lighter than under `neutral`.
    agx = 3,
};

/// The light that scatters in a lens, which `sr::bloom_routine` applies; libs/graphics/shaped-rendering/docs/bloom.md is the definition.
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

/// A look, as numbers on light after exposure and before the curve, so nothing it does clips after the curve's roll-off.
/// Each default does nothing, and all of them together leave an image byte for byte as it was.
struct sr::grade_settings
{
    /// Warmer above 0, cooler below: the red and blue channels move apart by this share.
    f32 temperature = 0.0f;

    /// Greener above 0, more magenta below: the green channel moves by this share.
    f32 tint = 0.0f;

    /// The share of light lost at the image's corners, falling off as the square of the distance from its middle.
    f32 vignette = 0.0f;

    /// A power about middle gray, so 1 is no change and gray stays gray.
    f32 contrast = 1.0f;

    /// A scale about luminance, so 1 is no change and 0 is gray.
    f32 saturation = 1.0f;

    [[nodiscard]] bool is_identity() const
    {
        return temperature == 0.0f && tint == 0.0f && vignette == 0.0f && contrast == 1.0f && saturation == 1.0f;
    }
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

    /// The light that scatters before any of this, which whoever draws the image applies with `sr::bloom_routine`; off by default.
    bloom_settings bloom = {};

    /// Applied after the exposure and before the curve.
    grade_settings grade = {};

    /// A fixed pattern under half an 8-bit step, added after the encode, so a smooth gradient shows no bands.
    bool dither = false;

    /// `2^exposure`, the factor module `display`'s `transform` takes.
    [[nodiscard]] f32 exposure_scale() const;
};

namespace sr
{
/// The exposure in stops that shows an image of mean luminance `average` as middle gray, 0.18: what an automatic exposure adapts toward.
/// `average` must be above 0.
[[nodiscard]] f32 exposure_for_average(f32 average);
} // namespace sr
