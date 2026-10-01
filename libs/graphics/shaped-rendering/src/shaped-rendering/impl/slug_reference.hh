#pragma once

#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sr::impl
{
/// The coverage prelude/slug.sgl's `slug_coverage` computes, computed on the CPU from the atlas's own copy of its textures.
///
/// The same arithmetic in the same order, in f32: what the GPU's result is held to, within the rounding the targets differ by.
/// It also tests glyph compilation on its own, since a wrongly sorted band shows here before any GPU runs.
[[nodiscard]] f32 slug_reference_coverage(slug_atlas const& atlas,
                                          slug_instance const& instance,
                                          tg::pos2f em,
                                          tg::vec2f em_per_pixel,
                                          bool weight_boost);
} // namespace sr::impl
