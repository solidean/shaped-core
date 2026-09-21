#pragma once

#include <shaped-graphics-language/fwd.hh>

/// The leaves every value of the abstract machine is made of.
/// A header of its own, since the builtin registry names them and stands below the check pass.

enum class sgl::check::value_kind : sgl::u8
{
    none,
    scalar_float,
    scalar_int,
    boolean,
};

/// One leaf of a value, held as its bits so that equality is exact: a NaN equals itself and 0.0 differs from -0.0.
struct sgl::check::scalar
{
    value_kind kind = value_kind::none;
    u32 bits = 0;

    [[nodiscard]] static scalar of(f32 v);
    [[nodiscard]] static scalar of(i32 v);
    [[nodiscard]] static scalar of(bool v);

    [[nodiscard]] f32 as_float() const;
    [[nodiscard]] i32 as_int() const { return i32(bits); }
    [[nodiscard]] bool as_bool() const { return bits != 0; }

    constexpr bool operator==(scalar const&) const = default;
};
