#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::scalar;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// ---- float ----------------------------------------------------------------------------------------------------------

void negate(leaves in, result& out)
{
    out.push_back(scalar::of(-in[0].as_float()));
}
void multiply(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() * in[1].as_float()));
}
void divide(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() / in[1].as_float()));
}
void add(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() + in[1].as_float()));
}
void subtract(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() - in[1].as_float()));
}
void less(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() < in[1].as_float()));
}
void less_equal(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() <= in[1].as_float()));
}
void greater(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() > in[1].as_float()));
}
void greater_equal(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() >= in[1].as_float()));
}
void equal(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() == in[1].as_float()));
}
void not_equal(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_float() != in[1].as_float()));
}

// ---- int: unsigned where it may overflow, so that it wraps and is no undefined behaviour of the interpreter --------

void negate_int(leaves in, result& out)
{
    out.push_back(scalar::of(i32(0u - in[0].bits)));
}
void add_int(leaves in, result& out)
{
    out.push_back(scalar::of(i32(in[0].bits + in[1].bits)));
}
void subtract_int(leaves in, result& out)
{
    out.push_back(scalar::of(i32(in[0].bits - in[1].bits)));
}
void multiply_int(leaves in, result& out)
{
    out.push_back(scalar::of(i32(in[0].bits * in[1].bits)));
}
void less_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() < in[1].as_int()));
}
void less_equal_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() <= in[1].as_int()));
}
void greater_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() > in[1].as_int()));
}
void greater_equal_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() >= in[1].as_int()));
}
/// Of `int` and of `bool` alike: both are their bits.
void equal_bits(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits == in[1].bits));
}
void not_equal_bits(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits != in[1].bits));
}
void abs_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() < 0 ? i32(0u - in[0].bits) : in[0].as_int()));
}
void min_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[1].as_int() < in[0].as_int() ? in[1].as_int() : in[0].as_int()));
}
void max_int(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].as_int() < in[1].as_int() ? in[1].as_int() : in[0].as_int()));
}
void clamp_int(leaves in, result& out)
{
    auto const low = in[0].as_int() < in[1].as_int() ? in[1].as_int() : in[0].as_int();
    out.push_back(scalar::of(in[2].as_int() < low ? in[2].as_int() : low));
}

// ---- uint: every operation is on the bits, which is exactly unsigned wrapping ----------------------------------------

void add_uint(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[0].bits + in[1].bits));
}
void subtract_uint(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[0].bits - in[1].bits));
}
void multiply_uint(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[0].bits * in[1].bits));
}
void less_uint(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits < in[1].bits));
}
void less_equal_uint(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits <= in[1].bits));
}
void greater_uint(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits > in[1].bits));
}
void greater_equal_uint(leaves in, result& out)
{
    out.push_back(scalar::of(in[0].bits >= in[1].bits));
}
void min_uint(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[1].bits < in[0].bits ? in[1].bits : in[0].bits));
}
void max_uint(leaves in, result& out)
{
    out.push_back(scalar::of_uint(in[0].bits < in[1].bits ? in[1].bits : in[0].bits));
}
void clamp_uint(leaves in, result& out)
{
    auto const low = in[0].bits < in[1].bits ? in[1].bits : in[0].bits;
    out.push_back(scalar::of_uint(in[2].bits < low ? in[2].bits : low));
}

/// `a op b` of a short or a ushort, where `Op` indexes `+ - * / %` and `data` names the type.
/// MSL is C++, which computes a 16-bit integer as an int, so the result converts back and wraps as on every other
/// target; a ushort product multiplies as a uint, which no product of two of them overflows.
template <int Op>
written write_narrow(call_context const& c)
{
    constexpr cc::string_view ops[] = {"+", "-", "*", "/", "%"};
    auto const own = Op < 2 ? precedence::additive : precedence::multiplicative;
    if (c.target != language::msl)
        return write_infix(ops[Op], own, c.arguments[0], c.arguments[1]);
    auto const& type = c.builtins.at(builtin_type_id(i32(c.data)));
    auto lhs = c.arguments[0];
    if (Op == 2 && type.leaf_kind == check::value_kind::scalar_ushort)
        lhs = {.text = cc::format("uint({})", lhs.text)};
    return {.text = cc::format("{}({})", type.msl, write_infix(ops[Op], own, lhs, c.arguments[1]).text)};
}

/// `-x` of a short, which MSL converts back from the int it computes.
written write_narrow_negate(call_context const& c)
{
    auto const negated = cc::format("-{}", wrapped(c.arguments[0], precedence::unary));
    if (c.target != language::msl)
        return {.text = negated, .binds = precedence::unary};
    return {.text = cc::format("{}({})", c.builtins.at(builtin_type_id(i32(c.data))).msl, negated)};
}

struct comparison
{
    cc::string_view op;
    cc::string_view name;
    evaluator of_float;
    evaluator of_int;
    evaluator of_uint;
};
} // namespace

void sgl::builtins::register_scalar_math(registry& r)
{
    using namespace impl;

    r.add_comment("// @pure: a call has no effect, so nobody can tell whether or when it ran.\n"
                  "// A @builtin without it is assumed to have one, and the compiler then keeps its place among its "
                  "neighbours.\n"
                  "//\n"
                  "// An @operator function is found through its operator alone: its name is documentation, and no "
                  "lookup sees it.");

    r.add_comment("// float, and half, which computes as float does and rounds each result to 16 bits");
    for (auto const type : {cc::string_view("float"), cc::string_view("half")})
    {
        add_infix(r, "*", cc::format("multiply{}", suffix_of(type)), type, type, type, multiply);
        add_infix(r, "/", cc::format("divide{}", suffix_of(type)), type, type, type, divide);
        add_operator(r, "%", cc::format("remainder{}", suffix_of(type)), type, type, type, remainder_floats,
                     float_remainder());
        add_infix(r, "+", cc::format("add{}", suffix_of(type)), type, type, type, add);
        add_infix(r, "-", cc::format("subtract{}", suffix_of(type)), type, type, type, subtract);
        add_negate(r, cc::format("negate{}", suffix_of(type)), type, negate);
    }

    r.add_comment("// int: `/` and `%` truncate toward zero, and a zero divisor has no value on any target");
    // WGSL folds an int of constants exactly, so a constant outside the ints is refused rather than wrapped (CHK-312)
    auto const constant_rule = [&](undefined_check unrepresentable, judged_operand judged = judged_operand::none)
    {
        r.functions.back().unrepresentable_when_constant = unrepresentable;
        r.functions.back().judged_last = judged;
    };
    // short has no constant rule of WGSL's, which has no short (EMIT-109)
    for (auto const type : {cc::string_view("int"), cc::string_view("short")})
    {
        auto const is_int = type == "int";
        auto const named = [&](cc::string_view name) { return cc::format("{}{}", name, suffix_of(type)); };
        auto const data = u32(index_of(registered_type(r, type)));
        auto const written = [&](cc::string_view op, custom_writer narrow)
        { return is_int ? infix(op) : spelling{.kind = spelling_kind::custom, .custom = narrow, .data = data}; };
        add_operator(r, "*", named("multiply"), type, type, type, multiply_int, written("*", write_narrow<2>));
        constant_rule(is_int ? product_unrepresentable : nullptr);
        add_operator(r, "/", named("divide"), type, type, type, divide_integers, written("/", write_narrow<3>),
                     integer_division_undefined);
        constant_rule(nullptr, judged_operand::divisor);
        add_operator(r, "%", named("remainder"), type, type, type, remainder_integers, written("%", write_narrow<4>),
                     integer_division_undefined);
        constant_rule(nullptr, judged_operand::divisor);
        add_operator(r, "+", named("add"), type, type, type, add_int, written("+", write_narrow<0>));
        constant_rule(is_int ? sum_unrepresentable : nullptr);
        add_operator(r, "-", named("subtract"), type, type, type, subtract_int, written("-", write_narrow<1>));
        constant_rule(is_int ? difference_unrepresentable : nullptr);
        if (is_int)
            add_negate(r, named("negate"), type, negate_int);
        else
            r.add(function_record{
                .signature = cc::format("@pure @operator(\"-\") fun {}(x: {}) -> {}", named("negate"), type, type),
                .evaluate = negate_int,
                .write = {.kind = spelling_kind::custom, .custom = write_narrow_negate, .data = data},
            });
        constant_rule(is_int ? negation_unrepresentable : nullptr);
        add_function(r, "abs", {"x", type}, type, abs_int);
        constant_rule(is_int ? negation_unrepresentable : nullptr);
        add_function(r, "min", {"a", type, "b", type}, type, min_int);
        add_function(r, "max", {"a", type, "b", type}, type, max_int);
        add_function(r, "clamp", {"x", type, "low", type, "high", type}, type, clamp_int, {}, {}, clamp_undefined);
    }

    r.add_comment("// uint: a zero divisor has no value, as for int");
    for (auto const type : {cc::string_view("uint"), cc::string_view("ushort")})
    {
        auto const named = [&](cc::string_view name) { return cc::format("{}{}", name, suffix_of(type)); };
        auto const data = u32(index_of(registered_type(r, type)));
        auto const written = [&](cc::string_view op, custom_writer narrow)
        { return type == "uint" ? infix(op) : spelling{.kind = spelling_kind::custom, .custom = narrow, .data = data}; };
        add_operator(r, "*", named("multiply"), type, type, type, multiply_uint, written("*", write_narrow<2>));
        add_operator(r, "/", named("divide"), type, type, type, divide_integers, written("/", write_narrow<3>),
                     integer_division_undefined);
        constant_rule(nullptr, judged_operand::divisor);
        add_operator(r, "%", named("remainder"), type, type, type, remainder_integers, written("%", write_narrow<4>),
                     integer_division_undefined);
        constant_rule(nullptr, judged_operand::divisor);
        add_operator(r, "+", named("add"), type, type, type, add_uint, written("+", write_narrow<0>));
        add_operator(r, "-", named("subtract"), type, type, type, subtract_uint, written("-", write_narrow<1>));
        add_function(r, "min", {"a", type, "b", type}, type, min_uint);
        add_function(r, "max", {"a", type, "b", type}, type, max_uint);
        add_function(r, "clamp", {"x", type, "low", type, "high", type}, type, clamp_uint, {}, {}, clamp_undefined);
    }

    r.add_comment("// comparisons, of every numeric scalar");
    comparison const comparisons[] = {
        {.op = "<", .name = "less", .of_float = less, .of_int = less_int, .of_uint = less_uint},
        {.op = "<=", .name = "less_equal", .of_float = less_equal, .of_int = less_equal_int, .of_uint = less_equal_uint},
        {.op = ">", .name = "greater", .of_float = greater, .of_int = greater_int, .of_uint = greater_uint},
        {.op = ">=",
         .name = "greater_equal",
         .of_float = greater_equal,
         .of_int = greater_equal_int,
         .of_uint = greater_equal_uint},
        {.op = "==", .name = "equal", .of_float = equal, .of_int = equal_bits, .of_uint = equal_bits},
        {.op = "!=", .name = "not_equal", .of_float = not_equal, .of_int = not_equal_bits, .of_uint = not_equal_bits},
    };
    for (auto const type : {cc::string_view("float"), cc::string_view("half")})
        for (auto const& c : comparisons)
            add_infix(r, c.op, cc::format("{}{}", c.name, suffix_of(type)), type, type, "bool", c.of_float);
    for (auto const type : {cc::string_view("int"), cc::string_view("short")})
        for (auto const& c : comparisons)
            add_infix(r, c.op, cc::format("{}{}", c.name, suffix_of(type)), type, type, "bool", c.of_int);
    for (auto const type : {cc::string_view("uint"), cc::string_view("ushort")})
        for (auto const& c : comparisons)
            add_infix(r, c.op, cc::format("{}{}", c.name, suffix_of(type)), type, type, "bool", c.of_uint);

    r.add_comment("// bool: `and`, `or` and `not` are the language's own, since no function could leave an operand "
                  "unevaluated");
    add_infix(r, "==", "equal_bool", "bool", "bool", "bool", equal_bits);
    add_infix(r, "!=", "not_equal_bool", "bool", "bool", "bool", not_equal_bits);
}
