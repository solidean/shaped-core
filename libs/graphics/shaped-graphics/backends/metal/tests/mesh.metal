// The tier-2 fixture for vertex input, indexed draws and inline constants, kept as source next to the blob compiled
// from it.
//
//   xcrun -sdk macosx metal -O2 -o mesh.metallib mesh.metal
//   xxd -i mesh.metallib > mesh.metallib.h
//
// Hand-written and temporary, for the reason raytrace.metal's own comment gives: the agreed shape is HLSL through
// SPIRV-Cross with all three artifacts checked in, and neither tool runs on an arm64 macOS host today.
// See libs/graphics/shaped-graphics/docs/TODO.md.
//
// The binding convention it is written against, which the backend encodes:
//
//   [[buffer(0..2)]]  one argument buffer per binding group, member [[id(n)]] = sg::binding::index
//   [[buffer(3)]]     the group sg reserves for itself
//   [[buffer(4)]]     the inline-constants block  (k_inline_constants_buffer_index)
//   [[buffer(5+n)]]   vertex-input slot n         (k_vertex_buffer_base_index + n)

#include <metal_stdlib>
using namespace metal;

/// The inline-constants block, 16 bytes: a colour every fragment is multiplied by.
/// A tint rather than a transform, so a block that never arrived reads as black and is not mistaken for geometry gone
/// wrong.
struct tint_constants
{
    float4 tint;
};

struct vertex_in
{
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct vertex_out
{
    float4 position [[position]];
    float4 color;
};

vertex vertex_out vertex_main(vertex_in in [[stage_in]], constant tint_constants& constants [[buffer(4)]])
{
    vertex_out out;
    out.position = float4(in.position, 0.0, 1.0);
    out.color = in.color * constants.tint;
    return out;
}

fragment float4 fragment_main(vertex_out in [[stage_in]])
{
    return in.color;
}

/// The compute half of the inline-constants path: one group holding the values, and the factor arriving as constants.
struct scale_bindings
{
    device uint* values [[id(0)]];
};

struct scale_constants
{
    uint factor;
};

kernel void scale_main(device scale_bindings& bindings [[buffer(0)]],
                       constant scale_constants& constants [[buffer(4)]],
                       uint tid [[thread_position_in_grid]])
{
    bindings.values[tid] = bindings.values[tid] * constants.factor;
}

/// An array binding and a scalar one in the same group, which is what `declare_array_buffer_access` addresses.
///
/// **The array occupies `count` consecutive slots from its own index**, so the scalar output sits at id 4 rather than
/// at id 1 — the spacing rule the backend states and does not check.
struct array_bindings
{
    array<device const uint*, 4> inputs [[id(0)]];
    device uint* output [[id(4)]];
};

kernel void array_sum_main(device array_bindings& bindings [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    uint sum = 0;
    for (uint i = 0; i < 4; ++i)
        sum += bindings.inputs[i][tid];
    bindings.output[tid] = sum;
}

/// Writes the vertex buffer a later draw reads, so the dependency crosses an encoder boundary.
///
/// One thread per vertex, six floats each — position then colour, matching `vertex_in`'s layout.
struct quad_bindings
{
    device float* vertices [[id(0)]];
};

kernel void emit_quad_main(device quad_bindings& bindings [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    const float2 positions[4] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)};

    device float* v = bindings.vertices + tid * 6;
    v[0] = positions[tid].x;
    v[1] = positions[tid].y;
    v[2] = 0.25;
    v[3] = 0.5;
    v[4] = 0.75;
    v[5] = 1.0;
}
