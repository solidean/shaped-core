// The cube's fragment stage — the WGSL twin of cube.hlsl's main_ps.
//
// A separate file because an sg WGSL file declares exactly one entry point; the interpolants must match cube_vs.wgsl's by location.

struct vs_output {
    @builtin(position) position: vec4f,
    @location(0) normal: vec3f,
    @location(1) color: vec3f,
}

@fragment
fn main_fs(input: vs_output) -> @location(0) vec4f {
    // A key light, a fill light and an ambient floor — the same three terms cube.hlsl uses, so both backends shade the cube identically.
    let n = normalize(input.normal);
    let key = saturate(dot(n, normalize(vec3f(0.45, 0.8, -0.4))));
    let fill = saturate(dot(n, normalize(vec3f(-0.7, 0.15, 0.6))));

    let lit = input.color * (0.25 + 0.8 * key + 0.25 * fill);
    return vec4f(lit, 1.0);
}
