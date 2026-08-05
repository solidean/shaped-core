#pragma once

#include <clean-core/fwd.hh>

namespace tg
{
// Pull in the shaped-core vocabulary types (i32, f32, isize, ...) so tg can write them bare without leaking them into the global namespace.
// This is the lowest tg fwd header, so every module fwd picks them up along the dependency chain.
using namespace cc::primitive_defines;

//
// Scalar-like types
//

/// a scalar angle (storage is radians); a unit-checked newtype over T.
template <class T>
struct angle;

using angle_f = angle<f32>;
using angle_d = angle<f64>;

} // namespace tg
