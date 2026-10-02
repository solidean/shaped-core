#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::scalar;
using check::value_kind;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// Every evaluator works on the bits and keeps the left operand's kind, so one serves int and uint of every width.

template <class Fn>
void pairwise(leaves in, result& out, Fn&& fn)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back({.kind = in[i].kind, .bits = fn(in[i], in[width + i])});
}
void bit_and(leaves in, result& out)
{
    pairwise(in, out, [](scalar a, scalar b) { return a.bits & b.bits; });
}
void bit_or(leaves in, result& out)
{
    pairwise(in, out, [](scalar a, scalar b) { return a.bits | b.bits; });
}
void bit_xor(leaves in, result& out)
{
    pairwise(in, out, [](scalar a, scalar b) { return a.bits ^ b.bits; });
}
void bit_not(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back({.kind = x.kind, .bits = ~x.bits});
}
/// The count's low five bits, on every target (EVAL-86).
void shift_left(leaves in, result& out)
{
    pairwise(in, out, [](scalar a, scalar b) { return a.bits << (b.bits & 31u); });
}
/// Arithmetic for an int, which keeps its sign, and logical for a uint.
void shift_right(leaves in, result& out)
{
    pairwise(in, out,
             [](scalar a, scalar b)
             {
                 auto const count = b.bits & 31u;
                 return a.kind == value_kind::scalar_int ? u32(a.as_int() >> count) : a.bits >> count;
             });
}

/// WGSL takes an operand of `&`, `|`, `^` and of a shift only as a unary expression, so anything looser is parenthesized.
cc::string operand(written w)
{
    return wrapped(cc::move(w), precedence::unary);
}

template <char Op>
written write_bitwise(call_context const& c)
{
    return {.text = cc::format("{} {} {}", operand(c.arguments[0]), Op, operand(c.arguments[1])),
            .binds = precedence::bitwise};
}

/// `a << b` everywhere, where HLSL masks the count to its low five bits and WGSL does at run time;
/// WGSL takes the count as unsigned, and MSL leaves a count past 31 undefined, so it masks.
/// MSL is C++, where a left shift of a negative int is undefined, so an int shifts as the uint of its bits there.
template <bool IsLeft, int Width, bool IsSigned>
written write_shift(call_context const& c)
{
    auto const op = IsLeft ? "<<" : ">>";
    if (c.target == language::msl && IsLeft && IsSigned)
    {
        auto const uint_type = Width == 1 ? cc::string("uint") : cc::format("uint{}", Width);
        auto const int_type = Width == 1 ? cc::string("int") : cc::format("int{}", Width);
        return {.text = cc::format("as_type<{}>(as_type<{}>({}) << {}({} & 31))", int_type, uint_type,
                                   c.arguments[0].text, uint_type, operand(c.arguments[1]))};
    }
    auto count = operand(c.arguments[1]);
    if (c.target == language::wgsl && IsSigned)
        count = Width == 1 ? cc::format("u32({})", count) : cc::format("vec{}u({})", Width, count);
    else if (c.target == language::msl)
        count = cc::format("({} & 31)", count);
    return {.text = cc::format("{} {} {}", operand(c.arguments[0]), op, count), .binds = precedence::bitwise};
}

constexpr cc::string_view k_as_type[] = {"as_type"};

/// `Write` over a vector and a scalar: WGSL takes no scalar beside a vector here, and MSL reinterprets a left operand
/// as the vector it shifts, so the scalar is written as the vector `c.data` names where either needs it (CHK-358).
template <custom_writer Write, bool IsScalarLeft>
written write_spread(call_context const& c)
{
    if (c.target == language::hlsl || (c.target == language::msl && !IsScalarLeft))
        return Write(c);
    auto const spread = impl::spread_arguments(c, IsScalarLeft ? 0 : 1);
    auto copy = c;
    copy.arguments = spread;
    return Write(copy);
}

struct integer_type
{
    cc::string_view name;
    custom_writer shift_left;
    custom_writer shift_right;
    /// The writers of a vector and its scalar, the scalar right and then left; none for a scalar type.
    custom_writer spread[2][5];
};

template <int Width, bool IsSigned>
constexpr integer_type integer_of(cc::string_view name)
{
    return {.name = name,
            .shift_left = write_shift<true, Width, IsSigned>,
            .shift_right = write_shift<false, Width, IsSigned>,
            .spread = {{write_spread<write_bitwise<'&'>, false>, write_spread<write_bitwise<'|'>, false>,
                        write_spread<write_bitwise<'^'>, false>, write_spread<write_shift<true, Width, IsSigned>, false>,
                        write_spread<write_shift<false, Width, IsSigned>, false>},
                       {write_spread<write_bitwise<'&'>, true>, write_spread<write_bitwise<'|'>, true>,
                        write_spread<write_bitwise<'^'>, true>, write_spread<write_shift<true, Width, IsSigned>, true>,
                        write_spread<write_shift<false, Width, IsSigned>, true>}}};
}
} // namespace

void sgl::builtins::register_bit_math(registry& r)
{
    using namespace impl;

    integer_type const types[] = {
        integer_of<1, true>("int"),    integer_of<2, true>("int2"),   integer_of<3, true>("int3"),
        integer_of<4, true>("int4"),   integer_of<1, false>("uint"),  integer_of<2, false>("uint2"),
        integer_of<3, false>("uint3"), integer_of<4, false>("uint4"),
    };

    r.add_comment("// bits of int, uint and their vectors, componentwise; a shift counts in the left side's own type,\n"
                  "// and only its low five bits count, so `x << 33` of a runtime 33 is `x << 1`");
    for (auto const& t : types)
    {
        auto const suffix = suffix_of(t.name);
        add_operator(r, "&", cc::format("bit_and{}", suffix), t.name, t.name, t.name, bit_and,
                     {.kind = spelling_kind::custom, .custom = write_bitwise<'&'>});
        add_operator(r, "|", cc::format("bit_or{}", suffix), t.name, t.name, t.name, bit_or,
                     {.kind = spelling_kind::custom, .custom = write_bitwise<'|'>});
        add_operator(r, "^", cc::format("bit_xor{}", suffix), t.name, t.name, t.name, bit_xor,
                     {.kind = spelling_kind::custom, .custom = write_bitwise<'^'>});
        add_operator(r, "<<", cc::format("shift_left{}", suffix), t.name, t.name, t.name, shift_left,
                     {.kind = spelling_kind::custom, .custom = t.shift_left, .msl_names = k_as_type});
        r.functions.back().unrepresentable_when_constant = shift_left_unrepresentable;
        r.functions.back().judged_last = judged_operand::shift_count;
        add_operator(r, ">>", cc::format("shift_right{}", suffix), t.name, t.name, t.name, shift_right,
                     {.kind = spelling_kind::custom, .custom = t.shift_right});
        r.functions.back().judged_last = judged_operand::shift_count;
        r.add(function_record{
            .signature = cc::format("@pure @operator(\"~\") fun bit_not{}(x: {}) -> {}", suffix, t.name, t.name),
            .evaluate = bit_not,
            .write = {.kind = spelling_kind::prefix, .text = "~", .binds = precedence::unary},
        });
    }

    r.add_comment("// an integer vector and its scalar, from either side, componentwise (CHK-358)");
    for (auto const& t : types)
    {
        if (t.name == "int" || t.name == "uint")
            continue;
        auto const element = t.name.starts_with("uint") ? cc::string_view("uint") : cc::string_view("int");
        auto const data = u32(index_of(registered_type(r, t.name)));
        auto const suffix = suffix_of(t.name);
        for (auto const left : {false, true})
        {
            auto const name = [&](cc::string_view stem)
            { return left ? cc::format("{}_scalar{}", stem, suffix) : cc::format("{}{}_scalar", stem, suffix); };
            auto const lhs = left ? element : t.name;
            auto const rhs = left ? t.name : element;
            auto const& w = t.spread[left ? 1 : 0];
            add_operator(r, "&", name("bit_and"), lhs, rhs, t.name,
                         left ? spread_evaluate<bit_and, true> : spread_evaluate<bit_and, false>,
                         {.kind = spelling_kind::custom, .custom = w[0], .data = data});
            add_operator(r, "|", name("bit_or"), lhs, rhs, t.name,
                         left ? spread_evaluate<bit_or, true> : spread_evaluate<bit_or, false>,
                         {.kind = spelling_kind::custom, .custom = w[1], .data = data});
            add_operator(r, "^", name("bit_xor"), lhs, rhs, t.name,
                         left ? spread_evaluate<bit_xor, true> : spread_evaluate<bit_xor, false>,
                         {.kind = spelling_kind::custom, .custom = w[2], .data = data});
            add_operator(r, "<<", name("shift_left"), lhs, rhs, t.name,
                         left ? spread_evaluate<shift_left, true> : spread_evaluate<shift_left, false>,
                         {.kind = spelling_kind::custom, .custom = w[3], .data = data, .msl_names = k_as_type});
            r.functions.back().unrepresentable_when_constant = left ? spread_check<shift_left_unrepresentable, true>
                                                                    : spread_check<shift_left_unrepresentable, false>;
            r.functions.back().judged_last = judged_operand::shift_count;
            add_operator(r, ">>", name("shift_right"), lhs, rhs, t.name,
                         left ? spread_evaluate<shift_right, true> : spread_evaluate<shift_right, false>,
                         {.kind = spelling_kind::custom, .custom = w[4], .data = data});
            r.functions.back().judged_last = judged_operand::shift_count;
        }
    }
}
