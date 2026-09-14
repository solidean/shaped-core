#pragma once

// The hit mesh's geometry, as a closest-hit reads it back to recompute the flat face normal.
//
// Two shapes are supported, matching sv::mesh_record. Indexed: `Indices` holds 3 indices per triangle into
// `Vertices`. Non-indexed: `Vertices` holds 3 positions per triangle and `Indices` is a bound-but-unread
// stand-in, since a binding group must cover every declared resource.
// Which one is live rides in the caller's frame constants — the trace binds one mesh per view.
//
// The two buffers are PARAMETERS rather than globals: they belong to the caller's binding group, and this file
// is the geometry, not the group. DXC inlines this, so passing them costs nothing.

struct Triangle3
{
    float3 v0;
    float3 v1;
    float3 v2;
};

// The three object-space corners of triangle `prim`.
// `[branch]` is load-bearing on the non-indexed path: a flattened select would read `Indices` out of range.
Triangle3 mesh_triangle(StructuredBuffer<float3> vertices, StructuredBuffer<uint> indices, uint prim, bool is_indexed)
{
    uint base = prim * 3;
    uint3 i;
    [branch] if (is_indexed)
        i = uint3(indices[base + 0], indices[base + 1], indices[base + 2]);
    else
        i = uint3(base + 0, base + 1, base + 2);

    Triangle3 t;
    t.v0 = vertices[i.x];
    t.v1 = vertices[i.y];
    t.v2 = vertices[i.z];
    return t;
}
