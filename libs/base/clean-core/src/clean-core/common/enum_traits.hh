#pragma once

#include <clean-core/fwd.hh>

#include <type_traits>

// What clean-core knows about an enum, and the one way an enum tells it.
//
// An enum can carry neither a hidden friend nor a member, so this customization point has only the trait tier —
// tiers 2 and 3 of [customization-points](../../../docs/customization-points.md) are structurally unavailable here.
// That has a consequence worth knowing before you reach for it: an explicit specialization must be written at a
// namespace enclosing cc, so it can never sit next to the enum.
// The CC_FLAG_ENUM_* macros in clean-core/common/flags.hh are what reconcile that with an operator only ADL can find —
// they take the enum's namespace as an argument and open it themselves.

namespace cc
{
/// How an enum's values map onto the bits of a cc::flags.
///
/// Which one an enum uses is not detectable, only declarable: nothing about `e = 4` says whether it means bit 4 or bit 2.
/// Getting it wrong silently shifts every flag, so there is no default — each opt-in macro names one encoding outright.
enum class flag_encoding
{
    /// The value is a bit INDEX, so `enum class foo { e1, e2, e3 }` needs nothing further: e1 takes bit 0, e2 bit 1.
    /// A value always names exactly one bit.
    bit_index,

    /// The value is the bit PATTERN itself, as in `e1 = 1u << 0`.
    /// One value may therefore name several bits at once, which is what makes a combined `all = 0b111` possible.
    bit_mask,
};
} // namespace cc

namespace cc::custom
{
/// Everything clean-core knows about an enum.
/// The primary is the "said nothing" case, so cc::custom::enum_traits<E> is well-formed for every enum.
///
/// Specialize it by hand only for an enum you cannot annotate at its definition; the CC_FLAG_ENUM_* macros are the normal path.
/// A flag enum must supply all three members — there is no default encoding to fall back on.
///
///     template <>
///     struct cc::custom::enum_traits<some_external::mode>
///     {
///         static constexpr bool is_flag_enum = true;
///         static constexpr cc::flag_encoding flag_encoding = cc::flag_encoding::bit_index;
///         using flag_storage_type = u32;
///     };
template <class EnumT>
struct enum_traits
{
    static constexpr bool is_flag_enum = false;
};
} // namespace cc::custom

namespace cc
{
/// EnumT may be used with cc::flags.
/// This asks the traits, not whether a specialization exists — an enum may declare other things about itself and still not be a flag enum.
template <class EnumT>
concept flag_enum = std::is_enum_v<EnumT> && cc::custom::enum_traits<EnumT>::is_flag_enum;
} // namespace cc
