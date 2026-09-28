#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>

/// The targets a raster pipeline is compiled for, and that a rendering it is bound in must have.
/// Backends bake these into the PSO, so a mismatch is not something a driver is required to catch.
struct sg::raster_target_formats
{
    cc::fixed_vector<pixel_format, max_color_targets> color; ///< in output-merger order
    pixel_format depth_stencil = pixel_format::undefined;    ///< `undefined` for no depth-stencil target
    int sample_count = 1;
};
