#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-shader-library/binding/impl/hlsl_tokens.hh>
#include <shaped-shader-library/fwd.hh>

namespace slib::impl
{
/// The `sg::sampler` a `#pragma sc static ...` attribute describes.
///
/// The keys are `sg::sampler`'s own field names and the values its own enumerator names, spelled exactly.
/// A second vocabulary between HLSL and sg would be one more table to keep in step for no benefit, and every
/// one of these names is already the one a C++ caller writes.
///
/// Everything omitted takes `sg::sampler`'s default, which is a trilinear repeating sampler.
/// Two shorthands cover the common case: `filter=linear` sets all three filters and `address=clamp_edge` all
/// three axes, and the tuple form addresses them individually in the order `sg::sampler` declares them.
[[nodiscard]] cc::result<sg::sampler> parse_sampler_state(annotation const& attribute);
} // namespace slib::impl
