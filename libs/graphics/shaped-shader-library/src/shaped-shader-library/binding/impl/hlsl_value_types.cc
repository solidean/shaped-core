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

    // Every matrix is column-major and the PASS writes that, exactly as it writes an address -- see
    // `matrix_offsets` in binding_groups.cc.
    // A shader declares `float4x3` and the rewrite makes it `column_major float4x3`, so the declaration is immune
    // to a `#pragma pack_matrix` or a `-Zpr` set anywhere else, and the mirror's floats are columns by
    // construction rather than by a default that held when someone last looked.
    //
    // Column-major is not a preference: MSL and WGSL have no row-major matrices at all, so a row-major HLSL
    // matrix has no expression on two of the four targets this dialect is for.
    //
    // The shapes are those whose columns are FULL float4s, which is what makes one extent true everywhere: a
    // matrix is C columns at a 16-byte stride, so `float4xC` is exactly 16C bytes on D3D, on SPIR-V, and under
    // WGSL's and MSL's own rules.
    // A narrower column pads, and the padding is where the targets stop agreeing -- D3D ends a `float3x3` at 44
    // where WGSL and MSL size it 48, so the member after it would sit in two different places.
    {"float4x1", "float[4]", 16, 16, 4},
    {"float4x2", "float[8]", 32, 16, 4},
    {"float4x3", "float[12]", 48, 16, 4},
    {"float4x4", "float[16]", 64, 16, 4},

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

constexpr cc::string_view k_narrow_column
    = ", because only a matrix whose columns are full float4s has one extent on every target: `float4xC` is 16C "
      "bytes everywhere, where a narrower column pads and D3D then ends the matrix sooner than WGSL and MSL do "
      "(spike Q14g)";
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

bool slib::impl::is_matrix_type(cc::string_view hlsl_type)
{
    return is_matrix_spelling(hlsl_type);
}

cc::string_view slib::impl::rejection_reason_for(cc::string_view hlsl_type)
{
    return is_matrix_spelling(hlsl_type) ? k_narrow_column : cc::string_view();
}
