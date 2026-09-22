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

// How many texels along x one thread produces.
//
// This is what makes the shader worth its name rather than a definition of the arithmetic: a thread reads a layer's
// weights once and spends them on NN_CONV_TEXELS outputs, so the weight traffic per texel falls by that factor.
// The weights are what dominate — every thread in a wave reads a DIFFERENT output channel's row, so those reads are
// strided where the input reads are a broadcast, and there is no reuse of them across texels otherwise.
//
// 32 was swept rather than picked, over a whole 256x256 tile: 1 is 91 ms, 8 is 18.5, 16 is 12.2, 32 is 11.6 and 48
// falls back to 16.4 as the accumulators start spilling.
// A larger tile prefers it more strongly still — at 512 it is 39 ms against 52 for 16.
//
// `sr::impl::oidn_network` dispatches against this, and the two must agree; a mismatch is a wrong image, which the
// oracle test against OIDN's own filter catches immediately.
#define NN_CONV_TEXELS 32

// One thread per output channel, for a run of NN_CONV_TEXELS texels along x.
//
// The channel is still the FASTEST dimension so that a wave writes one contiguous run per texel, which is the whole
// reason the feature maps are HWC.
[numthreads(64, 1, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint const o = id.x;
    uint const x0 = id.y * NN_CONV_TEXELS;
    uint const y = id.z;
    if (o >= gConstants.out_channels || x0 >= gConstants.width || y >= gConstants.height)
        return;

    // The bias sits after every weight of the layer, so one buffer carries both.
    float const bias = gWeights[gConstants.bias_offset + o];

    float sums[NN_CONV_TEXELS];
    [unroll] for (uint s = 0; s < NN_CONV_TEXELS; ++s)
        sums[s] = bias;

    uint const in_channels = gConstants.in_channels;
    uint w = gConstants.weight_offset + o * 9u * in_channels;

    for (int ky = -1; ky <= 1; ++ky)
    {
        int const sy = int(y) + ky;
        if (sy < 0 || sy >= int(gConstants.height))
        {
            w += 3u * in_channels; // the row is entirely padding, and padding is zero
            continue;
        }

        for (uint i = 0; i < in_channels; ++i)
        {
            // One row of the window, read once and spent on all three kernel columns.
            // Reading it per column instead would trip over the same values three times.
            float v[NN_CONV_TEXELS + 2];
            [unroll] for (uint j = 0; j < NN_CONV_TEXELS + 2; ++j)
                v[j] = source_at(int(x0) + int(j) - 1, sy, i);

            float const w0 = gWeights[w + i];
            float const w1 = gWeights[w + in_channels + i];
            float const w2 = gWeights[w + 2u * in_channels + i];

            [unroll] for (uint t = 0; t < NN_CONV_TEXELS; ++t)
                sums[t] += w0 * v[t] + w1 * v[t + 1] + w2 * v[t + 2];
        }

        w += 3u * in_channels;
    }

    // ReLU on every layer, the last one included — which is why the network's output is never negative, and why the
    // radiance it produces needs no clamp of its own.
    uint const base = (y * gConstants.width + x0) * gConstants.out_channels + o;
    [unroll] for (uint t = 0; t < NN_CONV_TEXELS; ++t)
        if (x0 + t < gConstants.width)
            gTarget[base + t * gConstants.out_channels] = max(sums[t], 0.0);
}
