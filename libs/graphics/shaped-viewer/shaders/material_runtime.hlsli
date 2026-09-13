#pragma once

#include "instance.hlsli" // sv::instance — the table entry the helpers below decode
#include "openpbr.hlsli" // sv::surface — the OpenPBR parameters a fragment writes — and the BSDF that consumes them

// The contract a GENERATED material shader is written against.
//
// A material type's `shader` is a fragment: it reads each attribute the type declares as an already-initialized local and writes
// `surface`.
// Everything around that — the bindless table declarations, the loads that initialize those locals, the samplers — is emitted
// per permutation by sv::generate_material_shader.
// This file is the hand-authored half: the structs both sides agree on, and the loads that are the same whatever the material.
//
// Nothing here declares a resource. The generated file declares the bindless tables, because which of them a permutation needs is
// what the permutation decides; a helper that touches one takes it as a parameter instead.

namespace sv
{
/// Where one mesh attribute's elements live.
///
/// One stride, not three: the geometric frequency is part of the permutation key, so the generated code already knows whether to
/// index by vertex, by corner or by primitive, and emits that index directly.
/// Carrying three strides and branching would trade a smaller permutation count for a branch per attribute per hit — worth
/// revisiting if the count ever becomes the problem, and pointless before then.
struct attribute_desc
{
    uint buffer; ///< index into gBindlessBuffers
    uint offset; ///< byte offset of element 0 within that buffer
    uint stride; ///< bytes between elements
};

/// What a hit knows about where it is, filled by the caller rather than by the material.
///
/// `corner` is the three vertex indices of the hit triangle, which is what makes a per-vertex attribute readable without the
/// material knowing whether the mesh is indexed — the caller resolves that when it fills this in.
struct shading_context
{
    uint param_buffer; ///< index into gBindlessBuffers of the buffer holding this instance's parameter block
    uint param_offset; ///< byte offset of that block

    uint primitive;      ///< PrimitiveIndex()
    uint3 corner;        ///< the three vertex indices of the hit triangle
    float3 barycentrics; ///< (1 - b.x - b.y, b.x, b.y), so the three weigh the three corners in order
};

/// One attribute descriptor out of a parameter block.
attribute_desc load_attribute_desc(ByteAddressBuffer params, uint offset)
{
    uint3 raw = params.Load3(offset);
    attribute_desc d;
    d.buffer = raw.x;
    d.offset = raw.y;
    d.stride = raw.z;
    return d;
}

// Element loads, by component count.
// Every one takes the buffer rather than indexing a table, so this file declares no resource of its own.

float load_element_f1(ByteAddressBuffer b, attribute_desc d, uint element)
{
    return asfloat(b.Load(d.offset + element * d.stride));
}
float2 load_element_f2(ByteAddressBuffer b, attribute_desc d, uint element)
{
    return asfloat(b.Load2(d.offset + element * d.stride));
}
float3 load_element_f3(ByteAddressBuffer b, attribute_desc d, uint element)
{
    return asfloat(b.Load3(d.offset + element * d.stride));
}
float4 load_element_f4(ByteAddressBuffer b, attribute_desc d, uint element)
{
    return asfloat(b.Load4(d.offset + element * d.stride));
}

// Barycentric interpolation of the three corners of the hit triangle.
// `e` is the three element indices the generated code resolved from the frequency — vertex indices for per_vertex, and
// `primitive * 3 + k` for per_corner.

float interpolate_f1(ByteAddressBuffer b, attribute_desc d, uint3 e, float3 w)
{
    return load_element_f1(b, d, e.x) * w.x + load_element_f1(b, d, e.y) * w.y + load_element_f1(b, d, e.z) * w.z;
}
float2 interpolate_f2(ByteAddressBuffer b, attribute_desc d, uint3 e, float3 w)
{
    return load_element_f2(b, d, e.x) * w.x + load_element_f2(b, d, e.y) * w.y + load_element_f2(b, d, e.z) * w.z;
}
float3 interpolate_f3(ByteAddressBuffer b, attribute_desc d, uint3 e, float3 w)
{
    return load_element_f3(b, d, e.x) * w.x + load_element_f3(b, d, e.y) * w.y + load_element_f3(b, d, e.z) * w.z;
}
float4 interpolate_f4(ByteAddressBuffer b, attribute_desc d, uint3 e, float3 w)
{
    return load_element_f4(b, d, e.x) * w.x + load_element_f4(b, d, e.y) * w.y + load_element_f4(b, d, e.z) * w.z;
}

/// Barycentric blend of three unit quaternions, as a rotation rather than as four numbers.
///
/// The alignment is what makes the sum mean anything.
/// `q` and `-q` are the same rotation, so two corners can describe neighbouring frames and still sit nearly antipodal in 4D;
/// summing those cancels toward zero and the normalized result is a frame belonging to neither, which reads as a swirl across
/// the triangle.
/// Aligning both corners into the first's hemisphere first is the fix, and it is why a rotation cannot go through
/// `interpolate_f4`.
///
/// This is a normalized linear blend rather than a spherical one: over one triangle the corners are close enough that the
/// angular error is far below what a shading normal resolves, and nlerp costs no trigonometry.
float4 interpolate_rotation(ByteAddressBuffer b, attribute_desc d, uint3 e, float3 w)
{
    float4 q0 = load_element_f4(b, d, e.x);
    float4 q1 = load_element_f4(b, d, e.y);
    float4 q2 = load_element_f4(b, d, e.z);

    q1 = dot(q1, q0) < 0.0 ? -q1 : q1;
    q2 = dot(q2, q0) < 0.0 ? -q2 : q2;

    float4 q = q0 * w.x + q1 * w.y + q2 * w.z;
    float len = length(q);

    // Three corners cannot cancel once aligned, so a zero length means the attribute held no rotation at all.
    return len > 1e-8 ? q / len : float4(0, 0, 0, 1);
}

/// The three element indices a per_corner attribute reads, in the order `barycentrics` weighs them.
uint3 corner_elements(shading_context ctx)
{
    uint base = ctx.primitive * 3;
    return uint3(base + 0, base + 1, base + 2);
}

/// The three vertex indices of triangle `primitive` on `inst`.
///
/// `[branch]` is load-bearing on the non-indexed path: a flattened select would read `index_buffer` out of range, and a
/// non-indexed mesh binds a stand-in there that has no elements to read.
uint3 triangle_corners(instance inst, ByteAddressBuffer index_buffer, uint primitive)
{
    uint base = primitive * 3;
    [branch] if (inst.is_indexed != 0)
        return uint3(index_buffer.Load(4 * (base + 0)), index_buffer.Load(4 * (base + 1)), index_buffer.Load(4 * (base + 2)));
    return uint3(base + 0, base + 1, base + 2);
}

/// The shading context for a hit on `inst`, which is all a generated material needs to know about where it is.
/// `bary` is the two barycentrics a hit attribute carries; the third is what is left of one.
shading_context make_context(instance inst, ByteAddressBuffer index_buffer, uint primitive, float2 bary)
{
    shading_context ctx;
    ctx.param_buffer = inst.param_buffer;
    ctx.param_offset = inst.param_offset;
    ctx.primitive = primitive;
    ctx.corner = triangle_corners(inst, index_buffer, primitive);
    ctx.barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    return ctx;
}
} // namespace sv
