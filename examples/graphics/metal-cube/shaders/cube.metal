// One cube, six coloured faces, a key light and a fill light — the metal twin of rotating-cube's cube.hlsl.
//
//   xcrun -sdk macosx metal -O2 -o cube.metallib cube.metal
//   xxd -i cube.metallib > cube.metallib.h
//
// Hand-written MSL with the compiled blob checked in beside it, for the reason the tier-2 fixtures give: slib has no
// metal language, so there is nothing to compile this at runtime and nothing to compile it from HLSL either.
// libs/graphics/shaped-graphics/docs/TODO.md carries the item that would replace this with a shader package.
//
// The buffer indices are sg's, and the backend encodes them — see metal_common.hh:
//
//   [[buffer(4)]]     the inline-constants block  (k_inline_constants_buffer_index)
//   [[buffer(5+n)]]   vertex-input slot n         (k_vertex_buffer_base_index + n)
//
// A [[stage_in]] input needs no buffer index of its own: the vertex descriptor sg builds names slot 0 at buffer 5,
// and an attribute's [[attribute(n)]] is its position in vertex_input_layout::attributes.

#include <metal_stdlib>
using namespace metal;

/// The 64-byte inline-constants block: one view-projection matrix.
/// MSL's float4x4 is column-major and tg::mat4f is too, so the sixteen floats go across untransposed.
struct cube_constants
{
    float4x4 view_projection;
};

struct vertex_in
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float3 color [[attribute(2)]];
};

struct vertex_out
{
    float4 position [[position]];
    float3 normal;
    float3 color;
};

vertex vertex_out main_vs(vertex_in in [[stage_in]], constant cube_constants& constants [[buffer(4)]])
{
    vertex_out out;
    out.position = constants.view_projection * float4(in.position, 1.0f);
    out.normal = in.normal;
    out.color = in.color;
    return out;
}

fragment float4 main_ps(vertex_out in [[stage_in]])
{
    // The same key/fill/ambient trio cube.hlsl uses, so the two backends draw the same cube.
    float3 n = normalize(in.normal);
    float key = saturate(dot(n, normalize(float3(0.45f, 0.8f, -0.4f))));
    float fill = saturate(dot(n, normalize(float3(-0.7f, 0.15f, 0.6f))));

    float3 lit = in.color * (0.25f + 0.8f * key + 0.25f * fill);
    return float4(lit, 1.0f);
}
