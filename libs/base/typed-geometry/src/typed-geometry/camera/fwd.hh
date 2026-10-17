#pragma once

#include <typed-geometry/scalar/fwd.hh>

namespace tg
{
enum class clip_depth : u8;

/// A view and a projection fused into one world-to-clip map, with its inverse and its depth convention.
template <class T>
struct view_projection;

using view_projection_f = view_projection<f32>;
using view_projection_d = view_projection<f64>;
} // namespace tg

/// The depth range a projection maps the view volume's near and far planes onto, in normalized device coordinates.
/// x and y are always [-1, 1], +y up; only depth differs between conventions.
enum class tg::clip_depth : tg::u8
{
    /// near to 1, far to 0: shaped-core's default, and the one tg::frustum::make_from_view_projection reads
    reverse_zero_to_one,
    /// near to 0, far to 1: D3D's, Vulkan's, Metal's and WebGPU's classic range
    zero_to_one,
    /// near to -1, far to 1: OpenGL's
    minus_one_to_one,
};
