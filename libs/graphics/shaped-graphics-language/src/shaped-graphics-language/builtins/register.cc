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
    register_bit_math(r);
    register_math(r);
    register_textures(r);
}

void impl::add_infix(registry& r,
                     cc::string_view op,
                     cc::string_view name,
                     cc::string_view lhs,
                     cc::string_view rhs,
                     cc::string_view result,
                     evaluator evaluate,
                     undefined_check undefined_when)
{
    add_operator(r, op, name, lhs, rhs, result, evaluate, infix(op), undefined_when);
}

void impl::add_operator(registry& r,
                        cc::string_view op,
                        cc::string_view name,
                        cc::string_view lhs,
                        cc::string_view rhs,
                        cc::string_view result,
                        evaluator evaluate,
                        spelling write,
                        undefined_check undefined_when)
{
    r.add(function_record{
        .signature = cc::format("@pure @operator(\"{}\") fun {}(a: {}, b: {}) -> {}", op, name, lhs, rhs, result),
        .evaluate = evaluate,
        .undefined_when = undefined_when,
        .write = cc::move(write),
    });
}

namespace
{
using check::scalar;
using check::value_kind;

constexpr u32 k_int_min_bits = 0x8000'0000u;

/// Past 2^23 every float is an integer already, and a NaN or an infinity is its own truncation.
f32 truncated(f32 x)
{
    if (!(x < 8388608.0f && x > -8388608.0f))
        return x;
    auto const t = f32(i32(x));
    return t == 0.0f && x < 0.0f ? -0.0f : t;
}
} // namespace

void impl::divide_integers(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const a = in[i];
        auto const b = in[width + i];
        if (a.kind == value_kind::scalar_uint)
            out.push_back(scalar::of_uint(b.bits == 0 ? 0 : a.bits / b.bits));
        else
            out.push_back(
                scalar::of(b.bits == 0 || (a.bits == k_int_min_bits && b.as_int() == -1) ? 0 : a.as_int() / b.as_int()));
    }
}

void impl::remainder_integers(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const a = in[i];
        auto const b = in[width + i];
        if (a.kind == value_kind::scalar_uint)
            out.push_back(scalar::of_uint(b.bits == 0 ? 0 : a.bits % b.bits));
        else
            out.push_back(
                scalar::of(b.bits == 0 || (a.bits == k_int_min_bits && b.as_int() == -1) ? 0 : a.as_int() % b.as_int()));
    }
}

cc::string_view impl::integer_division_undefined(cc::span<scalar const> in)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        if (in[width + i].bits == 0)
            return "an integer divided by zero";
        if (in[i].kind == value_kind::scalar_int && in[i].bits == k_int_min_bits && in[width + i].as_int() == -1)
            return "the most negative int divided by -1";
    }
    return {};
}

void impl::remainder_floats(cc::span<scalar const> in, cc::vector<scalar>& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const a = in[i].as_float();
        auto const b = in[width + i].as_float();
        out.push_back(scalar::of(a - b * truncated(a / b)));
    }
}

spelling impl::float_remainder()
{
    return {.kind = spelling_kind::custom,
            .custom = [](call_context const& c) -> written
            {
                if (c.target == language::msl)
                    return {.text = cc::format("fmod({}, {})", c.arguments[0].text, c.arguments[1].text)};
                return write_infix("%", precedence::multiplicative, c.arguments[0], c.arguments[1]);
            }};
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
