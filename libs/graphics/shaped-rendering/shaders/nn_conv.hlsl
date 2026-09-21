// One 3x3 convolution with a bias and a ReLU — the whole of the arithmetic the denoise network is made of.
//
// Sixteen of these, four max pools and four nearest upsamples are the entire U-Net, so this shader is where the
// member's cost and its correctness both live.
//
// FEATURE MAPS ARE HWC: index (y * width + x) * channels + c, channels innermost.
// That is what OIDN's own GPU kernels use, for the reason their comments give — adjacent threads want adjacent
// memory, and a thread per output CHANNEL is what makes a write coalesce.
//
// WEIGHTS ARE [o][ky][kx][i], which is `oihw` with the input channel moved innermost.
// The inner loop below walks `i`, so this is the order that makes it contiguous; `sr::impl::oidn_network` does that
// transpose once when it uploads them.
//
// Padding is ZERO and the output keeps the input's width and height, which is what OIDN's convolutions do — its
// descriptor gives the destination the source's H and W, and the skip connections could not concatenate otherwise.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct nn_conv_constants
{
    uint width;
    uint height;
    uint in_channels;
    uint out_channels;

    /// Where this layer's weights and bias start, in elements.
    /// One buffer holds the whole network, so a layer is a pair of offsets rather than a binding of its own.
    uint weight_offset;
    uint bias_offset;

    /// Whether the source is two feature maps to be read as one.
    ///
    /// A decoder layer takes the upsampled features CONCATENATED with an encoder skip, and doing that as a copy would
    /// move more memory than the convolution reads.
    /// So the concat is addressing: channels below `in_channels_a` come from the first buffer and the rest from the
    /// second, which is why `gSourceB` is always bound even when nothing reads it.
    uint in_channels_a;
    uint _pad;
};

#pragma sc push_constants
ConstantBuffer<nn_conv_constants> gConstants;

#pragma sc group 0
namespace nn_conv_bindings
{
    StructuredBuffer<float> gSourceA;
    StructuredBuffer<float> gSourceB;
    StructuredBuffer<float> gWeights;
    RWStructuredBuffer<float> gTarget;
}

using namespace nn_conv_bindings;

// One input channel at one texel, taken from whichever half of the concatenation holds it.
// Outside the image reads zero, which is the padding.
float source_at(int x, int y, uint c)
{
    if (x < 0 || y < 0 || x >= int(gConstants.width) || y >= int(gConstants.height))
        return 0.0;

    uint const a = gConstants.in_channels_a;
    uint const texel = uint(y) * gConstants.width + uint(x);

    if (c < a)
        return gSourceA[texel * a + c];

    uint const b = gConstants.in_channels - a;
    return gSourceB[texel * b + (c - a)];
}

// One thread per output channel of one texel.
//
// The channel is the FASTEST dimension so that a wave writes one contiguous run, which is the whole reason the
// feature maps are HWC.
[numthreads(64, 1, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint const o = id.x;
    uint const x = id.y;
    uint const y = id.z;
    if (o >= gConstants.out_channels || x >= gConstants.width || y >= gConstants.height)
        return;

    // The bias sits after every weight of the layer, so one buffer carries both.
    float sum = gWeights[gConstants.bias_offset + o];

    uint const in_channels = gConstants.in_channels;
    uint w = gConstants.weight_offset + o * 9u * in_channels;

    for (int ky = -1; ky <= 1; ++ky)
    {
        for (int kx = -1; kx <= 1; ++kx)
        {
            for (uint i = 0; i < in_channels; ++i)
                sum += gWeights[w + i] * source_at(int(x) + kx, int(y) + ky, i);
            w += in_channels;
        }
    }

    // ReLU on every layer, the last one included — which is why the network's output is never negative, and why the
    // radiance it produces needs no clamp of its own.
    gTarget[(y * gConstants.width + x) * gConstants.out_channels + o] = max(sum, 0.0);
}
