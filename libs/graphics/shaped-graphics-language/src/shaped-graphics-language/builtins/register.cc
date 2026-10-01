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
    register_sync(r);
    register_raytracing(r);
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
constexpr cc::string_view k_fmod[] = {"fmod"};

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

namespace
{
/// True where some int component of `a op b`, taken exactly, is outside the ints; a uint component never is.
template <class Op>
bool is_outside_ints(cc::span<scalar const> in, Op&& op)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        if (in[i].kind != value_kind::scalar_int)
            continue;
        auto const exact = op(i64(in[i].as_int()), i64(in[width + i].as_int()));
        if (exact < -2147483648ll || exact > 2147483647ll)
            return true;
    }
    return false;
}
} // namespace

cc::string_view impl::sum_unrepresentable(cc::span<scalar const> in)
{
    return is_outside_ints(in, [](i64 a, i64 b) { return a + b; }) ? "an int sum outside the ints" : "";
}

cc::string_view impl::difference_unrepresentable(cc::span<scalar const> in)
{
    return is_outside_ints(in, [](i64 a, i64 b) { return a - b; }) ? "an int difference outside the ints" : "";
}

cc::string_view impl::product_unrepresentable(cc::span<scalar const> in)
{
    return is_outside_ints(in, [](i64 a, i64 b) { return a * b; }) ? "an int product outside the ints" : "";
}

cc::string_view impl::negation_unrepresentable(cc::span<scalar const> in)
{
    for (auto const& x : in)
        if (x.kind == value_kind::scalar_int && x.bits == k_int_min_bits)
            return "the most negative int negated";
    return {};
}

cc::string_view impl::shift_left_unrepresentable(cc::span<scalar const> in)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const count = in[width + i].bits & 31u;
        if (in[i].kind == value_kind::scalar_int)
        {
            auto const exact = i64(in[i].as_int()) * (i64(1) << count);
            if (exact < -2147483648ll || exact > 2147483647ll)
                return "an int shifted left past its sign";
        }
        else if ((u64(in[i].bits) << count) >> 32 != 0)
            return "a uint shifted left past its top bit";
    }
    return {};
}

cc::string_view impl::negative_as_uint(cc::span<scalar const> in)
{
    for (auto const& x : in)
        if (x.kind == value_kind::scalar_int && x.as_int() < 0)
            return "a negative int converted to uint";
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
            },
            .msl_names = k_fmod};
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
                        cc::string_view doc,
                        undefined_check undefined_when)
{
    auto list = cc::string();
    for (auto i = isize(0); i + 1 < parameters.size(); i += 2)
        list.appendf("{}{}: {}", i == 0 ? "" : ", ", parameters[i], parameters[i + 1]);
    r.add(function_record{
        .signature = cc::format("@pure fun {}({}) -> {}", name, list, result),
        .doc = doc,
        .evaluate = evaluate,
        .undefined_when = undefined_when,
        .write = cc::move(write),
    });
}

cc::string_view impl::clamp_undefined(cc::span<scalar const> in)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const low = in[width + i];
        auto const high = in[2 * width + i];
        auto is_above = low.bits > high.bits;
        if (low.kind == value_kind::scalar_float)
            is_above = low.as_float() > high.as_float();
        else if (low.kind == value_kind::scalar_int)
            is_above = low.as_int() > high.as_int();
        if (is_above)
            return "clamp whose low bound is above its high one";
    }
    return {};
}

void impl::spread_leaves(cc::span<scalar const> in, bool is_scalar_left, cc::vector<scalar>& out)
{
    auto const width = in.size() - 1;
    auto const s = is_scalar_left ? in[0] : in[width];
    auto const vector = in.subspan({.offset = is_scalar_left ? 1 : 0, .size = width});
    for (auto i = isize(0); i < width; ++i)
        out.push_back(is_scalar_left ? s : vector[i]);
    for (auto i = isize(0); i < width; ++i)
        out.push_back(is_scalar_left ? vector[i] : s);
}

builtin_type_id impl::registered_type(registry const& r, cc::string_view name)
{
    for (auto i = isize(0); i < r.types.size(); ++i)
    {
        auto const declaration = cc::string_view(r.types[i].declaration);
        for (auto const keyword : {cc::string_view("struct "), cc::string_view("enum ")})
        {
            auto const at = declaration.find(keyword);
            if (at < 0)
                continue;
            auto const rest = declaration.subview(at + keyword.size());
            auto end = isize(0);
            while (end < rest.size() && rest[end] != ':' && rest[end] != '\n' && rest[end] != '[')
                ++end;
            if (rest.subview({.offset = 0, .size = end}) == name)
                return builtin_type_id(i32(i));
        }
    }
    return builtin_type_id::none;
}

cc::vector<written> impl::spread_arguments(call_context const& c, isize at)
{
    auto result = cc::vector<written>();
    for (auto const& a : c.arguments)
        result.push_back(a);
    auto const type = c.builtins.at(builtin_type_id(i32(c.data))).spelled_in(c.target);
    result[at] = {.text = cc::format("{}({})", type, c.arguments[at].text)};
    return result;
}

cc::string impl::suffix_of(cc::string_view type)
{
    if (type == "float")
        return "";
    if (type == "float3")
        return "_color";
    return cc::format("_{}", type);
}
