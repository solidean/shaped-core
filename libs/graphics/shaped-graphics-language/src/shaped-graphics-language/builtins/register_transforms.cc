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

/// Element (row, column) of the transpose of `matrix`, which has the same determinant.
f64 transposed_at(leaves matrix, isize row, isize column)
{
    return f64(matrix[row * 4 + column].as_float());
}

/// The determinant of rows 1 to 3 of `matrix` without column `skip`: the minor of element (0, skip).
f64 minor_of(leaves matrix, isize skip)
{
    isize kept[3] = {0, 0, 0};
    auto count = isize(0);
    for (auto column = isize(0); column < 4; ++column)
        if (column != skip)
            kept[count++] = column;
    auto const at = [&](isize row, isize k) { return transposed_at(matrix, row, kept[k]); };
    return at(1, 0) * (at(2, 1) * at(3, 2) - at(2, 2) * at(3, 1)) - at(1, 1) * (at(2, 0) * at(3, 2) - at(2, 2) * at(3, 0))
         + at(1, 2) * (at(2, 0) * at(3, 1) - at(2, 1) * at(3, 0));
}

/// Cofactor expansion along the first row, in f64 and rounded once.
void determinant(leaves in, result& out)
{
    auto sum = 0.0;
    for (auto column = isize(0); column < 4; ++column)
    {
        auto const term = transposed_at(in, 0, column) * minor_of(in, column);
        sum += column % 2 == 0 ? term : -term;
    }
    out.push_back(scalar::of(f32(sum)));
}

/// The columns, one behind the other, are the matrix's leaves already, for a `mat4` and a `mat3` alike.
void from_columns(leaves in, result& out)
{
    for (auto const& leaf : in)
        out.push_back(leaf);
}

/// Leaf c * n + r of an n by n matrix becomes leaf r * n + c: column c of the result is row c of `in`.
template <isize N>
void transpose(leaves in, result& out)
{
    for (auto column = isize(0); column < N; ++column)
        for (auto row = isize(0); row < N; ++row)
            out.push_back(in[row * N + column]);
}

/// WGSL's and MSL's constructor takes columns, and HLSL's takes rows.
written write_from_columns(call_context const& c)
{
    auto const mat4 = c.builtins.at(c.builtins.find_type("mat4")).spelled_in(c.target);
    auto const text = cc::format("{}({}, {}, {}, {})", mat4, c.arguments[0].text, c.arguments[1].text,
                                 c.arguments[2].text, c.arguments[3].text);
    if (c.target == language::hlsl)
        return {.text = cc::format("transpose({})", text)};
    return {.text = text};
}

/// Row `row` of `matrix * (x, y, z)`, for a `mat3`, whose element (row r, column c) is leaf c * 3 + r.
f32 transformed3(leaves matrix, isize row, f32 x, f32 y, f32 z)
{
    return matrix[row].as_float() * x + matrix[3 + row].as_float() * y + matrix[6 + row].as_float() * z;
}

void transform_float3(leaves in, result& out)
{
    for (auto row = isize(0); row < 3; ++row)
        out.push_back(scalar::of(transformed3(in, row, in[9].as_float(), in[10].as_float(), in[11].as_float())));
}

void multiply_mat3(leaves in, result& out)
{
    for (auto column = isize(0); column < 3; ++column)
    {
        auto const at = 9 + column * 3;
        for (auto row = isize(0); row < 3; ++row)
            out.push_back(
                scalar::of(transformed3(in, row, in[at].as_float(), in[at + 1].as_float(), in[at + 2].as_float())));
    }
}

/// HLSL's constructor takes rows, so the columns it is handed are transposed back.
written write_mat3_of_columns(call_context const& c)
{
    auto const columns = cc::format("{}, {}, {}", c.arguments[0].text, c.arguments[1].text, c.arguments[2].text);
    auto const mat3 = c.builtins.at(c.builtins.find_type("mat3")).spelled_in(c.target);
    if (c.target == language::hlsl)
        return {.text = cc::format("transpose({}({}))", mat3, columns)};
    return {.text = cc::format("{}({})", mat3, columns)};
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

constexpr cc::string_view k_mul[] = {"mul"};
constexpr cc::string_view k_transpose[] = {"transpose"};

void add_product(registry& r,
                 cc::string_view name,
                 cc::string_view matrix,
                 cc::string_view parameter,
                 cc::string_view rhs,
                 cc::string_view result_type,
                 evaluator evaluate,
                 custom_writer write)
{
    r.add(function_record{
        .signature
        = cc::format("@pure @operator(\"*\") fun {}(m: {}, {}: {}) -> {}", name, matrix, parameter, rhs, result_type),
        .evaluate = evaluate,
        .write = {.kind = spelling_kind::custom, .custom = write, .hlsl_names = k_mul},
    });
}
} // namespace

void sgl::builtins::register_transforms(registry& r)
{
    r.add_comment("// A matrix stands on the left; what it does to its right side depends on what that is:\n"
                  "// a position takes the translation (w = 1) and keeps its w, a direction does not (w = 0) and drops "
                  "it.");
    add_product(r, "transform_position", "mat4", "p", "pos3", "hpos4", transform_position, write_position);
    add_product(r, "transform_direction", "mat4", "v", "vec3", "vec3", transform_direction, write_direction);
    add_product(r, "transform_float4", "mat4", "v", "float4", "float4", transform_float4, write_product);
    add_product(r, "multiply_mat4", "mat4", "b", "mat4", "mat4", multiply_mat4, write_product);

    r.add_comment("// a matrix of its four columns");
    r.add(function_record{
        .signature = "@pure fun mat4(c0: float4, c1: float4, c2: float4, c3: float4) -> mat4",
        .doc = "/// `c0` to `c2` are where the axes go, and `c3` is where the origin goes.",
        .evaluate = from_columns,
        .write = {.kind = spelling_kind::custom, .custom = write_from_columns, .hlsl_names = k_transpose},
    });

    r.add_comment("// what a matrix is on its own; every target has it under this name");
    impl::add_function(r, "determinant", {"m", "mat4"}, "float", determinant, {},
                       "/// The factor `m` scales a volume by, negative where it mirrors and 0 where it flattens one.");
    impl::add_function(r, "transpose", {"m", "mat4"}, "mat4", transpose<4>, {},
                       "/// `m` mirrored across its diagonal: its rows as columns, which is its inverse where it only "
                       "rotates.");

    r.add_comment("// a mat3 is a value of function bodies alone (CHK-397): made of its columns, times a three-vector, "
                  "and transposed");
    r.add(function_record{
        .signature = "@pure fun mat3(c0: float3, c1: float3, c2: float3) -> mat3",
        .doc = "/// The matrix of three columns, `c0` first.",
        .evaluate = from_columns,
        .write = {.kind = spelling_kind::custom, .custom = write_mat3_of_columns, .hlsl_names = k_transpose},
    });
    add_product(r, "transform_float3", "mat3", "v", "float3", "float3", transform_float3, write_product);
    add_product(r, "transform_direction3", "mat3", "v", "vec3", "vec3", transform_float3, write_product);
    add_product(r, "multiply_mat3", "mat3", "b", "mat3", "mat3", multiply_mat3, write_product);
    impl::add_function(r, "transpose", {"m", "mat3"}, "mat3", transpose<3>, {},
                       "/// `m` mirrored across its diagonal: its rows as columns, which is its inverse where it only "
                       "rotates.");
}
