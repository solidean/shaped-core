#pragma once

#include "fwd.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

// The pictures: a page of text through the same sampler on each backend, and the comparison between them.
//
// The probe states the gap as numbers; these show it.
// The page lies on a floor to the horizon, and faces the camera flat at a scale where the two drivers' blends differ most.
// Each view is rendered twice, as the level the sampler picks and as the page a renderer would show.

/// One scene under one sampler setting, as both renders.
struct lod_probe::floor_view
{
    cc::string scene; ///< "floor", "mip-colors" for the floor with a colour per mip, or "page-lod<L>" / "tinted-lod<L>" for a flat page of level L
    int anisotropy = 1;
    float sampler_bias = 0;
    cc::vector<float> lod; ///< per pixel; -1 where no floor was drawn
    cc::vector<u8> rgba;   ///< per pixel, the detail texture
};

/// Every view one context rendered.
struct lod_probe::floor_capture
{
    cc::string backend;
    tg::vec2i size;
    cc::vector<floor_view> views;
};

namespace lod_probe
{
/// Renders every view on `ctx` and reads both renders back.
/// The shader library holding floor.sgl must be alive.
[[nodiscard]] cc::shared_async<cc::result<floor_capture>> capture_floor(sg::context_handle ctx);

/// Writes each backend's renders and, given two backends, one comparison figure per view; returns the paths written.
/// The figure is three rows of two: each backend's render, each backend's level map with a contour per level, and then
/// both backends' contours over the first's map (white the first's, black the second's) beside the renders' difference.
[[nodiscard]] cc::result<cc::vector<cc::string>> write_floor_images(cc::span<floor_capture const> captures,
                                                                    cc::string_view base);
} // namespace lod_probe
