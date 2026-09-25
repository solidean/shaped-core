// The denoiser's inputs, packed into the nine channels the network was trained on.
//
// Radiance in 0-2, diffuse albedo in 3-5, world normal in 6-8, HWC — which is the order `enc_conv0`'s weights expect,
// and the reason it has nine input channels.
//
// Each is prepared the way OIDN prepares it, and they differ: radiance is scaled, clamped to non-negative and taken
// through the transfer curve; albedo is clamped to [0, 1] and left alone; a normal is clamped to [-1, 1] and mapped
// onto [0, 1] because the network's inputs are unsigned.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "nn_transfer.hlsli"

struct nn_input_constants
{
    /// The TENSOR's extent, which is the image's rounded up to a multiple of sixteen so four pools can halve it.
    uint width;
    uint height;

    /// The IMAGE's extent. Everything past it is the padding, and reads there are clamped to the edge rather than
    /// left at zero — a black border would be an edge the network can see, and it would filter towards it.
    uint source_width;
    uint source_height;

    /// Where this tile's tensor starts in the image, which is its interior's origin less the overlap.
    /// Negative on the first tile of a row or column, and that is the point: the clamp below then repeats the edge
    /// exactly as it does for the padding, so an edge tile sees what an interior one does.
    int source_offset_x;
    int source_offset_y;

    /// What the radiance is multiplied by before the curve, which is what brings a scene into the range the network
    /// was trained over.
    /// OIDN derives one by measuring the image; this takes it from the caller, and 1 means "already in that range".
    float input_scale;
    float _pad0;
};

#pragma sc push_constants
ConstantBuffer<nn_input_constants> gConstants;

#pragma sc group 0
namespace nn_input_bindings
{
    Texture2D<float4> gColor;
    Texture2D<float4> gAlbedo;
    Texture2D<float4> gNormal;

    RWStructuredBuffer<float> gTarget; // HWC, nine channels
}

using namespace nn_input_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gConstants.width || id.y >= gConstants.height)
        return;

    // Clamped into the image, so both the padding and a tile's overhang repeat its edge.
    int2 const sample = int2(gConstants.source_offset_x + int(id.x), gConstants.source_offset_y + int(id.y));
    int3 const p = int3(clamp(sample.x, 0, int(gConstants.source_width) - 1),
                        clamp(sample.y, 0, int(gConstants.source_height) - 1), 0);

    float3 color = sanitize(gColor.Load(p).rgb) * gConstants.input_scale;
    color = max(color, float3(0, 0, 0));
    color = pu_forward(color);

    float3 const albedo = clamp(sanitize(gAlbedo.Load(p).rgb), 0.0, 1.0);

    // A missing normal reads as zero and maps to the middle of the range, which is what an absent guide should be.
    float3 const normal = clamp(sanitize(gNormal.Load(p).rgb), -1.0, 1.0) * 0.5 + 0.5;

    // Twelve, not nine: every tensor's channel count is padded to a multiple of four so the convolution can read
    // four at a time, and the three padding channels carry a hard zero rather than whatever was in memory.
    uint const base = (id.y * gConstants.width + id.x) * 12u;
    gTarget[base + 0] = color.x;
    gTarget[base + 1] = color.y;
    gTarget[base + 2] = color.z;
    gTarget[base + 3] = albedo.x;
    gTarget[base + 4] = albedo.y;
    gTarget[base + 5] = albedo.z;
    gTarget[base + 6] = normal.x;
    gTarget[base + 7] = normal.y;
    gTarget[base + 8] = normal.z;
    gTarget[base + 9] = 0.0;
    gTarget[base + 10] = 0.0;
    gTarget[base + 11] = 0.0;
}
