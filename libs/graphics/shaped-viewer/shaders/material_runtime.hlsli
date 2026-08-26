#pragma once

// The contract a GENERATED material shader is written against.
//
// A material type's `shader` is a fragment: it reads each attribute the type declares as an already-initialized local and writes
// `surface`. Everything around that — the bindless table declarations, the loads that initialize those locals, the samplers — is
// emitted per permutation by sv::generate_material_shader.
// This file is the hand-authored half: the structs both sides agree on, and the loads that are the same whatever the material.
//
// Nothing here declares a resource. The generated file declares the bindless tables, because which of them a permutation needs is
// what the permutation decides; a helper that touches one takes it as a parameter instead.

/// Where one mesh attribute's elements live.
///
/// One stride, not three: the geometric frequency is part of the permutation key, so the generated code already knows whether to
/// index by vertex, by corner or by primitive, and emits that index directly.
/// Carrying three strides and branching would trade a smaller permutation count for a branch per attribute per hit — worth
/// revisiting if the count ever becomes the problem, and pointless before then.
struct sv_attribute_desc
{
    uint buffer; ///< index into gBindlessBuffers
    uint offset; ///< byte offset of element 0 within that buffer
    uint stride; ///< bytes between elements
};

/// What a hit knows about where it is, filled by the caller rather than by the material.
///
/// `corner` is the three vertex indices of the hit triangle, which is what makes a per-vertex attribute readable without the
/// material knowing whether the mesh is indexed — the caller resolves that when it fills this in.
struct sv_shading_context
{
    uint param_buffer; ///< index into gBindlessBuffers of the buffer holding this instance's parameter block
    uint param_offset; ///< byte offset of that block

    uint primitive;      ///< PrimitiveIndex()
    uint3 corner;        ///< the three vertex indices of the hit triangle
    float3 barycentrics; ///< (1 - b.x - b.y, b.x, b.y), so the three weigh the three corners in order
};

/// What a material produces: one shaded surface point, in the vocabulary every material type writes.
struct sv_surface
{
    float3 albedo;
    float metallic;
    float roughness;
    float3 emissive;
    float3 normal; ///< tangent space, so (0, 0, 1) is the geometric normal
    float occlusion;
    float opacity;
};

/// The surface a fragment starts from, so one that writes only `albedo` still produces a usable result.
sv_surface sv_default_surface()
{
    sv_surface s;
    s.albedo = float3(0.8, 0.8, 0.8);
    s.metallic = 0.0;
    s.roughness = 0.5;
    s.emissive = float3(0, 0, 0);
    s.normal = float3(0, 0, 1);
    s.occlusion = 1.0;
    s.opacity = 1.0;
    return s;
}

/// One attribute descriptor out of a parameter block.
sv_attribute_desc sv_load_attribute_desc(ByteAddressBuffer params, uint offset)
{
    uint3 raw = params.Load3(offset);
    sv_attribute_desc d;
    d.buffer = raw.x;
    d.offset = raw.y;
    d.stride = raw.z;
    return d;
}

// Element loads, by component count.
// Every one takes the buffer rather than indexing a table, so this file declares no resource of its own.

float sv_load_element_f1(ByteAddressBuffer b, sv_attribute_desc d, uint element)
{
    return asfloat(b.Load(d.offset + element * d.stride));
}
float2 sv_load_element_f2(ByteAddressBuffer b, sv_attribute_desc d, uint element)
{
    return asfloat(b.Load2(d.offset + element * d.stride));
}
float3 sv_load_element_f3(ByteAddressBuffer b, sv_attribute_desc d, uint element)
{
    return asfloat(b.Load3(d.offset + element * d.stride));
}
float4 sv_load_element_f4(ByteAddressBuffer b, sv_attribute_desc d, uint element)
{
    return asfloat(b.Load4(d.offset + element * d.stride));
}

// Barycentric interpolation of the three corners of the hit triangle.
// `e` is the three element indices the generated code resolved from the frequency — vertex indices for per_vertex, and
// `primitive * 3 + k` for per_corner.

float sv_interpolate_f1(ByteAddressBuffer b, sv_attribute_desc d, uint3 e, float3 w)
{
    return sv_load_element_f1(b, d, e.x) * w.x + sv_load_element_f1(b, d, e.y) * w.y + sv_load_element_f1(b, d, e.z) * w.z;
}
float2 sv_interpolate_f2(ByteAddressBuffer b, sv_attribute_desc d, uint3 e, float3 w)
{
    return sv_load_element_f2(b, d, e.x) * w.x + sv_load_element_f2(b, d, e.y) * w.y + sv_load_element_f2(b, d, e.z) * w.z;
}
float3 sv_interpolate_f3(ByteAddressBuffer b, sv_attribute_desc d, uint3 e, float3 w)
{
    return sv_load_element_f3(b, d, e.x) * w.x + sv_load_element_f3(b, d, e.y) * w.y + sv_load_element_f3(b, d, e.z) * w.z;
}
float4 sv_interpolate_f4(ByteAddressBuffer b, sv_attribute_desc d, uint3 e, float3 w)
{
    return sv_load_element_f4(b, d, e.x) * w.x + sv_load_element_f4(b, d, e.y) * w.y + sv_load_element_f4(b, d, e.z) * w.z;
}

/// The three element indices a per_corner attribute reads, in the order `barycentrics` weighs them.
uint3 sv_corner_elements(sv_shading_context ctx)
{
    uint base = ctx.primitive * 3;
    return uint3(base + 0, base + 1, base + 2);
}

/// One scene item, as a closest-hit reads it by `InstanceID()` — mirrors sv::instance_gpu (resources/instance_data.hh).
///
/// Keep the two in lockstep: this is a byte layout, not a description of one.
struct sv_instance
{
    uint param_buffer;
    uint param_offset;
    uint vertices;
    uint indices;
    uint is_indexed;
    uint3 _padding;
};

/// The three vertex indices of triangle `primitive` on `inst`.
///
/// `[branch]` is load-bearing on the non-indexed path: a flattened select would read `index_buffer` out of range, and a
/// non-indexed mesh binds a stand-in there that has no elements to read.
uint3 sv_triangle_corners(sv_instance inst, ByteAddressBuffer index_buffer, uint primitive)
{
    uint base = primitive * 3;
    [branch] if (inst.is_indexed != 0)
        return uint3(index_buffer.Load(4 * (base + 0)), index_buffer.Load(4 * (base + 1)), index_buffer.Load(4 * (base + 2)));
    return uint3(base + 0, base + 1, base + 2);
}

/// The shading context for a hit on `inst`, which is all a generated material needs to know about where it is.
/// `bary` is the two barycentrics a hit attribute carries; the third is what is left of one.
sv_shading_context sv_make_context(sv_instance inst, ByteAddressBuffer index_buffer, uint primitive, float2 bary)
{
    sv_shading_context ctx;
    ctx.param_buffer = inst.param_buffer;
    ctx.param_offset = inst.param_offset;
    ctx.primitive = primitive;
    ctx.corner = sv_triangle_corners(inst, index_buffer, primitive);
    ctx.barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    return ctx;
}
