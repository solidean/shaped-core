#include "builtins.hh"

#include <clean-core/common/assert.hh>

namespace
{
using sgl::check::builtin;

constexpr builtin last_builtin = builtin::equal_int;
} // namespace

cc::string_view sgl::check::to_string(builtin b)
{
    switch (b)
    {
    case builtin::none:
        return "";
    case builtin::scalar_float:
        return "float";
    case builtin::float3:
        return "float3";
    case builtin::float4:
        return "float4";
    case builtin::vec3:
        return "vec3";
    case builtin::pos3:
        return "pos3";
    case builtin::hpos4:
        return "hpos4";
    case builtin::mat4:
        return "mat4";
    case builtin::scalar_int:
        return "int";
    case builtin::boolean:
        return "bool";
    case builtin::normalize:
        return "normalize";
    case builtin::dot:
        return "dot";
    case builtin::saturate:
        return "saturate";
    case builtin::transform_position:
        return "transform_position";
    case builtin::transform_direction:
        return "transform_direction";
    case builtin::scale_color:
        return "scale_color";
    case builtin::multiply:
        return "multiply";
    case builtin::add:
        return "add";
    case builtin::subtract:
        return "subtract";
    case builtin::less:
        return "less";
    case builtin::equal:
        return "equal";
    case builtin::add_int:
        return "add_int";
    case builtin::subtract_int:
        return "subtract_int";
    case builtin::multiply_int:
        return "multiply_int";
    case builtin::less_int:
        return "less_int";
    case builtin::equal_int:
        return "equal_int";
    }
    CC_UNREACHABLE("unknown builtin");
}

sgl::check::builtin sgl::check::builtin_of(cc::string_view name)
{
    for (auto i = int(builtin::none) + 1; i <= int(last_builtin); ++i)
        if (to_string(builtin(i)) == name)
            return builtin(i);
    return builtin::none;
}

bool sgl::check::is_type(builtin b)
{
    CC_ASSERT(b != builtin::none, "only a builtin is a type or a function");
    return int(b) <= int(builtin::boolean);
}
