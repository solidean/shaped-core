// The network's 2x nearest upsample, which is how the decoder climbs back up.
//
// Exactly OIDN's: one source texel written to the four destination texels under it, per channel.
// Nearest rather than bilinear is not an approximation — it is what the weights were trained against, and a smoother
// filter here would be a different network.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct nn_upsample_constants
{
    uint width;  // of the SOURCE, which is half the destination's
    uint height;
    uint channels;
    uint _pad;
};

#pragma sc push_constants
ConstantBuffer<nn_upsample_constants> gConstants;

#pragma sc group 0
namespace nn_upsample_bindings
{
    StructuredBuffer<float> gSource;
    RWStructuredBuffer<float> gTarget;
}

using namespace nn_upsample_bindings;

[numthreads(64, 1, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint const c = id.x;
    uint const x = id.y;
    uint const y = id.z;
    if (c >= gConstants.channels || x >= gConstants.width || y >= gConstants.height)
        return;

    uint const channels = gConstants.channels;
    float const v = gSource[(y * gConstants.width + x) * channels + c];

    uint const target_width = gConstants.width * 2u;
    uint const row0 = ((y * 2u) * target_width + x * 2u) * channels + c;
    uint const row1 = ((y * 2u + 1u) * target_width + x * 2u) * channels + c;

    gTarget[row0] = v;
    gTarget[row0 + channels] = v;
    gTarget[row1] = v;
    gTarget[row1 + channels] = v;
}
