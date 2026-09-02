// Downsamples one mip level into the next with a box filter — the plain average, and deliberately nothing
// more: it is separable, cheap, and correct for the "fill the chain we did not upload" job.
//
// Better filters (Kaiser, and a gamma-correct average for sRGB content) belong in routines of their own rather
// than behind a flag here, which is why this one is named for its filter.
//
// One entry point per view dimension, because HLSL cannot abstract over them.
// Arrays average within a slice and never across slices, so their entry points halve x/y only; a 3D texture is
// the one shape that halves in z too, which is why it averages 8 taps rather than 4.
//
// One dispatch per level, with the source bound as a single-mip view of level N and the target as the UAV of
// N+1, so no level is ever read and written by the same dispatch.

#include "sc/portable.hlsli"

// A source extent that is odd on some axis means the level is not exactly twice the target there; clamping the
// second tap keeps it inside the level instead of sampling the border.
#define HI(base, size) min((base) + 1, (size) - 1)

// -- 1D --------------------------------------------------------------------------------------------------

#define SC_GROUP 0
SC_BINDING Texture1D<float4> gSource1D;
SC_BINDING RWTexture1D<float4> gTarget1D;
#undef SC_GROUP

[numthreads(64, 1, 1)] void main_1d_cs(uint3 id : SV_DispatchThreadID)
{
    uint target_size;
    gTarget1D.GetDimensions(target_size);
    if (id.x >= target_size)
        return;

    uint source_size;
    gSource1D.GetDimensions(source_size);

    uint base = id.x * 2;
    gTarget1D[id.x] = (gSource1D.Load(int2(base, 0)) + gSource1D.Load(int2(HI(base, source_size), 0))) * 0.5;
}

// -- 1D array --------------------------------------------------------------------------------------------

#define SC_GROUP 0
SC_BINDING Texture1DArray<float4> gSource1DArray;
SC_BINDING RWTexture1DArray<float4> gTarget1DArray;
#undef SC_GROUP

[numthreads(64, 1, 1)] void main_1d_array_cs(uint3 id : SV_DispatchThreadID)
{
    uint target_size, target_slices;
    gTarget1DArray.GetDimensions(target_size, target_slices);
    if (id.x >= target_size || id.y >= target_slices)
        return;

    uint source_size, source_slices;
    gSource1DArray.GetDimensions(source_size, source_slices);

    uint base = id.x * 2;
    gTarget1DArray[uint2(id.x, id.y)] = (gSource1DArray.Load(int3(base, id.y, 0))
                                         + gSource1DArray.Load(int3(HI(base, source_size), id.y, 0)))
                                        * 0.5;
}

// -- 2D --------------------------------------------------------------------------------------------------

#define SC_GROUP 0
SC_BINDING Texture2D<float4> gSource2D;
SC_BINDING RWTexture2D<float4> gTarget2D;
#undef SC_GROUP

[numthreads(8, 8, 1)] void main_2d_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 target_size;
    gTarget2D.GetDimensions(target_size.x, target_size.y);
    if (any(id.xy >= target_size))
        return;

    uint2 source_size;
    gSource2D.GetDimensions(source_size.x, source_size.y);

    uint2 base = id.xy * 2;
    uint2 hi = HI(base, source_size);

    float4 sum = gSource2D.Load(int3(base.x, base.y, 0)) + gSource2D.Load(int3(hi.x, base.y, 0))
                 + gSource2D.Load(int3(base.x, hi.y, 0)) + gSource2D.Load(int3(hi.x, hi.y, 0));

    gTarget2D[id.xy] = sum * 0.25;
}

// -- 2D array (also every cube and cube array, whose UAV is a 2D array of faces) ----------------------------

#define SC_GROUP 0
SC_BINDING Texture2DArray<float4> gSource2DArray;
SC_BINDING RWTexture2DArray<float4> gTarget2DArray;
#undef SC_GROUP

[numthreads(8, 8, 1)] void main_2d_array_cs(uint3 id : SV_DispatchThreadID)
{
    uint3 target_size;
    gTarget2DArray.GetDimensions(target_size.x, target_size.y, target_size.z);
    if (any(id >= target_size))
        return;

    uint3 source_size;
    gSource2DArray.GetDimensions(source_size.x, source_size.y, source_size.z);

    // A cube face is a slice, and averaging across faces would bleed one face into its neighbour — so z indexes
    // the slice and is never halved.
    uint2 base = id.xy * 2;
    uint2 hi = HI(base, source_size.xy);

    float4 sum = gSource2DArray.Load(int4(base.x, base.y, id.z, 0)) + gSource2DArray.Load(int4(hi.x, base.y, id.z, 0))
                 + gSource2DArray.Load(int4(base.x, hi.y, id.z, 0)) + gSource2DArray.Load(int4(hi.x, hi.y, id.z, 0));

    gTarget2DArray[id] = sum * 0.25;
}

// -- 3D --------------------------------------------------------------------------------------------------

#define SC_GROUP 0
SC_BINDING Texture3D<float4> gSource3D;
SC_BINDING RWTexture3D<float4> gTarget3D;
#undef SC_GROUP

[numthreads(4, 4, 4)] void main_3d_cs(uint3 id : SV_DispatchThreadID)
{
    uint3 target_size;
    gTarget3D.GetDimensions(target_size.x, target_size.y, target_size.z);
    if (any(id >= target_size))
        return;

    uint3 source_size;
    gSource3D.GetDimensions(source_size.x, source_size.y, source_size.z);

    // The one shape whose z halves with the rest, so this is an 8-tap average over the whole voxel.
    uint3 base = id * 2;
    uint3 hi = HI(base, source_size);

    float4 sum = gSource3D.Load(int4(base.x, base.y, base.z, 0)) + gSource3D.Load(int4(hi.x, base.y, base.z, 0))
                 + gSource3D.Load(int4(base.x, hi.y, base.z, 0)) + gSource3D.Load(int4(hi.x, hi.y, base.z, 0))
                 + gSource3D.Load(int4(base.x, base.y, hi.z, 0)) + gSource3D.Load(int4(hi.x, base.y, hi.z, 0))
                 + gSource3D.Load(int4(base.x, hi.y, hi.z, 0)) + gSource3D.Load(int4(hi.x, hi.y, hi.z, 0));

    gTarget3D[id] = sum * 0.125;
}
