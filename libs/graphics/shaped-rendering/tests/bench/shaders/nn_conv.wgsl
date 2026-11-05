// nn_conv.hlsl written by hand in WGSL, for the benchmark against SGL's text on WebGPU.
// The algorithm and its blocking are the HLSL's, NN_CONV_TEXELS included; the bindings are SGL's nn_conv_buffers and
// nn_conv_constants, slot for slot, so one binding group serves both contenders.
// WGSL has no [unroll]: the fixed-count loops are left to the compiler, as a WGSL author would write them.

const texels: u32 = 8u;

@group(0) @binding(0) var<storage, read> nn_conv_buffers_source_a: array<vec4f>;
@group(0) @binding(1) var<storage, read> nn_conv_buffers_source_b: array<vec4f>;
@group(0) @binding(2) var<storage, read> nn_conv_buffers_weights: array<f32>;
@group(0) @binding(3) var<storage, read_write> nn_conv_buffers_target: array<f32>;

struct nn_conv_constants {
    width: u32,
    height: u32,
    in_channels: u32,
    out_channels: u32,
    weight_offset: u32,
    bias_offset: u32,
    in_channels_a: u32,
}

@group(3) @binding(0) var<uniform> constants: nn_conv_constants;

@compute @workgroup_size(64, 1, 1)
fn main_cs(@builtin(global_invocation_id) id: vec3u) {
    let o = id.x;
    let x0 = id.y * texels;
    let y = id.z;
    if (o >= constants.out_channels || x0 >= constants.width || y >= constants.height) {
        return;
    }

    let bias = nn_conv_buffers_weights[constants.bias_offset + o];
    var sums: array<f32, 8>;
    for (var s = 0u; s < texels; s++) {
        sums[s] = bias;
    }

    let in_channels = constants.in_channels;
    let out_channels = constants.out_channels;
    let column = in_channels * out_channels;

    for (var ky = -1; ky <= 1; ky++) {
        let sy = i32(y) + ky;
        if (sy < 0 || sy >= i32(constants.height)) {
            continue;
        }

        let row = constants.weight_offset + u32(ky + 1) * 3u * column;

        var texel_at: array<u32, 10>;
        var inside: array<bool, 10>;
        for (var j = 0u; j < texels + 2u; j++) {
            let sx = i32(x0) + i32(j) - 1;
            inside[j] = sx >= 0 && sx < i32(constants.width);
            texel_at[j] = select(0u, u32(sy) * constants.width + u32(sx), inside[j]);
        }

        let a = constants.in_channels_a;
        let b = in_channels - a;

        for (var i = 0u; i < in_channels; i += 4u) {
            let from_a = i < a;
            let stride4 = select(b, a, from_a) >> 2u;
            let c4 = select(i - a, i, from_a) >> 2u;

            var v: array<vec4f, 10>;
            for (var j = 0u; j < texels + 2u; j++) {
                let at = texel_at[j] * stride4 + c4;
                if (inside[j]) {
                    v[j] = select(nn_conv_buffers_source_b[at], nn_conv_buffers_source_a[at], from_a);
                } else {
                    v[j] = vec4f(0.0);
                }
            }

            for (var d = 0u; d < 4u; d++) {
                let at_w = row + (i + d) * out_channels + o;
                let w0 = nn_conv_buffers_weights[at_w];
                let w1 = nn_conv_buffers_weights[at_w + column];
                let w2 = nn_conv_buffers_weights[at_w + 2u * column];
                for (var t = 0u; t < texels; t++) {
                    sums[t] += w0 * v[t][d] + w1 * v[t + 1u][d] + w2 * v[t + 2u][d];
                }
            }
        }
    }

    let base = (y * constants.width + x0) * out_channels + o;
    for (var t = 0u; t < texels; t++) {
        if (x0 + t < constants.width) {
            nn_conv_buffers_target[base + t * out_channels] = max(sums[t], 0.0);
        }
    }
}
