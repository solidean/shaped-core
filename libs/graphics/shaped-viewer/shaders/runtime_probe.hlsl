#include "quadric_runtime.hlsli"

// A probe over the material and quadric runtimes, so a test can hold their SGL port to the same numbers.
//
// The quadric root solve is the numerically delicate half: it re-expresses the quadric about the ray's closest approach, and
// a port that formed one product in another order would move a grazing hit or lose a small primitive seen from afar.
// So this calls the real `load_quadric`, `roots_of` and `intersect_quadric`, and the real attribute loads, rather than
// anything reimplemented, and tests/shaders/runtime_probe.sgl does the same against the SGL modules.
//
// One work item per case, writing `runtime_probe_results_per_case` float4s.

namespace sv
{
/// One ray against one quadric record, and one triangle's attributes — mirrors `sv_test::runtime_probe_case` lane for lane.
struct runtime_probe_case
{
    float3 origin; ///< the ray, in the quadric set's space
    uint quadric;  ///< which record of `Quadrics`
    float3 dir;
    float t_min;
    float t_max;
    uint primitive;   ///< the triangle whose corners and attributes are read
    uint is_indexed;  ///< whether `Indices` holds its corners
    uint desc_offset; ///< where in `Attributes` the attribute descriptors start
    float2 bary;      ///< the two barycentrics a hit attribute carries
    uint2 _pad;
};
} // namespace sv

#pragma sc group 0
namespace runtime_probe_bindings
{
    StructuredBuffer<sv::runtime_probe_case> Cases;

    /// `sv::quadric_gpu` records, as a batch's primitive buffer holds them.
    ByteAddressBuffer Quadrics;

    /// Four attribute descriptors from `desc_offset` — a float, a float2, a float3 and a float4 one, the last a rotation —
    /// and the elements they point at.
    ByteAddressBuffer Attributes;

    ByteAddressBuffer Indices;

    /// Eight per case, in the order `RuntimeProbe` writes them.
    RWStructuredBuffer<float4> Results;
}

[numthreads(64, 1, 1)] void RuntimeProbe(uint3 tid : SV_DispatchThreadID)
{
    uint item = tid.x;

    uint count = 0u;
    uint stride = 0u;
    runtime_probe_bindings::Cases.GetDimensions(count, stride);
    if (item >= count)
        return;

    sv::runtime_probe_case c = runtime_probe_bindings::Cases[item];

    sv::quadric_primitive prim = sv::load_quadric(runtime_probe_bindings::Quadrics, c.quadric);
    sv::quadric_result hit = sv::intersect_quadric(prim, c.origin, c.dir, c.t_min, c.t_max);
    sv::quadric_roots roots = sv::roots_of(prim.surface, c.origin - prim.origin, c.dir);

    sv::instance inst;
    inst.param_buffer = 7;
    inst.param_offset = 64;
    inst.vertices = 0;
    inst.indices = 0;
    inst.is_indexed = c.is_indexed;
    inst.link_mask = 0xffffffffu;
    inst._padding = uint2(0, 0);

    sv::shading_context ctx = sv::make_context(inst, runtime_probe_bindings::Indices, c.primitive, c.bary);
    sv::shading_context qctx = sv::make_quadric_context(inst, c.quadric);

    sv::attribute_desc d1 = sv::load_attribute_desc(runtime_probe_bindings::Attributes, c.desc_offset);
    sv::attribute_desc d2 = sv::load_attribute_desc(runtime_probe_bindings::Attributes, c.desc_offset + 12);
    sv::attribute_desc d3 = sv::load_attribute_desc(runtime_probe_bindings::Attributes, c.desc_offset + 24);
    sv::attribute_desc d4 = sv::load_attribute_desc(runtime_probe_bindings::Attributes, c.desc_offset + 36);

    // Per vertex through the triangle's corners, and per corner through `corner_elements`, as a generated material reads them.
    float f1 = sv::interpolate_f1(runtime_probe_bindings::Attributes, d1, ctx.corner, ctx.barycentrics);
    float2 f2 = sv::interpolate_f2(runtime_probe_bindings::Attributes, d2, sv::corner_elements(ctx), ctx.barycentrics);
    float3 f3 = sv::interpolate_f3(runtime_probe_bindings::Attributes, d3, ctx.corner, ctx.barycentrics);
    float4 f4 = sv::interpolate_f4(runtime_probe_bindings::Attributes, d4, ctx.corner, ctx.barycentrics);
    float4 rotation = sv::interpolate_rotation(runtime_probe_bindings::Attributes, d4, ctx.corner, ctx.barycentrics);

    uint base = item * 8;
    runtime_probe_bindings::Results[base + 0] = float4(hit.t, hit.normal);
    runtime_probe_bindings::Results[base + 1] = float4(hit.valid ? 1.0 : 0.0, float(roots.count), roots.t0, roots.t1);
    runtime_probe_bindings::Results[base + 2] = float4(roots.shifted_origin, roots.t_closest);
    runtime_probe_bindings::Results[base + 3] = float4(f3, f1);
    runtime_probe_bindings::Results[base + 4] = f4;
    runtime_probe_bindings::Results[base + 5] = rotation;
    runtime_probe_bindings::Results[base + 6] = float4(float3(ctx.corner), ctx.barycentrics.x);
    runtime_probe_bindings::Results[base + 7] = float4(f2, ctx.barycentrics.z, float(qctx.primitive + qctx.param_offset));
}
