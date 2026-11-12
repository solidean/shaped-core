// svgf_atrous.hlsl and the part of svgf_common.hlsli it uses, written by hand in WGSL, for the benchmark against
// SGL's text on WebGPU.
// The algorithm is the HLSL's line for line; the bindings are SGL's svgf_atrous_images and svgf_atrous_constants,
// slot for slot, so one binding group serves both contenders.

const k_svgf_has_albedo: u32 = 1u << 0u;
const k_svgf_remodulate_out: u32 = 1u << 2u;
const k_svgf_has_specular_albedo: u32 = 1u << 3u;
const k_svgf_albedo_floor: f32 = 1e-3;

@group(0) @binding(0) var svgf_atrous_images_source: texture_2d<f32>;
@group(0) @binding(1) var svgf_atrous_images_normal_depth: texture_2d<f32>;
@group(0) @binding(2) var svgf_atrous_images_albedo: texture_2d<f32>;
@group(0) @binding(3) var svgf_atrous_images_specular_albedo: texture_2d<f32>;
@group(0) @binding(4) var svgf_atrous_images_color: texture_2d<f32>;
@group(0) @binding(5) var svgf_atrous_images_target: texture_storage_2d<rgba32float, write>;

struct svgf_atrous_constants {
    step: i32,
    flags: u32,
    luminance_sigma: f32,
    normal_power: f32,
    depth_sigma: f32,
}

@group(3) @binding(0) var<uniform> constants: svgf_atrous_constants;

fn svgf_reflectance(a: vec3f, specular: vec3f, flags: u32) -> vec3f {
    var r = a;
    if ((flags & k_svgf_has_specular_albedo) != 0u) {
        r += specular;
    }
    return max(r, vec3f(k_svgf_albedo_floor));
}

fn svgf_luminance(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

fn svgf_is_surface(n: vec3f, d: f32) -> bool {
    return dot(n, n) > 0.0 && d > 0.0;
}

fn svgf_geometry_weight(n_p: vec3f, d_p: f32, n_q: vec3f, d_q: f32, offset: vec2f, normal_power: f32,
                        depth_sigma: f32) -> f32 {
    let surface_p = svgf_is_surface(n_p, d_p);
    let surface_q = svgf_is_surface(n_q, d_q);
    if (surface_p != surface_q) {
        return 0.0;
    }
    if (!surface_p) {
        return 1.0;
    }
    let w_n = pow(saturate(dot(normalize(n_p), normalize(n_q))), normal_power);
    let allowed = depth_sigma * d_p * max(length(offset), 1.0) + 1e-5;
    return w_n * exp(-abs(d_q - d_p) / allowed);
}

const kernel = array<f32, 5>(1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

@compute @workgroup_size(8, 8, 1)
fn atrous_cs(@builtin(global_invocation_id) id: vec3u) {
    let size = textureDimensions(svgf_atrous_images_target);
    if (any(id.xy >= size)) {
        return;
    }

    let p = vec2i(id.xy);
    let flags = constants.flags;

    let centre = textureLoad(svgf_atrous_images_source, p, 0);
    let nd_p = textureLoad(svgf_atrous_images_normal_depth, p, 0);
    let centre_lum = svgf_luminance(centre.rgb);

    var blurred_variance = 0.0;
    var blur_weight = 0.0;
    for (var by = -1; by <= 1; by++) {
        for (var bx = -1; bx <= 1; bx++) {
            let q = clamp(p + vec2i(bx, by), vec2i(0), vec2i(size) - 1);
            let w = select(0.25, 0.5, bx == 0) * select(0.25, 0.5, by == 0);
            blurred_variance += textureLoad(svgf_atrous_images_source, q, 0).a * w;
            blur_weight += w;
        }
    }
    blurred_variance /= blur_weight;

    let lum_width = constants.luminance_sigma * sqrt(max(blurred_variance, 0.0)) + 1e-6;

    var sum = vec3f(0.0);
    var variance_sum = 0.0;
    var weight_sum = 0.0;
    for (var dy = -2; dy <= 2; dy++) {
        for (var dx = -2; dx <= 2; dx++) {
            let offset = vec2i(dx, dy) * constants.step;
            let q = p + offset;
            if (any(q < vec2i(0)) || any(q >= vec2i(size))) {
                continue;
            }

            let c = textureLoad(svgf_atrous_images_source, q, 0);
            let nd_q = textureLoad(svgf_atrous_images_normal_depth, q, 0);

            var w = kernel[dx + 2] * kernel[dy + 2];
            w *= svgf_geometry_weight(nd_p.xyz, nd_p.w, nd_q.xyz, nd_q.w, vec2f(offset), constants.normal_power,
                                      constants.depth_sigma);
            w *= exp(-abs(svgf_luminance(c.rgb) - centre_lum) / lum_width);

            sum += c.rgb * w;
            variance_sum += c.a * w * w;
            weight_sum += w;
        }
    }

    var result = sum / weight_sum;
    let variance = variance_sum / (weight_sum * weight_sum);

    var out_alpha = variance;
    if ((flags & k_svgf_remodulate_out) != 0u) {
        if ((flags & k_svgf_has_albedo) != 0u) {
            result *= svgf_reflectance(textureLoad(svgf_atrous_images_albedo, p, 0).rgb, textureLoad(svgf_atrous_images_specular_albedo, p, 0).rgb, flags);
        }
        out_alpha = textureLoad(svgf_atrous_images_color, p, 0).a;
    }
    textureStore(svgf_atrous_images_target, id.xy, vec4f(result, out_alpha));
}
