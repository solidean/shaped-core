#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::scalar;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// Every evaluator here reads its width off `in`, so one serves `float`, `float3`, `float4` and `vec3` alike.

/// By iteration, since the library links no math; two runs of one tree meet the same bits either way.
f32 root_of(f32 x)
{
    if (!(x > 0.0f) || x - x != 0.0f)
        return x == 0.0f ? 0.0f : x - x;
    auto guess = x < 1.0f ? 1.0f : x;
    for (auto i = 0; i < 48; ++i)
        guess = 0.5f * (guess + x / guess);
    return guess;
}

/// Summed left to right, which is the order a run is compared by.
f32 dot_of(leaves a, leaves b)
{
    auto sum = a[0].as_float() * b[0].as_float();
    for (auto i = isize(1); i < a.size(); ++i)
        sum = sum + a[i].as_float() * b[i].as_float();
    return sum;
}

template <class Fn>
void pairwise(leaves in, result& out, Fn&& fn)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(fn(in[i].as_float(), in[width + i].as_float())));
}

void add(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return a + b; });
}
void subtract(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return a - b; });
}
void multiply(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return a * b; });
}
void divide(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return a / b; });
}
void min_of(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return b < a ? b : a; });
}
void max_of(leaves in, result& out)
{
    pairwise(in, out, [](f32 a, f32 b) { return a < b ? b : a; });
}

/// `v * s`: the scalar is the last leaf.
void scale(leaves in, result& out)
{
    auto const width = in.size() - 1;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(in[i].as_float() * in[width].as_float()));
}
/// `s * v`: the scalar is the first leaf.
void prescale(leaves in, result& out)
{
    for (auto i = isize(1); i < in.size(); ++i)
        out.push_back(scalar::of(in[0].as_float() * in[i].as_float()));
}
void unscale(leaves in, result& out)
{
    auto const width = in.size() - 1;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(in[i].as_float() / in[width].as_float()));
}
void negate(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of(-x.as_float()));
}

void saturate(leaves in, result& out)
{
    for (auto const& leaf : in)
    {
        auto const x = leaf.as_float();
        out.push_back(scalar::of(x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x)));
    }
}
void abs_of(leaves in, result& out)
{
    for (auto const& leaf : in)
        out.push_back(scalar::of(leaf.as_float() < 0.0f ? -leaf.as_float() : leaf.as_float()));
}
void clamp(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const x = in[i].as_float();
        auto const low = in[width + i].as_float();
        auto const high = in[2 * width + i].as_float();
        auto const raised = x < low ? low : x;
        out.push_back(scalar::of(high < raised ? high : raised));
    }
}
/// `t` is one float, whatever the width.
void mix(leaves in, result& out)
{
    auto const width = (in.size() - 1) / 2;
    auto const t = in[2 * width].as_float();
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(in[i].as_float() * (1.0f - t) + in[width + i].as_float() * t));
}

/// Componentwise on the bits, which is two's-complement wrapping for int and plain wrapping for uint alike.
template <class Fn>
void bitwise_pairs(leaves in, result& out, Fn&& fn)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back({.kind = in[i].kind, .bits = fn(in[i].bits, in[width + i].bits)});
}
void add_bits(leaves in, result& out)
{
    bitwise_pairs(in, out, [](u32 a, u32 b) { return a + b; });
}
void subtract_bits(leaves in, result& out)
{
    bitwise_pairs(in, out, [](u32 a, u32 b) { return a - b; });
}
void multiply_bits(leaves in, result& out)
{
    bitwise_pairs(in, out, [](u32 a, u32 b) { return a * b; });
}

// ---- integer vectors, componentwise; each leaf says whether it is an int or a uint ----------------------------------

/// `a < b` of two leaves of one kind.
bool is_below(scalar a, scalar b)
{
    if (a.kind == check::value_kind::scalar_float)
        return a.as_float() < b.as_float();
    if (a.kind == check::value_kind::scalar_int)
        return a.as_int() < b.as_int();
    return a.bits < b.bits;
}

void min_ints(leaves in, result& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(is_below(in[width + i], in[i]) ? in[width + i] : in[i]);
}
void max_ints(leaves in, result& out)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(is_below(in[i], in[width + i]) ? in[width + i] : in[i]);
}
void clamp_ints(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
    {
        auto const raised = is_below(in[i], in[width + i]) ? in[width + i] : in[i];
        out.push_back(is_below(in[2 * width + i], raised) ? in[2 * width + i] : raised);
    }
}
/// Wraps where it negates the most negative int, as every target does.
void abs_ints(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of(x.as_int() < 0 ? i32(0u - x.bits) : x.as_int()));
}
/// Wraps where it negates the most negative int, as every target does.
void negate_ints(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of(i32(0u - x.bits)));
}
void sign_ints(leaves in, result& out)
{
    for (auto const& x : in)
        out.push_back(scalar::of(x.as_int() < 0 ? -1 : x.as_int() > 0 ? 1 : 0));
}

/// MSL has no `sign` of an integer, and `clamp(x, T(-1), T(1))` is one; `data` names `T`.
/// HLSL's `sign` gives an `int` whatever it takes, which a 16-bit integer converts back.
written write_sign_ints(call_context const& c)
{
    auto const& type = c.builtins.at(builtin_type_id(i32(c.data)));
    if (c.target == language::msl)
    {
        if (type.leaf_count == 1 && !check::is_16_bit(type.leaf_kind))
            return {.text = cc::format("clamp({}, -1, 1)", c.arguments[0].text)};
        return {.text = cc::format("clamp({0}, {1}(-1), {1}(1))", c.arguments[0].text, type.msl)};
    }
    if (c.target == language::hlsl && check::is_16_bit(type.leaf_kind))
        return {.text = cc::format("{}(sign({}))", type.hlsl, c.arguments[0].text)};
    return {.text = cc::format("sign({})", c.arguments[0].text)};
}
constexpr cc::string_view k_sign[] = {"sign"};
constexpr cc::string_view k_clamp[] = {"clamp"};

// ---- comparisons, componentwise and whole ------------------------------------------------------------------------

/// Equal as the targets compare: a float as a float, so a NaN is unequal to itself and -0.0 equals 0.0.
bool is_same(scalar a, scalar b)
{
    return a.kind == check::value_kind::scalar_float ? a.as_float() == b.as_float() : a.bits == b.bits;
}

/// `op(a, b)` per component, which gives a bool vector.
template <class Op>
void compare_each(leaves in, result& out, Op&& op)
{
    auto const width = in.size() / 2;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(scalar::of(bool(op(in[i], in[width + i]))));
}
void less_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return is_below(a, b); });
}
void less_equal_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return is_below(a, b) || is_same(a, b); });
}
void greater_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return is_below(b, a); });
}
void greater_equal_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return is_below(b, a) || is_same(a, b); });
}
void equal_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return is_same(a, b); });
}
void not_equal_each(leaves in, result& out)
{
    compare_each(in, out, [](scalar a, scalar b) { return !is_same(a, b); });
}
void equal_whole(leaves in, result& out)
{
    auto each = result();
    equal_each(in, each);
    auto is_equal = true;
    for (auto const& x : each)
        is_equal = is_equal && x.as_bool();
    out.push_back(scalar::of(is_equal));
}
void not_equal_whole(leaves in, result& out)
{
    auto each = result();
    equal_whole(in, each);
    out.push_back(scalar::of(!each[0].as_bool()));
}
void any_of(leaves in, result& out)
{
    auto is_any = false;
    for (auto const& x : in)
        is_any = is_any || x.as_bool();
    out.push_back(scalar::of(is_any));
}
void all_of(leaves in, result& out)
{
    auto is_all = true;
    for (auto const& x : in)
        is_all = is_all && x.as_bool();
    out.push_back(scalar::of(is_all));
}

/// `all(a == b)` and `any(a != b)` on every target, which is the whole value's `==` and `!=` (EMIT-144).
template <bool IsEqual>
written write_whole(call_context const& c)
{
    auto const inner = write_infix(IsEqual ? "==" : "!=", precedence::comparison, c.arguments[0], c.arguments[1]);
    return {.text = cc::format("{}({})", IsEqual ? "all" : "any", inner.text)};
}
constexpr cc::string_view k_all[] = {"all"};
constexpr cc::string_view k_any[] = {"any"};

/// `cond` first, then `if_true` and `if_false`: a bool condition picks the whole value.
void select_whole(leaves in, result& out)
{
    auto const width = (in.size() - 1) / 2;
    out.push_back_range(in.subspan({.offset = in[0].as_bool() ? 1 : 1 + width, .size = width}));
}
/// A bool vector picks each component.
void select_each(leaves in, result& out)
{
    auto const width = in.size() / 3;
    for (auto i = isize(0); i < width; ++i)
        out.push_back(in[i].as_bool() ? in[width + i] : in[2 * width + i]);
}

/// `select(cond, if_true, if_false)` in HLSL, and `select(if_false, if_true, cond)` in WGSL and MSL (EMIT-145).
/// MSL's `select` takes a scalar condition beside scalars only, so `data` is the width it is spread to, or 0.
written write_select(call_context const& c)
{
    auto const& cond = c.arguments[0].text;
    auto const& if_true = c.arguments[1].text;
    auto const& if_false = c.arguments[2].text;
    if (c.target == language::hlsl)
        return {.text = cc::format("select({}, {}, {})", cond, if_true, if_false)};
    if (c.target == language::msl && c.data > 0)
        return {.text = cc::format("select({}, {}, bool{}({}))", if_false, if_true, c.data, cond)};
    return {.text = cc::format("select({}, {}, {})", if_false, if_true, cond)};
}
constexpr cc::string_view k_select[] = {"select"};

/// The scalar of a prelude vector or scalar by its name: `half` of `half3`, and `float` of `vec3`, `pos3` and `hpos4`.
cc::string_view element_of(cc::string_view type)
{
    for (auto const family : {"half", "ushort", "short", "uint", "int", "bool"})
        if (type.starts_with(family))
            return family;
    return "float";
}

bool is_16_bit_name(cc::string_view type)
{
    auto const element = element_of(type);
    return element == "half" || element == "short" || element == "ushort";
}

/// `v % s` in MSL is `fmod` of two vectors, since MSL's `fmod` takes no scalar beside a vector.
template <bool IsScalarLeft>
written write_spread_remainder(call_context const& c)
{
    if (c.target != language::msl)
        return write_infix("%", precedence::multiplicative, c.arguments[0], c.arguments[1]);
    auto const spread = impl::spread_arguments(c, IsScalarLeft ? 0 : 1);
    return {.text = cc::format("fmod({}, {})", spread[0].text, spread[1].text)};
}
constexpr cc::string_view k_fmod[] = {"fmod"};

void dot(leaves in, result& out)
{
    auto const width = in.size() / 2;
    out.push_back(
        scalar::of(dot_of(in.subspan({.offset = 0, .size = width}), in.subspan({.offset = width, .size = width}))));
}
void length(leaves in, result& out)
{
    out.push_back(scalar::of(root_of(dot_of(in, in))));
}
void normalize(leaves in, result& out)
{
    auto const size = root_of(dot_of(in, in));
    for (auto const& x : in)
        out.push_back(scalar::of(x.as_float() / size));
}
} // namespace

void sgl::builtins::register_vector_math(registry& r)
{
    using namespace impl;
    auto const named
        = [](cc::string_view name, cc::string_view type) { return cc::format("{}{}", name, suffix_of(type)); };

    cc::string_view const numbers[] = {"float", "float2", "float3", "float4", "half", "half2", "half3", "half4"};
    cc::string_view const plain_vectors[] = {"float2", "float3", "float4", "half2", "half3", "half4"};
    cc::string_view const scaled[] = {"float2", "float3", "float4", "vec3", "half2", "half3", "half4"};
    cc::string_view const measured[] = {"vec3", "float2", "float3", "float4", "half2", "half3", "half4"};
    cc::string_view const integer_vectors[] = {"int2",   "int3",   "int4",   "uint2",   "uint3",   "uint4",
                                               "short2", "short3", "short4", "ushort2", "ushort3", "ushort4"};

    r.add_comment("// what a float and a plain vector of floats share, component by component, and so do the halves");
    for (auto const type : numbers)
    {
        auto const element = element_of(type);
        add_function(r, "saturate", {"x", type}, type, saturate);
        add_function(r, "abs", {"x", type}, type, abs_of);
        add_function(r, "min", {"a", type, "b", type}, type, min_of);
        add_function(r, "max", {"a", type, "b", type}, type, max_of);
        add_function(r, "clamp", {"x", type, "low", type, "high", type}, type, clamp, {}, {}, clamp_undefined);
        add_function(r, "mix", {"a", type, "b", type, "t", element}, type, mix, {.hlsl = "lerp"},
                     "/// `a` where `t` is 0 and `b` where it is 1.");
    }

    r.add_comment("// float3 and float4 are plain numbers, so all of their arithmetic is componentwise");
    for (auto const type : plain_vectors)
    {
        add_infix(r, "+", named("add", type), type, type, type, add);
        add_infix(r, "-", named("subtract", type), type, type, type, subtract);
        add_infix(r, "*", named("multiply", type), type, type, type, multiply);
        add_infix(r, "/", named("divide", type), type, type, type, divide);
        add_operator(r, "%", named("remainder", type), type, type, type, remainder_floats, float_remainder());
    }

    r.add_comment("// integer vectors wrap componentwise, and divide componentwise as their scalars do");
    // WGSL, whose constant rules these are, has no 16-bit integer (EMIT-109)
    auto const constant_rule = [&](cc::string_view type, undefined_check unrepresentable)
    {
        if (!is_16_bit_name(type))
            r.functions.back().unrepresentable_when_constant = unrepresentable;
    };
    for (auto const type : integer_vectors)
    {
        add_infix(r, "+", named("add", type), type, type, type, add_bits);
        constant_rule(type, sum_unrepresentable);
        add_infix(r, "-", named("subtract", type), type, type, type, subtract_bits);
        constant_rule(type, difference_unrepresentable);
        add_infix(r, "*", named("multiply", type), type, type, type, multiply_bits);
        constant_rule(type, product_unrepresentable);
        add_infix(r, "/", named("divide", type), type, type, type, divide_integers, integer_division_undefined);
        r.functions.back().judged_last = judged_operand::divisor;
        add_infix(r, "%", named("remainder", type), type, type, type, remainder_integers, integer_division_undefined);
        r.functions.back().judged_last = judged_operand::divisor;
    }

    r.add_comment("// a plain vector and its scalar, from either side, componentwise (CHK-358);\n"
                  "// `*` and `/` of a float vector by a float are the scaling further below");
    auto const spread_infix = [&](cc::string_view op, cc::string_view name, cc::string_view vector,
                                  cc::string_view element, bool is_scalar_left, evaluator evaluate, spelling write)
    {
        auto const full = is_scalar_left ? cc::format("{}_scalar{}", name, suffix_of(vector))
                                         : cc::format("{}{}_scalar", name, suffix_of(vector));
        add_operator(r, op, full, is_scalar_left ? element : vector, is_scalar_left ? vector : element, vector,
                     evaluate, cc::move(write));
    };
    for (auto const type : plain_vectors)
    {
        auto const data = u32(index_of(registered_type(r, type)));
        auto const element = element_of(type);
        spread_infix("+", "add", type, element, false, spread_evaluate<add, false>, infix("+"));
        spread_infix("+", "add", type, element, true, spread_evaluate<add, true>, infix("+"));
        spread_infix("-", "subtract", type, element, false, spread_evaluate<subtract, false>, infix("-"));
        spread_infix("-", "subtract", type, element, true, spread_evaluate<subtract, true>, infix("-"));
        spread_infix("/", "divide", type, element, true, spread_evaluate<divide, true>, infix("/"));
        spread_infix(
            "%", "remainder", type, element, false, spread_evaluate<remainder_floats, false>,
            {.kind = spelling_kind::custom, .custom = write_spread_remainder<false>, .data = data, .msl_names = k_fmod});
        spread_infix(
            "%", "remainder", type, element, true, spread_evaluate<remainder_floats, true>,
            {.kind = spelling_kind::custom, .custom = write_spread_remainder<true>, .data = data, .msl_names = k_fmod});
    }
    for (auto const type : integer_vectors)
    {
        auto const element = element_of(type);
        for (auto const left : {false, true})
        {
            spread_infix("+", "add", type, element, left,
                         left ? spread_evaluate<add_bits, true> : spread_evaluate<add_bits, false>, infix("+"));
            constant_rule(type,
                          left ? spread_check<sum_unrepresentable, true> : spread_check<sum_unrepresentable, false>);
            spread_infix("-", "subtract", type, element, left,
                         left ? spread_evaluate<subtract_bits, true> : spread_evaluate<subtract_bits, false>, infix("-"));
            constant_rule(type, left ? spread_check<difference_unrepresentable, true>
                                     : spread_check<difference_unrepresentable, false>);
            spread_infix("*", "multiply", type, element, left,
                         left ? spread_evaluate<multiply_bits, true> : spread_evaluate<multiply_bits, false>, infix("*"));
            constant_rule(type, left ? spread_check<product_unrepresentable, true>
                                     : spread_check<product_unrepresentable, false>);
            spread_infix("/", "divide", type, element, left,
                         left ? spread_evaluate<divide_integers, true> : spread_evaluate<divide_integers, false>,
                         infix("/"));
            r.functions.back().undefined_when = left ? spread_check<integer_division_undefined, true>
                                                     : spread_check<integer_division_undefined, false>;
            r.functions.back().judged_last = judged_operand::divisor;
            spread_infix("%", "remainder", type, element, left,
                         left ? spread_evaluate<remainder_integers, true> : spread_evaluate<remainder_integers, false>,
                         infix("%"));
            r.functions.back().undefined_when = left ? spread_check<integer_division_undefined, true>
                                                     : spread_check<integer_division_undefined, false>;
            r.functions.back().judged_last = judged_operand::divisor;
        }
    }

    r.add_comment("// what the integers share with the floats, componentwise (CHK-359); `sign` of an int is an int,\n"
                  "// and the signed families negate, wrapping as their scalars do (CHK-358)");
    auto const add_sign = [&](cc::string_view type)
    {
        add_function(r, "sign", {"x", type}, type, sign_ints,
                     {.kind = spelling_kind::custom,
                      .custom = write_sign_ints,
                      .data = u32(index_of(registered_type(r, type))),
                      .hlsl_names = k_sign,
                      .wgsl_names = k_sign,
                      .msl_names = k_clamp});
    };
    add_sign("int");
    add_sign("short");
    for (auto const type : integer_vectors)
    {
        add_function(r, "min", {"a", type, "b", type}, type, min_ints);
        add_function(r, "max", {"a", type, "b", type}, type, max_ints);
        add_function(r, "clamp", {"x", type, "low", type, "high", type}, type, clamp_ints, {}, {}, clamp_undefined);
        if (!type.starts_with("int") && !type.starts_with("short"))
            continue;
        add_function(r, "abs", {"x", type}, type, abs_ints);
        constant_rule(type, negation_unrepresentable);
        add_sign(type);
        add_negate(r, named("negate", type), type, negate_ints);
        constant_rule(type, negation_unrepresentable);
    }

    r.add_comment("// two vectors compared: an ordering per component (CHK-362), and `==` and `!=` of the whole "
                  "value,\n"
                  "// which `equal` and `not_equal` are per component (CHK-363)");
    cc::string_view const ordered[]
        = {"float2", "float3", "float4", "int2",   "int3",   "int4",   "uint2",   "uint3",   "uint4",
           "half2",  "half3",  "half4",  "short2", "short3", "short4", "ushort2", "ushort3", "ushort4"};
    for (auto const type : ordered)
    {
        auto const bools = cc::format("bool{}", type.back());
        add_infix(r, "<", named("less", type), type, type, bools, less_each);
        add_infix(r, "<=", named("less_equal", type), type, type, bools, less_equal_each);
        add_infix(r, ">", named("greater", type), type, type, bools, greater_each);
        add_infix(r, ">=", named("greater_equal", type), type, type, bools, greater_equal_each);
    }
    cc::string_view const compared[]
        = {"float2", "float3", "float4", "int2",   "int3",   "int4",    "uint2",   "uint3",
           "uint4",  "bool2",  "bool3",  "bool4",  "vec3",   "pos3",    "hpos4",   "half2",
           "half3",  "half4",  "short2", "short3", "short4", "ushort2", "ushort3", "ushort4"};
    for (auto const type : compared)
    {
        auto const bools = cc::format("bool{}", type.back());
        add_operator(r, "==", named("same", type), type, type, "bool", equal_whole,
                     {.kind = spelling_kind::custom,
                      .custom = write_whole<true>,
                      .hlsl_names = k_all,
                      .wgsl_names = k_all,
                      .msl_names = k_all});
        add_operator(r, "!=", named("differs", type), type, type, "bool", not_equal_whole,
                     {.kind = spelling_kind::custom,
                      .custom = write_whole<false>,
                      .hlsl_names = k_any,
                      .wgsl_names = k_any,
                      .msl_names = k_any});
        add_function(r, "equal", {"a", type, "b", type}, bools, equal_each, infix("=="));
        add_function(r, "not_equal", {"a", type, "b", type}, bools, not_equal_each, infix("!="));
    }

    r.add_comment("// a bool vector read whole (CHK-364), and `select`, which evaluates all three of its arguments\n"
                  "// and gives `if_true` where `cond` holds (CHK-365)");
    for (auto const type : {cc::string_view("bool2"), cc::string_view("bool3"), cc::string_view("bool4")})
    {
        add_function(r, "any", {"m", type}, "bool", any_of, {}, "/// Whether some component of `m` is true.");
        add_function(r, "all", {"m", type}, "bool", all_of, {}, "/// Whether every component of `m` is true.");
    }
    for (auto const type : k_selectable)
    {
        auto const width = type.back() >= '2' && type.back() <= '4' ? u32(type.back() - '0') : u32(0);
        add_function(r, "select", {"cond", "bool", "if_true", type, "if_false", type}, type, select_whole,
                     {.kind = spelling_kind::custom,
                      .custom = write_select,
                      .data = width,
                      .hlsl_names = k_select,
                      .wgsl_names = k_select,
                      .msl_names = k_select});
        if (width == 0 || type == "vec3" || type == "pos3" || type == "hpos4")
            continue;
        auto const bools = cc::format("bool{}", width);
        add_function(r, "select", {"cond", bools, "if_true", type, "if_false", type}, type, select_each,
                     {.kind = spelling_kind::custom,
                      .custom = write_select,
                      .hlsl_names = k_select,
                      .wgsl_names = k_select,
                      .msl_names = k_select});
    }

    r.add_comment("// a direction adds to a direction; what `vec3 * vec3` would mean is a question, so it is no "
                  "operator");
    add_infix(r, "+", "add_vec3", "vec3", "vec3", "vec3", add);
    add_infix(r, "-", "subtract_vec3", "vec3", "vec3", "vec3", subtract);

    r.add_comment("// scaling, from either side");
    for (auto const type : scaled)
    {
        auto const element = element_of(type);
        add_infix(r, "*", named("scale", type), type, element, type, scale);
        add_infix(r, "*", named("prescale", type), element, type, type, prescale);
        add_infix(r, "/", named("unscale", type), type, element, type, unscale);
        add_negate(r, named("negate", type), type, negate);
    }

    r.add_comment("// a position moves by a direction, and two positions differ by one; `pos3 + pos3` means nothing");
    add_infix(r, "+", "translate_pos3", "pos3", "vec3", "pos3", add);
    add_infix(r, "-", "translate_back_pos3", "pos3", "vec3", "pos3", subtract);
    add_infix(r, "-", "subtract_pos3", "pos3", "pos3", "vec3", subtract);

    r.add_comment("// lengths and angles");
    for (auto const type : measured)
    {
        add_function(r, "dot", {"a", type, "b", type}, element_of(type), dot);
        add_function(r, "length", {"v", type}, element_of(type), length);
        add_function(r, "normalize", {"v", type}, type, normalize);
    }
}
