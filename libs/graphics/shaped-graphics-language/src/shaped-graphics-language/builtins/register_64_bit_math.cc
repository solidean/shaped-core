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

// Every evaluator here reads its width and its kind off `in`, so one serves `long`, `ulong3` and `double4` alike.
// An integer computes on its 64 bits unsigned, which is two's-complement wrapping for a long and plain wrapping for a
// ulong, and is no undefined behaviour of the interpreter.

constexpr u64 k_long_min_bits = u64(1) << 63;

bool is_double(scalar x)
{
    return x.kind == value_kind::scalar_double;
}

/// `a < b` of two leaves of one 64-bit kind.
bool is_below(scalar a, scalar b)
{
    if (is_double(a))
        return a.as_double() < b.as_double();
    if (a.kind == value_kind::scalar_long)
        return a.as_long() < b.as_long();
    return a.as_ulong() < b.as_ulong();
}

/// `a == b` as the targets compare: a double as a double, so a NaN is unequal to itself and -0.0 equals 0.0.
bool is_same(scalar a, scalar b)
{
    return is_double(a) ? a.as_double() == b.as_double() : a.wide_bits() == b.wide_bits();
}

/// `op(a, b)` per component, the vectors being the two halves of `in`.
template <class Op>
void each_pair(leaves in, result& out, Op&& op)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(op(in[i], in[width + i]));
}

/// `op` of two integers' bits, or `float_op` of two doubles.
template <class IntOp, class FloatOp>
void arithmetic(leaves in, result& out, IntOp&& int_op, FloatOp&& float_op)
{
    each_pair(in, out,
              [&](scalar a, scalar b)
              {
                  return is_double(a) ? scalar::of_double(float_op(a.as_double(), b.as_double()))
                                      : scalar::of_wide(a.kind, int_op(a.wide_bits(), b.wide_bits()));
              });
}

void add(leaves in, result& out)
{
    arithmetic(in, out, [](u64 a, u64 b) { return a + b; }, [](f64 a, f64 b) { return a + b; });
}
void subtract(leaves in, result& out)
{
    arithmetic(in, out, [](u64 a, u64 b) { return a - b; }, [](f64 a, f64 b) { return a - b; });
}
void multiply(leaves in, result& out)
{
    arithmetic(in, out, [](u64 a, u64 b) { return a * b; }, [](f64 a, f64 b) { return a * b; });
}

/// An integer `/` and `%` truncate toward zero; a zero divisor and the most negative long over -1 have no value on any
/// target, which `division_undefined` reports, and the interpreter gives 0 where nothing stopped it.
scalar divided(scalar a, scalar b, bool is_remainder)
{
    if (is_double(a))
        return scalar::of_double(a.as_double() / b.as_double());
    auto const zero = scalar::of_wide(a.kind, 0);
    if (b.wide_bits() == 0)
        return zero;
    if (a.kind == value_kind::scalar_ulong)
        return scalar::of_ulong(is_remainder ? a.as_ulong() % b.as_ulong() : a.as_ulong() / b.as_ulong());
    if (a.wide_bits() == k_long_min_bits && b.as_long() == -1)
        return zero;
    return scalar::of_long(is_remainder ? a.as_long() % b.as_long() : a.as_long() / b.as_long());
}
void divide(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return divided(a, b, false); });
}
void remainder(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return divided(a, b, true); });
}

cc::string_view division_undefined(leaves in)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
    {
        if (in[width + i].wide_bits() == 0)
            return "an integer divided by zero";
        if (in[i].kind == value_kind::scalar_long && in[i].wide_bits() == k_long_min_bits && in[width + i].as_long() == -1)
            return "the most negative long divided by -1";
    }
    return {};
}

/// Wraps where it negates the most negative long, as every target does; a double flips its sign.
void negate(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of_wide(x.kind, is_double(x) ? x.wide_bits() ^ k_long_min_bits : 0 - x.wide_bits()));
}
/// A double's sign bit cleared, so `abs(-0.0)` is 0.0; a long wraps where it is the most negative one.
void absolute(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(is_double(x)      ? scalar::of_wide(x.kind, x.wide_bits() & ~k_long_min_bits)
                      : x.as_long() < 0 ? scalar::of_wide(x.kind, 0 - x.wide_bits())
                                        : x);
}
void minimum(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return is_below(b, a) ? b : a; });
}
void maximum(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return is_below(a, b) ? b : a; });
}
void clamped(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const raised = is_below(in[i], in[width + i]) ? in[width + i] : in[i];
        out.push_back(is_below(in[2 * width + i], raised) ? in[2 * width + i] : raised);
    }
}

void less(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(is_below(a, b)); });
}
void less_equal(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(is_below(a, b) || is_same(a, b)); });
}
void greater(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(is_below(b, a)); });
}
void greater_equal(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(is_below(b, a) || is_same(a, b)); });
}
void equal(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(is_same(a, b)); });
}
void not_equal(leaves in, result& out)
{
    each_pair(in, out, [](scalar a, scalar b) { return scalar::of(!is_same(a, b)); });
}

// ---- bits of long and ulong --------------------------------------------------------------------------------------------

template <class Op>
void bitwise(leaves in, result& out, Op&& op)
{
    each_pair(in, out, [&](scalar a, scalar b) { return scalar::of_wide(a.kind, op(a.wide_bits(), b.wide_bits())); });
}
void bit_and(leaves in, result& out)
{
    bitwise(in, out, [](u64 a, u64 b) { return a & b; });
}
void bit_or(leaves in, result& out)
{
    bitwise(in, out, [](u64 a, u64 b) { return a | b; });
}
void bit_xor(leaves in, result& out)
{
    bitwise(in, out, [](u64 a, u64 b) { return a ^ b; });
}
void bit_not(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of_wide(x.kind, ~x.wide_bits()));
}
/// The count's low six bits, on every target (EVAL-104).
void shift_left(leaves in, result& out)
{
    bitwise(in, out, [](u64 a, u64 b) { return a << (b & 63u); });
}
/// Arithmetic for a long, which keeps its sign, and logical for a ulong.
void shift_right(leaves in, result& out)
{
    each_pair(in, out,
              [](scalar a, scalar b)
              {
                  auto const count = b.wide_bits() & 63u;
                  return a.kind == value_kind::scalar_long ? scalar::of_long(a.as_long() >> count)
                                                           : scalar::of_ulong(a.as_ulong() >> count);
              });
}

/// Whether `text` is a count written as a literal, digits and a suffix of `U` and `L`, which CHK-452 holds to 0 to
/// 63 already.
bool is_literal_count(cc::string_view text)
{
    auto digits = isize(0);
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
        ++digits;
    auto is_suffix = true;
    for (auto i = digits; i < text.size(); ++i)
        is_suffix = is_suffix && (text[i] == 'U' || text[i] == 'L');
    return digits > 0 && is_suffix;
}

/// `a << b` and `a >> b`, the count masked to its low six bits where it is no literal: HLSL and MSL leave a count of 64
/// or more undefined.
/// MSL is C++, where a left shift of a negative long is undefined, so a long shifts left as the ulong of its bits there.
/// `data` names the type of `a`.
template <bool IsLeft>
written write_shift(call_context const& c)
{
    auto const& type = c.builtins.at(builtin_type_id(i32(c.data)));
    auto const op = IsLeft ? "<<" : ">>";
    auto const count = wrapped(c.arguments[1], precedence::unary);
    auto const masked = is_literal_count(count) ? count : cc::format("({} & 63)", count);
    auto const left = wrapped(c.arguments[0], precedence::unary);
    if (c.target == language::msl && IsLeft && type.leaf_kind == value_kind::scalar_long)
    {
        auto const width = type.leaf_count;
        auto const unsigned_type = width == 1 ? cc::string("ulong") : cc::format("ulong{}", width);
        auto const unsigned_count = width == 1 ? masked : cc::format("{}({})", unsigned_type, masked);
        return {.text = cc::format("as_type<{}>(as_type<{}>({}) << {})", type.msl, unsigned_type, c.arguments[0].text,
                                   unsigned_count)};
    }
    return {.text = cc::format("{} {} {}", left, op, masked), .binds = precedence::bitwise};
}

template <char Op>
written write_bitwise(call_context const& c)
{
    return {.text = cc::format("{} {} {}", wrapped(c.arguments[0], precedence::unary), Op,
                               wrapped(c.arguments[1], precedence::unary)),
            .binds = precedence::bitwise};
}

constexpr cc::string_view k_as_type[] = {"as_type"};

cc::string type_of(cc::string_view family, i32 width)
{
    return width == 1 ? cc::string(family) : cc::format("{}{}", family, width);
}
} // namespace

void sgl::builtins::register_64_bit_math(registry& r)
{
    using namespace impl;
    auto const named
        = [](cc::string_view name, cc::string_view type) { return cc::format("{}{}", name, suffix_of(type)); };

    r.add_comment("// long, ulong and double (CHK-450): arithmetic componentwise, a vector beside its scalar from "
                  "either "
                  "side,\n"
                  "// and the integers' `/` and `%` truncating toward zero, with no value for a zero divisor");
    for (auto const family : {cc::string_view("long"), cc::string_view("ulong"), cc::string_view("double")})
    {
        auto const is_integer = family != "double";
        for (auto width = 1; width <= 4; ++width)
        {
            auto const type = type_of(family, width);
            auto const t = cc::string_view(type);
            add_infix(r, "+", named("add", t), t, t, t, add);
            add_infix(r, "-", named("subtract", t), t, t, t, subtract);
            add_infix(r, "*", named("multiply", t), t, t, t, multiply);
            add_infix(r, "/", named("divide", t), t, t, t, divide, is_integer ? division_undefined : nullptr);
            if (is_integer)
            {
                r.functions.back().judged_last = judged_operand::divisor;
                add_infix(r, "%", named("remainder", t), t, t, t, remainder, division_undefined);
                r.functions.back().judged_last = judged_operand::divisor;
            }
            if (family != "ulong")
            {
                add_negate(r, named("negate", t), t, negate);
                add_function(r, "abs", {"x", t}, t, absolute);
            }
            add_function(r, "min", {"a", t, "b", t}, t, minimum);
            add_function(r, "max", {"a", t, "b", t}, t, maximum);
            add_function(r, "clamp", {"x", t, "low", t, "high", t}, t, clamped, {}, {}, clamp_undefined);
            if (width == 1)
                continue;

            for (auto const left : {false, true})
            {
                auto const lhs = left ? family : t;
                auto const rhs = left ? t : family;
                auto const name = [&](cc::string_view stem)
                {
                    return left ? cc::format("{}_scalar{}", stem, suffix_of(t))
                                : cc::format("{}{}_scalar", stem, suffix_of(t));
                };
                add_infix(r, "+", name("add"), lhs, rhs, t,
                          left ? spread_evaluate<add, true> : spread_evaluate<add, false>);
                add_infix(r, "-", name("subtract"), lhs, rhs, t,
                          left ? spread_evaluate<subtract, true> : spread_evaluate<subtract, false>);
                add_infix(r, "*", name("multiply"), lhs, rhs, t,
                          left ? spread_evaluate<multiply, true> : spread_evaluate<multiply, false>);
                add_infix(r, "/", name("divide"), lhs, rhs, t,
                          left ? spread_evaluate<divide, true> : spread_evaluate<divide, false>,
                          !is_integer ? nullptr
                          : left      ? spread_check<division_undefined, true>
                                      : spread_check<division_undefined, false>);
                if (!is_integer)
                    continue;
                r.functions.back().judged_last = judged_operand::divisor;
                add_infix(r, "%", name("remainder"), lhs, rhs, t,
                          left ? spread_evaluate<remainder, true> : spread_evaluate<remainder, false>,
                          left ? spread_check<division_undefined, true> : spread_check<division_undefined, false>);
                r.functions.back().judged_last = judged_operand::divisor;
            }
        }

        r.add_comment(
            cc::format("// {}: the comparisons of a scalar, and a double's `!=` true for a NaN (EMIT-186)", family));
        add_infix(r, "<", named("less", family), family, family, "bool", less);
        add_infix(r, "<=", named("less_equal", family), family, family, "bool", less_equal);
        add_infix(r, ">", named("greater", family), family, family, "bool", greater);
        add_infix(r, ">=", named("greater_equal", family), family, family, "bool", greater_equal);
        add_infix(r, "==", named("equal", family), family, family, "bool", equal);
        add_operator(r, "!=", named("not_equal", family), family, family, "bool", not_equal,
                     is_integer ? infix("!=") : float_not_equal());
    }

    r.add_comment("// bits of long, ulong and their vectors, componentwise; only a count's low six bits count "
                  "(CHK-452)");
    for (auto const family : {cc::string_view("long"), cc::string_view("ulong")})
        for (auto width = 1; width <= 4; ++width)
        {
            auto const type = type_of(family, width);
            auto const t = cc::string_view(type);
            auto const data = u32(index_of(registered_type(r, t)));
            add_operator(r, "&", named("bit_and", t), t, t, t, bit_and,
                         {.kind = spelling_kind::custom, .custom = write_bitwise<'&'>});
            add_operator(r, "|", named("bit_or", t), t, t, t, bit_or,
                         {.kind = spelling_kind::custom, .custom = write_bitwise<'|'>});
            add_operator(r, "^", named("bit_xor", t), t, t, t, bit_xor,
                         {.kind = spelling_kind::custom, .custom = write_bitwise<'^'>});
            add_operator(
                r, "<<", named("shift_left", t), t, t, t, shift_left,
                {.kind = spelling_kind::custom, .custom = write_shift<true>, .data = data, .msl_names = k_as_type});
            r.functions.back().judged_last = judged_operand::shift_count;
            add_operator(r, ">>", named("shift_right", t), t, t, t, shift_right,
                         {.kind = spelling_kind::custom, .custom = write_shift<false>, .data = data});
            r.functions.back().judged_last = judged_operand::shift_count;
            r.add(function_record{
                .signature = cc::format("@pure @operator(\"~\") fun {}(x: {}) -> {}", named("bit_not", t), t, t),
                .evaluate = bit_not,
                .write = {.kind = spelling_kind::prefix, .text = "~", .binds = precedence::unary},
            });
        }
}
