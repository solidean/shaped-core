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

struct comparison
{
    cc::string_view op;
    cc::string_view name;
    evaluator of_float;
    evaluator of_int;
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

    r.add_comment("// float");
    add_infix(r, "*", "multiply", "float", "float", "float", multiply);
    add_infix(r, "/", "divide", "float", "float", "float", divide);
    add_infix(r, "+", "add", "float", "float", "float", add);
    add_infix(r, "-", "subtract", "float", "float", "float", subtract);
    add_negate(r, "negate", "float", negate);

    r.add_comment("// int: division is left out until the language says what dividing by zero is");
    add_infix(r, "*", "multiply_int", "int", "int", "int", multiply_int);
    add_infix(r, "+", "add_int", "int", "int", "int", add_int);
    add_infix(r, "-", "subtract_int", "int", "int", "int", subtract_int);
    add_negate(r, "negate_int", "int", negate_int);
    add_function(r, "abs", {"x", "int"}, "int", abs_int);
    add_function(r, "min", {"a", "int", "b", "int"}, "int", min_int);
    add_function(r, "max", {"a", "int", "b", "int"}, "int", max_int);
    add_function(r, "clamp", {"x", "int", "low", "int", "high", "int"}, "int", clamp_int);

    r.add_comment("// comparisons, of float and of int");
    comparison const comparisons[] = {
        {.op = "<", .name = "less", .of_float = less, .of_int = less_int},
        {.op = "<=", .name = "less_equal", .of_float = less_equal, .of_int = less_equal_int},
        {.op = ">", .name = "greater", .of_float = greater, .of_int = greater_int},
        {.op = ">=", .name = "greater_equal", .of_float = greater_equal, .of_int = greater_equal_int},
        {.op = "==", .name = "equal", .of_float = equal, .of_int = equal_bits},
        {.op = "!=", .name = "not_equal", .of_float = not_equal, .of_int = not_equal_bits},
    };
    for (auto const& c : comparisons)
        add_infix(r, c.op, c.name, "float", "float", "bool", c.of_float);
    for (auto const& c : comparisons)
        add_infix(r, c.op, cc::format("{}_int", c.name), "int", "int", "bool", c.of_int);

    r.add_comment("// bool: `and`, `or` and `not` are the language's own, since no function could leave an operand "
                  "unevaluated");
    add_infix(r, "==", "equal_bool", "bool", "bool", "bool", equal_bits);
    add_infix(r, "!=", "not_equal_bool", "bool", "bool", "bool", not_equal_bits);
}
