// sr's linear view depth, as the device depth FSR 3.1 reads.
// Paired with sr::fsr_upscale_routine.
//
// FSR reconstructs view-space positions from a rasterizer's depth buffer and a near plane, where sr's guide is the
// view depth itself, 0 or less where a ray missed.
// Written as an inverted depth with an infinite far plane — `near / depth` — which FSR's own docs recommend, and which
// FSR turns back into exactly this view depth with the same `near`.
// A miss becomes 0, the infinitely far plane.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct fsr_prepare_depth_constants
{
    float near_plane; // the near plane FSR is told; any positive value, as long as both sides use the same one
    float3 _pad;
};

#pragma sc push_constants
ConstantBuffer<fsr_prepare_depth_constants> gConstants;

#pragma sc group 0
namespace fsr_prepare_depth_bindings
{
    Texture2D<float4> gLinearDepth;
    RWTexture2D<float> gDeviceDepth;
}

using namespace fsr_prepare_depth_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gDeviceDepth.GetDimensions(size.x, size.y);
    if (id.x >= size.x || id.y >= size.y)
        return;

    float depth = gLinearDepth.Load(int3(id.xy, 0)).r;
    gDeviceDepth[id.xy] = depth > 0.0 ? gConstants.near_plane / depth : 0.0;
}
