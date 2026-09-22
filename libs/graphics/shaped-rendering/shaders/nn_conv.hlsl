// One 3x3 convolution with a bias and a ReLU — the whole of the arithmetic the denoise network is made of.
//
// Sixteen of these, four max pools and four nearest upsamples are the entire U-Net, so this shader is where the
// member's cost and its correctness both live.
//
// FEATURE MAPS ARE HWC: index (y * width + x) * channels + c, channels innermost.
// That is what OIDN's own GPU kernels use, for the reason their comments give — adjacent threads want adjacent
// memory, and a thread per output CHANNEL is what makes a write coalesce.
//
// WEIGHTS ARE [ky][kx][i][o], which is `oihw` turned inside out so the OUTPUT channel is innermost.
//
// That is the layout the wave wants rather than the one a thread wants: a lane's output channel is what varies across
// the wave, so putting `o` innermost makes one weight load touch one or two cache lines instead of sixty-four rows
// scattered `9 * in_channels` floats apart.
// `sr::impl::oidn_network` does that transpose once when it uploads them.
// It is worth about 1.1x at the same blocking, and 1.25x once the blocking is re-tuned — cheaper weights move the
// best run of texels from 32 down to 16.
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
// 16 was swept rather than picked, over a whole 256x256 tile: 8 is 8.3 ms, 12 is 7.7, 16 is 7.6, 20 is 7.9 and 24
// is 8.4, with 48 and beyond falling away as the accumulators spill.
// Blocking the OUTPUT CHANNELS too was tried under both weight layouts and does not pay — 16x1 is 8.0 ms where 16x2
// is 10.9 and 8x2 is 9.4.
// The sixty-four lanes of a wave already read the same input, so that traffic is a broadcast rather than something a
// second blocking dimension could amortize, and the registers it costs buy nothing back.
//
// `sr::impl::oidn_network` dispatches against this, and the two must agree; a mismatch is a wrong image, which the
// oracle test against OIDN's own filter catches immediately.
#define NN_CONV_TEXELS 16

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
    uint const out_channels = gConstants.out_channels;

    // One kernel column's worth of weights, which is the stride between kx blocks in the layout above.
    uint const column = in_channels * out_channels;

    for (int ky = -1; ky <= 1; ++ky)
    {
        int const sy = int(y) + ky;
        if (sy < 0 || sy >= int(gConstants.height))
            continue; // the row is entirely padding, and padding is zero

        uint const row = gConstants.weight_offset + uint(ky + 1) * 3u * column;

        // Where each column of the window sits, worked out ONCE for the row rather than per input channel.
        //
        // The bounds test and the concat split do not depend on the channel, so leaving them in the inner loop meant
        // thirty-four branches for every ninety-six multiply-adds.
        uint texels[NN_CONV_TEXELS + 2];
        bool inside[NN_CONV_TEXELS + 2];
        [unroll] for (uint j = 0; j < NN_CONV_TEXELS + 2; ++j)
        {
            int const sx = int(x0) + int(j) - 1;
            inside[j] = sx >= 0 && sx < int(gConstants.width);
            texels[j] = inside[j] ? uint(sy) * gConstants.width + uint(sx) : 0u;
        }

        uint const a = gConstants.in_channels_a;
        uint const b = in_channels - a;

        // Which half of the concatenation a channel comes from is decided once per channel, not once per element.
        for (uint i = 0; i < in_channels; ++i)
        {
            bool const from_a = i < a;
            uint const stride = from_a ? a : b;
            uint const c = from_a ? i : i - a;

            // One row of the window, read once and spent on all three kernel columns.
            // Reading it per column instead would trip over the same values three times.
            float v[NN_CONV_TEXELS + 2];
            [unroll] for (uint j = 0; j < NN_CONV_TEXELS + 2; ++j)
            {
                uint const at = texels[j] * stride + c;
                v[j] = inside[j] ? (from_a ? gSourceA[at] : gSourceB[at]) : 0.0;
            }

            uint const at_w = row + i * out_channels + o;
            float const w0 = gWeights[at_w];
            float const w1 = gWeights[at_w + column];
            float const w2 = gWeights[at_w + 2u * column];

            [unroll] for (uint t = 0; t < NN_CONV_TEXELS; ++t)
                sums[t] += w0 * v[t] + w1 * v[t + 1] + w2 * v[t + 2];
        }
    }

    // ReLU on every layer, the last one included — which is why the network's output is never negative, and why the
    // radiance it produces needs no clamp of its own.
    uint const base = (y * gConstants.width + x0) * out_channels + o;
    [unroll] for (uint t = 0; t < NN_CONV_TEXELS; ++t)
        if (x0 + t < gConstants.width)
            gTarget[base + t * out_channels] = max(sums[t], 0.0);
}
