// The cube's vertex stage — the WGSL twin of cube.hlsl's main_vs, for the WebGPU backend.
//
// Two rules of sg's WGSL make this file look different from its HLSL sibling, and both are the backend's rather than WGSL's:
// an attribute's `@location` is its index in the pipeline's vertex_input_layout, and inline constants live at group 3, binding 0.
// See libs/graphics/shaped-graphics/backends/webgpu/docs/wgsl.md.

struct vs_input {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) color: vec3f,
}

struct vs_output {
    @builtin(position) position: vec4f,
    @location(0) normal: vec3f,
    @location(1) color: vec3f,
}

struct cube_constants {
    view_projection: mat4x4f,
}

@group(3) @binding(0) var<uniform> constants: cube_constants;

@vertex
fn main_vs(input: vs_input) -> vs_output {
    var output: vs_output;
    output.position = constants.view_projection * vec4f(input.position, 1.0);
    output.normal = input.normal;
    output.color = input.color;
    return output;
}
