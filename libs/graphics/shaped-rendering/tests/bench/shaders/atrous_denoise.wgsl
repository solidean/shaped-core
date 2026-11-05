// atrous_denoise.hlsl written by hand in WGSL, for the benchmark against SGL's text on WebGPU.
// The algorithm is the HLSL's line for line; the bindings are SGL's atrous_images and atrous_constants, slot for slot,
// so one binding group serves both contenders.

const k_has_albedo: u32 = 1u << 0u;
const k_has_normal: u32 = 1u << 1u;
const k_has_depth: u32 = 1u << 2u;
const k_demodulate_in: u32 = 1u << 3u;
const k_remodulate_out: u32 = 1u << 4u;
const k_has_specular_albedo: u32 = 1u << 5u;
const k_has_sample_counts: u32 = 1u << 6u;
const k_albedo_floor: f32 = 1e-3;

@group(0) @binding(0) var atrous_images_source: texture_2d<f32>;
@group(0) @binding(1) var atrous_images_albedo: texture_2d<f32>;
@group(0) @binding(2) var atrous_images_specular_albedo: texture_2d<f32>;
@group(0) @binding(3) var atrous_images_normal: texture_2d<f32>;
@group(0) @binding(4) var atrous_images_depth: texture_2d<f32>;
@group(0) @binding(5) var atrous_images_sample_counts: texture_2d<f32>;
@group(0) @binding(6) var atrous_images_target: texture_storage_2d<rgba32float, write>;

struct atrous_constants {
    step: i32,
    flags: u32,
    luminance_scale: f32,
    normal_power: f32,
    depth_sigma: f32,
}

@group(3) @binding(0) var<uniform> constants: atrous_constants;

fn luminance(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

fn albedo_at(p: vec2i) -> vec3f {
    var a = textureLoad(atrous_images_albedo, p, 0).rgb;
    if ((constants.flags & k_has_specular_albedo) != 0u) {
        a += textureLoad(atrous_images_specular_albedo, p, 0).rgb;
    }
    return max(a, vec3f(k_albedo_floor));
}

fn source_at(p: vec2i) -> vec4f {
    var c = textureLoad(atrous_images_source, p, 0);
    if ((constants.flags & k_demodulate_in) != 0u) {
        c = vec4f(c.rgb / albedo_at(p), c.a);
    }
    return c;
}

const kernel = array<f32, 5>(1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

@compute @workgroup_size(8, 8, 1)
fn main_cs(@builtin(global_invocation_id) id: vec3u) {
    let size = textureDimensions(atrous_images_target);
    if (any(id.xy >= size)) {
        return;
    }

    let p = vec2i(id.xy);
    let flags = constants.flags;

    let centre = source_at(p);
    let centre_lum = luminance(centre.rgb);

    var centre_normal = vec3f(0.0);
    var centre_has_normal = false;
    if ((flags & k_has_normal) != 0u) {
        let n = textureLoad(atrous_images_normal, p, 0).rgb;
        centre_has_normal = dot(n, n) > 0.0;
        centre_normal = select(n, normalize(n), centre_has_normal);
    }

    var centre_depth = 0.0;
    if ((flags & k_has_depth) != 0u) {
        centre_depth = textureLoad(atrous_images_depth, p, 0).r;
    }

    var luminance_scale = constants.luminance_scale;
    if ((flags & k_has_sample_counts) != 0u) {
        luminance_scale /= sqrt(max(textureLoad(atrous_images_sample_counts, p, 0).r, 1.0));
    }
    let lum_width = luminance_scale * (abs(centre_lum) + 1e-2) + 1e-6;

    var sum = vec3f(0.0);
    var weight_sum = 0.0;

    for (var dy = -2; dy <= 2; dy++) {
        for (var dx = -2; dx <= 2; dx++) {
            let offset = vec2i(dx, dy) * constants.step;
            let q = p + offset;
            if (any(q < vec2i(0)) || any(q >= vec2i(size))) {
                continue;
            }

            let c = source_at(q);
            var w = kernel[dx + 2] * kernel[dy + 2];

            let lum_ratio = (luminance(c.rgb) - centre_lum) / lum_width;
            w *= exp(-lum_ratio * lum_ratio);

            if ((flags & k_has_normal) != 0u) {
                let n = textureLoad(atrous_images_normal, q, 0).rgb;
                let tap_has_normal = dot(n, n) > 0.0;
                if (centre_has_normal != tap_has_normal) {
                    w = 0.0;
                } else if (centre_has_normal) {
                    w *= pow(saturate(dot(centre_normal, normalize(n))), constants.normal_power);
                }
            }

            if ((flags & k_has_depth) != 0u) {
                let d = textureLoad(atrous_images_depth, q, 0).r;
                let centre_missed = centre_depth <= 0.0;
                let tap_missed = d <= 0.0;
                if (centre_missed != tap_missed) {
                    w = 0.0;
                } else if (!centre_missed) {
                    let allowed = constants.depth_sigma * centre_depth * length(vec2f(offset)) + 1e-5;
                    w *= exp(-abs(d - centre_depth) / allowed);
                }
            }

            sum += c.rgb * w;
            weight_sum += w;
        }
    }

    var result = sum / weight_sum;
    if ((flags & k_remodulate_out) != 0u) {
        result *= albedo_at(p);
    }
    textureStore(atrous_images_target, id.xy, vec4f(result, centre.a));
}
