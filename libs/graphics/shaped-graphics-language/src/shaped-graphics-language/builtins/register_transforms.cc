#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

namespace
{
using check::scalar;
using leaves = cc::span<scalar const>;
using result = cc::vector<scalar>;

// Column-major: element (row r, column c) of a matrix is leaf c * 4 + r.

/// Row `row` of `matrix * (x, y, z, w)`.
f32 transformed(leaves matrix, isize row, f32 x, f32 y, f32 z, f32 w)
{
    return matrix[row].as_float() * x + matrix[4 + row].as_float() * y + matrix[8 + row].as_float() * z
         + matrix[12 + row].as_float() * w;
}

void transform_position(leaves in, result& out)
{
    for (auto row = isize(0); row < 4; ++row)
        out.push_back(scalar::of(transformed(in, row, in[16].as_float(), in[17].as_float(), in[18].as_float(), 1.0f)));
}

void transform_direction(leaves in, result& out)
{
    for (auto row = isize(0); row < 3; ++row)
        out.push_back(scalar::of(transformed(in, row, in[16].as_float(), in[17].as_float(), in[18].as_float(), 0.0f)));
}

void transform_float4(leaves in, result& out)
{
    for (auto row = isize(0); row < 4; ++row)
        out.push_back(scalar::of(
            transformed(in, row, in[16].as_float(), in[17].as_float(), in[18].as_float(), in[19].as_float())));
}

/// Column c of the product is the left matrix times column c of the right one.
void multiply_mat4(leaves in, result& out)
{
    for (auto column = isize(0); column < 4; ++column)
    {
        auto const at = 16 + column * 4;
        for (auto row = isize(0); row < 4; ++row)
            out.push_back(scalar::of(transformed(in, row, in[at].as_float(), in[at + 1].as_float(),
                                                 in[at + 2].as_float(), in[at + 3].as_float())));
    }
}

/// A matrix times what stands to its right: HLSL has a function for it, and every other target the operator.
written product(call_context const& c, written lhs, written rhs)
{
    if (c.target == language::hlsl)
        return {.text = cc::format("mul({}, {})", lhs.text, rhs.text)};
    return write_infix("*", precedence::multiplicative, cc::move(lhs), cc::move(rhs));
}

/// `(v, w)` as the target's four-vector, which is what a matrix takes.
written widened(call_context const& c, written const& v, cc::string_view w)
{
    auto const float4 = c.builtins.at(c.builtins.find_type("float4")).spelled_in(c.target);
    return {.text = cc::format("{}({}, {})", float4, v.text, w)};
}

written write_position(call_context const& c)
{
    return product(c, c.arguments[0], widened(c, c.arguments[1], "1.0"));
}

written write_direction(call_context const& c)
{
    return {.text = cc::format(
                "{}.xyz", wrapped(product(c, c.arguments[0], widened(c, c.arguments[1], "0.0")), precedence::primary))};
}

written write_product(call_context const& c)
{
    return product(c, c.arguments[0], c.arguments[1]);
}

void add_product(registry& r,
                 cc::string_view name,
                 cc::string_view parameter,
                 cc::string_view rhs,
                 cc::string_view result_type,
                 evaluator evaluate,
                 custom_writer write)
{
    r.add(function_record{
        .signature = cc::format("@pure @operator(\"*\") fun {}(m: mat4, {}: {}) -> {}", name, parameter, rhs, result_type),
        .evaluate = evaluate,
        .write = {.kind = spelling_kind::custom, .custom = write},
    });
}
} // namespace

void sgl::builtins::register_transforms(registry& r)
{
    r.add_comment("// A matrix stands on the left; what it does to its right side depends on what that is:\n"
                  "// a position takes the translation (w = 1) and keeps its w, a direction does not (w = 0) and drops "
                  "it.");
    add_product(r, "transform_position", "p", "pos3", "hpos4", transform_position, write_position);
    add_product(r, "transform_direction", "v", "vec3", "vec3", transform_direction, write_direction);
    add_product(r, "transform_float4", "v", "float4", "float4", transform_float4, write_product);
    add_product(r, "multiply_mat4", "b", "mat4", "mat4", multiply_mat4, write_product);
}
