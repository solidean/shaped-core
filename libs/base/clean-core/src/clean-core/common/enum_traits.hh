#pragma once

#include <clean-core/fwd.hh>

#include <type_traits>

// What clean-core knows about an enum, and the one way an enum tells it.
//
// An enum can carry neither a hidden friend nor a member, so this customization point has only the trait tier —
// tiers 2 and 3 of [customization-points](../../../docs/customization-points.md) are structurally unavailable here.
// That has a consequence worth knowing before you reach for it: an explicit specialization must be written at a
// namespace enclosing cc, so it can never sit next to the enum.
// CC_FLAG_ENUM in clean-core/common/flags.hh is what reconciles that with an operator only ADL can find — it takes the
// enum's namespace as an argument and opens it itself.

namespace cc::custom
{
/// Everything clean-core knows about an enum.
/// The primary is the "said nothing" case, so cc::custom::enum_traits<E> is well-formed for every enum.
///
/// Specialize it by hand only for an enum you cannot annotate at its definition; CC_FLAG_ENUM is the normal path.
///
///     template <>
///     struct cc::custom::enum_traits<some_external::mode>
///     {
///         static constexpr bool is_flag_enum = true;
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
