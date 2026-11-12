#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/resources.hh>

// What check_expr.cc, check_calls.cc and check_overloads.cc share of their helpers.

namespace sgl::check::impl
{
/// Appends one argument to every parallel list of `a`.
inline void add_argument(call_arguments& a,
                         written_argument w,
                         type_id type,
                         cc::string_view name = {},
                         number_literal number = {},
                         i32 literal = -1,
                         i32 function = -1)
{
    a.written.push_back(w);
    a.types.push_back(type);
    a.names.push_back(name);
    a.numbers.push_back(number);
    a.literals.push_back(literal);
    a.functions.push_back(function);
    a.undefineds.push_back(false);
    a.leading_dots.push_back({});
}

/// The name of the scalar family whose leaves are `kind`, which is the stem of its vectors' names; empty for none.
inline cc::string_view family_of(value_kind kind)
{
    switch (kind)
    {
    case value_kind::scalar_float:
        return "float";
    case value_kind::scalar_int:
        return "int";
    case value_kind::scalar_uint:
        return "uint";
    case value_kind::scalar_half:
        return "half";
    case value_kind::scalar_short:
        return "short";
    case value_kind::scalar_ushort:
        return "ushort";
    case value_kind::scalar_long:
        return "long";
    case value_kind::scalar_ulong:
        return "ulong";
    case value_kind::scalar_double:
        return "double";
    case value_kind::boolean:
        return "bool";
    case value_kind::none:
        return {};
    }
    return {};
}
} // namespace sgl::check::impl
