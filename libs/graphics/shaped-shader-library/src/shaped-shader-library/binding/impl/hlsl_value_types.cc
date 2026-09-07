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

    // The one matrix the table carries, and Q14g is why it is the only one: a float4x4 is four vectors of four
    // whichever orientation is in force, where a float3x4 is three rows of 16 row-major and four rows of 12
    // column-major -- the same 64-byte total with different member offsets, which is what a mirror reproduces.
    // The pass cannot see the orientation, so it admits only the matrix that does not have one.
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

/// A type the pass refuses for a reason worth naming, and the reason.
/// Keyed by the leading text rather than by the whole name, so every `floatNxM` reaches the matrix sentence.
struct rejection
{
    cc::string_view prefix;
    cc::string_view reason;
};

constexpr rejection k_rejections[] = {
    {"float1x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"},
    {"float2x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"},
    {"float3x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"},
    {"float4x", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"},
    {"matrix", ", because only float4x4 packs the same in both matrix orientations (spike Q14g)"},
};
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
    for (auto const& entry : k_rejections)
        if (hlsl_type.starts_with(entry.prefix))
            return entry.reason;
    return {};
}
