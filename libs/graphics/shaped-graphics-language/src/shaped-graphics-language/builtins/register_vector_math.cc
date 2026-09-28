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

    cc::string_view const numbers[] = {"float", "float2", "float3", "float4"};
    cc::string_view const plain_vectors[] = {"float2", "float3", "float4"};
    cc::string_view const scaled[] = {"float2", "float3", "float4", "vec3"};
    cc::string_view const measured[] = {"vec3", "float2", "float3", "float4"};
    cc::string_view const integer_vectors[] = {"int2", "int3", "int4", "uint2", "uint3", "uint4"};

    r.add_comment("// what a float and a plain vector of floats share, component by component");
    for (auto const type : numbers)
    {
        add_function(r, "saturate", {"x", type}, type, saturate);
        add_function(r, "abs", {"x", type}, type, abs_of);
        add_function(r, "min", {"a", type, "b", type}, type, min_of);
        add_function(r, "max", {"a", type, "b", type}, type, max_of);
        add_function(r, "clamp", {"x", type, "low", type, "high", type}, type, clamp);
        add_function(r, "mix", {"a", type, "b", type, "t", "float"}, type, mix, {.hlsl = "lerp"},
                     "/// `a` where `t` is 0 and `b` where it is 1.");
    }

    r.add_comment("// float3 and float4 are plain numbers, so all of their arithmetic is componentwise");
    for (auto const type : plain_vectors)
    {
        add_infix(r, "+", named("add", type), type, type, type, add);
        add_infix(r, "-", named("subtract", type), type, type, type, subtract);
        add_infix(r, "*", named("multiply", type), type, type, type, multiply);
        add_infix(r, "/", named("divide", type), type, type, type, divide);
    }

    r.add_comment("// integer vectors wrap componentwise, and division is left out as it is for their scalars");
    for (auto const type : integer_vectors)
    {
        add_infix(r, "+", named("add", type), type, type, type, add_bits);
        add_infix(r, "-", named("subtract", type), type, type, type, subtract_bits);
        add_infix(r, "*", named("multiply", type), type, type, type, multiply_bits);
    }

    r.add_comment("// a direction adds to a direction; what `vec3 * vec3` would mean is a question, so it is no "
                  "operator");
    add_infix(r, "+", "add_vec3", "vec3", "vec3", "vec3", add);
    add_infix(r, "-", "subtract_vec3", "vec3", "vec3", "vec3", subtract);

    r.add_comment("// scaling, from either side");
    for (auto const type : scaled)
    {
        add_infix(r, "*", named("scale", type), type, "float", type, scale);
        add_infix(r, "*", named("prescale", type), "float", type, type, prescale);
        add_infix(r, "/", named("unscale", type), type, "float", type, unscale);
        add_negate(r, named("negate", type), type, negate);
    }

    r.add_comment("// a position moves by a direction, and two positions differ by one; `pos3 + pos3` means nothing");
    add_infix(r, "+", "translate_pos3", "pos3", "vec3", "pos3", add);
    add_infix(r, "-", "translate_back_pos3", "pos3", "vec3", "pos3", subtract);
    add_infix(r, "-", "subtract_pos3", "pos3", "pos3", "vec3", subtract);

    r.add_comment("// lengths and angles");
    for (auto const type : measured)
    {
        add_function(r, "dot", {"a", type, "b", type}, "float", dot);
        add_function(r, "length", {"v", type}, "float", length);
        add_function(r, "normalize", {"v", type}, type, normalize);
    }
}
