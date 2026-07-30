#pragma once

// The hit mesh's geometry, as a closest-hit reads it back to recompute the flat face normal.
//
// Two shapes are supported, matching sv::mesh_record. Indexed: `Indices` holds 3 indices per triangle into
// `Vertices`. Non-indexed: `Vertices` holds 3 positions per triangle and `Indices` is a bound-but-unread
// stand-in, since a binding group must cover every declared resource.
// Which one is live rides in the caller's frame constants — the trace binds one mesh per view.

StructuredBuffer<float3> Vertices : register(t2);
StructuredBuffer<uint> Indices : register(t3);

struct Triangle3
{
    float3 v0;
    float3 v1;
    float3 v2;
};

// The three object-space corners of triangle `prim`.
// `[branch]` is load-bearing on the non-indexed path: a flattened select would read `Indices` out of range.
Triangle3 mesh_triangle(uint prim, bool is_indexed)
{
    uint base = prim * 3;
    uint3 i;
    [branch] if (is_indexed)
        i = uint3(Indices[base + 0], Indices[base + 1], Indices[base + 2]);
    else
        i = uint3(base + 0, base + 1, base + 2);

    Triangle3 t;
    t.v0 = Vertices[i.x];
    t.v1 = Vertices[i.y];
    t.v2 = Vertices[i.z];
    return t;
}
