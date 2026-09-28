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

/// What tg knows about a scalar type — its zero, one, epsilon and whether it is exact (see scalar/traits.hh).
template <class T>
struct scalar_traits;

/// a scalar split against base two: significand * 2^exponent, exactly (see scalar/traits.hh).
template <class T>
struct pow2_split;

/// a scalar angle (storage is radians); a unit-checked newtype over T.
template <class T>
struct angle;

using angle_f = angle<f32>;
using angle_d = angle<f64>;

namespace impl
{
template <int Bits, bool Signed>
struct fixed_integer;
}

/// a two's-complement integer of exactly Bits bits (32 or a multiple of 64), wrapping on every operator
/// (see scalar/fixed_int/fixed_int.hh).
template <int Bits>
using fixed_int = impl::fixed_integer<Bits, true>;

/// the unsigned counterpart of fixed_int.
template <int Bits>
using fixed_uint = impl::fixed_integer<Bits, false>;

using fi32 = fixed_int<32>;
using fi64 = fixed_int<64>;
using fi128 = fixed_int<128>;
using fi192 = fixed_int<192>;
using fi256 = fixed_int<256>;

using fu32 = fixed_uint<32>;
using fu64 = fixed_uint<64>;
using fu128 = fixed_uint<128>;
using fu192 = fixed_uint<192>;
using fu256 = fixed_uint<256>;

/// a quotient with its remainder, from one long division (see scalar/fixed_int/fixed_arith.hh).
template <class T>
struct div_mod_result;

/// floor and ceil of one quotient (see scalar/fixed_int/fixed_arith.hh).
template <class T>
struct floor_ceil_result;

} // namespace tg
