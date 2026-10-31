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

/// The columns, one behind the other, are the matrix's leaves already, for every matrix alike.
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

/// Column `i` of an n by n matrix is its leaves from n * i on; row `i` takes one from each column.
template <isize N, bool IsRow>
void matrix_part(leaves in, result& out)
{
    auto const i = isize(in[N * N].as_int());
    for (auto k = isize(0); k < N; ++k)
        out.push_back(IsRow ? in[k * N + i] : in[i * N + k]);
}

/// A column or a row exists for an index from 0 below the matrix's size, and no target defines another.
template <isize N>
cc::string_view matrix_part_undefined(leaves in)
{
    auto const i = in[N * N].as_int();
    return i < 0 || i >= N ? "a matrix index outside the matrix" : cc::string_view();
}

/// WGSL's and MSL's `m[i]` is column `i`, and HLSL's is row `i`, since HLSL holds an SGL matrix as its transpose.
template <bool IsRow>
written write_matrix_part(call_context const& c)
{
    auto const is_native = (c.target == language::hlsl) == IsRow;
    auto const matrix
        = is_native ? wrapped(c.arguments[0], precedence::primary) : cc::format("transpose({})", c.arguments[0].text);
    return {.text = cc::format("{}[{}]", matrix, c.arguments[1].text)};
}

// An affine3's leaves are its three rows, one after the other; HLSL holds it as `float3x4`, its rows as rows, and
// WGSL and MSL as a `mat3x4f` / `float3x4` whose columns are those rows, so `v * a` there is `mul(a, v)` in HLSL.

/// Row `row` of an affine3 dotted with `(x, y, z, w)`.
f32 affine_row(leaves a, isize row, f32 x, f32 y, f32 z, f32 w)
{
    return a[row * 4].as_float() * x + a[row * 4 + 1].as_float() * y + a[row * 4 + 2].as_float() * z
         + a[row * 4 + 3].as_float() * w;
}

template <int W>
void affine_apply(leaves in, result& out)
{
    for (auto row = isize(0); row < 3; ++row)
        out.push_back(scalar::of(affine_row(in, row, in[12].as_float(), in[13].as_float(), in[14].as_float(),
                                            W < 0 ? in[15].as_float() : f32(W))));
}

void affine_row_at(leaves in, result& out)
{
    auto const i = isize(in[12].as_int());
    for (auto k = isize(0); k < 4; ++k)
        out.push_back(in[i * 4 + k]);
}

cc::string_view affine_row_undefined(leaves in)
{
    auto const i = in[12].as_int();
    return i < 0 || i >= 3 ? "a matrix index outside the matrix" : cc::string_view();
}

/// `a` times `(v, w)`, or times `v` itself where `w` is empty: HLSL's `mul`, and the row vector on the left elsewhere.
written write_affine_apply(call_context const& c, cc::string_view w)
{
    auto const float4 = c.builtins.at(c.builtins.find_type("float4")).spelled_in(c.target);
    auto const v = w.empty() ? c.arguments[1].text : cc::format("{}({}, {})", float4, c.arguments[1].text, w);
    if (c.target == language::hlsl)
        return {.text = cc::format("mul({}, {})", c.arguments[0].text, v)};
    return {.text = cc::format("({} * {})", v, wrapped(c.arguments[0], precedence::primary))};
}

written write_affine_point(call_context const& c)
{
    return write_affine_apply(c, "1.0");
}

written write_affine_direction(call_context const& c)
{
    return write_affine_apply(c, "0.0");
}

written write_affine_float4(call_context const& c)
{
    return write_affine_apply(c, "");
}

/// Every target's constructor of `float3x4` / `mat3x4f` takes the rows as they are held.
written write_affine_of_rows(call_context const& c)
{
    auto const spelled = c.builtins.at(c.builtins.find_type("affine3")).spelled_in(c.target);
    return {.text = cc::format("{}({}, {}, {})", spelled, c.arguments[0].text, c.arguments[1].text, c.arguments[2].text)};
}

/// Every target's `a[i]` is row `i`, as the affine3 is held.
written write_affine_row(call_context const& c)
{
    return {.text = cc::format("{}[{}]", wrapped(c.arguments[0], precedence::primary), c.arguments[1].text)};
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
written write_of_columns(call_context const& c, cc::string_view matrix)
{
    auto columns = cc::string();
    for (auto i = isize(0); i < c.arguments.size(); ++i)
        columns.appendf("{}{}", i == 0 ? "" : ", ", c.arguments[i].text);
    auto const spelled = c.builtins.at(c.builtins.find_type(matrix)).spelled_in(c.target);
    if (c.target == language::hlsl)
        return {.text = cc::format("transpose({}({}))", spelled, columns)};
    return {.text = cc::format("{}({})", spelled, columns)};
}

written write_mat3_of_columns(call_context const& c)
{
    return write_of_columns(c, "mat3");
}

written write_mat2_of_columns(call_context const& c)
{
    return write_of_columns(c, "mat2");
}

/// Row `row` of `matrix * (x, y)`, for a `mat2`, whose element (row r, column c) is leaf c * 2 + r.
f32 transformed2(leaves matrix, isize row, f32 x, f32 y)
{
    return matrix[row].as_float() * x + matrix[2 + row].as_float() * y;
}

void transform_float2(leaves in, result& out)
{
    for (auto row = isize(0); row < 2; ++row)
        out.push_back(scalar::of(transformed2(in, row, in[4].as_float(), in[5].as_float())));
}

void multiply_mat2(leaves in, result& out)
{
    for (auto column = isize(0); column < 2; ++column)
    {
        auto const at = 4 + column * 2;
        for (auto row = isize(0); row < 2; ++row)
            out.push_back(scalar::of(transformed2(in, row, in[at].as_float(), in[at + 1].as_float())));
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
    // a clip-space position stays one through a further projective matrix, as a model-then-view-projection chain is
    add_product(r, "transform_hpos4", "mat4", "v", "hpos4", "hpos4", transform_float4, write_product);
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
    r.add(function_record{
        .signature = "@pure fun mat3(c0: vec3, c1: vec3, c2: vec3) -> mat3",
        .doc = "/// A basis of three directions, `c0` first: where x, y and z go, as a tangent frame is built.",
        .evaluate = from_columns,
        .write = {.kind = spelling_kind::custom, .custom = write_mat3_of_columns, .hlsl_names = k_transpose},
    });
    add_product(r, "transform_float3", "mat3", "v", "float3", "float3", transform_float3, write_product);
    add_product(r, "transform_direction3", "mat3", "v", "vec3", "vec3", transform_float3, write_product);
    add_product(r, "multiply_mat3", "mat3", "b", "mat3", "mat3", multiply_mat3, write_product);
    impl::add_function(r, "transpose", {"m", "mat3"}, "mat3", transpose<3>, {},
                       "/// `m` mirrored across its diagonal: its rows as columns, which is its inverse where it only "
                       "rotates.");

    r.add_comment("// one column or one row of a matrix, which core.sgl gives every matrix as `m.column(i)` and "
                  "`m.row(i)`");
    struct matrix_shape
    {
        cc::string_view matrix;
        cc::string_view vector;
        evaluator column;
        evaluator row;
        undefined_check undefined;
    };
    matrix_shape const shapes[] = {
        {"mat4", "float4", matrix_part<4, false>, matrix_part<4, true>, matrix_part_undefined<4>},
        {"mat3", "float3", matrix_part<3, false>, matrix_part<3, true>, matrix_part_undefined<3>},
        {"mat2", "float2", matrix_part<2, false>, matrix_part<2, true>, matrix_part_undefined<2>},
    };
    for (auto const& shape : shapes)
        for (auto const is_row : {false, true})
            r.add(function_record{
                .signature = cc::format("@internal @pure fun matrix_{}(m: {}, i: int) -> {}", is_row ? "row" : "column",
                                        shape.matrix, shape.vector),
                .evaluate = is_row ? shape.row : shape.column,
                .undefined_when = shape.undefined,
                .write = {.kind = spelling_kind::custom,
                          .custom = is_row ? write_matrix_part<true> : write_matrix_part<false>,
                          .hlsl_names = k_transpose,
                          .wgsl_names = k_transpose,
                          .msl_names = k_transpose},
            });

    r.add_comment("// an affine3 translates a position and leaves a direction alone (CHK-408); core.sgl names its "
                  "parts");
    r.add(function_record{
        .signature = "@internal @pure fun affine3_of_rows(r0: float4, r1: float4, r2: float4) -> affine3",
        .evaluate = from_columns,
        .write = {.kind = spelling_kind::custom, .custom = write_affine_of_rows},
    });
    r.add(function_record{
        .signature = "@internal @pure fun affine3_row(a: affine3, i: int) -> float4",
        .evaluate = affine_row_at,
        .undefined_when = affine_row_undefined,
        .write = {.kind = spelling_kind::custom, .custom = write_affine_row},
    });
    add_product(r, "transform_position_affine", "affine3", "p", "pos3", "pos3", affine_apply<1>, write_affine_point);
    add_product(r, "transform_direction_affine", "affine3", "v", "vec3", "vec3", affine_apply<0>, write_affine_direction);
    add_product(r, "transform_float4_affine", "affine3", "v", "float4", "float3", affine_apply<-1>, write_affine_float4);

    r.add_comment("// a mat2 is a mat3 of the plane: a value of function bodies alone, made of its columns, times a "
                  "float2");
    r.add(function_record{
        .signature = "@pure fun mat2(c0: float2, c1: float2) -> mat2",
        .doc = "/// The matrix of two columns, `c0` first.",
        .evaluate = from_columns,
        .write = {.kind = spelling_kind::custom, .custom = write_mat2_of_columns, .hlsl_names = k_transpose},
    });
    add_product(r, "transform_float2", "mat2", "v", "float2", "float2", transform_float2, write_product);
    add_product(r, "multiply_mat2", "mat2", "b", "mat2", "mat2", multiply_mat2, write_product);
    impl::add_function(r, "transpose", {"m", "mat2"}, "mat2", transpose<2>, {},
                       "/// `m` mirrored across its diagonal: its rows as columns, which is its inverse where it only "
                       "rotates.");
}
