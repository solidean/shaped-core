#include "register.hh"

#include <clean-core/string/format.hh>

using namespace sgl;
using namespace sgl::builtins;

void sgl::builtins::register_builtins(registry& r)
{
    register_types(r);
    register_scalar_math(r);
    register_vector_math(r);
    register_transforms(r);
    register_conversions(r);
    register_textures(r);
}

void impl::add_infix(registry& r,
                     cc::string_view op,
                     cc::string_view name,
                     cc::string_view lhs,
                     cc::string_view rhs,
                     cc::string_view result,
                     evaluator evaluate)
{
    r.add(function_record{
        .signature = cc::format("@pure @operator(\"{}\") fun {}(a: {}, b: {}) -> {}", op, name, lhs, rhs, result),
        .evaluate = evaluate,
        .write = infix(op),
    });
}

void impl::add_negate(registry& r, cc::string_view name, cc::string_view type, evaluator evaluate)
{
    r.add(function_record{
        .signature = cc::format("@pure @operator(\"-\") fun {}(x: {}) -> {}", name, type, type),
        .evaluate = evaluate,
        .write = {.kind = spelling_kind::prefix, .text = "-", .binds = precedence::unary},
    });
}

void impl::add_function(registry& r,
                        cc::string_view name,
                        cc::span<cc::string_view const> parameters,
                        cc::string_view result,
                        evaluator evaluate,
                        spelling write,
                        cc::string_view doc)
{
    auto list = cc::string();
    for (auto i = isize(0); i + 1 < parameters.size(); i += 2)
        list.appendf("{}{}: {}", i == 0 ? "" : ", ", parameters[i], parameters[i + 1]);
    r.add(function_record{
        .signature = cc::format("@pure fun {}({}) -> {}", name, list, result),
        .doc = doc,
        .evaluate = evaluate,
        .write = cc::move(write),
    });
}

cc::string impl::suffix_of(cc::string_view type)
{
    if (type == "float")
        return "";
    if (type == "float3")
        return "_color";
    return cc::format("_{}", type);
}
