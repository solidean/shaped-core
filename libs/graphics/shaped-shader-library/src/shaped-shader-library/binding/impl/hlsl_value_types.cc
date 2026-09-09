#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-shader-library/binding/impl/hlsl_value_types.hh>

using namespace cc::primitive_defines;

namespace
{
using sg::vertex_attribute_format;

/// One row of the table.
/// `format` is read only when `is_vertex_attribute`, which keeps the table constexpr -- a cc::optional in it
/// would not be.
struct table_entry
{
    cc::string_view name;
    cc::string_view cpp_type;
    isize size = 4;
    isize constant_block_align = 4;
    isize cpp_align = 4;
    bool is_vertex_attribute = false;
    vertex_attribute_format format = vertex_attribute_format::f32;
};

// The mirror spells its members as plain `float` / `int` / `unsigned` and friends rather than reaching for a
// vector type, because generated package code sits below anything that could define one.
constexpr table_entry k_table[] = {
    {"float", "float", 4, 4, 4, true, vertex_attribute_format::f32},
    {"float2", "float[2]", 8, 4, 4, true, vertex_attribute_format::vec2f},
    {"float3", "float[3]", 12, 4, 4, true, vertex_attribute_format::vec3f},
    {"float4", "float[4]", 16, 4, 4, true, vertex_attribute_format::vec4f},

    {"int", "int", 4, 4, 4, true, vertex_attribute_format::i32},
    {"int2", "int[2]", 8, 4, 4, true, vertex_attribute_format::vec2i},
    {"int3", "int[3]", 12, 4, 4, true, vertex_attribute_format::vec3i},
    {"int4", "int[4]", 16, 4, 4, true, vertex_attribute_format::vec4i},

    {"uint", "unsigned", 4, 4, 4, true, vertex_attribute_format::u32},
    {"uint2", "unsigned[2]", 8, 4, 4, true, vertex_attribute_format::vec2u},
    {"uint3", "unsigned[3]", 12, 4, 4, true, vertex_attribute_format::vec3u},
    {"uint4", "unsigned[4]", 16, 4, 4, true, vertex_attribute_format::vec4u},

    // Four bytes in a constant block, and no vertex attribute format at all -- the reason sr::gpu_boolean exists.
    {"bool", "unsigned", 4, 4, 4},

    // A matrix is keyed on its ORIENTATION as well as its shape, because that is what its layout depends on --
    // and the orientation is part of the declaration rather than something the pass has to guess.
    // A bare `float4x4` is refused for that reason: its default comes from `#pragma pack_matrix` or `-Zpr`, which
    // the pass cannot see, and it decides whether the mirror's sixteen floats are read as rows or as columns.
    //
    // The shapes admitted are those whose stored vectors are full float4s, so the matrix is exactly V rows of 16
    // with no partial tail: row-major stores R vectors of C, column-major stores C vectors of R.
    // Q14g measured what a partial tail costs -- the next member packs into it on DXIL and the SPIR-V validator
    // calls that an overlap, since it measures the matrix as `stride * V` and DXC does not.
    {"row_major float1x4", "float[4]", 16, 16, 4},
    {"row_major float2x4", "float[8]", 32, 16, 4},
    {"row_major float3x4", "float[12]", 48, 16, 4},
    {"row_major float4x4", "float[16]", 64, 16, 4},

    {"column_major float4x1", "float[4]", 16, 16, 4},
    {"column_major float4x2", "float[8]", 32, 16, 4},
    {"column_major float4x3", "float[12]", 48, 16, 4},
    {"column_major float4x4", "float[16]", 64, 16, 4},

    // `half` and `min16float` are the same 32 bits as `float` unless `-enable-16bit-types` is passed, and
    // nothing in ssc passes it.
    // Q14h pins that, so adding the flag is a failing test rather than a wrong number.
    {"half", "float", 4, 4, 4},
    {"half2", "float[2]", 8, 4, 4},
    {"half3", "float[3]", 12, 4, 4},
    {"half4", "float[4]", 16, 4, 4},

    {"min16float", "float", 4, 4, 4},
    {"min16float2", "float[2]", 8, 4, 4},
    {"min16float3", "float[3]", 12, 4, 4},
    {"min16float4", "float[4]", 16, 4, 4},

    // The 64-bit types obey rules of their own, which Q14i measured: a scalar aligns to 8, a vector starts a
    // whole row, and neither is kept off a row boundary -- a double3 is 24 bytes and crosses one outright.
    {"double", "double", 8, 8, 8},
    {"double2", "double[2]", 16, 16, 8},
    {"double3", "double[3]", 24, 16, 8},
    {"double4", "double[4]", 32, 16, 8},

    {"int64_t", "long long", 8, 8, 8},
    {"int64_t2", "long long[2]", 16, 16, 8},
    {"int64_t3", "long long[3]", 24, 16, 8},
    {"int64_t4", "long long[4]", 32, 16, 8},

    {"uint64_t", "unsigned long long", 8, 8, 8},
    {"uint64_t2", "unsigned long long[2]", 16, 16, 8},
    {"uint64_t3", "unsigned long long[3]", 24, 16, 8},
    {"uint64_t4", "unsigned long long[4]", 32, 16, 8},
};

/// Every sg::vertex_attribute_format enumerator, so a `format=` override can be checked against the real set.
/// The two a member's type can never reach are the last: `rgba8_unorm` and `rgba8_uint` are what the override
/// exists for.
constexpr cc::string_view k_formats[] = {
    "f32",   "vec2f", "vec3f", "vec4f", "i32",   "vec2i",       "vec3i",
    "vec4i", "u32",   "vec2u", "vec3u", "vec4u", "rgba8_unorm", "rgba8_uint",
};

/// Whether a spelling names a matrix at all, with or without its orientation qualifier.
[[nodiscard]] constexpr bool is_matrix_spelling(cc::string_view spelling)
{
    for (auto const& needle : {"float1x", "float2x", "float3x", "float4x", "matrix"})
        if (spelling.contains(cc::string_view(needle)))
            return true;
    return false;
}

constexpr cc::string_view k_needs_orientation
    = ", because a matrix's layout depends on its orientation — write `row_major` or `column_major`, since the "
      "default comes from a compile flag the pass cannot see";

constexpr cc::string_view k_partial_row
    = ", because a matrix must store full float4s (row_major floatRx4, column_major float4xC) — this one leaves a "
      "partial last row that the next member packs into, and SPIR-V refuses the module (spike Q14g)";
} // namespace

cc::optional<slib::impl::hlsl_value_type> slib::impl::value_type_of(cc::string_view hlsl_type)
{
    for (auto const& entry : k_table)
    {
        if (entry.name != hlsl_type)
            continue;

        hlsl_value_type result;
        result.cpp_type = entry.cpp_type;
        result.size = entry.size;
        result.constant_block_align = entry.constant_block_align;
        result.cpp_align = entry.cpp_align;
        if (entry.is_vertex_attribute)
            result.format = entry.format;
        return result;
    }

    return cc::nullopt;
}

bool slib::impl::is_vertex_attribute_format(cc::string_view name)
{
    for (auto const& candidate : k_formats)
        if (candidate == name)
            return true;
    return false;
}

cc::string_view slib::impl::rejection_reason_for(cc::string_view hlsl_type)
{
    if (!is_matrix_spelling(hlsl_type))
        return {};

    auto const oriented = hlsl_type.starts_with("row_major ") || hlsl_type.starts_with("column_major ");
    return oriented ? k_partial_row : k_needs_orientation;
}
