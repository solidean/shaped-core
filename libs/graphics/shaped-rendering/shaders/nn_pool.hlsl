// The network's 2x2 max pool, which is how the encoder halves its resolution.
//
// Exactly OIDN's: the maximum of the four source texels under each destination one, per channel.
// The source's width and height must both be even, which the member guarantees by padding the image up to a multiple
// of sixteen before the first layer — four pools deep.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct nn_pool_constants
{
    uint width;  // of the DESTINATION, which is half the source's
    uint height;
    uint channels;
    uint _pad;
};

#pragma sc push_constants
ConstantBuffer<nn_pool_constants> gConstants;

#pragma sc group 0
namespace nn_pool_bindings
{
    StructuredBuffer<float> gSource;
    RWStructuredBuffer<float> gTarget;
}

using namespace nn_pool_bindings;

[numthreads(64, 1, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint const c = id.x;
    uint const x = id.y;
    uint const y = id.z;
    if (c >= gConstants.channels || x >= gConstants.width || y >= gConstants.height)
        return;

    uint const source_width = gConstants.width * 2u;
    uint const channels = gConstants.channels;

    uint const row0 = ((y * 2u) * source_width + x * 2u) * channels + c;
    uint const row1 = ((y * 2u + 1u) * source_width + x * 2u) * channels + c;

    float const a = gSource[row0];
    float const b = gSource[row0 + channels];
    float const d = gSource[row1];
    float const e = gSource[row1 + channels];

    gTarget[(y * gConstants.width + x) * channels + c] = max(max(a, b), max(d, e));
}
